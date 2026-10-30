#pragma once

#include "nix_async/task.hh"

#include <functional>
#include <utility>

namespace nix_async {

class Executor;

// Immediate FIFO scheduling policy. Lifecycle is retained by Task/Future handles,
// not an unbounded registry. The referenced executor must outlive this adapter.
class Scheduler
{
public:
    explicit Scheduler(Executor & executor) noexcept : executor_(executor) {}

    template<typename T>
    Future<T> submit(Task<T> task) const
    {
        auto future = task.get_future();
        task.mark_pending();
        try {
            enqueue([task]() noexcept { task.execute(); });
        } catch (...) {
            // A rejected submission is terminal, not an orphaned Pending task.
            task.reject(std::current_exception());
            throw;
        }
        return future;
    }

    template<typename F>
    auto spawn(F && work) const
    {
        Task task(std::forward<F>(work));
        return submit(std::move(task));
    }

private:
    Executor & executor_;
    void enqueue(std::move_only_function<void() noexcept> work) const;
};

} // namespace nix_async
