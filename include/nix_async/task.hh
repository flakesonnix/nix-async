#pragma once

#include "nix_async/promise.hh"

#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace nix_async {

class Scheduler;

// Copyable handle to one execution, not a copy of the callable. Only Scheduler
// can schedule/run it. Retaining either a Task or a Future retains its result.
template<typename T>
class Task
{
public:
    explicit Task(std::move_only_function<T()> work) : data_(std::make_shared<Data>(std::move(work))) {}

    Future<T> get_future() const { return checked_data().promise.get_future(); }

    TaskState state() const
    {
        auto & state = detail::FutureAccess::state(get_future());
        std::lock_guard lock(state.mutex);
        return state.status;
    }

private:
    struct Data
    {
        std::move_only_function<T()> work;
        Promise<T> promise;

        explicit Data(std::move_only_function<T()> work) : work(std::move(work))
        {
            if (!this->work)
                throw std::invalid_argument("task callable must not be empty");
        }
    };

    std::shared_ptr<Data> data_;

    Data & checked_data() const
    {
        if (!data_)
            throw std::future_error(std::future_errc::no_state);
        return *data_;
    }

    void mark_pending() const
    {
        auto & state = detail::FutureAccess::state(get_future());
        std::lock_guard lock(state.mutex);
        if (state.status != TaskState::Created)
            throw std::logic_error("task can only be submitted once");
        state.status = TaskState::Pending;
    }

    void execute() const noexcept
    {
        auto & data = checked_data();
        {
            auto & state = detail::FutureAccess::state(get_future());
            std::lock_guard lock(state.mutex);
            if (state.status != TaskState::Pending)
                std::terminate(); // Scheduler must enqueue each task exactly once.
            state.status = TaskState::Running;
        }
        auto work = std::move(data.work);
        try {
            if constexpr (std::is_void_v<T>) {
                work();
                data.promise.set_value();
            } else
                data.promise.set_value(work());
        } catch (...) {
            data.promise.set_exception(std::current_exception());
        }
    }

    void reject(std::exception_ptr error) const
    {
        auto & data = checked_data();
        data.work = nullptr; // Rejected work will never execute; release captures.
        data.promise.set_exception(error);
    }

    friend class Scheduler;
};

template<typename F> Task(F) -> Task<std::invoke_result_t<F &>>;

} // namespace nix_async
