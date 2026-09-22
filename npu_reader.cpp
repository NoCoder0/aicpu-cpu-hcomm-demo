// 审阅入口：同一文件包含两种编译目标，避免把标准 READ 藏在另一个封装库中。
// STANDARD_HCOMM_AICPU：下面的 StandardReadKernel 在 AICPU 上执行。
// 默认：main 在 A3 宿主机上执行，用标准 Hcomm* 外部接口创建通信资源。
// 本版在独立 CANN 9.1 容器构建；实际验证状态见 BENCHMARK_VALIDATION.md。
#include <hcomm_primitives.h>
#include <cstdint>
#include <chrono>
#include "benchmark.h"

struct ReadArgs {
    ThreadHandle thread;
    ChannelHandle channel;
    uint64_t destination;
    uint64_t source;
    uint32_t generation;
    uint32_t issued;
    uint64_t submitNs;
    uint64_t completionNotifyId;
    int32_t readResult;
    int32_t drainResult;
    int32_t batchStartResult;
    int32_t batchEndResult;
    int32_t notifyResult;
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
    const auto begin = std::chrono::steady_clock::now();

    // AICPU 原语先生成通信任务；使用公开批量接口明确提交时点。
    // Start / READ / Drain / End 必须在同一个 AICPU 执行线程内调用。
    args->batchStartResult = HcommBatchModeStart("standard-read");
    if (args->batchStartResult != 0) return 0;

    // 真正的数据读取：标准上游 HCOMM READ，目标为 HBM，源为导入的 Host DRAM 地址。
    for (uint32_t block = 0; block < BLOCK_COUNT; ++block) {
        args->readResult = HcommReadOnThread(args->thread, args->channel,
            reinterpret_cast<void *>(args->destination + GUARD_BYTES + block * DEST_STRIDE),
            reinterpret_cast<const void *>(args->source + SourceOffset(block, args->generation)), BLOCK_BYTES);
        if (args->readResult != 0) break;
        ++args->issued;
    }

    // READ 返回不等于数据已经到达。Drain 生成完成等待，随后 Record 向 ACL 等待流发信号。
    args->drainResult = HcommChannelDrainOnThread(args->thread, args->channel);
    // 将通信线程上的 Drain 完成依赖显式连接到 ACL 等待流。
    args->notifyResult = HcommAclrtNotifyRecordOnThread(args->thread, args->completionNotifyId);
    args->batchEndResult = HcommBatchModeEnd("standard-read");
    args->submitNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count();
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
    const char *kernelConfig, int control, uint32_t warmup, uint32_t rounds)
{
    // 此 9.1 分支尚无公开的 HcommThreadResGetInfo。
    // 使用公开 HcommAclrtNotifyRecordOnThread + ACL wait/reset 建立完成依赖。
    // 单独 aclrtSynchronizeDevice() 不能替代对 AICPU_TS 通信线程的显式等待。

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

    ReadArgs args{};
    void *deviceArgs = nullptr;
    CHECK_API(aclrtMalloc(&deviceArgs, sizeof(args), ACL_MEM_MALLOC_NORMAL_ONLY));

    aclrtArgsHandle packed = nullptr;
    aclrtParamHandle parameter = nullptr;
    CHECK_API(aclrtKernelArgsInit(function, &packed));
    CHECK_API(aclrtKernelArgsAppend(packed, &deviceArgs, sizeof(deviceArgs), &parameter));
    CHECK_API(aclrtKernelArgsFinalize(packed));
    aclrtStream launchStream = nullptr;
    CHECK_API(aclrtCreateStream(&launchStream));
    aclrtStream completionStream = nullptr;
    CHECK_API(aclrtCreateStream(&completionStream));
    aclrtNotify completionNotify = nullptr;
    CHECK_API(aclrtCreateNotify(&completionNotify, ACL_NOTIFY_DEFAULT));
    uint32_t completionNotifyId = 0;
    CHECK_API(aclrtGetNotifyId(completionNotify, &completionNotifyId));
    aclrtLaunchKernelAttr attribute{};
    attribute.id = ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT;
    attribute.value.timeout = 30;
    aclrtLaunchKernelCfg config{};
    config.numAttrs = 1;
    config.attrs = &attribute;
    std::vector<unsigned char> actual(BUFFER_BYTES);
    for (uint32_t generation = 1; generation <= warmup + rounds; ++generation) {
        uint32_t wireGeneration = htonl(generation), acknowledged = 0;
        Transfer(control, &wireGeneration, sizeof(wireGeneration), true);
        Transfer(control, &acknowledged, sizeof(acknowledged), false);
        Require(acknowledged == wireGeneration, "source generation acknowledgement");
        CHECK_API(aclrtMemset(hbm, BUFFER_BYTES, GUARD_VALUE, BUFFER_BYTES));
        args = {thread, channel, reinterpret_cast<uint64_t>(hbm), reinterpret_cast<uint64_t>(remote.addr),
            generation, 0, 0, completionNotifyId, -1, -1, -1, -1, -1};
        CHECK_API(aclrtMemcpy(deviceArgs, sizeof(args), &args, sizeof(args), ACL_MEMCPY_HOST_TO_DEVICE));

        // 主机墙钟只包围下发和同步，不包含建链、源数据准备、清零、参数拷贝和校验。
        const auto begin = std::chrono::steady_clock::now();
        int32_t launchRc = aclrtLaunchKernelWithConfig(function, 1, launchStream, &config, packed, nullptr);
        int32_t streamRc = launchRc == 0 ? aclrtSynchronizeStream(launchStream) : launchRc;
        const auto kernelReturned = std::chrono::steady_clock::now();
        int32_t waitRc = streamRc == 0 ? aclrtWaitAndResetNotify(completionNotify, completionStream, 30000) : streamRc;
        int32_t deviceRc = waitRc == 0 ? aclrtSynchronizeStream(completionStream) : waitRc;
        const auto completed = std::chrono::steady_clock::now();
        CHECK_API(launchRc);
        CHECK_API(streamRc);
        CHECK_API(waitRc);
        CHECK_API(deviceRc);
        CHECK_API(aclrtMemcpy(&args, sizeof(args), deviceArgs, sizeof(args), ACL_MEMCPY_DEVICE_TO_HOST));
        CHECK_API(args.batchStartResult);
        CHECK_API(args.readResult);
        CHECK_API(args.drainResult);
        CHECK_API(args.batchEndResult);
        CHECK_API(args.notifyResult);
        Require(args.issued == BLOCK_COUNT, "not all logical READ calls issued");
        CHECK_API(aclrtMemcpy(actual.data(), actual.size(), hbm, BUFFER_BYTES, ACL_MEMCPY_DEVICE_TO_HOST));
        for (size_t offset = 0; offset < actual.size(); ++offset) {
            unsigned char expected = GUARD_VALUE;
            if (offset >= GUARD_BYTES && offset < GUARD_BYTES + BLOCK_COUNT * DEST_STRIDE) {
                size_t relative = offset - GUARD_BYTES;
                if (relative % DEST_STRIDE < BLOCK_BYTES)
                    expected = PayloadByte(relative / DEST_STRIDE, relative % DEST_STRIDE, generation);
            }
            if (actual[offset] != expected) {
                fprintf(stderr, "Mismatch generation=%u offset=%zu actual=%u expected=%u\n",
                    generation, offset, actual[offset], expected);
                throw std::runtime_error("HBM payload/guard mismatch");
            }
        }
        auto us = [](std::chrono::steady_clock::duration value) {
            return std::chrono::duration<double, std::micro>(value).count();
        };
        // submit_us 是核内 API 生成/提交时间，不能当作 RDMA 传输完成时延。
        printf("SAMPLE generation=%u warmup=%u blocks=%u block_bytes=%u payload_bytes=%llu "
               "submit_us=%.3f launch_stream_us=%.3f completion_wait_us=%.3f host_complete_us=%.3f verified=1\n",
            generation, generation <= warmup, BLOCK_COUNT, BLOCK_BYTES,
            static_cast<unsigned long long>(PAYLOAD_BYTES), args.submitNs / 1000.0,
            us(kernelReturned - begin), us(completed - kernelReturned), us(completed - begin));
    }
    uint32_t finish = 0;
    Transfer(control, &finish, sizeof(finish), true);
    CHECK_API(aclrtDestroyStream(launchStream));
    CHECK_API(aclrtDestroyStream(completionStream));
    CHECK_API(aclrtDestroyNotify(completionNotify));
    CHECK_API(aclrtFree(deviceArgs));
    CHECK_API(aclrtBinaryUnLoad(binary));
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    alarm(300); // 最终超时退出进程，失败时不提前释放可能仍被 DMA 引用的 MR。
    try {
        Require(argc >= 2 && argc <= 4, "usage: npu_reader <standard_read.json> [rounds=100] [warmup=10]");
        auto parseCount = [](const char *value, bool zeroAllowed) {
            char *end = nullptr;
            unsigned long n = strtoul(value, &end, 10);
            Require(end != value && *end == 0 && n <= 10000 && (zeroAllowed || n > 0), "round count 1..10000");
            return static_cast<uint32_t>(n);
        };
        uint32_t rounds = argc > 2 ? parseCount(argv[2], false) : 100;
        uint32_t warmup = argc > 3 ? parseCount(argv[3], true) : 10;
        VerifyLibrary();
        Stage("1. 初始化 ACL，选择 NPU2，创建 DEVICE/ROCE Endpoint");
        CHECK_API(aclInit(nullptr));
        CHECK_API(aclrtSetDevice(NPU_DEVICE_ID));
        int32_t physicalId = -1;
        CHECK_API(aclrtGetPhyDevIdByLogicDevId(NPU_DEVICE_ID, &physicalId));
        Require(physicalId == NPU_DEVICE_ID, "selected physical device must match RDMA IP");
        EndpointDesc local = MakeEndpoint(true, static_cast<uint32_t>(physicalId));
        EndpointHandle endpoint = nullptr;
        // 这是卡侧网络资源入口，由本仓固定的 hcomm 版本提供 DEVICE/ROCE 实现。
        CHECK_API(HcommEndpointCreate(&local, &endpoint));
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
            peer.memory.size >= SOURCE_BYTES && peer.listenPort > 0, "Host memory/listen port");

        Stage("4. HcommChannelCreate 创建连接，GetStatus 推进建链直到 READY");
        ChannelHandle channel = CreateChannel(endpoint, false, static_cast<uint16_t>(peer.listenPort));
        WaitChannelReady(channel);
        Barrier(control, false, 1); // 两端都 READY，Host 在此后保持 DRAM/MR 有效。

        Stage("5. HcommThreadAlloc 创建 AICPU_TS 通信线程，启动标准 READ kernel");
        ThreadHandle thread = 0;
        uint32_t notifyCount = 1;
        CHECK_API(HcommThreadAlloc(COMM_ENGINE_AICPU_TS, 1, &notifyCount, &thread));
        LaunchRead(thread, channel, hbm, peer.memory, argv[1], control, warmup, rounds);
        puts("PASS: 1600 x 656-byte standard HCOMM READ; all generations and guards verified");
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
