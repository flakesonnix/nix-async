# Phase 1: standalone core runtime

Baseline: `2e3eb24` (clean worktree). C++23 and the pinned Nix 2.34.9 environment
are preserved. `src/plugin.cc` and its 16 tests are unchanged. No evaluator
integration, networking, timers, cancellation or event loop is introduced.

## Components and ownership

- `Task<T>` is a copyable handle to one owned callable and its producer. It starts
  Created; copying the handle does not duplicate execution. The scheduler alone
  can mark it Pending. The queued closure retains the task until execution ends.
- `Scheduler` is an immediate FIFO-dispatch policy over a referenced `Executor`.
  It marks Pending, rejects repeated submissions, enqueues one execution and
  publishes admission errors. Task/Future handles, not an unbounded registration
  table, retain task state/results. The executor must outlive the scheduler.
- `Executor` owns an explicitly sized, non-movable thread pool, a mutex-protected
  FIFO queue and a condition variable. It executes work outside queue locks.
  With multiple workers, dequeue order does not imply start/completion order.
  `spawn` delegates to the scheduler; low-level `post` accepts only `noexcept`
  callables. Ordinary potentially throwing work belongs in tasks.
- `Future<T>` is a copyable, multi-consumer handle. `get()` returns `const T &`;
  `Future<void>::get()` returns void. It can be called repeatedly/concurrently.
  Keep a result-owning future alive while using that reference. No destructive
  move-out operation exists. Move-only values are supported for read-only
  inspection. Reference/const-qualified result types are not supported.
- `Promise<T>` is the move-only producer. Repeated `get_future()` returns shared
  consumers. Standard `std::promise`/`std::shared_future` own result storage,
  blocking waits and exception transport. Destroying/replacing an incomplete
  Promise destroys the standard producer and publishes `broken_promise`.

The task callable is moved out of task storage during execution and released when
execution returns; keeping a completed Task does not retain callable captures.
Results can outlive the executor. Dropping all consumers does not cancel work.
External resources captured by reference must outlive their tasks. These handles
do not make pointees, mutable result internals or arbitrary user code thread-safe.

## Publication and lifecycle

Normal states are `Created -> Pending -> Running -> Completed | Failed`.
A submission rejected by a stopped executor (or a queue allocation failure) makes
the already-Pending task Failed and rethrows the admission error to the submitter;
its existing Future also carries that error. Duplicate submission throws
`logic_error` without modifying the original task. Destroying an unscheduled task
abandons its producer and makes any surviving future report `broken_promise`.

A shared metadata mutex serializes state changes, producer completion and observer
registration. `Future::wait/get` first use the standard future's wait, then acquire
that mutex: the native result may wake waiters before terminal metadata is
published, so the extra synchronization prevents a successful `get()` from
preceding Completed/Failed. `is_ready` and task inspection use the same mutex.
Result construction failure leaves a manual promise incomplete, permitting retry;
a task catches it and publishes a Failed exception instead. No state lock or
queue lock is held while invoking the task's callable.

Default/moved-from Futures and moved-from Promises/Tasks reject access with
`future_error(no_state)`. Repeated producer completion throws
`future_error(promise_already_satisfied)`; a null exception is `invalid_argument`
and does not complete the result. Concurrent completion attempts serialize with
one winner. As with standard smart pointers, concurrent const handle operations
are supported, not moving/destroying/assigning the **same handle object** while
another thread uses it. Result constructors must not reenter their own producer
or metadata lock, just as blocking inside a standard promise's result constructor
can deadlock its completion operation.

## Composition without worker waits

`when_all` accepts a vector, initializer list or one-or-more homogeneous Futures.
For non-void inputs it returns `Future<vector<T>>`, requiring copy-constructible
values; void inputs return `Future<void>`. Empty input completes immediately.
Invalid handles throw synchronously before any observers are registered.

Small internal `noexcept` completion observers decrement an atomic counter. The
last completion gathers already-ready standard futures in input order and
publishes the aggregate. There is no extra thread, pool job waiting for input, or
polling loop. Failure is reported only after all inputs finish, using the first
exception in input order (including value-copy failure during collection).
Duplicate inputs, already-ready inputs, and concurrent subscription/completion
are supported. Input futures remain usable; composition never consumes results.

Observers run inline on the completing thread, **after releasing input metadata
locks**. On already-ready inputs that can be the caller registering composition.
Large result copies therefore cost time on that thread, and deeply nested
compositions can use a deep call stack; no general continuation/trampoline
scheduler is claimed. Public user continuations are not exposed in this phase.

The aggregate retains the inputs' standard result storage, not their observer
states. This avoids a cycle of input -> observer -> aggregate -> input. Pending
subscriptions retain the aggregate until inputs complete (including abandonment).
Dropping the combined Future neither cancels input work nor leaves a completed
aggregate permanently rooted. Lifetime tests check both consumer-drop orders.

## Shutdown and scheduling limits

Admission and the stop flag share the queue mutex. Every accepted item is drained
exactly once; submissions after shutdown begins fail. Shutdown wakes workers and
joins them without holding the queue mutex. A separate join mutex serializes
concurrent external shutdown callers. It is idempotent. Partial thread-creation
failure stops and joins workers already started. No threads are detached.

A worker calling its own executor's `shutdown` gets `logic_error` before acquiring
the join mutex, even if an external shutdown caller is already joining it. Active
worker IDs are protected and removed on exit so later reused OS thread IDs cannot
cause false self-shutdown detection. Destruction must occur on an external owner
thread; destroying a pool on its own worker violates its lifetime contract and
terminates via the noexcept destructor. The owner must also exclude destruction
while external callers are using the executor.

Shutdown cannot interrupt a running callable, and it may wait indefinitely for
one. There is no cancellation, bounded admission, fairness guarantee beyond FIFO
dequeue, work stealing or worker helping. Waiting inside a worker for another job
in a saturated pool can deadlock. `when_all` itself never introduces that wait.
These are explicit constraints, not silently implemented async I/O features.

## Build and tests

`nix_async_runtime` builds only `src/executor.cc` and `src/scheduler.cc`; the generic
Task/Future/Promise/composition implementations live in public headers. It depends
only on the C++ standard library and Threads. `NIX_ASYNC_BUILD_PLUGIN=OFF` bypasses
all Nix, pkg-config and Boost discovery. The flake checks both the combined
package and a runtime-only derivation without Nix development inputs.

CTest uses small always-active C++ checks (not `assert`, which Release disables)
and one bounded process per case. Latches/barriers gate concurrent tasks and
readers; there are no arbitrary sleeps or timing-based speedup claims. Coverage
includes lifecycle, admission/duplicate races, competing producers, shared reads,
FIFO behavior, drain/repeated/concurrent shutdown, callable/result exceptions,
broken producers, capture/result lifetimes, ordered/error/void/empty composition,
subscription races, nested composition and a single-worker no-wait composition
test. `examples/runtime.cc` independently gates two workers before printing 42.

Runtime-only ThreadSanitizer build:

```sh
cmake -S . -B build-tsan -G Ninja -DNIX_ASYNC_BUILD_PLUGIN=OFF \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer -g' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread'
cmake --build build-tsan -j 4
ctest --test-dir build-tsan --output-on-failure \
  -E '^runtime\.(future\.construction_failure|scheduler\.result_failure)$'
```

**Observed TSan limitation:** GCC 16.2.0/libtsan hangs in its `pthread_once`
interceptor when a standard promise retries completion after a result constructor
throws. A Nix-free, nix-async-free reproducer also hangs:

```cpp
#include <future>
#include <stdexcept>
#include <utility>
struct Value {
    Value() = default;
    Value(const Value &) { throw std::runtime_error("copy failed"); }
    Value(Value &&) = default;
};
int main() {
    std::promise<Value> p;
    Value value;
    try { p.set_value(value); } catch (const std::runtime_error &) {}
    p.set_value(std::move(value)); // hangs under this libtsan, works normally
}
```

Compile with `c++ -std=c++23 -pthread -g -fsanitize=thread repro.cc -o repro`.
Both exception-retry tests remain enabled and required in Debug, UBSan and normal
flake checks; only the command above excludes them for this TSan toolchain. This
is not a claim of a clean full-suite TSan run. The host Nix/GC is not instrumented.

Standalone AddressSanitizer plus UBSan can be run without involving Nix or Boehm:

```sh
cmake -S . -B build-asan -G Ninja -DNIX_ASYNC_BUILD_PLUGIN=OFF \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-asan -j 4
ctest --test-dir build-asan --output-on-failure
```

## Evaluator boundary

The plugin is deliberately not linked to or calling the runtime yet. No worker
receives `EvalState`, `Value`, thunks or borrowed evaluator storage. Native task
handles, GC finalization, Boehm worker registration, impurity policies and
evaluator-thread result conversion still require a separate integration phase.
Nothing here changes the Phase 0 verdict about arbitrary Nix evaluation.
