// 持久化线程池；参考UB方案的generation发布与done计数，避免逐轮创建线程。
#pragma once
#include "aggregate_protocol.h"
#include <atomic>
#include <sched.h>
#include <thread>
#include <vector>
#include <cstring>

inline void CpuRelax() { asm volatile("yield" : : : "memory"); }
inline void PinCpu(int cpu)
{
    if (cpu < 0) return;
    Require(cpu < CPU_SETSIZE, "CPU affinity range");
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    Require(pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0, "set CPU affinity");
}

class GatherPool {
public:
    explicit GatherPool(const std::vector<int> &cpus) : count_(cpus.size())
    {
        for (uint32_t lane = 0; lane < count_; ++lane) {
            workers_.emplace_back([this, lane, cpus] {
                try { PinCpu(cpus[lane]); }
                catch (...) { affinityError_.store(true); }
                ready_.fetch_add(1);
                uint32_t observed = 0;
                for (;;) {
                    uint32_t next;
                    while ((next = generation_.load(std::memory_order_acquire)) == observed && !stop_.load()) CpuRelax();
                    if (stop_.load()) return;
                    observed = next;
                    for (uint32_t i = BLOCK_COUNT * lane / count_; i < BLOCK_COUNT * (lane + 1) / count_; ++i)
                        std::memcpy(destination_ + uint64_t(i) * BLOCK_BYTES,
                            reinterpret_cast<const void *>(request_->addresses[i]), BLOCK_BYTES);
                    done_.fetch_add(1, std::memory_order_acq_rel);
                }
            });
        }
        while (ready_.load() != count_) CpuRelax();
        // 主进程在进入测试前检查；绑定失败不静默退化成不同测试配置。
        if (affinityError_.load()) {
            Stop();
            throw std::runtime_error("gather worker affinity failed");
        }
    }
    ~GatherPool() { Stop(); }
    void Run(unsigned char *destination, const AggregateRequest *request)
    {
        destination_ = destination;
        request_ = request;
        done_.store(0, std::memory_order_relaxed);
        generation_.fetch_add(1, std::memory_order_release);
        while (done_.load(std::memory_order_acquire) != count_) CpuRelax();
    }
private:
    void Stop()
    {
        stop_.store(true);
        for (auto &worker : workers_) if (worker.joinable()) worker.join();
    }
    uint32_t count_;
    std::vector<std::thread> workers_;
    std::atomic<uint32_t> generation_{0}, done_{0}, ready_{0};
    std::atomic<bool> stop_{false}, affinityError_{false};
    unsigned char *destination_ = nullptr;
    const AggregateRequest *request_ = nullptr;
};
