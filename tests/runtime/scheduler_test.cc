#include "nix_async/executor.hh"
#include "nix_async/scheduler.hh"

#include <atomic>
#include <barrier>
#include <future>
#include <latch>
#include <memory>
#include <thread>
#include <vector>

#include "test.hh"

using namespace nix_async;

void lifecycle()
{
    Executor executor(1);
    Scheduler scheduler(executor);
    std::latch blocker_running(1);
    std::promise<void> release_blocker;
    auto blocker_gate = release_blocker.get_future().share();
    auto blocker = scheduler.spawn([&blocker_running, blocker_gate] {
        blocker_running.count_down();
        blocker_gate.wait();
    });
    blocker_running.wait();
    std::latch task_running(1);
    std::promise<void> release_task;
    auto task_gate = release_task.get_future().share();
    Task task([&task_running, task_gate] {
        task_running.count_down();
        task_gate.wait();
        return 42;
    });
    CHECK(task.state() == TaskState::Created);
    auto future = scheduler.submit(task);
    CHECK(task.state() == TaskState::Pending);
    release_blocker.set_value();
    task_running.wait();
    CHECK(task.state() == TaskState::Running);
    CHECK(!future.is_ready());
    release_task.set_value();
    CHECK(future.get() == 42);
    CHECK(task.state() == TaskState::Completed);
    expect_throw<std::logic_error>([&] { scheduler.submit(task); });
    CHECK(task.state() == TaskState::Completed);
    blocker.get();
}

void failed()
{
    Executor executor(1);
    Scheduler scheduler(executor);
    Task task([]() -> int { throw std::runtime_error("native failure"); });
    auto future = scheduler.submit(task);
    expect_throw<std::runtime_error>([&] { future.get(); });
    CHECK(task.state() == TaskState::Failed);
}

struct ThrowingMove
{
    ThrowingMove() = default;
    ThrowingMove(const ThrowingMove &) = delete;
    ThrowingMove(ThrowingMove &&) { throw std::runtime_error("result move failed"); }
};

void result_failure()
{
    Executor executor(1);
    Scheduler scheduler(executor);
    Task task([] { return ThrowingMove{}; });
    auto future = scheduler.submit(task);
    expect_throw<std::runtime_error>([&] { future.get(); });
    CHECK(task.state() == TaskState::Failed);
    CHECK(executor.spawn([] { return 42; }).get() == 42);
}

void rejection()
{
    Executor executor(1);
    Scheduler scheduler(executor);
    executor.shutdown();
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> observer = resource;
    Task task([resource] { return *resource; });
    resource.reset();
    auto future = task.get_future();
    expect_throw<std::runtime_error>([&] { scheduler.submit(task); });
    CHECK(task.state() == TaskState::Failed);
    CHECK(future.is_ready());
    CHECK(observer.expired());
    expect_throw<std::runtime_error>([&] { future.get(); });
}

void concurrent_inspection()
{
    Executor executor(1);
    Scheduler scheduler(executor);
    std::promise<void> release;
    auto gate = release.get_future().share();
    Task task([gate] { gate.wait(); return 42; });
    std::atomic<bool> bad_state{false};
    std::latch readers_started(4);
    std::vector<std::jthread> readers;
    for (int i = 0; i < 4; ++i)
        readers.emplace_back([task, &bad_state, &readers_started] {
            readers_started.count_down();
            auto previous = TaskState::Created;
            do {
                auto current = task.state();
                if (current < previous || current == TaskState::Failed)
                    bad_state = true;
                previous = current;
            } while (previous != TaskState::Completed && previous != TaskState::Failed);
        });
    readers_started.wait();
    auto future = scheduler.submit(task);
    release.set_value();
    CHECK(future.get() == 42);
    readers.clear();
    CHECK(!bad_state);
}

void duplicate_submission()
{
    Executor executor(2);
    Scheduler scheduler(executor);
    std::atomic<int> executions{0};
    std::atomic<int> accepted{0};
    std::atomic<int> rejected{0};
    Task task([&] { ++executions; return 42; });
    std::barrier start(9);
    std::vector<std::jthread> callers;
    for (int i = 0; i < 8; ++i)
        callers.emplace_back([&] {
            start.arrive_and_wait();
            try {
                scheduler.submit(task);
                ++accepted;
            } catch (const std::logic_error &) {
                ++rejected;
            }
        });
    start.arrive_and_wait();
    callers.clear();
    CHECK(task.get_future().get() == 42);
    CHECK(accepted == 1 && rejected == 7 && executions == 1);
}

int main(int argc, char ** argv)
{
    return run_test(argc, argv, {{"lifecycle", lifecycle}, {"failed", failed}, {"result_failure", result_failure}, {"rejection", rejection},
        {"concurrent_inspection", concurrent_inspection}, {"duplicate_submission", duplicate_submission}});
}
