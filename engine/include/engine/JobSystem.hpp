#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace engine {
// Section 3: a persistent pool of worker threads that drain a per-frame list of jobs
class JobSystem {
public:
    using Job = std::function<void()>;

    explicit JobSystem(int workerCount);
    ~JobSystem();

    JobSystem(const JobSystem &) = delete;
    JobSystem &operator=(const JobSystem &) = delete;

    void Submit(Job job);

    // Runs every submitted job across the pool and the calling thread
    void RunAndWait();

    int WorkerCount() const { return static_cast<int>(workers.size()); }

private:
    void WorkerLoop();

    // The ThreadExample's worker()
    void Drain(size_t count);

    std::vector<Job> jobs;

    // The ThreadExample's SharedData::nextJobIndex, and the count of jobs run
    std::atomic<size_t> nextJobIndex{0};
    std::atomic<size_t> jobsFinished{0};

    // Everything below is guarded by `mutex`
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable done;
    size_t jobCount = 0;  // this frame's job count; workers snapshot it on waking
    uint64_t generation = 0;  // bumped once per RunAndWait
    int acknowledged = 0;  // workers that have woken for this generation
    int draining = 0;  // workers currently inside Drain()
    bool stopping = false;

    std::vector<std::thread> workers;
};
}  // namespace engine
