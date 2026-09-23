// Focused regression test linked against the source-built experimental plugin.
// No fake QP, MR or driver responses are used; only wire decoding is exercised.
#include <gtest/gtest.h>
#include "endpoint_pair.h"
#include "experimental/base_comm/channel/host_cpu_roce_channel.h"
#include "experimental/base_comm/comm_mem/roce_mem.h"
#include <hcomm_res.h>

using hcomm_experimental::HostCpuRoceChannel;

TEST(HybridImportedMemory, DecodesA3DeviceWireWithoutByteShift)
{
    hcomm_experimental::RoceRegedMemMgr manager;
    EndpointDesc peer{};
    ASSERT_EQ(EndpointDescInit(&peer, 1), HCCL_SUCCESS);
    peer.protocol = COMM_PROTOCOL_ROCE;
    peer.loc.locType = ENDPOINT_LOC_TYPE_DEVICE;
    peer.loc.device.devPhyId = 2;
    std::vector<char> descriptor;
    auto append = [&descriptor](const auto &value) {
        const auto *p = reinterpret_cast<const char *>(&value);
        descriptor.insert(descriptor.end(), p, p + sizeof(value));
    };
    uint8_t type = 1;
    uint64_t logical = 0x12c0c0200000, size = 4096, device = 0x12c0c0400000;
    uint32_t memType = 0, key = 0xabc123;
    append(type); append(logical); append(size); append(device); append(memType); append(key); append(peer);
    CommMem memory{};
    ASSERT_EQ(manager.MemoryImport(descriptor.data(), descriptor.size(), &memory), HCCL_SUCCESS);
    EXPECT_EQ(memory.addr, reinterpret_cast<void *>(device));
    EXPECT_EQ(memory.size, size);
    EXPECT_EQ(memory.type, COMM_MEM_TYPE_DEVICE);
    std::shared_ptr<Hccl::RemoteRdmaRmaBuffer> buffer;
    ASSERT_EQ(manager.FindImportedBuffer(peer, device + 128, 656, buffer), HCCL_SUCCESS);
    EXPECT_EQ(buffer->GetRkey(), key);
    ASSERT_EQ(manager.MemoryUnimport(descriptor.data(), descriptor.size()), HCCL_SUCCESS);
    descriptor[0] = 0;
    EXPECT_EQ(manager.MemoryImport(descriptor.data(), descriptor.size(), &memory), HCCL_E_PARA);
    descriptor[0] = 1;
    descriptor.erase(descriptor.begin() + 1); // Keep endpoint trailer intact, truncate the payload.
    EXPECT_EQ(manager.MemoryImport(descriptor.data(), descriptor.size(), &memory), HCCL_E_PARA);
}

TEST(HybridImportedMemory, ResolvesImportedRkeyAndRejectsWrongPeerAndBounds)
{
    hcomm_experimental::RoceRegedMemMgr manager;
    EndpointDesc peer{};
    ASSERT_EQ(EndpointDescInit(&peer, 1), HCCL_SUCCESS);
    peer.protocol = COMM_PROTOCOL_ROCE;
    peer.commAddr.type = COMM_ADDR_TYPE_IP_V4;
    peer.commAddr.addr.s_addr = 0x0300a814;
    peer.loc.locType = ENDPOINT_LOC_TYPE_HOST;
    Hccl::ExchangeRdmaBufferDto dto(0x100000, 4096, 0xabc, "npu-hbm");
    Hccl::BinaryStream stream;
    dto.Serialize(stream);
    std::vector<char> descriptor;
    stream.Dump(descriptor);
    const char *endpointBytes = reinterpret_cast<const char *>(&peer);
    descriptor.insert(descriptor.end(), endpointBytes, endpointBytes + sizeof(peer));
    CommMem memory{};
    ASSERT_EQ(manager.MemoryImport(descriptor.data(), descriptor.size(), &memory), HCCL_SUCCESS);
    std::shared_ptr<Hccl::RemoteRdmaRmaBuffer> buffer;
    ASSERT_EQ(manager.FindImportedBuffer(peer, 0x100080, 656, buffer), HCCL_SUCCESS);
    EXPECT_EQ(buffer->GetRkey(), 0xabcU);
    EXPECT_EQ(manager.FindImportedBuffer(peer, 0x100fff, 2, buffer), HCCL_E_NOT_FOUND);
    EXPECT_EQ(manager.FindImportedBuffer(peer, UINT64_MAX - 8, 16, buffer), HCCL_E_PARA);
    EXPECT_EQ(manager.FindImportedBuffer(peer, 0x100000, 0, buffer), HCCL_E_PARA);
    auto wrongPeer = peer;
    wrongPeer.commAddr.addr.s_addr = 0x0400a814;
    EXPECT_EQ(manager.FindImportedBuffer(wrongPeer, 0x100000, 16, buffer), HCCL_E_NOT_FOUND);
    ASSERT_EQ(manager.MemoryUnimport(descriptor.data(), descriptor.size()), HCCL_SUCCESS);
    EXPECT_EQ(manager.FindImportedBuffer(peer, 0x100000, 16, buffer), HCCL_E_NOT_FOUND);
}

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
