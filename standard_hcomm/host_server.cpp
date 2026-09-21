// 鲲鹏无卡端：标准 HCOMM HOST/ROCE Endpoint + 上游 host-only NIC 插件。
// 与旧版 host_server.c 的区别：应用不再调用 ibv_create_qp/modify_qp/reg_mr。
#include "common.h"
#include <dlfcn.h>

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    alarm(300);
    try {
        const bool probeOnly = argc == 2 && std::string(argv[1]) == "--probe-endpoint";
        Require(argc == 1 || probeOnly, "usage: host_server [--probe-endpoint]");
        // 记录实际加载库，排查新头文件误配旧 hcomm 库的情况。
        Dl_info library{};
        void *symbol = dlsym(RTLD_DEFAULT, "HcommEndpointCreate");
        Require(symbol != nullptr && dladdr(symbol, &library) != 0, "resolve HCOMM library");
        printf("LIBRARY HcommEndpointCreate => %s\n", library.dli_fname);
        Stage("1. 创建 HOST/ROCE Endpoint，hcomm 插件管理 mlx5 网络资源");
        EndpointDesc desc = MakeEndpoint(false);
        EndpointHandle endpoint = nullptr;
        CHECK_API(HcommEndpointCreate(&desc, &endpoint));
        if (probeOnly) {
            CHECK_API(HcommEndpointDestroy(endpoint));
            puts("ENDPOINT PROBE ONLY: success does not prove channel or RDMA READ support");
            return 0;
        }

        Stage("2. 分配并注册 Host DRAM，填入唯一被测字符串");
        void *dram = nullptr;
        Require(posix_memalign(&dram, BUFFER_BYTES, BUFFER_BYTES) == 0, "allocate Host DRAM");
        memset(dram, 0, BUFFER_BYTES);
        memcpy(dram, PAYLOAD, sizeof(PAYLOAD));
        CommMem memory{COMM_MEM_TYPE_HOST, dram, BUFFER_BYTES};
        HcommMemHandle registration = nullptr;
        CHECK_API(HcommMemReg(endpoint, "host-dram", &memory, &registration));
        void *description = nullptr;
        uint32_t bytes = 0, listenPort = 0;
        CHECK_API(HcommMemExport(endpoint, registration, &description, &bytes));
        CHECK_API(HcommEndpointGetListenPort(endpoint, &listenPort));
        Require(listenPort > 0 && listenPort <= UINT16_MAX, "hcomm listen port");

        Stage("3. TCP 交换 opaque MR 描述符；标准接口导入对端 HBM");
        int control = OpenControl(true);
        PeerMemory peer = ExchangeMemory(control, true, listenPort, description, bytes);
        CHECK_API(HcommMemImport(endpoint, peer.descriptor.data(), peer.descriptor.size(), &peer.memory));
        Require(peer.memory.type == COMM_MEM_TYPE_DEVICE && peer.memory.addr != nullptr, "remote HBM");

        Stage("4. 标准 HcommChannelCreate/GetStatus 完成 Host 侧建链");
        ChannelHandle channel = CreateChannel(endpoint, true, static_cast<uint16_t>(listenPort));
        WaitChannelReady(channel);
        Barrier(control, true, 1);

        Stage("5. 保持 DRAM/MR 有效，等待 NPU 完成 READ 和数据验证");
        // 本端是 RDMA READ responder，不需要再发送一个 READ，也不通过 TCP 发送 payload。
        Barrier(control, true, 2);
        puts("PASS: NPU confirmed standard HCOMM READ and HBM validation");

        Stage("6. 先销毁通道，再注销 MR 和释放源 DRAM");
        CHECK_API(HcommChannelDestroy(&channel, 1));
        CHECK_API(HcommMemUnimport(endpoint, peer.descriptor.data(), peer.descriptor.size()));
        CHECK_API(HcommMemUnreg(endpoint, registration));
        CHECK_API(HcommEndpointDestroy(endpoint));
        free(dram);
        close(control);
        return 0;
    } catch (const std::exception &error) {
        fprintf(stderr, "FAIL (not validated): %s\n", error.what());
        std::_Exit(1);
    }
}
