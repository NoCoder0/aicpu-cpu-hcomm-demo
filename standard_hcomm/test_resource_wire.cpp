// Focused regression test linked against the source-built experimental plugin.
// No fake QP, MR or driver responses are used; only wire decoding is exercised.
#include <gtest/gtest.h>
#include "experimental/base_comm/channel/host_cpu_roce_channel.h"

using hcomm_experimental::HostCpuRoceChannel;

TEST(HybridResourceWire, AcceptsDrainRecordsAndRejectsInvalidPeerData)
{
    HcommChannelDesc desc{};
    desc.exchangeAllMems = true;
    HostCpuRoceChannel channel(nullptr, desc);
    // Length calculation only examines count; no network resources are created.
    channel.connections_.emplace_back(nullptr);
    ASSERT_EQ(channel.BuildExchangeDataLengthHybird(), HCCL_SUCCESS);
    ASSERT_EQ(channel.exchangeDataTotalSize_, 109U);
    auto &wire = channel.exchangeDataForRecv_;
    wire.resize(109, 0);
    uint32_t qpCount = 1;
    memcpy(wire.data(), &qpCount, sizeof(qpCount));
    hccl::MemMsg notify{};
    notify.addr = reinterpret_cast<void *>(0x12340000);
    notify.len = 4;
    notify.lkey = 0x123;
    memcpy(wire.data() + 4, &notify, sizeof(notify));
    notify.addr = reinterpret_cast<void *>(0x56780000);
    notify.lkey = 0x456;
    memcpy(wire.data() + 4 + sizeof(notify), &notify, sizeof(notify));
    ASSERT_EQ(channel.ParseRecvExchangeDataHybird(), HCCL_SUCCESS);
    EXPECT_EQ(channel.remoteMemMsg_[hccl::DATA_NOTIFY_MEM].lkey, 0x123U);
    EXPECT_EQ(channel.remoteMemMsg_[hccl::NOTIFY_SRC_MEM].addr, reinterpret_cast<void *>(0x56780000));
    EXPECT_EQ(channel.remoteMemMsg_[hccl::NOTIFY_SRC_MEM].lkey, 0x456U);
    qpCount = 2;
    memcpy(wire.data(), &qpCount, sizeof(qpCount));
    EXPECT_EQ(channel.ParseRecvExchangeDataHybird(), HCCL_E_PARA);
    qpCount = 1;
    memcpy(wire.data(), &qpCount, sizeof(qpCount));
    // Nonzero extra MR counts require a different payload size and are rejected.
    memcpy(wire.data() + wire.size() - sizeof(qpCount), &qpCount, sizeof(qpCount));
    EXPECT_EQ(channel.ParseRecvExchangeDataHybird(), HCCL_E_NOT_SUPPORT);
    wire.pop_back();
    EXPECT_EQ(channel.ParseRecvExchangeDataHybird(), HCCL_E_PARA);
}

TEST(HybridResourceWire, KeepsCollectiveLayoutAndRejectsUnsupportedResourceShape)
{
    HcommChannelDesc desc{};
    HostCpuRoceChannel channel(nullptr, desc);
    ASSERT_EQ(channel.BuildExchangeDataLengthHybird(), HCCL_SUCCESS);
    EXPECT_EQ(channel.exchangeDataTotalSize_, 245U);
    channel.channelDesc_.exchangeAllMems = true;
    EXPECT_EQ(channel.BuildExchangeDataLengthHybird(), HCCL_E_NOT_SUPPORT);
    channel.connections_.emplace_back(nullptr);
    channel.notifyNum_ = 1;
    EXPECT_EQ(channel.BuildExchangeDataLengthHybird(), HCCL_E_NOT_SUPPORT);
}
