#include "control.h"
#include "acl/acl.h"
#include "hccp.h"
#include "rdma_lite.h"
#include "runtime/rt.h"
#include "runtime/rts/rts_device.h"
#include <chrono>
#include <thread>

static void Check(int code, const char *operation)
{
    printf("%s rc=%d\n", operation, code);
    if (code != 0) Die(operation);
}
#define CHECK(call) Check((int)(call), #call)

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    alarm(90);
    CHECK(aclInit(nullptr));
    CHECK(aclrtSetDevice(0));
    aclrtStream stream = nullptr;
    CHECK(aclrtCreateStream(&stream));
    void *hbm = nullptr;
    CHECK(aclrtMalloc(&hbm, DEMO_BYTES, ACL_MEM_MALLOC_HUGE_FIRST));
    CHECK(aclrtMemset(hbm, DEMO_BYTES, 0xa5, DEMO_BYTES));
    // Same network-service initialization as hcomm adapter_tdt.cc:hrtOpenTsd.
    const char serviceArg[] = "--hdcType=6";
    rtProcExtParam parameter{};
    parameter.paramInfo = serviceArg; parameter.paramLen = strlen(serviceArg);
    rtNetServiceOpenArgs service{};
    service.extParamList = &parameter; service.extParamCnt = 1;
    CHECK(rtOpenNetService(&service));
    RaInitConfig config{};
    config.phyId = 0; config.nicPosition = NETWORK_OFFLINE; config.hdcType = 6;
    CHECK(RaInit(&config));
    unsigned version = 0;
    CHECK(RaGetInterfaceVersion(0, 46, &version));
    printf("TYPICAL_QP_MODIFY version=%u sizeof(TypicalQp)=%zu sizeof(SendWrV2)=%zu sizeof(WC)=%zu\n",
           version, sizeof(TypicalQp), sizeof(SendWrV2), sizeof(rdma_lite_wc_v2));
    RdevInitInfo info{};
    info.mode = NETWORK_OFFLINE; info.notifyType = NOTIFY;
    info.enabled2mbLite = true; info.disabledLiteThread = true;
    rdev device{};
    device.phyId = 0; device.family = AF_INET;
    inet_pton(AF_INET, "20.168.0.1", &device.localIp.addr);
    void *rdma = nullptr, *qp = nullptr;
    CHECK(RaRdevInitV2(info, device, &rdma));
    TypicalQp local{}, remote{};
    CHECK(RaTypicalQpCreate(rdma, 0, 2, &local, &qp));
    local.retryCnt = 7; local.retryTime = 14;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) Die("socket");
    SocketTimeout(fd);
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(DEMO_PORT);
    inet_pton(AF_INET, "10.1.101.32", &address.sin_addr);
    if (connect(fd, (sockaddr *)&address, sizeof(address))) Die("connect Host4");
    char line[512], gid[INET6_ADDRSTRLEN];
    unsigned long long sourceAddress;
    unsigned sourceKey, length, hostMtu;
    ReadLine(fd, line, sizeof(line));
    if (sscanf(line, "HOST1 %u %u %45s %llx %u %u %u", &remote.qpn, &remote.psn, gid,
               &sourceAddress, &sourceKey, &length, &hostMtu) != 7 || length != sizeof(DEMO_TEXT) ||
        hostMtu < 1 || hostMtu > 5) Die("Host metadata");
    if (inet_pton(AF_INET6, gid, remote.gid) != 1) Die("Host GID");
    CHECK(RaTypicalQpModify(qp, &local, &remote));
    QpAttr attr{};
    CHECK(RaGetQpAttr(qp, &attr));
    printf("RaGetQpAttr raw: qpn=%u psn=%u gidIdx=%u pathMtu=%d\n", attr.qpn, attr.psn, attr.gidIdx, attr.pathMtu);
    // Installed CANN 9.0.0 RaGetQpAttr only writes the first 32 bytes; pathMtu is unavailable.
    // This demo is strictly 16 bytes, below every verbs MTU (minimum 256), with one response packet.
    // Do not extend it to larger transfers without querying/matching both QP path MTUs.
    GidText(local.gid, gid);
    printf("NPU RDMA=20.168.0.1 gid=%s gid_index=%u qpn=%u NPU_mtu=unavailable Host_mtu=%u\n",
           gid, local.gidIdx, local.qpn, hostMtu);
    snprintf(line, sizeof(line), "NPU1 %u %u %s %u\n", local.qpn, local.psn, gid, hostMtu);
    SendLine(fd, line);
    ReadLine(fd, line, sizeof(line));
    if (strcmp(line, "READY")) Die("Host QP not ready");
    MrInfoT mr{};
    mr.addr = hbm; mr.size = DEMO_BYTES; mr.access = RA_ACCESS_LOCAL_WRITE;
    CHECK(RaMrReg(qp, &mr));
    SgList sge{};
    sge.addr = (uint64_t)(uintptr_t)hbm; sge.len = length; sge.lkey = mr.lkey;
    SendWrV2 wr{};
    wr.wrId = 1; wr.bufList = &sge; wr.bufNum = 1; wr.dstAddr = sourceAddress;
    wr.rkey = sourceKey; wr.op = RA_WR_RDMA_READ; wr.sendFlag = RA_SEND_SIGNALED;
    SendWrRsp response{};
    CHECK(RaSendWrV2(qp, &wr, &response));
    CHECK(rtRDMADBSend(response.db.dbIndex, response.db.dbInfo, stream));
    rdma_lite_wc_v2 completion{};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    int count = 0;
    while ((count = RaPollCq(qp, true, 1, &completion)) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    printf("CQ count=%d wr_id=%llu status=%d opcode=%d vendor_err=%u byte_len=%u\n", count,
           completion.wc.wr_id, completion.wc.status, completion.wc.opcode,
           completion.wc.vendor_err, completion.wc.byte_len);
    if (count != 1 || completion.wc.status != RDMA_LITE_WC_SUCCESS || completion.wc.wr_id != 1 ||
        completion.wc.opcode != RDMA_LITE_WC_RDMA_READ) Die("RDMA completion");
    CHECK(aclrtSynchronizeStream(stream));
    unsigned char result[DEMO_BYTES];
    CHECK(aclrtMemcpy(result, sizeof(result), hbm, DEMO_BYTES, ACL_MEMCPY_DEVICE_TO_HOST));
    if (memcmp(result, DEMO_TEXT, sizeof(DEMO_TEXT))) Die("HBM payload mismatch");
    for (size_t i = length; i < sizeof(result); ++i) {
        if (result[i] != 0xa5) Die("HBM guard modified");
    }
    printf("PASS: HBM='%s' bytes=%u SGE=1 guard_bytes=%zu unchanged\n", result, length, sizeof(result) - length);
    SendLine(fd, "PASS\n");
    close(fd);
    CHECK(RaMrDereg(qp, &mr));
    CHECK(RaQpDestroy(qp));
    CHECK(RaRdevDeinit(rdma, NOTIFY));
    CHECK(RaDeinit(&config));
    CHECK(rtCloseNetService());
    CHECK(aclrtFree(hbm));
    CHECK(aclrtDestroyStream(stream));
    CHECK(aclrtResetDevice(0));
    CHECK(aclFinalize());
    return 0;
}
