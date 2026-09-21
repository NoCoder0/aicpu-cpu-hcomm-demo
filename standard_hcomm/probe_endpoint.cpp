// 最小现场能力探测：只调用标准 Endpoint 接口，不建 QP、不访问 Host 内存。
// 独立编译是为了能检查旧库，而不要求旧库导出新版的 ThreadResGetInfo 等符号。
#include <hcomm_res.h>
#include <acl/acl.h>
#include <cstdio>
#include <unistd.h>
#include <dlfcn.h>

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    alarm(30);
    Dl_info library{};
    void *symbol = dlsym(RTLD_DEFAULT, "HcommEndpointCreate");
    if (symbol == nullptr || !dladdr(symbol, &library)) return 1;
    printf("LIBRARY %s\n", library.dli_fname);
    int rc = aclInit(nullptr);
    printf("aclInit rc=%d\n", rc);
    if (rc != 0) return 1;
    rc = aclrtSetDevice(0);
    printf("aclrtSetDevice(0) rc=%d\n", rc);
    if (rc != 0) return 1;
    EndpointDesc desc{};
    if (EndpointDescInit(&desc, 1) != 0) return 1;
    desc.protocol = COMM_PROTOCOL_ROCE;
    desc.commAddr.type = COMM_ADDR_TYPE_IP_V4;
    if (inet_pton(AF_INET, "20.168.0.1", &desc.commAddr.addr) != 1) return 1;
    desc.loc.locType = ENDPOINT_LOC_TYPE_DEVICE;
    desc.loc.device.devPhyId = 0;
    EndpointHandle endpoint = nullptr;
    rc = HcommEndpointCreate(&desc, &endpoint);
    printf("HcommEndpointCreate(DEVICE, ROCE, 20.168.0.1) rc=%d\n", rc);
    if (rc == 0) {
        int cleanup = HcommEndpointDestroy(endpoint);
        printf("HcommEndpointDestroy rc=%d\n", cleanup);
        if (cleanup != 0) rc = cleanup;
    }
    int reset = aclrtResetDevice(0);
    int final = aclFinalize();
    printf("aclrtResetDevice rc=%d, aclFinalize rc=%d\n", reset, final);
    puts("Endpoint-only probe: this is NOT a successful RDMA READ test.");
    return rc == 0 && reset == 0 && final == 0 ? 0 : 1;
}
