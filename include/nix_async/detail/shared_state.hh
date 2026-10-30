#pragma once

#include "nix_async/task_state.hh"

#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace nix_async::detail {

// std::promise/shared_future own result storage, waiting and exceptions. This
// lock also orders lifecycle publication and completion observers against get().
template<typename T>
struct SharedState
{
    using Observer = std::move_only_function<void() noexcept>;

    std::mutex mutex;
    std::optional<std::promise<T>> producer{std::in_place};
    std::shared_future<T> result{producer->get_future().share()};
    TaskState status = TaskState::Created;
    bool ready = false;
    std::vector<Observer> observers;

    template<typename Complete>
    void complete(Complete && complete_result, TaskState terminal)
    {
        std::vector<Observer> notifications;
        {
            std::lock_guard lock(mutex);
            if (ready)
                throw std::future_error(std::future_errc::promise_already_satisfied);
            // If constructing the result throws, leave the state incomplete so
            // the caller can publish an exception (or retry a manual promise).
            complete_result(*producer);
            status = terminal;
            ready = true;
            producer.reset();
            notifications.swap(observers);
        }
        for (auto & notify : notifications)
            notify();
    }

    void abandon() noexcept
    {
        std::vector<Observer> notifications;
        {
            std::lock_guard lock(mutex);
            if (ready)
                return;
            // Let the standard promise publish its standard broken_promise.
            producer.reset();
            status = TaskState::Failed;
            ready = true;
            notifications.swap(observers);
        }
        for (auto & notify : notifications)
            notify();
    }

    void subscribe(Observer observer)
    {
        {
            std::lock_guard lock(mutex);
            if (!ready) {
                observers.push_back(std::move(observer));
                return;
            }
        }
        observer(); // Never invoke observers under an input's lock.
    }
};

struct FutureAccess;

} // namespace nix_async::detail
