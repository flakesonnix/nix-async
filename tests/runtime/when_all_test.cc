#include "nix_async/executor.hh"
#include "nix_async/when_all.hh"

#include <barrier>
#include <future>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include "test.hh"

using namespace nix_async;

void empty()
{
    auto combined = when_all<int>({});
    CHECK(combined.is_ready());
    CHECK(combined.get().empty());
    auto done = when_all<void>({});
    CHECK(done.is_ready());
    done.get();
}

void ordering()
{
    Promise<int> first;
    Promise<int> second;
    auto combined = when_all(first.get_future(), second.get_future());
    second.set_value(22);
    CHECK(!combined.is_ready());
    first.set_value(11);
    CHECK((combined.get() == std::vector<int>{11, 22}));
    CHECK((combined.get() == std::vector<int>{11, 22}));
}

void already_ready_and_duplicates()
{
    Promise<int> promise;
    auto future = promise.get_future();
    promise.set_value(21);
    auto combined = when_all(future, future);
    CHECK(combined.is_ready());
    CHECK((combined.get() == std::vector<int>{21, 21}));
    CHECK((when_all(future).get() == std::vector<int>{21}));
}

void failure()
{
    Promise<int> first;
    Promise<int> second;
    auto combined = when_all(first.get_future(), second.get_future());
    second.set_exception(std::make_exception_ptr(std::runtime_error("second")));
    CHECK(!combined.is_ready()); // Failure does not mean the other work stopped.
    first.set_exception(std::make_exception_ptr(std::logic_error("first")));
    // Deterministic first error in INPUT order, not completion order.
    expect_throw<std::logic_error>([&] { combined.get(); });
}

void broken_input()
{
    Future<std::vector<int>> combined;
    Promise<int> other;
    {
        Promise<int> abandoned;
        combined = when_all(abandoned.get_future(), other.get_future());
    }
    CHECK(!combined.is_ready());
    other.set_value(1);
    expect_future_error(std::future_errc::broken_promise, [&] { combined.get(); });
}

void invalid()
{
    Promise<int> promise;
    Future<int> invalid;
    expect_future_error(std::future_errc::no_state, [&] { when_all(promise.get_future(), invalid); });
    promise.set_value(42);
    CHECK(promise.get_future().get() == 42);
}

void void_inputs()
{
    Promise<void> first;
    Promise<void> second;
    auto combined = when_all(first.get_future(), second.get_future());
    first.set_value();
    CHECK(!combined.is_ready());
    second.set_value();
    combined.get();
    Promise<void> error;
    auto failed = when_all(error.get_future());
    error.set_exception(std::make_exception_ptr(std::runtime_error("void failure")));
    expect_throw<std::runtime_error>([&] { failed.get(); });
}

void no_worker_waiting()
{
    Executor executor(1);
    Promise<int> first;
    Promise<int> second;
    auto combined = when_all(first.get_future(), second.get_future());
    // Even with only one worker and incomplete inputs, composition must leave
    // the pool available to execute the very work that completes those inputs.
    executor.spawn([&] { first.set_value(21); }).get();
    CHECK(!combined.is_ready());
    executor.spawn([&] { second.set_value(21); }).get();
    CHECK((combined.get() == std::vector<int>{21, 21}));
}

void concurrent_completion()
{
    for (int round = 0; round < 50; ++round) {
        Promise<int> first;
        Promise<int> second;
        std::barrier start(3);
        Future<std::vector<int>> combined;
        std::jthread a([&] { start.arrive_and_wait(); first.set_value(1); });
        std::jthread b([&] { start.arrive_and_wait(); second.set_value(2); });
        start.arrive_and_wait();
        // Race subscription with completion, including the already-ready path.
        combined = when_all(first.get_future(), second.get_future());
        CHECK((combined.get() == std::vector<int>{1, 2}));
    }
}

void lifetime()
{
    std::weak_ptr<int> weak;
    Future<std::vector<std::shared_ptr<int>>> combined;
    {
        Promise<std::shared_ptr<int>> producer;
        auto value = std::make_shared<int>(42);
        weak = value;
        combined = when_all(producer.get_future());
        producer.set_value(std::move(value));
    }
    CHECK(!weak.expired());
    CHECK(*combined.get()[0] == 42);
    combined = {};
    CHECK(weak.expired());

    // Dropping consumers before producers also must not leave an observer cycle.
    {
        Promise<std::shared_ptr<int>> producer;
        auto value = std::make_shared<int>(42);
        weak = value;
        auto discarded = when_all(producer.get_future());
        discarded = {};
        producer.set_value(std::move(value));
    }
    CHECK(weak.expired());
}

void overlap()
{
    Executor executor(2);
    std::latch both_running(2);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto work = [gate, &both_running] {
        both_running.count_down();
        gate.wait();
        return 21;
    };
    auto first = executor.spawn(work);
    auto second = executor.spawn(work);
    auto combined = when_all(first, second);
    both_running.wait();
    CHECK(!combined.is_ready());
    release.set_value();
    CHECK((combined.get() == std::vector<int>{21, 21}));
}

struct ThrowingCopy
{
    ThrowingCopy() = default;
    ThrowingCopy(const ThrowingCopy &) { throw std::runtime_error("aggregate copy failed"); }
    ThrowingCopy(ThrowingCopy &&) = default;
};

void copy_failure()
{
    Promise<ThrowingCopy> producer;
    auto combined = when_all(producer.get_future());
    producer.set_value(ThrowingCopy{});
    CHECK(combined.is_ready());
    expect_throw<std::runtime_error>([&] { combined.get(); });
    producer.get_future().get(); // Failed composition does not damage its input.
}

void nested()
{
    Promise<int> first;
    Promise<int> second;
    auto inner = when_all(first.get_future(), second.get_future());
    auto outer = when_all(inner, inner);
    first.set_value(11);
    CHECK(!outer.is_ready());
    second.set_value(22);
    CHECK((outer.get() == std::vector<std::vector<int>>{{11, 22}, {11, 22}}));
}

int main(int argc, char ** argv)
{
    return run_test(argc, argv, {{"empty", empty}, {"ordering", ordering},
        {"already_ready_and_duplicates", already_ready_and_duplicates}, {"failure", failure},
        {"broken_input", broken_input}, {"invalid", invalid}, {"void_inputs", void_inputs},
        {"no_worker_waiting", no_worker_waiting}, {"concurrent_completion", concurrent_completion},
        {"lifetime", lifetime}, {"overlap", overlap}, {"copy_failure", copy_failure}, {"nested", nested}});
}
