#include "nix_async/executor.hh"
#include "nix_async/when_all.hh"

#include <future>
#include <iostream>
#include <latch>
#include <memory>

int main()
{
    nix_async::Executor executor(2);
    auto started = std::make_shared<std::latch>(2);
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto work = [started, gate] {
        started->count_down();
        gate.wait();
        return 21;
    };
    auto a = executor.spawn(work);
    auto b = executor.spawn(work);
    auto combined = nix_async::when_all(a, b);
    started->wait(); // Both tasks must be running before either may complete.
    release.set_value();
    const auto & results = combined.get();
    std::cout << results[0] + results[1] << '\n';
}
