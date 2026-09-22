// 鲲鹏无卡端：标准 HCOMM HOST/ROCE Endpoint + 上游 host-only NIC 插件。
// 应用只调用公开 Hcomm 接口，由库管理 QP、MR 和建链。
#include "common.h"
#include <dlfcn.h>

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    alarm(300);
    try {
        // 记录实际加载库，排查新头文件误配旧 hcomm 库的情况。
        Dl_info library{};
        void *symbol = dlsym(RTLD_DEFAULT, "HcommEndpointCreate");
        Require(symbol != nullptr && dladdr(symbol, &library) != 0, "resolve HCOMM library");
        printf("LIBRARY HcommEndpointCreate => %s\n", library.dli_fname);
        Stage("1. 创建 HOST/ROCE Endpoint，hcomm 插件管理 mlx5 网络资源");
        EndpointDesc desc = MakeEndpoint(false);
        EndpointHandle endpoint = nullptr;
        CHECK_API(HcommEndpointCreate(&desc, &endpoint));
        Stage("2. 分配并注册离散源池；每轮填入包含轮次和块编号的数据");
        void *dram = nullptr;
        Require(posix_memalign(&dram, 4096, SOURCE_BYTES) == 0, "allocate Host DRAM");
        memset(dram, 0, SOURCE_BYTES); // 预触页不在测量区间内。
        CommMem memory{COMM_MEM_TYPE_HOST, dram, SOURCE_BYTES};
        HcommMemHandle registration = nullptr;
        CHECK_API(HcommMemReg(endpoint, "host-dram", &memory, &registration));
        void *description = nullptr;
        uint32_t bytes = 0, listenPort = HCOMM_LISTEN_PORT;
        CHECK_API(HcommMemExport(endpoint, registration, &description, &bytes));
        // 此 9.1 分支对插件 Endpoint 的 GetListenPort 返回 NOT_SUPPORT。
        // 使用公开 ChannelDesc.port；实际监听仍由 HcommChannelCreate 内部完成。

        Stage("3. TCP 交换 opaque MR 描述符，NPU 使用标准接口导入 Host DRAM");
        int control = OpenControl(true);
        ExchangeMemory(control, true, listenPort, description, bytes);
        // Host 仅作 READ responder，不访问对端 HBM，因此无需导入 NPU 的 MR。
        // hcomm Channel 自己交换建链资源；应用不解析 opaque 描述符中的 rkey。

        Stage("4. 标准 HcommChannelCreate/GetStatus 完成 Host 侧建链");
        ChannelHandle channel = CreateChannel(endpoint, true, static_cast<uint16_t>(listenPort));
        WaitChannelReady(channel);
        Barrier(control, true, 1);

        Stage("5. 保持 DRAM/MR 有效，等待 NPU 完成 READ 和数据验证");
        // 本端是 RDMA READ responder，不需要再发送一个 READ，也不通过 TCP 发送 payload。
        uint32_t expectedGeneration = 1;
        for (;;) {
            uint32_t wireGeneration = 0;
            Transfer(control, &wireGeneration, sizeof(wireGeneration), false);
            uint32_t generation = ntohl(wireGeneration);
            if (generation == 0) break;
            Require(generation == expectedGeneration++, "round generation mismatch");
            for (uint32_t block = 0; block < BLOCK_COUNT; ++block) {
                auto *source = static_cast<unsigned char *>(dram) + SourceOffset(block, generation);
                for (uint32_t byte = 0; byte < BLOCK_BYTES; ++byte)
                    source[byte] = PayloadByte(block, byte, generation);
            }
            // CPU 源数据准备好后再回复；下一轮请求只会在 NPU 完成同步和校验后到达。
            Transfer(control, &wireGeneration, sizeof(wireGeneration), true);
        }
        Barrier(control, true, 2);
        puts("PASS: NPU confirmed standard HCOMM READ and HBM validation");

        Stage("6. 先销毁通道，再注销 MR 和释放源 DRAM");
        CHECK_API(HcommChannelDestroy(&channel, 1));
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
