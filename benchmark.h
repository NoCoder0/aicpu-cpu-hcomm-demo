// 两端和 AICPU 共用的数据布局；逻辑数据块数与网络报文数没有一一对应关系。
#pragma once
#include <cstddef>
#include <cstdint>

constexpr uint32_t BLOCK_COUNT = 1600;
constexpr uint32_t BLOCK_BYTES = 656;
constexpr uint32_t SOURCE_POOL_BLOCKS = 262144; // 与 UB 参考测试的源池规模一致。
constexpr uint64_t PAYLOAD_BYTES = uint64_t(BLOCK_COUNT) * BLOCK_BYTES;
constexpr uint64_t SOURCE_BYTES = uint64_t(SOURCE_POOL_BLOCKS) * BLOCK_BYTES;
constexpr uint64_t DEST_STRIDE = 2 * BLOCK_BYTES;
constexpr uint64_t GUARD_BYTES = 4096;
constexpr uint64_t BUFFER_BYTES = GUARD_BYTES + BLOCK_COUNT * DEST_STRIDE + GUARD_BYTES;
constexpr unsigned char GUARD_VALUE = 0xa5;
static_assert(PAYLOAD_BYTES == 1049600, "benchmark payload size");

// 奇数乘数在 2^18 源池中构成排列；1600 个源块互不重叠，每轮改变所选地址。
inline uint64_t SourceOffset(uint32_t block, uint32_t generation)
{
    return uint64_t((block * 8191U + generation * 131U) & (SOURCE_POOL_BLOCKS - 1)) * BLOCK_BYTES;
}
inline unsigned char PayloadByte(uint32_t block, uint32_t byte, uint32_t generation)
{
    uint32_t value = generation * 0x9e3779b9U ^ block * 0x85ebca6bU ^ byte * 0xc2b2ae35U;
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    return static_cast<unsigned char>(value);
}
