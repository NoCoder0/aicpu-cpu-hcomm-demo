#include "../demo/control.h"
#include "acl/acl.h"
#include "typical_read.h"
#include <dlfcn.h>
#include <fstream>
#include <set>
#include <string>

static void Check(int code, const char *operation)
{
    printf("%s rc=%d\n", operation, code);
    if (code != 0) Die(operation);
}
#define CHECK(call) Check(static_cast<int>(call), #call)

static void ShowLibrary(const char *symbol)
{
    void *address = dlsym(RTLD_DEFAULT, symbol);
    Dl_info info{};
    if (!address || !dladdr(address, &info)) Die("resolve hcomm library");
    printf("LIBRARY %s => %s\n", symbol, info.dli_fname);
}

static void CheckReadValidation(const AscendQPInfo &qp, const AscendMrInfo &local,
    const AscendMrInfo &remote, aclrtStream stream)
{
    AscendReadCompletion completion{};
    if (HcclReadByAscendQP(nullptr, &local, &remote, 15000, stream, &completion) != HCCL_E_PTR) {
        Die("READ null-QP validation");
    }
    AscendMrInfo invalid = local;
    invalid.size = 0;
    if (HcclReadByAscendQP(&qp, &invalid, &remote, 15000, stream, &completion) != HCCL_E_PARA) {
        Die("READ zero-length validation");
    }
    invalid.size = 257;
    AscendMrInfo largerRemote = remote;
    largerRemote.size = 257;
    if (HcclReadByAscendQP(&qp, &invalid, &largerRemote, 15000, stream, &completion) != HCCL_E_PARA) {
        Die("READ multi-packet validation");
    }
    invalid = local;
    invalid.addr = UINT64_MAX;
    if (HcclReadByAscendQP(&qp, &invalid, &remote, 15000, stream, &completion) != HCCL_E_PARA) {
        Die("READ address-overflow validation");
    }
    if (HcclReadByAscendQP(&qp, &local, &remote, 0, stream, &completion) != HCCL_E_PARA) {
        Die("READ timeout validation");
    }
    printf("PASS: hcomm READ rejects null QP, zero length, multi-packet length, overflow and zero timeout\n");
}

static void ShowLoadedCommunicationLibraries()
{
    std::ifstream maps("/proc/self/maps");
    std::set<std::string> paths;
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libhcomm.so") == std::string::npos &&
            line.find("libhccl_plf.so") == std::string::npos &&
            line.find("libra.so") == std::string::npos &&
            line.find("libra_hdc.so") == std::string::npos) continue;
        auto offset = line.find('/');
        if (offset != std::string::npos) paths.insert(line.substr(offset));
    }
    for (const auto &path : paths) printf("LOADED %s\n", path.c_str());
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    alarm(90);
    ShowLibrary("HcclGetCommName");
    ShowLibrary("HcclReadByAscendQP");
    ShowLibrary("hcclCreateAscendQP");
    CHECK(aclInit(nullptr));
    CHECK(aclrtSetDevice(0));
    aclrtStream stream = nullptr;
    CHECK(aclrtCreateStream(&stream));
    CHECK(HcclRdmaInitForRead());
    ShowLoadedCommunicationLibraries();
    void *hbm = nullptr;
    CHECK(hcclAllocWindowMem(&hbm, DEMO_BYTES));
    CHECK(aclrtMemset(hbm, DEMO_BYTES, 0xa5, DEMO_BYTES));
    AscendMrInfo mr{};
    mr.addr = reinterpret_cast<uintptr_t>(hbm);
    mr.size = DEMO_BYTES;
    CHECK(hcclRegisterMem(&mr));
    AscendQPInfo local{}, remote{};
    CHECK(hcclCreateAscendQP(&local));
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) Die("socket");
    SocketTimeout(fd);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(DEMO_PORT);
    inet_pton(AF_INET, "10.1.101.32", &address.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address))) Die("connect Host4");
    char line[512], gid[INET6_ADDRSTRLEN];
    unsigned long long sourceAddress;
    unsigned sourceKey, length, hostMtu;
    ReadLine(fd, line, sizeof(line));
    if (sscanf(line, "HOST1 %u %u %45s %llx %u %u %u", &remote.qpn, &remote.psn, gid,
               &sourceAddress, &sourceKey, &length, &hostMtu) != 7 || length != sizeof(DEMO_TEXT) ||
        hostMtu < 1 || hostMtu > 5) Die("Host metadata");
    if (inet_pton(AF_INET6, gid, remote.gid) != 1) Die("Host GID");
    AscendQPQos qos{};
    CHECK(hcclModifyAscendQPEx(&local, &remote, &qos));
    GidText(local.gid, gid);
    printf("NPU hcomm QP qpn=%u gid=%s gid_index=%u\n", local.qpn, gid, local.gidIdx);
    snprintf(line, sizeof(line), "NPU1 %u %u %s %u\n", local.qpn, local.psn, gid, hostMtu);
    SendLine(fd, line);
    ReadLine(fd, line, sizeof(line));
    if (strcmp(line, "READY")) Die("Host QP not ready");
    AscendMrInfo destination = mr;
    destination.size = length;
    AscendMrInfo source{};
    source.addr = sourceAddress;
    source.size = length;
    source.key = sourceKey;
    CheckReadValidation(local, destination, source, stream);
    AscendReadCompletion completion{};
    CHECK(HcclReadByAscendQP(&local, &destination, &source, 15000, stream, &completion));
    printf("HCOMM READ CQ wr_id=%llu status=%u opcode=%u byte_len=%u vendor_err=%u\n",
           static_cast<unsigned long long>(completion.wrId), completion.status, completion.opcode,
           completion.byteLen, completion.vendorError);
    CHECK(aclrtSynchronizeStream(stream));
    unsigned char result[DEMO_BYTES];
    CHECK(aclrtMemcpy(result, sizeof(result), hbm, DEMO_BYTES, ACL_MEMCPY_DEVICE_TO_HOST));
    if (memcmp(result, DEMO_TEXT, sizeof(DEMO_TEXT))) Die("HBM payload mismatch");
    for (size_t i = length; i < sizeof(result); ++i) {
        if (result[i] != 0xa5) Die("HBM guard modified");
    }
    printf("PASS: source-built hcomm HBM='%s' bytes=%u SGE=1 guard_bytes=%zu unchanged\n",
           result, length, sizeof(result) - length);
    SendLine(fd, "PASS\n");
    close(fd);
    CHECK(hcclDestroyAscendQP(&local));
    CHECK(hcclDeRegisterMem(&mr));
    CHECK(hcclFreeWindowMem(hbm));
    CHECK(hcclAscendRdmaDeInit());
    CHECK(aclrtDestroyStream(stream));
    CHECK(aclrtResetDevice(0));
    CHECK(aclFinalize());
    return 0;
}
