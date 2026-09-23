// 聚合协议使用独立缓存行的doorbell/ready；所有地址都属于公开注册的MR。
#pragma once
#include "benchmark.h"
#include <hcomm_primitives.h>

constexpr uint64_t AlignUp(uint64_t n, uint64_t align) { return (n + align - 1) / align * align; }
constexpr uint32_t SCATTER_LANES = 6;
struct AggregateRequest {
    uint64_t generation;
    uint64_t addresses[BLOCK_COUNT];
};
struct alignas(64) AggregateSignal {
    uint64_t generation;
    int32_t status;
    unsigned char reserved[52];
};
constexpr uint64_t HOST_REQUEST = AlignUp(SOURCE_BYTES, 4096);
constexpr uint64_t HOST_DOORBELL = AlignUp(HOST_REQUEST + sizeof(AggregateRequest), 64);
constexpr uint64_t HOST_AGGREGATE = AlignUp(HOST_DOORBELL + sizeof(AggregateSignal), 4096);
constexpr uint64_t HOST_READY_SOURCE = AlignUp(HOST_AGGREGATE + PAYLOAD_BYTES, 64);
constexpr uint64_t HOST_AGGREGATE_BYTES = HOST_READY_SOURCE + sizeof(AggregateSignal);
constexpr uint64_t NPU_SCRATCH = AlignUp(BUFFER_BYTES, 4096);
constexpr uint64_t NPU_READY = AlignUp(NPU_SCRATCH + PAYLOAD_BYTES, 64);
constexpr uint64_t NPU_REQUEST = AlignUp(NPU_READY + sizeof(AggregateSignal), 4096);
constexpr uint64_t NPU_DOORBELL = AlignUp(NPU_REQUEST + sizeof(AggregateRequest), 64);
constexpr uint64_t NPU_AGGREGATE_BYTES = NPU_DOORBELL + sizeof(AggregateSignal);

struct alignas(64) AggregateArgs {
    ThreadHandle thread;
    ChannelHandle channel;
    uint64_t localBase;
    uint64_t remoteBase;
    uint64_t completionNotifyId;
    uint32_t generation;
    uint32_t nextLane;
    uint32_t completedLanes;
    uint32_t phase;
    int32_t error;
    uint32_t reserved;
    uint64_t beginNs;
    uint64_t requestNs;
    uint64_t waitHostNs;
    uint64_t scatterBeginNs;
    uint64_t scatterNs;
    uint64_t totalNs;
};
