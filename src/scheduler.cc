#include "nix_async/scheduler.hh"
#include "nix_async/executor.hh"

#include <utility>

namespace nix_async {

void Scheduler::enqueue(std::move_only_function<void() noexcept> work) const
{
    executor_.post(std::move(work));
}

} // namespace nix_async
