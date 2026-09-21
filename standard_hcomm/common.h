// 标准 HCOMM demo 的公共控制面。此处只交换 opaque 内存描述符和监听端口，
// 不解析 rkey/QPN/PSN，不调用 verbs/RA，也不通过 TCP 搬运被测字符串。
#pragma once
#include <hcomm_res.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

constexpr const char *HOST_CONTROL_IP = "10.1.101.32";
constexpr const char *HOST_RDMA_IP = "20.168.0.17";
constexpr const char *NPU_RDMA_IP = "20.168.0.1";
constexpr uint16_t CONTROL_PORT = 19516; // 与已有 demo 的19515隔离。
constexpr size_t BUFFER_BYTES = 4096;
constexpr char PAYLOAD[] = "hello rdma demo";

inline void Require(bool ok, const char *operation)
{
    if (!ok) throw std::runtime_error(operation);
}

inline void Check(int32_t rc, const char *operation)
{
    printf("API %s => %d\n", operation, rc);
    if (rc != 0) throw std::runtime_error(std::string(operation) + " rc=" + std::to_string(rc));
}
#define CHECK_API(call) Check(static_cast<int32_t>(call), #call)

inline void Stage(const char *name) { printf("\nSTAGE %s\n", name); }

inline EndpointDesc MakeEndpoint(bool device, uint32_t physicalId = 0)
{
    EndpointDesc desc{};
    CHECK_API(EndpointDescInit(&desc, 1));
    desc.protocol = COMM_PROTOCOL_ROCE;
    desc.commAddr.type = COMM_ADDR_TYPE_IP_V4;
    Require(inet_pton(AF_INET, device ? NPU_RDMA_IP : HOST_RDMA_IP, &desc.commAddr.addr) == 1, "RDMA IP");
    desc.loc.locType = device ? ENDPOINT_LOC_TYPE_DEVICE : ENDPOINT_LOC_TYPE_HOST;
    // 这组固定拓扑中，RoCE 按上述 IP 选设备；DEVICE physicalId 从 ACL 查询。
    // 其他拓扑字段保留 EndpointDescInit 的未指定值，不伪造 superPod/superDev 编号。
    if (device) desc.loc.device.devPhyId = physicalId;
    else desc.loc.host.id = 0;
    return desc;
}

inline void ConfigureSocket(int fd)
{
    Require(fd >= 0, "control socket");
    timeval timeout{120, 0};
    Require(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0, "SO_RCVTIMEO");
    Require(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0, "SO_SNDTIMEO");
}

inline int OpenControl(bool server)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ConfigureSocket(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONTROL_PORT);
    Require(inet_pton(AF_INET, HOST_CONTROL_IP, &addr.sin_addr) == 1, "control IP");
    if (!server) {
        Require(connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "connect control");
        return fd;
    }
    int one = 1;
    Require(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0, "SO_REUSEADDR");
    Require(bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0, "bind control");
    Require(listen(fd, 1) == 0, "listen control");
    printf("Control listening %s:%u\n", HOST_CONTROL_IP, CONTROL_PORT);
    int peer = accept(fd, nullptr, nullptr);
    close(fd);
    ConfigureSocket(peer);
    return peer;
}

// TCP 是字节流，必须处理短读/短写；一条发送不等于一条接收。
inline void Transfer(int fd, void *data, size_t size, bool sendData)
{
    auto *p = static_cast<unsigned char *>(data);
    while (size != 0) {
        ssize_t n = sendData ? send(fd, p, size, MSG_NOSIGNAL) : recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        Require(n > 0, sendData ? "control send" : "control receive/peer closed");
        p += n;
        size -= static_cast<size_t>(n);
    }
}

struct PeerMemory {
    uint32_t listenPort = 0;
    // HcommMemUnimport 仍需要原始描述符，所以一直保留到清理结束。
    std::vector<unsigned char> descriptor;
    CommMem memory{};
};

inline PeerMemory ExchangeMemory(int fd, bool server, uint32_t localPort, void *opaque, uint32_t length)
{
    constexpr uint32_t MAGIC = 0x48434d31; // HCM1：本 demo 的控制协议版本。
    constexpr uint32_t MAX_DESCRIPTOR = 64 * 1024;
    Require(opaque != nullptr && length > 0 && length <= MAX_DESCRIPTOR, "exported descriptor length");
    uint32_t outgoing[3] = {htonl(MAGIC), htonl(localPort), htonl(length)};
    PeerMemory peer;
    auto sendLocal = [&] {
        Transfer(fd, outgoing, sizeof(outgoing), true);
        Transfer(fd, opaque, length, true);
    };
    auto receivePeer = [&] {
        uint32_t incoming[3]{};
        Transfer(fd, incoming, sizeof(incoming), false);
        Require(ntohl(incoming[0]) == MAGIC, "control protocol version");
        peer.listenPort = ntohl(incoming[1]);
        uint32_t bytes = ntohl(incoming[2]);
        Require(peer.listenPort <= UINT16_MAX && bytes > 0 && bytes <= MAX_DESCRIPTOR, "peer descriptor bounds");
        peer.descriptor.resize(bytes);
        Transfer(fd, peer.descriptor.data(), bytes, false);
    };
    // 固定发送顺序避免两端同时发送大描述符导致死锁。
    if (server) { sendLocal(); receivePeer(); }
    else { receivePeer(); sendLocal(); }
    return peer;
}

inline ChannelHandle CreateChannel(EndpointHandle endpoint, bool server, uint16_t listenPort)
{
    HcommChannelDesc desc{};
    CHECK_API(HcommChannelDescInit(&desc, 1));
    desc.remoteEndpoint = MakeEndpoint(server); // Host 对端是 DEVICE；NPU 对端是 HOST。
    desc.role = server ? HCOMM_SOCKET_ROLE_SERVER : HCOMM_SOCKET_ROLE_CLIENT;
    desc.port = listenPort; // 来自 HcommEndpointGetListenPort，不猜测内部默认端口。
    desc.exchangeAllMems = true;
    desc.notifyNum = 0;
    desc.channelName = "standard-host-to-hbm"; // 两端相同，供 hcomm 匹配本条连接。
    desc.roceAttr.queueNum = 1;
    desc.roceAttr.tc = 0;
    desc.roceAttr.sl = 0;
    ChannelHandle channel = 0;
    CommEngine engine = server ? COMM_ENGINE_CPU : COMM_ENGINE_AICPU_TS;
    CHECK_API(HcommChannelCreate(endpoint, engine, &desc, 1, &channel));
    return channel;
}

inline void WaitChannelReady(ChannelHandle channel)
{
    // GetStatus 既查询也推进建链，不能创建句柄后仅 sleep，或直接开始读写。
    // 状态值取自此版本公开文档：0=READY，1=CONNECTING，2..5=失败。
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    int32_t previous = -1;
    do {
        int32_t status = -1;
        // 高频轮询只在失败时打印 API 错误，避免 CONNECTING 日志淹没建链过程。
        int32_t rc = HcommChannelGetStatus(&channel, 1, &status);
        if (rc != 0) Check(rc, "HcommChannelGetStatus");
        if (status != previous) printf("CHANNEL status=%d\n", status);
        previous = status;
        if (status == 0) return;
        Require(status == 1, "hcomm channel failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("hcomm channel timeout");
}

inline void Barrier(int fd, bool server, uint32_t value)
{
    uint32_t sent = htonl(value), received = 0;
    if (server) { Transfer(fd, &sent, sizeof(sent), true); Transfer(fd, &received, sizeof(received), false); }
    else { Transfer(fd, &received, sizeof(received), false); Transfer(fd, &sent, sizeof(sent), true); }
    Require(ntohl(received) == value, "peer stage mismatch");
}
