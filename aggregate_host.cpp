// 鲲鹏侧：收到AICPU地址表后，16个常驻worker gather，再通过标准Hcomm写回。
#include "common.h"
#include "aggregate_protocol.h"
#include "gather_pool.h"
#include <sstream>

static std::vector<int> GatherCpus()
{
    std::vector<int> result;
    const char *setting = getenv("HCOMM_GATHER_CPUS");
    if (!setting) return std::vector<int>(16, -1);
    std::stringstream input(setting);
    std::string token;
    while (std::getline(input, token, ',')) {
        size_t parsed = 0;
        int cpu = std::stoi(token, &parsed);
        Require(parsed == token.size() && cpu >= 0 && cpu < CPU_SETSIZE, "gather CPU list");
        for (int existing : result) Require(existing != cpu, "duplicate gather CPU");
        result.push_back(cpu);
    }
    Require(result.size() == 16, "this comparison uses exactly 16 gather workers");
    return result;
}
static uint64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    alarm(300);
    try {
        // 可显式绑定Host服务线程和worker；启动时记录，保证测量配置可追溯。
        const char *mainCpu = getenv("HCOMM_HOST_CPU");
        if (mainCpu) PinCpu(std::stoi(mainCpu));
        auto cpus = GatherCpus();
        printf("AGGREGATE gather_threads=16 service_cpu=%s gather_cpus=%s\n",
            mainCpu ? mainCpu : "unbound", getenv("HCOMM_GATHER_CPUS") ? getenv("HCOMM_GATHER_CPUS") : "unbound");
        GatherPool pool(cpus);
        EndpointDesc desc = MakeEndpoint(false);
        EndpointHandle endpoint = nullptr;
        CHECK_API(HcommEndpointCreate(&desc, &endpoint));
        void *allocation = nullptr;
        Require(posix_memalign(&allocation, 4096, HOST_AGGREGATE_BYTES) == 0, "allocate aggregate Host MR");
        auto *dram = static_cast<unsigned char *>(allocation);
        memset(dram, 0, HOST_AGGREGATE_BYTES);
        CommMem memory{COMM_MEM_TYPE_HOST, dram, HOST_AGGREGATE_BYTES};
        HcommMemHandle registration = nullptr;
        CHECK_API(HcommMemReg(endpoint, "host-dram", &memory, &registration));
        void *opaque = nullptr;
        uint32_t bytes = 0;
        CHECK_API(HcommMemExport(endpoint, registration, &opaque, &bytes));
        int control = OpenControl(true);
        PeerMemory peer = ExchangeMemory(control, true, HCOMM_LISTEN_PORT, opaque, bytes);
        // 聚合版Host主动写HBM，必须导入NPU注册描述符。
        CHECK_API(HcommMemImport(endpoint, peer.descriptor.data(), peer.descriptor.size(), &peer.memory));
        Require(peer.memory.type == COMM_MEM_TYPE_DEVICE && peer.memory.addr && peer.memory.size >= NPU_AGGREGATE_BYTES,
            "aggregate NPU MR shape");
        ChannelHandle channel = CreateChannel(endpoint, true, HCOMM_LISTEN_PORT);
        WaitChannelReady(channel);
        Barrier(control, true, 1);
        uint32_t expectedGeneration = 1;
        auto *request = reinterpret_cast<AggregateRequest *>(dram + HOST_REQUEST);
        auto *doorbell = reinterpret_cast<volatile AggregateSignal *>(dram + HOST_DOORBELL);
        auto *ready = reinterpret_cast<AggregateSignal *>(dram + HOST_READY_SOURCE);
        for (;;) {
            uint32_t wireGeneration;
            Transfer(control, &wireGeneration, sizeof(wireGeneration), false);
            uint32_t generation = ntohl(wireGeneration);
            if (generation == 0) break;
            Require(generation == expectedGeneration++, "aggregate generation order");
            for (uint32_t block = 0; block < BLOCK_COUNT; ++block) {
                auto *source = dram + SourceOffset(block, generation);
                for (uint32_t byte = 0; byte < BLOCK_BYTES; ++byte) source[byte] = PayloadByte(block, byte, generation);
            }
            Transfer(control, &wireGeneration, sizeof(wireGeneration), true);
            const uint64_t deadline = NowNs() + 30000000000ULL;
            while (doorbell->generation != generation) {
                if (NowNs() >= deadline) throw std::runtime_error("Host request doorbell timeout");
                CpuRelax();
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint64_t serviceBegin = NowNs();
            int32_t status = request->generation == generation ? 0 : -2001;
            for (uint32_t i = 0; i < BLOCK_COUNT && status == 0; ++i) {
                // 验证实际传来的地址表，不能悄悄用本地重算的地址代替请求。
                if (request->addresses[i] != reinterpret_cast<uint64_t>(dram) + SourceOffset(i, generation)) status = -2002;
            }
            const uint64_t gatherBegin = NowNs();
            if (status == 0) pool.Run(dram + HOST_AGGREGATE, request);
            const uint64_t gatherDone = NowNs();
            if (status == 0) {
                status = HcommWriteNbi(channel, static_cast<unsigned char *>(peer.memory.addr) + NPU_SCRATCH,
                    dram + HOST_AGGREGATE, PAYLOAD_BYTES);
                if (status == 0) status = HcommChannelFence(channel); // Host等待写CQ完成。
            }
            const uint64_t writeDone = NowNs();
            *ready = AggregateSignal{};
            ready->generation = (uint64_t(generation) << 32) | static_cast<uint32_t>(status);
            int32_t readyRc = HcommWriteNbi(channel, static_cast<unsigned char *>(peer.memory.addr) + NPU_READY,
                ready, sizeof(ready->generation));
            if (readyRc == 0) readyRc = HcommChannelFence(channel);
            const uint64_t readyDone = NowNs();
            if (readyRc != 0) Check(readyRc, "ready write/fence");
            if (status != 0) Check(status, "aggregate host request/gather/write");
            printf("HOST_SAMPLE generation=%u gather_us=%.3f write_us=%.3f ready_us=%.3f service_us=%.3f\n",
                generation, (gatherDone - gatherBegin) / 1000.0, (writeDone - gatherDone) / 1000.0,
                (readyDone - writeDone) / 1000.0, (readyDone - serviceBegin) / 1000.0);
        }
        Barrier(control, true, 2);
        puts("PASS: aggregate Host received final NPU validation");
        CHECK_API(HcommChannelDestroy(&channel, 1));
        CHECK_API(HcommMemUnimport(endpoint, peer.descriptor.data(), peer.descriptor.size()));
        CHECK_API(HcommMemUnreg(endpoint, registration));
        CHECK_API(HcommEndpointDestroy(endpoint));
        free(dram);
        close(control);
        return 0;
    } catch (const std::exception &error) {
        fprintf(stderr, "FAIL aggregate Host: %s\n", error.what());
        std::_Exit(1);
    }
}
