#include "Jobs.h"
#include <objbase.h>
#pragma comment(lib, "Ole32.lib")
namespace lab {
void JobSystem::Start() {
    if (workerCount_) return;
    workerCount_ = ConsoleSpec::WorkerThreads;
    stop_ = false;
    for (uint32_t i = 0; i < workerCount_; ++i) workers_[i] = std::thread([this, i] { Run(i); });
}
void JobSystem::Submit(Function function, void* context, uint32_t first, uint32_t last, Group* group) {
    if (group) group->remaining.fetch_add(1, std::memory_order_relaxed);
    if(!function||last<first)Fail("job arguments");
    Job job{function,context,first,last,group};
    {
        std::lock_guard lock(mutex_);
        if (count_ < Capacity && !stop_ && workerCount_) {
            queue_[tail_] = job;
            tail_ = (tail_ + 1) % Capacity;
            ++count_;
            ready_.notify_one();
            return;
        }
    }
    Execute(job);
}
bool JobSystem::Take(Job& job,Group* group) {
    std::lock_guard lock(mutex_);
    // A waiting render thread helps only its own group. PhysX jobs stay on workers.
    for(uint32_t offset=0;offset<count_;++offset){
        uint32_t index=(tail_+Capacity-1-offset)%Capacity;
        if(queue_[index].group!=group)continue;
        job=queue_[index];tail_=(tail_+Capacity-1)%Capacity;
        if(index!=tail_)queue_[index]=queue_[tail_];
        --count_;return true;
    }
    return false;
}
void JobSystem::Execute(const Job& job) {
    
    
    job.function(job.context, job.first, job.last);
    if(job.group){
        // WakeByAddressAll uses the address as a key; do not access the Group after publication.
        void* address=static_cast<void*>(&job.group->remaining);
        job.group->remaining.fetch_sub(1,std::memory_order_release);
        WakeByAddressAll(address);
    }
}
void JobSystem::Wait(Group& group) {
    static_assert(sizeof(std::atomic<uint32_t>)==sizeof(uint32_t));
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    for(;;){
        uint32_t remaining=group.remaining.load(std::memory_order_acquire);
        if(!remaining)return;
        Job job;
        if(Take(job,&group)){Execute(job);continue;}
        
        if(!WaitOnAddress(static_cast<volatile void*>(&group.remaining),&remaining,sizeof(remaining),INFINITE))
            Fail("Job group wait failed");
    }
}
void JobSystem::ParallelFor(uint32_t count, uint32_t grain, Function function, void* context) {
    if (!count) return;
    grain = std::max(1u, grain);
    if (count <= grain) { function(context, 0, count); return; }
    Group group;
    for (uint32_t i = 0; i < count; i += grain) Submit(function, context, i, std::min(count, i + grain), &group);
    Wait(group);
}
void JobSystem::Run(uint32_t index) {
    wchar_t name[96]; swprintf_s(name, L"%ls worker %u", GetCoreIdentity().diagnosticLabelW, index);
    SetThreadDescription(GetCurrentThread(), name);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stop_ || count_; });
            if (!count_ && stop_) break;
            job = queue_[head_]; head_ = (head_ + 1) % Capacity; --count_;
        }
        Execute(job);
    }
    CoUninitialize();
}
void JobSystem::Stop() {
    { std::lock_guard lock(mutex_); stop_ = true; }
    ready_.notify_all();
    for (uint32_t i = 0; i < workerCount_; ++i) if (workers_[i].joinable()) workers_[i].join();
    workerCount_ = 0;
}
JobSystem::~JobSystem() { Stop(); }
}
