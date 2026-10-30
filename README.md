# nix-async

An independent C++ project exploring asynchronous **native operations** initiated
and consumed from Nix expressions, without a private Nix fork. Not part of Nix or
nixray. The goal is genuine overlapping operations, not synchronous calls renamed
to async.

## Status: Phase 0

Implemented and tested against **Nix 2.34.9 on x86_64-linux**:

- A native plugin loaded through `plugin-files` into unmodified Nix.
- One real namespace, `builtins.async`, containing a synchronous integer identity
  probe: `builtins.async.probe 42` returns `42`.
- CMake/CTest integration tests, including errors, laziness, integer boundaries,
  reuse under GC pressure, and ELF plugin contract checks.
- A pinned Nix flake for building, testing and development.

**No tasks, futures, promises, executor, scheduler, timers, cancellation or async
I/O are implemented yet.** `spawn`, `await`, `all`, `race`, `sleep` and `cancel`
are proposed functionality, not available APIs. There is no concurrency or
performance claim in this milestone.

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

See the [Phase 0 architecture report](docs/phase-0.md) for matching upstream
source references, C API findings, GC/ownership constraints and the precise
native-operation/evaluator distinction.

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

Outside the flake, supply Nix **2.34.9** development headers and their transitive
dependencies through `pkg-config` (`nix-expr`), compatible Boost headers, GCC or
Clang with C++23 support, CMake >= 3.24, a build tool, and `readelf`. Set
`-DNIX_EXECUTABLE=/path/to/matching/nix` if needed. CMake deliberately queries only
Nix compilation flags; a plugin must resolve Nix symbols from its host.

Plugin-only UndefinedBehaviorSanitizer run:

```sh
cmake -S . -B build-ubsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=undefined -fno-sanitize-recover=all' \
  -DCMAKE_MODULE_LINKER_FLAGS='-fsanitize=undefined'
cmake --build build-ubsan
ctest --test-dir build-ubsan --output-on-failure
```

This instruments the plugin, **not the host Nix or Boehm GC**. It is not a
whole-evaluator memory-safety check. ASan/TSan are not validated for this milestone;
there is no concurrent runtime to test yet.

## Limitations

- Compatibility is limited to the pinned Linux/Nix build; no stable binary ABI.
- `probe` is a temporary feasibility tool, not the future task API.
- No automatic plugin installation, evaluator patch, parser extension or fork.
- GC stress tests exercise Nix-owned values, not future native task finalization.
- Pure evaluation works for this deterministic probe. Future I/O must explicitly
  address impurity, restricted evaluation and Nix string/store-path contexts;
  loading a plugin does not enforce those policies automatically.

License: [MIT](LICENSE).
