#include "nix_async/executor.hh"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace nix_async {

Executor::Executor(std::size_t worker_count)
{
    if (worker_count == 0)
        throw std::invalid_argument("executor requires at least one worker");
    workers_.reserve(worker_count);
    active_worker_ids_.reserve(worker_count);
    try {
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
            std::lock_guard lock(mutex_);
            active_worker_ids_.push_back(workers_.back().get_id());
        }
    } catch (...) {
        // Also join already-created workers if starting a later thread fails.
        shutdown();
        throw;
    }
}

Executor::~Executor()
{
    shutdown();
}

void Executor::post(Work work)
{
    if (!work)
        throw std::invalid_argument("executor work must not be empty");
    {
        std::lock_guard lock(mutex_);
        if (stopping_)
            throw std::runtime_error("executor is shut down");
        queue_.push_back(std::move(work));
    }
    wake_.notify_one();
}

void Executor::shutdown()
{
    // Check before taking join_mutex_: an external caller could already hold it
    // while joining this worker. No worker may wait for its own pool to drain.
    {
        std::lock_guard lock(mutex_);
        if (std::ranges::find(active_worker_ids_, std::this_thread::get_id()) != active_worker_ids_.end())
            throw std::logic_error("cannot shut down an executor from its worker");
        stopping_ = true;
    }
    wake_.notify_all();
    std::lock_guard join_lock(join_mutex_);
    for (auto & worker : workers_)
        if (worker.joinable())
            worker.join();
}

void Executor::worker_loop() noexcept
{
    while (true) {
        Work work;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                std::erase(active_worker_ids_, std::this_thread::get_id());
                return; // Empty queue is only a wake-up condition at shutdown.
            }
            work = std::move(queue_.front());
            queue_.pop_front();
        }
        work();
    }
}

} // namespace nix_async
