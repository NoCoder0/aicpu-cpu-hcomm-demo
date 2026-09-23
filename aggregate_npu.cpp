// A3控制面沿用基础版；聚合数据面在aggregate_kernel.cpp中直接调用标准Hcomm接口。
#include "common.h"
#include "aggregate_protocol.h"
#include <acl/acl.h>
#include <dlfcn.h>

static void VerifyLibrary()
{
    Dl_info info{};
    void *symbol = dlsym(RTLD_DEFAULT, "HcommEndpointCreate");
    Require(symbol != nullptr && dladdr(symbol, &info) != 0, "resolve HCOMM library");
    printf("LIBRARY HcommEndpointCreate => %s\n", info.dli_fname);
}

static void LaunchAggregate(ThreadHandle thread, ChannelHandle channel, void *hbm, const CommMem &remote,
    const char *kernelConfig, int control, uint32_t warmup, uint32_t rounds)
{
    aclrtBinaryLoadOption option{};
    option.type = ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    option.value.cpuKernelMode = 0;
    aclrtBinaryLoadOptions options{};
    options.numOpt = 1;
    options.options = &option;
    aclrtBinHandle binary = nullptr;
    CHECK_API(aclrtBinaryLoadFromFile(kernelConfig, &options, &binary));
    aclrtFuncHandle function = nullptr;
    CHECK_API(aclrtBinaryGetFunction(binary, "AggregateKernel", &function));
    AggregateArgs args{};
    void *deviceArgs = nullptr;
    CHECK_API(aclrtMalloc(&deviceArgs, sizeof(args), ACL_MEM_MALLOC_NORMAL_ONLY));
    aclrtArgsHandle packed = nullptr;
    aclrtParamHandle parameter = nullptr;
    CHECK_API(aclrtKernelArgsInit(function, &packed));
    CHECK_API(aclrtKernelArgsAppend(packed, &deviceArgs, sizeof(deviceArgs), &parameter));
    CHECK_API(aclrtKernelArgsFinalize(packed));
    aclrtStream launchStream = nullptr, completionStream = nullptr;
    CHECK_API(aclrtCreateStream(&launchStream));
    CHECK_API(aclrtCreateStream(&completionStream));
    aclrtNotify completionNotify = nullptr;
    CHECK_API(aclrtCreateNotify(&completionNotify, ACL_NOTIFY_DEFAULT));
    uint32_t notifyId = 0;
    CHECK_API(aclrtGetNotifyId(completionNotify, &notifyId));
    aclrtLaunchKernelAttr attribute{};
    attribute.id = ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT;
    attribute.value.timeout = 60;
    aclrtLaunchKernelCfg config{};
    config.numAttrs = 1;
    config.attrs = &attribute;
    std::vector<unsigned char> actual(BUFFER_BYTES);
    for (uint32_t generation = 1; generation <= warmup + rounds; ++generation) {
        uint32_t wire = htonl(generation), ack = 0;
        Transfer(control, &wire, sizeof(wire), true);
        Transfer(control, &ack, sizeof(ack), false);
        Require(wire == ack, "aggregate source generation acknowledgement");
        // 目标和scratch每轮清零，参数/地址表准备均在测量之外。
        CHECK_API(aclrtMemset(hbm, NPU_AGGREGATE_BYTES, GUARD_VALUE, NPU_AGGREGATE_BYTES));
        AggregateRequest request{};
        request.generation = generation;
        for (uint32_t i = 0; i < BLOCK_COUNT; ++i)
            request.addresses[i] = reinterpret_cast<uint64_t>(remote.addr) + SourceOffset(i, generation);
        AggregateSignal doorbell{};
        doorbell.generation = generation;
        CHECK_API(aclrtMemcpy(static_cast<unsigned char *>(hbm) + NPU_REQUEST, sizeof(request),
            &request, sizeof(request), ACL_MEMCPY_HOST_TO_DEVICE));
        CHECK_API(aclrtMemcpy(static_cast<unsigned char *>(hbm) + NPU_DOORBELL, sizeof(doorbell),
            &doorbell, sizeof(doorbell), ACL_MEMCPY_HOST_TO_DEVICE));
        args = AggregateArgs{};
        args.thread = thread;
        args.channel = channel;
        args.localBase = reinterpret_cast<uint64_t>(hbm);
        args.remoteBase = reinterpret_cast<uint64_t>(remote.addr);
        args.completionNotifyId = notifyId;
        args.generation = generation;
        CHECK_API(aclrtMemcpy(deviceArgs, sizeof(args), &args, sizeof(args), ACL_MEMCPY_HOST_TO_DEVICE));
        const auto begin = std::chrono::steady_clock::now();
        int32_t launchRc = aclrtLaunchKernelWithConfig(function, SCATTER_LANES, launchStream, &config, packed, nullptr);
        int32_t streamRc = launchRc == 0 ? aclrtSynchronizeStream(launchStream) : launchRc;
        const auto kernelReturned = std::chrono::steady_clock::now();
        // Kernel已包含等Host与scatter；仍需消费请求通信线程的Drain完成通知再释放/复用资源。
        int32_t waitRc = streamRc == 0 ? aclrtWaitAndResetNotify(completionNotify, completionStream, 30000) : streamRc;
        int32_t completeRc = waitRc == 0 ? aclrtSynchronizeStream(completionStream) : waitRc;
        const auto completed = std::chrono::steady_clock::now();
        CHECK_API(launchRc);
        CHECK_API(streamRc);
        CHECK_API(waitRc);
        CHECK_API(completeRc);
        CHECK_API(aclrtMemcpy(&args, sizeof(args), deviceArgs, sizeof(args), ACL_MEMCPY_DEVICE_TO_HOST));
        CHECK_API(args.error);
        Require(args.nextLane == SCATTER_LANES && args.completedLanes == SCATTER_LANES, "all scatter lanes completed");
        CHECK_API(aclrtMemcpy(actual.data(), actual.size(), hbm, actual.size(), ACL_MEMCPY_DEVICE_TO_HOST));
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
                throw std::runtime_error("aggregate HBM payload/guard mismatch");
            }
        }
        auto us = [](std::chrono::steady_clock::duration value) {
            return std::chrono::duration<double, std::micro>(value).count();
        };
        printf("SAMPLE generation=%u warmup=%u blocks=%u block_bytes=%u payload_bytes=%llu "
               "request_us=%.3f wait_host_us=%.3f scatter_us=%.3f kernel_e2e_us=%.3f "
               "launch_stream_us=%.3f completion_wait_us=%.3f host_complete_us=%.3f verified=1\n",
            generation, generation <= warmup, BLOCK_COUNT, BLOCK_BYTES, static_cast<unsigned long long>(PAYLOAD_BYTES),
            args.requestNs / 1000.0, args.waitHostNs / 1000.0, args.scatterNs / 1000.0, args.totalNs / 1000.0,
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
        Require(argc >= 2 && argc <= 4, "usage: aggregate_npu <standard_read.json> [rounds=100] [warmup=10]");
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
        CHECK_API(aclrtMalloc(&hbm, NPU_AGGREGATE_BYTES, ACL_MEM_MALLOC_NORMAL_ONLY));
        CHECK_API(aclrtMemset(hbm, NPU_AGGREGATE_BYTES, 0xa5, NPU_AGGREGATE_BYTES));
        CommMem memory{COMM_MEM_TYPE_DEVICE, hbm, NPU_AGGREGATE_BYTES};
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
            peer.memory.size >= HOST_AGGREGATE_BYTES && peer.listenPort > 0, "Host memory/listen port");

        Stage("4. HcommChannelCreate 创建连接，GetStatus 推进建链直到 READY");
        ChannelHandle channel = CreateChannel(endpoint, false, static_cast<uint16_t>(peer.listenPort));
        WaitChannelReady(channel);
        Barrier(control, false, 1); // 两端都 READY，Host 在此后保持 DRAM/MR 有效。

        Stage("5. HcommThreadAlloc 创建 AICPU_TS 通信线程，启动请求/Host gather/写回/六lane scatter kernel");
        ThreadHandle thread = 0;
        uint32_t notifyCount = 1;
        CHECK_API(HcommThreadAlloc(COMM_ENGINE_AICPU_TS, 1, &notifyCount, &thread));
        LaunchAggregate(thread, channel, hbm, peer.memory, argv[1], control, warmup, rounds);
        puts("PASS: 1600 x 656-byte standard HCOMM aggregate; all generations and guards verified");
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
