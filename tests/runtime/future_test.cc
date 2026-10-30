#include "nix_async/promise.hh"

#include <atomic>
#include <barrier>
#include <future>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include "test.hh"

using namespace nix_async;

void result()
{
    Promise<int> promise;
    auto future = promise.get_future();
    CHECK(!future.is_ready());
    promise.set_value(42);
    CHECK(future.is_ready());
    CHECK(future.get() == 42);
    CHECK(future.get() == 42);
    CHECK(promise.get_future().get() == 42);
}

void waiting()
{
    Promise<int> promise;
    auto future = promise.get_future();
    std::latch started(1);
    std::atomic<int> result{0};
    std::jthread consumer([&] {
        started.count_down();
        future.wait();
        result = future.get();
    });
    started.wait();
    CHECK(result == 0);
    promise.set_value(42);
    consumer.join();
    CHECK(result == 42);
}

void errors()
{
    Future<int> invalid;
    CHECK(!invalid.valid());
    expect_future_error(std::future_errc::no_state, [&] { invalid.wait(); });
    expect_future_error(std::future_errc::no_state, [&] { invalid.get(); });
    expect_future_error(std::future_errc::no_state, [&] { invalid.is_ready(); });
    Promise<int> promise;
    auto future = promise.get_future();
    expect_throw<std::invalid_argument>([&] { promise.set_exception(nullptr); });
    CHECK(!future.is_ready());
    promise.set_exception(std::make_exception_ptr(std::runtime_error("native failure")));
    CHECK(future.is_ready());
    expect_throw<std::runtime_error>([&] { future.get(); });
    expect_throw<std::runtime_error>([&] { future.get(); });
    expect_future_error(std::future_errc::promise_already_satisfied, [&] { promise.set_value(1); });
    expect_future_error(std::future_errc::promise_already_satisfied,
        [&] { promise.set_exception(std::make_exception_ptr(std::logic_error("second"))); });
}

void broken_promise()
{
    Future<int> future;
    {
        Promise<int> promise;
        future = promise.get_future();
    }
    CHECK(future.is_ready());
    expect_future_error(std::future_errc::broken_promise, [&] { future.get(); });
}

void move_ownership()
{
    Promise<int> first;
    auto abandoned = first.get_future();
    Promise<int> second;
    auto result = second.get_future();
    first = std::move(second);
    expect_future_error(std::future_errc::broken_promise, [&] { abandoned.get(); });
    expect_future_error(std::future_errc::no_state, [&] { second.get_future(); });
    expect_future_error(std::future_errc::no_state, [&] { second.set_value(0); });
    Promise<int> owner(std::move(first));
    owner.set_value(42);
    CHECK(result.get() == 42);
    expect_future_error(std::future_errc::promise_already_satisfied, [&] { owner.set_value(43); });
}

void concurrent_consumers()
{
    Promise<int> promise;
    auto future = promise.get_future();
    std::barrier start(9);
    std::atomic<int> sum{0};
    std::vector<std::jthread> consumers;
    for (int i = 0; i < 8; ++i)
        consumers.emplace_back([future, &start, &sum] {
            start.arrive_and_wait();
            sum += future.get();
        });
    start.arrive_and_wait();
    promise.set_value(21);
    consumers.clear();
    CHECK(sum == 168);
}

void competing_producers()
{
    Promise<int> promise;
    auto future = promise.get_future();
    std::barrier start(9);
    std::atomic<int> winners{0};
    std::atomic<int> rejected{0};
    std::vector<std::jthread> producers;
    for (int i = 0; i < 8; ++i)
        producers.emplace_back([&, i] {
            start.arrive_and_wait();
            try {
                promise.set_value(i);
                ++winners;
            } catch (const std::future_error & error) {
                if (error.code() == std::make_error_code(std::future_errc::promise_already_satisfied))
                    ++rejected;
            }
        });
    start.arrive_and_wait();
    producers.clear();
    CHECK(winners == 1);
    CHECK(rejected == 7);
    CHECK(future.get() >= 0 && future.get() < 8);
}

void void_and_move_only()
{
    Promise<void> done;
    done.set_value();
    done.get_future().get();
    Promise<std::unique_ptr<int>> promise;
    promise.set_value(std::make_unique<int>(42));
    auto future = promise.get_future();
    CHECK(*future.get() == 42);
    CHECK(&future.get() == &future.get());
}

struct ThrowingCopy
{
    ThrowingCopy() = default;
    ThrowingCopy(const ThrowingCopy &) { throw std::runtime_error("copy failed"); }
    ThrowingCopy(ThrowingCopy &&) = default;
};

void construction_failure()
{
    Promise<ThrowingCopy> promise;
    auto future = promise.get_future();
    ThrowingCopy value;
    expect_throw<std::runtime_error>([&] { promise.set_value(value); });
    CHECK(!future.is_ready());
    promise.set_value(std::move(value));
    CHECK(future.is_ready());
    future.get();
}

int main(int argc, char ** argv)
{
    return run_test(argc, argv, {{"result", result}, {"waiting", waiting}, {"errors", errors},
        {"broken_promise", broken_promise}, {"move_ownership", move_ownership},
        {"concurrent_consumers", concurrent_consumers}, {"competing_producers", competing_producers},
        {"void_and_move_only", void_and_move_only}, {"construction_failure", construction_failure}});
}
