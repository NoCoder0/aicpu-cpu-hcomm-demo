// 参考UB聚合的四阶段流程，以标准Hcomm接口用于A3 RoCE。
// 六个AICPU lane共享同步块；lane 0发布请求，所有lane在ready之后scatter。
#include "aggregate_protocol.h"
#include <chrono>
#include <cstring>

static uint64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void Invalidate(uint64_t start, uint64_t bytes)
{
    for (uint64_t p = start & ~uint64_t(63); p < start + bytes; p += 64)
        asm volatile("dc civac, %0" : : "r"(p) : "memory");
    asm volatile("dsb sy" : : : "memory");
}
static void Clean(uint64_t start, uint64_t bytes)
{
    for (uint64_t p = start & ~uint64_t(63); p < start + bytes; p += 64)
        asm volatile("dc cvac, %0" : : "r"(p) : "memory");
}
static int32_t PublishRequest(AggregateArgs &a)
{
    int32_t rc = HcommBatchModeStart("aggregate-request");
    if (rc != 0) return rc;
    // Request和doorbell都已在计时前上传HBM；仅发送元数据，没有经TCP搬运payload。
    rc = HcommWriteOnThread(a.thread, a.channel, reinterpret_cast<void *>(a.remoteBase + HOST_REQUEST),
        reinterpret_cast<const void *>(a.localBase + NPU_REQUEST), sizeof(AggregateRequest));
    if (rc == 0) rc = HcommChannelDrainOnThread(a.thread, a.channel);
    if (rc == 0) rc = HcommWriteOnThread(a.thread, a.channel,
        reinterpret_cast<void *>(a.remoteBase + HOST_DOORBELL),
        reinterpret_cast<const void *>(a.localBase + NPU_DOORBELL), sizeof(AggregateSignal));
    if (rc == 0) rc = HcommChannelDrainOnThread(a.thread, a.channel);
    if (rc == 0) rc = HcommAclrtNotifyRecordOnThread(a.thread, a.completionNotifyId);
    const int32_t end = HcommBatchModeEnd("aggregate-request");
    return rc != 0 ? rc : end;
}

extern "C" __attribute__((visibility("default"))) uint32_t AggregateKernel(void *rawArgs)
{
    if (!rawArgs) return 2;
    auto *a = reinterpret_cast<AggregateArgs *>(*static_cast<uint64_t *>(rawArgs));
    if (!a) return 2;
    const uint32_t lane = __atomic_fetch_add(&a->nextLane, 1U, __ATOMIC_ACQ_REL);
    if (lane >= SCATTER_LANES) return 2;
    const uint64_t deadline = NowNs() + 30000000000ULL;
    if (lane == 0) {
        a->beginNs = NowNs();
        int32_t rc = PublishRequest(*a);
        const uint64_t requested = NowNs();
        a->requestNs = requested - a->beginNs;
        if (rc == 0) {
            auto *ready = reinterpret_cast<volatile AggregateSignal *>(a->localBase + NPU_READY);
            for (;;) {
                // ready从未被AICPU写入，失效时不会把旧脏数据覆盖RDMA结果。
                Invalidate(a->localBase + NPU_READY, sizeof(AggregateSignal));
                // generation和status合成一个对齐64位值，避免读到分开更新的字段。
                const uint64_t token = ready->generation;
                if ((token >> 32) == a->generation) {
                    rc = static_cast<int32_t>(token & 0xffffffffU);
                    break;
                }
                if (NowNs() >= deadline) { rc = -1001; break; }
                asm volatile("yield" : : : "memory");
            }
        }
        a->scatterBeginNs = NowNs();
        a->waitHostNs = a->scatterBeginNs - requested;
        __atomic_store_n(&a->error, rc, __ATOMIC_RELEASE);
        __atomic_store_n(&a->phase, 1U, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&a->phase, __ATOMIC_ACQUIRE) == 0) {
            if (NowNs() >= deadline) {
                __atomic_store_n(&a->error, -1002, __ATOMIC_RELEASE);
                return 0;
            }
            asm volatile("yield" : : : "memory");
        }
    }
    if (__atomic_load_n(&a->error, __ATOMIC_ACQUIRE) != 0) return 0;
    const uint32_t begin = BLOCK_COUNT * lane / SCATTER_LANES;
    const uint32_t end = BLOCK_COUNT * (lane + 1) / SCATTER_LANES;
    // 每个lane失效自己读取的连续暂存区，避免跨轮命中旧payload。
    Invalidate(a->localBase + NPU_SCRATCH + uint64_t(begin) * BLOCK_BYTES,
        uint64_t(end - begin) * BLOCK_BYTES);
    for (uint32_t block = begin; block < end; ++block) {
        uint64_t dst = a->localBase + GUARD_BYTES + uint64_t(block) * DEST_STRIDE;
        uint64_t src = a->localBase + NPU_SCRATCH + uint64_t(block) * BLOCK_BYTES;
        std::memcpy(reinterpret_cast<void *>(dst), reinterpret_cast<const void *>(src), BLOCK_BYTES);
        // CPU写入目标后显式clean到PoC；dsb在本lane末尾统一执行。
        Clean(dst, BLOCK_BYTES);
    }
    asm volatile("dsb sy" : : : "memory");
    if (__atomic_add_fetch(&a->completedLanes, 1U, __ATOMIC_ACQ_REL) == SCATTER_LANES) {
        const uint64_t finished = NowNs();
        a->scatterNs = finished - a->scatterBeginNs;
        a->totalNs = finished - a->beginNs;
        Clean(reinterpret_cast<uint64_t>(a), sizeof(*a));
        asm volatile("dsb sy" : : : "memory");
    }
    return 0;
}
