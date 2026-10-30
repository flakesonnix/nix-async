#pragma once

#include "nix_async/future.hh"

#include <concepts>
#include <stdexcept>
#include <utility>

namespace nix_async {

// One producer, multiple shared consumers. Destroying an incomplete producer
// publishes broken_promise and notifies composition observers as well as waiters.
template<typename T>
class Promise
{
public:
    Promise() : state_(std::make_shared<detail::SharedState<T>>()) {}
    Promise(const Promise &) = delete;
    Promise & operator=(const Promise &) = delete;
    Promise(Promise &&) noexcept = default;

    Promise & operator=(Promise && other) noexcept
    {
        if (this != &other) {
            if (state_)
                state_->abandon();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    ~Promise()
    {
        if (state_)
            state_->abandon();
    }

    Future<T> get_future() const
    {
        checked_state();
        return Future<T>(state_);
    }

    template<typename U>
        requires (!std::is_void_v<T> && std::convertible_to<U &&, T>)
    void set_value(U && value)
    {
        checked_state().complete([&](auto & producer) { producer.set_value(std::forward<U>(value)); },
            TaskState::Completed);
    }

    void set_value() requires std::is_void_v<T>
    {
        checked_state().complete([](auto & producer) { producer.set_value(); }, TaskState::Completed);
    }

    void set_exception(std::exception_ptr error)
    {
        auto & state = checked_state();
        if (!error)
            throw std::invalid_argument("cannot complete a promise with a null exception");
        state.complete([&](auto & producer) { producer.set_exception(error); }, TaskState::Failed);
    }

private:
    std::shared_ptr<detail::SharedState<T>> state_;

    detail::SharedState<T> & checked_state() const
    {
        if (!state_)
            throw std::future_error(std::future_errc::no_state);
        return *state_;
    }
};

} // namespace nix_async
