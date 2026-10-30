#pragma once

#include "nix_async/promise.hh"

#include <atomic>
#include <concepts>
#include <future>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace nix_async {
namespace detail {

template<typename T>
using AllResult = std::conditional_t<std::is_void_v<T>, void, std::vector<T>>;

template<typename T>
struct AllState
{
    // Keep standard result storage, NOT input observer states: retaining input
    // Future<T>s here would create input -> observer -> aggregate -> input cycles.
    std::vector<std::shared_future<T>> inputs;
    Promise<AllResult<T>> promise;
    std::atomic<std::size_t> remaining;

    explicit AllState(std::vector<std::shared_future<T>> inputs)
        : inputs(std::move(inputs)), remaining(this->inputs.size()) {}

    void finish() noexcept
    {
        try {
            if constexpr (std::is_void_v<T>) {
                for (const auto & input : inputs)
                    input.get();
                promise.set_value();
            } else {
                std::vector<T> results;
                results.reserve(inputs.size());
                for (const auto & input : inputs)
                    results.push_back(input.get());
                promise.set_value(std::move(results));
            }
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    }

    void arrive() noexcept
    {
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
            finish(); // Every input is ready: get() cannot wait on pending work.
    }
};

} // namespace detail

// Homogeneous shared futures: copy values in input order once ALL inputs finish.
// Void inputs produce Future<void>. Non-void results must be copy-constructible.
// Invalid futures throw synchronously, before installing any subscriptions.
template<typename T>
    requires (std::is_void_v<T> || std::copy_constructible<T>)
Future<detail::AllResult<T>> when_all(std::vector<Future<T>> futures)
{
    std::vector<std::shared_future<T>> inputs;
    inputs.reserve(futures.size());
    for (const auto & future : futures)
        inputs.push_back(detail::FutureAccess::state(future).result);
    auto aggregate = std::make_shared<detail::AllState<T>>(std::move(inputs));
    auto result = aggregate->promise.get_future();
    if (futures.empty())
        aggregate->finish();
    else
        for (const auto & future : futures)
            detail::FutureAccess::state(future).subscribe([aggregate]() noexcept { aggregate->arrive(); });
    return result;
}

template<typename T>
    requires (std::is_void_v<T> || std::copy_constructible<T>)
auto when_all(std::initializer_list<Future<T>> futures)
{
    return when_all<T>(std::vector<Future<T>>(futures));
}

template<typename T, typename... Rest>
    requires ((std::same_as<Rest, Future<T>> && ...) && (std::is_void_v<T> || std::copy_constructible<T>))
auto when_all(Future<T> first, Rest... rest)
{
    return when_all<T>(std::vector<Future<T>>{std::move(first), std::move(rest)...});
}

} // namespace nix_async
