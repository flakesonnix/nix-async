#include "nix_async/executor.hh"

#include <atomic>
#include <barrier>
#include <future>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include "test.hh"

using namespace nix_async;

void execution()
{
    Executor executor(2);
    CHECK(executor.spawn([] { return 42; }).get() == 42);
    executor.spawn([] {}).get();
    auto ptr = std::make_unique<int>(21);
    CHECK(executor.spawn([ptr = std::move(ptr)] { return *ptr * 2; }).get() == 42);
    expect_throw<std::invalid_argument>([] { Executor invalid(0); });
    expect_throw<std::invalid_argument>([&] { executor.post({}); });
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
    both_running.wait(); // Cannot be reached by a serial executor.
    CHECK(!first.is_ready() && !second.is_ready());
    release.set_value();
    CHECK(first.get() + second.get() == 42);
}

void fifo()
{
    Executor executor(1);
    std::latch running(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto blocker = executor.spawn([gate, &running] { running.count_down(); gate.wait(); });
    running.wait();
    std::vector<int> order;
    std::vector<Future<void>> queued;
    for (int i = 0; i < 100; ++i)
        queued.push_back(executor.spawn([&, i] { order.push_back(i); }));
    CHECK(order.empty());
    release.set_value();
    executor.shutdown();
    CHECK(order.size() == 100);
    for (int i = 0; i < 100; ++i) {
        CHECK(order[i] == i);
        queued[i].get();
    }
    blocker.get();
}

void shutdown()
{
    Executor executor(3);
    std::atomic<int> count{0};
    std::vector<Future<int>> futures;
    for (int i = 0; i < 200; ++i)
        futures.push_back(executor.spawn([&] { return ++count; }));
    executor.shutdown();
    executor.shutdown();
    // A new caller may reuse the ID of an exited worker. It is not a worker.
    std::jthread later_caller([&] { executor.shutdown(); });
    later_caller.join();
    CHECK(count == 200);
    for (const auto & future : futures) {
        CHECK(future.is_ready());
        CHECK(future.get() > 0);
    }
    expect_throw<std::runtime_error>([&] { executor.spawn([] {}); });
}

void destructor_drains()
{
    Future<int> future;
    std::atomic<int> count{0};
    {
        Executor executor(2);
        for (int i = 0; i < 100; ++i)
            future = executor.spawn([&] { return ++count; });
    }
    CHECK(count == 100);
    CHECK(future.is_ready());
    CHECK(future.get() > 0);
}

void exceptions()
{
    Executor executor(1);
    auto failed = executor.spawn([]() -> int { throw std::runtime_error("task failed"); });
    expect_throw<std::runtime_error>([&] { failed.get(); });
    CHECK(executor.spawn([] { return 42; }).get() == 42);
    auto self_shutdown = executor.spawn([&] { executor.shutdown(); });
    expect_throw<std::logic_error>([&] { self_shutdown.get(); });
    CHECK(executor.spawn([] { return 21; }).get() == 21);
}

void repeated()
{
    for (int round = 0; round < 20; ++round) {
        Executor executor(2);
        for (int i = 0; i < 25; ++i)
            CHECK(executor.spawn([i] { return i; }).get() == i);
    }
}

void concurrent_submission_shutdown()
{
    Executor executor(4);
    std::atomic<int> accepted{0};
    std::atomic<int> executed{0};
    std::barrier start(6);
    std::vector<std::jthread> callers;
    for (int i = 0; i < 4; ++i)
        callers.emplace_back([&] {
            start.arrive_and_wait();
            for (int j = 0; j < 200; ++j) {
                try {
                    executor.spawn([&] { ++executed; });
                    ++accepted;
                } catch (const std::runtime_error &) {
                    break;
                }
            }
        });
    callers.emplace_back([&] { start.arrive_and_wait(); executor.shutdown(); });
    start.arrive_and_wait();
    executor.shutdown();
    callers.clear();
    CHECK(accepted == executed);
}

void capture_lifetime()
{
    Executor executor(1);
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> observer = resource;
    auto future = executor.spawn([resource] { return *resource; });
    resource.reset();
    executor.shutdown();
    CHECK(future.get() == 42);
    CHECK(observer.expired());
}

void worker_shutdown_during_join()
{
    Executor executor(1);
    std::latch running(1);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto worker = executor.spawn([gate, &running, &executor] {
        running.count_down();
        gate.wait();
        executor.shutdown();
    });
    running.wait();
    std::jthread external([&] { executor.shutdown(); });
    int accepted = 0;
    std::atomic<int> executed{0};
    // Observe the shutdown admission boundary, not an assumed scheduling delay.
    while (true) {
        try {
            executor.post([&executed]() noexcept { ++executed; });
            ++accepted;
        } catch (const std::runtime_error &) {
            break;
        }
        std::this_thread::yield();
    }
    release.set_value();
    expect_throw<std::logic_error>([&] { worker.get(); });
    external.join();
    CHECK(executed == accepted);
}

int main(int argc, char ** argv)
{
    return run_test(argc, argv, {{"execution", execution}, {"overlap", overlap}, {"fifo", fifo},
        {"shutdown", shutdown}, {"destructor_drains", destructor_drains}, {"exceptions", exceptions},
        {"repeated", repeated}, {"concurrent_submission_shutdown", concurrent_submission_shutdown},
        {"capture_lifetime", capture_lifetime}, {"worker_shutdown_during_join", worker_shutdown_during_join}});
}
