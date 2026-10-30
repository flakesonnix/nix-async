# nix-async

[![CI](https://github.com/flakesonnix/nix-async/actions/workflows/ci.yml/badge.svg)](https://github.com/flakesonnix/nix-async/actions/workflows/ci.yml)

An independent C++ project exploring asynchronous **native operations** initiated
and consumed from Nix expressions, without a private Nix fork. Not part of Nix or
nixray. The goal is genuine overlapping operations, not synchronous calls renamed
to async.

## Status: Phase 1

Implemented and tested against **Nix 2.34.9 on x86_64-linux**:

- A native plugin loaded through `plugin-files` into unmodified Nix.
- One real namespace, `builtins.async`, containing a synchronous integer identity
  probe: `builtins.async.probe 42` returns `42`.
- A standalone C++ runtime: tasks, shared futures, move-only promises, a fixed-size
  thread-pool executor, a scheduler and non-waiting `when_all` composition.
- CMake/CTest runtime and plugin tests, including deterministic concurrency,
  shutdown races, errors, lifetimes and GC-pressure reuse of the plugin probe.
- A pinned Nix flake for building, testing and development.

**The runtime is not connected to the Nix plugin yet.** Only `probe` is exposed
to Nix. Nix `spawn`, `await`, `all`, `race`, `sleep` and `cancel` remain proposed
APIs. There are no timers, cancellation, networking or event-loop backends.

## Architecture and feasibility

`nix_plugin_entry()` registers a zero-arity `__async` PrimOp, which Nix exposes as
the lazy constant `builtins.async`. When forced, it builds an attribute set with
a nested `probe` PrimOp. There are no evaluator pointers stored in plugin-owned
state, no background threads, and no direct linkage to Nix libraries.

The plugin interface can support native tasks that return handles immediately.
It does **not** supply evaluator continuations: a future `await` would block the
calling evaluator while native operations continue independently. Arbitrary Nix
closures cannot simply be evaluated on worker threads in the same `EvalState`.
New syntax and evaluator suspension/resumption are outside this plugin's scope.

The native library does not include or link Nix. `Scheduler` registers a task's
transition to Pending and dispatches it to `Executor`; workers transition tasks
through Running to Completed/Failed. Task handles retain lifecycle information
without an unbounded scheduler registry. `Executor::spawn` is a convenience over
the same scheduler. Standard promises/shared futures handle results and errors;
completion observers implement composition without a waiting thread.

See [Phase 1 ownership and concurrency semantics](docs/phase-1.md).

See the [Phase 0 architecture report](docs/phase-0.md) for matching upstream
source references, C API findings, GC/ownership constraints and the precise
native-operation/evaluator distinction.

## Native runtime example

```cpp
#include "nix_async/executor.hh"
#include "nix_async/when_all.hh"

nix_async::Executor executor(2);
auto a = executor.spawn([] { return 21; });
auto b = executor.spawn([] { return 21; });
auto combined = nix_async::when_all(a, b);
const auto & results = combined.get(); // ordered vector<int>: [21, 21]
```

Submission alone does not prove overlap. The runnable
[`examples/runtime.cc`](examples/runtime.cc) holds both tasks at a gate and only
releases them after both have started. Build it as `build/nix_async_example`.

`Future<T>` is copyable and multi-consumer: `get()` returns `const T &`, valid
while a result-owning future remains alive (`void` for `Future<void>`). Move-only
results can be inspected, not extracted. `Promise<T>` is move-only; abandoning it
notifies consumers with `broken_promise`. `when_all` takes homogeneous futures,
copies results in input order, waits for **all** completions even on failure, and
reports the first input-order error. Non-void composition requires copyable values.

## Build and use

Enable Nix's `nix-command` and `flakes` experimental features. The flake pins both
the development headers and test executable to the same Nix package. Build:

```sh
nix build
nix develop -c nix --option plugin-files "$PWD/result/lib/nix-async/libnix_async.so" \
  eval --store dummy:// --json --expr 'builtins.async.probe (20 + 22)'
# 42
```

The module is installed under `lib/nix-async/`. Load it explicitly per command;
no system configuration changes are needed. Put `--option plugin-files` **before**
`eval`, because plugins are loaded before the subcommand runs. `dummy://` avoids
needing a store daemon for these evaluation-only examples.

**ABI warning:** Nix's C++ plugin API is explicitly unstable. Use the matching
flake-provided Nix executable, not an arbitrary system Nix. Exact version checks
give useful diagnostics but cannot detect incompatible patches, compiler ABI or
build configuration. Do not load this module into Lix or another Nix release.
Only load trusted plugins: native plugins run with the evaluator process's full
permissions. This project does not enable `importNative` or
`allow-unsafe-native-code-during-evaluation`.

## Development

The selected Nix headers require **C++23**, so this milestone uses C++23 rather
than C++20. Nix's headers depend on Boost; the project does not use Boost.Asio or
any external asynchronous runtime.

```sh
nix develop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
nix --option plugin-files "$PWD/build/libnix_async.so" \
  eval --store dummy:// --json --file examples/basic.nix
nix flake check
```

Runtime-only builds need neither Nix development headers nor Boost:

```sh
cmake -S . -B build-runtime -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DNIX_ASYNC_BUILD_PLUGIN=OFF
cmake --build build-runtime
ctest --test-dir build-runtime --output-on-failure
./build-runtime/nix_async_example
```

Use the `nix_async_runtime` CMake target when embedding via `add_subdirectory`.
Installation includes `libnix_async_runtime.a` and `include/nix_async/`.

Outside the flake, supply Nix **2.34.9** development headers and their transitive
dependencies through `pkg-config` (`nix-expr`), compatible Boost headers, GCC or
Clang with C++23 support, CMake >= 3.24, a build tool, and `readelf`. Set
`-DNIX_EXECUTABLE=/path/to/matching/nix` if needed. CMake deliberately queries only
Nix compilation flags; a plugin must resolve Nix symbols from its host.

Runtime and plugin UndefinedBehaviorSanitizer run (inside `nix develop`):

```sh
cmake -S . -B build-ubsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=undefined -fno-sanitize-recover=all' \
  -DCMAKE_MODULE_LINKER_FLAGS='-fsanitize=undefined' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=undefined'
cmake --build build-ubsan
ctest --test-dir build-ubsan --output-on-failure
```

This instruments native runtime code and the plugin, **not the host Nix or Boehm
GC**. It is not a whole-evaluator memory-safety check. See the Phase 1 report for
standalone ASan/TSan commands and the observed GCC TSan exception-retry limitation.

## Limitations

- Compatibility is limited to the pinned Linux/Nix build; no stable binary ABI.
- `probe` is a temporary feasibility tool, not the future task API.
- No automatic plugin installation, evaluator patch, parser extension or fork.
- GC stress tests exercise Nix-owned values, not future native task finalization.
- Native tasks are non-cancellable; shutdown drains accepted work and can wait
  indefinitely for a non-returning task. The queue is currently unbounded.
- Do not block pool workers waiting for work queued to that same saturated pool.
  `when_all` does not introduce such waits, but arbitrary user callables can.
- Pure evaluation works for this deterministic probe. Future I/O must explicitly
  address impurity, restricted evaluation and Nix string/store-path contexts;
  loading a plugin does not enforce those policies automatically.

License: [MIT](LICENSE).
