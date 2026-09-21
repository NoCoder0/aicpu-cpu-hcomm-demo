// 审阅入口：同一文件包含两种编译目标，避免把标准 READ 藏在另一个封装库中。
// STANDARD_HCOMM_AICPU：下面的 StandardReadKernel 在 AICPU 上执行。
// 默认：main 在 A3 宿主机上执行，用标准 Hcomm* 外部接口创建通信资源。
// 本版在独立 CANN 9.1 容器构建；实际验证状态见 RESULT_NOCODER.md。
#include <hcomm_primitives.h>
#include <cstdint>

struct ReadArgs {
    ThreadHandle thread;
    ChannelHandle channel;
    uint64_t destination;
    uint64_t source;
    uint64_t bytes;
    int32_t readResult;
    int32_t drainResult;
    int32_t batchStartResult;
    int32_t batchEndResult;
};

#ifdef STANDARD_HCOMM_AICPU

// ACL kernel 参数区内存放一个指向 ReadArgs 的设备地址。
// 这里的 thread/channel 都是标准 HCOMM 在 AICPU 侧创建的句柄，
// 不是 RA qp handle，也不能在宿主机上 reinterpret_cast 后调用。
extern "C" __attribute__((visibility("default"))) uint32_t StandardReadKernel(void *rawArgs)
{
    if (rawArgs == nullptr) return 2;
    auto *args = reinterpret_cast<ReadArgs *>(*static_cast<uint64_t *>(rawArgs));
    if (args == nullptr) return 2;

    // AICPU 原语先生成通信任务；使用公开批量接口明确提交时点。
    // Start / READ / Drain / End 必须在同一个 AICPU 执行线程内调用。
    args->batchStartResult = HcommBatchModeStart("standard-read");
    if (args->batchStartResult != 0) return 0;

    // 真正的数据读取：标准上游 HCOMM READ，目标为 HBM，源为导入的 Host DRAM 地址。
    args->readResult = HcommReadOnThread(args->thread, args->channel,
        reinterpret_cast<void *>(args->destination), reinterpret_cast<const void *>(args->source), args->bytes);
    if (args->readResult != 0) return 0; // 错误保存在结果块，由 host 读取并判失败。

    // READ 返回不等于数据已经到达。Drain 生成完成等待，BatchEnd 提交，Host 再同步设备。
    args->drainResult = HcommChannelDrainOnThread(args->thread, args->channel);
    if (args->drainResult != 0) return 0;
    args->batchEndResult = HcommBatchModeEnd("standard-read");
    return 0;
}

#else

#include "common.h"
#include <acl/acl.h>
#include <dlfcn.h>

static void VerifyLibrary()
{
    Dl_info info{};
    void *symbol = dlsym(RTLD_DEFAULT, "HcommEndpointCreate");
    Require(symbol != nullptr && dladdr(symbol, &info) != 0, "resolve HCOMM library");
    printf("LIBRARY HcommEndpointCreate => %s\n", info.dli_fname);
}

static void LaunchRead(ThreadHandle thread, ChannelHandle channel, void *hbm, const CommMem &remote,
    const char *kernelConfig)
{
    // 此 9.1 分支尚无公开的 HcommThreadResGetInfo。
    // 后面用 ACL 的设备同步接口等待当前设备的通信任务，不解引用 hcomm 私有句柄。

    // 自定义的只是“调用标准 API 的用户 kernel”，不是自定义通信接口或 READ 后端。
    // 该 kernel 必须与本版 hcomm 的设备库一起正确打包部署；不能混用旧 ccl_kernel.so。
    aclrtBinaryLoadOption option{};
    option.type = ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    option.value.cpuKernelMode = 0;
    aclrtBinaryLoadOptions options{};
    options.numOpt = 1;
    options.options = &option;
    aclrtBinHandle binary = nullptr;
    CHECK_API(aclrtBinaryLoadFromFile(kernelConfig, &options, &binary));
    aclrtFuncHandle function = nullptr;
    CHECK_API(aclrtBinaryGetFunction(binary, "StandardReadKernel", &function));

    ReadArgs args{thread, channel, reinterpret_cast<uint64_t>(hbm), reinterpret_cast<uint64_t>(remote.addr),
        sizeof(PAYLOAD), -1, -1, -1, -1};
    void *deviceArgs = nullptr;
    CHECK_API(aclrtMalloc(&deviceArgs, sizeof(args), ACL_MEM_MALLOC_NORMAL_ONLY));
    CHECK_API(aclrtMemcpy(deviceArgs, sizeof(args), &args, sizeof(args), ACL_MEMCPY_HOST_TO_DEVICE));

    aclrtArgsHandle packed = nullptr;
    aclrtParamHandle parameter = nullptr;
    CHECK_API(aclrtKernelArgsInit(function, &packed));
    CHECK_API(aclrtKernelArgsAppend(packed, &deviceArgs, sizeof(deviceArgs), &parameter));
    CHECK_API(aclrtKernelArgsFinalize(packed));
    aclrtStream launchStream = nullptr;
    CHECK_API(aclrtCreateStream(&launchStream));
    aclrtLaunchKernelAttr attribute{};
    attribute.id = ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT;
    attribute.value.timeout = 30;
    aclrtLaunchKernelCfg config{};
    config.numAttrs = 1;
    config.attrs = &attribute;
    CHECK_API(aclrtLaunchKernelWithConfig(function, 1, launchStream, &config, packed, nullptr));

    // 第一次同步保证 AICPU 已提交 READ/Drain 并写回返回码；设备同步等待通信任务完成。
    CHECK_API(aclrtSynchronizeStream(launchStream));
    CHECK_API(aclrtMemcpy(&args, sizeof(args), deviceArgs, sizeof(args), ACL_MEMCPY_DEVICE_TO_HOST));
    CHECK_API(args.batchStartResult);
    CHECK_API(args.readResult);
    CHECK_API(args.drainResult);
    CHECK_API(args.batchEndResult);
    CHECK_API(aclrtSynchronizeDevice());
    CHECK_API(aclrtDestroyStream(launchStream));
    CHECK_API(aclrtFree(deviceArgs));
    CHECK_API(aclrtBinaryUnLoad(binary));
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    alarm(300); // 最终超时退出进程，失败时不提前释放可能仍被 DMA 引用的 MR。
    try {
        const bool probeOnly = argc == 2 && std::string(argv[1]) == "--probe-endpoint";
        const bool probeThread = argc == 2 && std::string(argv[1]) == "--probe-thread";
        Require(argc == 2, "usage: npu_reader <standard_read.json> | --probe-endpoint | --probe-thread");
        VerifyLibrary();
        Stage("1. 初始化 ACL，选择 NPU2，创建 DEVICE/ROCE Endpoint");
        CHECK_API(aclInit(nullptr));
        CHECK_API(aclrtSetDevice(NPU_DEVICE_ID));
        int32_t physicalId = -1;
        CHECK_API(aclrtGetPhyDevIdByLogicDevId(NPU_DEVICE_ID, &physicalId));
        Require(physicalId == NPU_DEVICE_ID, "selected physical device must match RDMA IP");
        EndpointDesc local = MakeEndpoint(true, static_cast<uint32_t>(physicalId));
        EndpointHandle endpoint = nullptr;
        // 这是卡侧网络资源入口。上游 v9.0.0 在这里拒绝 DEVICE + ROCE。
        CHECK_API(HcommEndpointCreate(&local, &endpoint));
        if (probeOnly || probeThread) {
            if (probeThread) {
                // 单独核验标准 AICPU_TS 线程及配套设备 kernel 的实际加载。
                // 没有 Channel，也没有发 READ；此探测不能输出端到端 PASS。
                ThreadHandle thread = 0;
                uint32_t notifyCount = 1;
                CHECK_API(HcommThreadAlloc(COMM_ENGINE_AICPU_TS, 1, &notifyCount, &thread));
                CHECK_API(aclrtSynchronizeDevice());
                CHECK_API(HcommThreadFree(&thread, 1));
            }
            CHECK_API(HcommEndpointDestroy(endpoint));
            CHECK_API(aclrtResetDevice(NPU_DEVICE_ID));
            CHECK_API(aclFinalize());
            puts("RESOURCE PROBE ONLY: success does not prove channel or RDMA READ support");
            return 0;
        }

        Stage("2. 分配 HBM，使用 HcommMemReg/Export 注册并导出内存");
        void *hbm = nullptr;
        CHECK_API(aclrtMalloc(&hbm, BUFFER_BYTES, ACL_MEM_MALLOC_NORMAL_ONLY));
        CHECK_API(aclrtMemset(hbm, BUFFER_BYTES, 0xa5, BUFFER_BYTES));
        CommMem memory{COMM_MEM_TYPE_DEVICE, hbm, BUFFER_BYTES};
        HcommMemHandle registration = nullptr;
        CHECK_API(HcommMemReg(endpoint, "npu-hbm", &memory, &registration));
        void *description = nullptr;
        uint32_t descriptionBytes = 0;
        CHECK_API(HcommMemExport(endpoint, registration, &description, &descriptionBytes));

        Stage("3. TCP 交换 opaque 描述符，HcommMemImport 导入 Host DRAM");
        int control = OpenControl(false);
        PeerMemory peer = ExchangeMemory(control, false, 0, description, descriptionBytes);
        CHECK_API(HcommMemImport(endpoint, peer.descriptor.data(), peer.descriptor.size(), &peer.memory));
        Require(peer.memory.type == COMM_MEM_TYPE_HOST && peer.memory.addr != nullptr &&
            peer.memory.size >= sizeof(PAYLOAD) && peer.listenPort > 0, "Host memory/listen port");

        Stage("4. HcommChannelCreate 创建连接，GetStatus 推进建链直到 READY");
        ChannelHandle channel = CreateChannel(endpoint, false, static_cast<uint16_t>(peer.listenPort));
        WaitChannelReady(channel);
        Barrier(control, false, 1); // 两端都 READY，Host 在此后保持 DRAM/MR 有效。

        Stage("5. HcommThreadAlloc 创建 AICPU_TS 通信线程，启动标准 READ kernel");
        ThreadHandle thread = 0;
        uint32_t notifyCount = 1;
        CHECK_API(HcommThreadAlloc(COMM_ENGINE_AICPU_TS, 1, &notifyCount, &thread));
        LaunchRead(thread, channel, hbm, peer.memory, argv[1]);

        Stage("6. 仅为验证将 HBM 回拷 CPU，检查16字节数据和4080字节哨兵");
        unsigned char actual[BUFFER_BYTES];
        CHECK_API(aclrtMemcpy(actual, sizeof(actual), hbm, BUFFER_BYTES, ACL_MEMCPY_DEVICE_TO_HOST));
        printf("HBM first 16 bytes:");
        for (size_t i = 0; i < sizeof(PAYLOAD); ++i) printf(" %02x", actual[i]);
        puts("");
        Require(memcmp(actual, PAYLOAD, sizeof(PAYLOAD)) == 0, "HBM payload mismatch");
        for (size_t i = sizeof(PAYLOAD); i < sizeof(actual); ++i) Require(actual[i] == 0xa5, "HBM guard mismatch");
        puts("PASS: standard HcommReadOnThread HBM='hello rdma demo', bytes=16, guards=4080 unchanged");
        Barrier(control, false, 2); // 只有验证通过后，才通知 Host 可以清理源 MR。

        Stage("7. 已无在途请求，释放线程/通道，再注销和释放内存");
        CHECK_API(HcommThreadFree(&thread, 1));
        CHECK_API(HcommChannelDestroy(&channel, 1));
        CHECK_API(HcommMemUnimport(endpoint, peer.descriptor.data(), peer.descriptor.size()));
        CHECK_API(HcommMemUnreg(endpoint, registration));
        CHECK_API(HcommEndpointDestroy(endpoint));
        CHECK_API(aclrtFree(hbm));
        close(control);
        CHECK_API(aclrtResetDevice(NPU_DEVICE_ID));
        CHECK_API(aclFinalize());
        return 0;
    } catch (const std::exception &error) {
        fprintf(stderr, "FAIL (not validated): %s\n", error.what());
        // 由退出进程回收资源；不在完成状态未知时注销 MR / 释放 HBM。
        std::_Exit(1);
    }
}
#endif
