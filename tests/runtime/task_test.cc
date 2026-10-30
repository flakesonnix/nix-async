#include "nix_async/task.hh"

#include <future>
#include <utility>

#include "test.hh"

using namespace nix_async;

void initial()
{
    Task task([] { return 42; });
    CHECK(task.state() == TaskState::Created);
    CHECK(!task.get_future().is_ready());
    auto copy = task;
    CHECK(copy.state() == TaskState::Created);
    Task moved(std::move(copy));
    CHECK(moved.state() == TaskState::Created);
    expect_future_error(std::future_errc::no_state, [&] { copy.state(); });
}

void abandoned()
{
    Future<int> future;
    {
        Task task([] { return 42; });
        future = task.get_future();
    }
    expect_future_error(std::future_errc::broken_promise, [&] { future.get(); });
}

void empty()
{
    expect_throw<std::invalid_argument>([] { Task<int> task(std::move_only_function<int()>{}); });
}

int main(int argc, char ** argv)
{
    return run_test(argc, argv, {{"initial", initial}, {"abandoned", abandoned}, {"empty", empty}});
}
