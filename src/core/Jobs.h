#pragma once
#include "Core.h"
#include <mutex>
#include <condition_variable>

namespace lab {
class JobSystem {
public:
    using Function = void(*)(void*, uint32_t, uint32_t);
    struct Group {
        std::atomic<uint32_t> remaining{};
        bool Complete() const { return remaining.load(std::memory_order_acquire)==0; }
    };
private:
    struct Job { Function function{}; void* context{}; uint32_t first{}, last{}; Group* group{}; };
    static constexpr uint32_t Capacity = 4096;
    static constexpr uint32_t MaxWorkers = ConsoleSpec::WorkerThreads;
    std::array<Job, Capacity> queue_{};
    std::array<std::thread, MaxWorkers> workers_;
    std::mutex mutex_;
    std::condition_variable ready_;
    uint32_t head_{}, tail_{}, count_{}, workerCount_{};
    bool stop_{};
    bool Take(Job&,Group*);
    void Execute(const Job&);
    void Run(uint32_t);
public:
    JobSystem() = default;
    JobSystem(const JobSystem&) = delete;
    ~JobSystem();
    void Start();
    void Stop();
    void Submit(Function, void*, uint32_t first = 0, uint32_t last = 1, Group* = nullptr);
    void Wait(Group&);
    void ParallelFor(uint32_t count, uint32_t grain, Function, void*);
    uint32_t WorkerCount() const { return workerCount_; }
};
}
