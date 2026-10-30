#pragma once

#include "nix_async/scheduler.hh"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace nix_async {

class Executor
{
public:
    using Work = std::move_only_function<void() noexcept>;

    explicit Executor(std::size_t worker_count);
    ~Executor();
    Executor(const Executor &) = delete;
    Executor & operator=(const Executor &) = delete;
    Executor(Executor &&) = delete;
    Executor & operator=(Executor &&) = delete;

    // Low-level fire-and-forget work must not throw. Use spawn()/Scheduler for
    // arbitrary callables: their exceptions are captured in a task's Future.
    void post(Work work);

    template<typename F>
    auto spawn(F && work) { return Scheduler(*this).spawn(std::forward<F>(work)); }

    // Stop accepting, drain all accepted work, then join. Idempotent and safe
    // for concurrent external callers; throws logic_error on a pool worker.
    void shutdown();

private:
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Work> queue_;
    bool stopping_ = false;
    std::mutex join_mutex_;
    std::vector<std::thread> workers_;
    // Protected by mutex_, removed on worker exit to tolerate ID reuse. Reading
    // thread::get_id() while another caller joins that thread would race.
    std::vector<std::thread::id> active_worker_ids_;

    void worker_loop() noexcept;
};

} // namespace nix_async
