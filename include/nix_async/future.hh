#pragma once

#include "nix_async/detail/shared_state.hh"

#include <memory>
#include <type_traits>

namespace nix_async {

template<typename T> class Promise;

// Copyable shared result. get() returns const T& (void for Future<void>), valid
// while a future owning the result remains alive. No destructive consumption.
template<typename T>
class Future
{
    static_assert(std::is_void_v<T> || (std::is_object_v<T> && !std::is_const_v<T>));

public:
    Future() = default;

    bool valid() const noexcept { return static_cast<bool>(state_); }

    bool is_ready() const
    {
        auto & state = checked_state();
        std::lock_guard lock(state.mutex);
        return state.ready;
    }

    void wait() const
    {
        auto & state = checked_state();
        state.result.wait();
        // The standard result can wake waiters before complete() has published
        // the task's terminal status. Synchronize with that publication too.
        std::lock_guard lock(state.mutex);
    }

    decltype(auto) get() const
    {
        wait();
        return state_->result.get();
    }

private:
    std::shared_ptr<detail::SharedState<T>> state_;

    explicit Future(std::shared_ptr<detail::SharedState<T>> state) : state_(std::move(state)) {}

    detail::SharedState<T> & checked_state() const
    {
        if (!state_)
            throw std::future_error(std::future_errc::no_state);
        return *state_;
    }

    friend class Promise<T>;
    friend struct detail::FutureAccess;
};

namespace detail {

struct FutureAccess
{
    template<typename T>
    static SharedState<T> & state(const Future<T> & future) { return future.checked_state(); }
};

} // namespace detail
} // namespace nix_async
