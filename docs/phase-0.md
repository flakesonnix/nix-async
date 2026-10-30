# Phase 0: Nix plugin feasibility

## Selected implementation and evidence

Investigation date: 2026-10-30. The repository was empty. Local tools were Nix
2.34.9 (`nix` and `nix-instantiate`), GCC 16.2.0, CMake 4.4.4, Ninja and pkg-config
on x86_64-linux. Clang was not on PATH. The installed evaluator uses Boehm GC
(`NIX_USE_BOEHMGC=1`). No Nix fork or evaluator changes were made.

Upstream release tag **2.34.9** resolves to commit
**`5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc`**. The exact source fetched by the
installed nixpkgs package was inspected locally; its NAR hash is
`sha256-ZJwg/qG4M1Impy774HKMZ3Hsq7vlBJy7sXl1fI9QDXI=`. The pinned nixpkgs revision
is `02c087f3fe673c9e317f206a627cb6b910336dbe`. Its Linux `nix-expr` package has no
patches. Nixpkgs filters some unrelated functional tests from the source; the
evaluator/plugin files studied here match upstream. See the [package definition].

All source links below pin that upstream commit, not a moving branch. The
installed `nix-expr.pc` declares version 2.34.9 and `-std=c++23`; C++23 is therefore
required for the plugin build. Boost is a Nix-header dependency, not a runtime
architecture choice by nix-async.

## Plugin loading and the namespace

The authoritative `plugin-files` documentation is embedded in [libmain/plugin.cc].
Nix `dlopen`s a DSO with `RTLD_LAZY | RTLD_LOCAL`, then calls its optional C symbol
`nix_plugin_entry()`. Static registration is also supported. The documentation
explicitly calls the API unstable, requires compatible headers/libraries, and
says **not to link Nix libraries directly**. The loader intentionally keeps the
DSO handle alive. That is not a task-shutdown or native-resource cleanup protocol.
The [upstream plugin test] demonstrates `RegisterPrimOp` and loading before `eval`.

The relevant C++ interfaces are real, source-verified interfaces:

```cpp
using PrimOpFun = void(EvalState &, const PosIdx, Value **, Value &);
RegisterPrimOp(PrimOp &&);
```

[eval.hh] defines `PrimOp`, including `name`, `args`, `arity`, `impl` and
`internal`; [primops.hh] declares registration. [createBaseEnv] copies registered
PrimOps into each evaluator before sorting builtins. [addPrimOp] supports arity
zero by turning the PrimOp into an application with a dummy argument: **a lazy
constant**, not an evaluator continuation.

Important discrepancy: the comment in `primops.hh` says the zero-arity callback
runs during evaluator initialization. In this release the actual `addPrimOp`
implementation defers the callback until the constant is forced. The plugin
uses the implementation's behavior and does not modify builtins during forcing.

`addConstant`/`addPrimOp` strip a leading `__` in the builtins attribute name.
Registering **one** constant named `__async` therefore creates `builtins.async`
(and the base-environment binding `__async`). Its initializer allocates a normal
attribute set whose `probe` member is a PrimOp value. Nested functions do not
need individual global registrations. A dotted registration name would instead
create a literal dotted key; it is not a namespace mechanism.

The proof of concept in `src/plugin.cc` implements exactly this namespace and a
synchronous integer identity operation. Only process-lifetime PrimOp metadata
is static; it contains no evaluator-owned pointers or captured evaluator state.
Each forced namespace allocates its values/bindings through that evaluator.

## EvalState, values, thunks and threads

[eval.hh] exposes mutable evaluator state: symbol tables, environments, caches,
allocation statistics, debugging state and call depth. [value.hh] distinguishes
the public `ValueType` (integer, attributes, list, function, external value,
thunk, etc.) from internal representations such as `tThunk`, `tApp`, `tPrimOp`
and `tPrimOpApp`. There is no built-in future/promise value type.

[forceValue] replaces a thunk with a blackhole, evaluates its expression and
environment, then overwrites the value with the result (or records/restores error
state). These are ordinary mutable operations, not atomic claims on a shared
future. [callFunction] invokes a PrimOp's `impl` directly on the evaluator's call
stack and proceeds when it returns. Its signature has an output `Value &`, not
a pending result or continuation. A worker must not write that output later.

The [C evaluator API] says multiple evaluator states can be created for
multithreaded operation. This does **not** grant permission to force the same
closure/thunk or access one `EvalState` concurrently. Separate-state embedding
is a different integration: values, environments and lazy closures cannot simply
be transplanted between states. Nix's thread-aware allocation caches and Boehm's
thread support are not evidence that shared expression evaluation is safe.

**Project constraint:** all forcing, argument validation, Nix allocations, error
construction and result conversion stay on the invoking evaluator thread.
Native workers must receive copied/owned native inputs, not `Value *`, `Env *`,
`Expr *`, `EvalState *` or borrowed Nix string views. Phase 0 has no workers.

## What the C API changes (and does not)

The [C API manual] explicitly says the C API is not yet stable. It nevertheless
provides documented interop and ownership operations worth considering:

- `nix_alloc_primop` allocates a callback-backed PrimOp.
- `nix_register_primop` moves it into the registry for **subsequent** evaluator
  states; the original reference must then be released with `nix_gc_decref` and
  must not be used to initialize a value. See [C value API].
- `nix_value_force` is synchronous and mutates a thunk.
- `nix_value_incref` / `nix_value_decref` and the general `nix_gc_*` functions
  provide explicitly rooted references for C callers.
- `nix_gc_register_finalizer` exposes GC finalization, not deterministic shutdown.

The [C callback wrapper] still calls the PrimOp synchronously, checks its result
and rejects a thunk return. It does not add a scheduler hook or suspension token.
The C API is useful for embedding/FFI, but cannot make the host evaluator
resumable. This C++ proof of concept uses the plugin loader's direct C++ interface
and does not depend on a C API DSO being loaded by the host CLI.

## Garbage collection and ownership

[eval-gc.cc] initializes Boehm, disables general interior-pointer scanning,
registers tagged-pointer displacements and calls `GC_set_no_dls(1)`. Consequently,
**do not rely on plugin static storage being scanned as a GC root**. [allocation]
allocates Nix values on the GC heap and maintains thread-local allocation caches.
A raw pointer in an ordinary C++ heap container is not a sufficient rooting
protocol. C API reference counting is backed by a traceable concurrent map; see
[C GC implementation]. `shared_ptr<Value>` is not a substitute for that protocol.

The current plugin retains no such pointers. Returned attribute bindings belong
to Nix, while the nested PrimOp points to process-lifetime native metadata that
does not refer back to Nix values. The 30,000-call reuse test starts with a 1 MiB
GC heap and requires more than the final statistics-triggered GC cycle.

For native tasks, the feasible ownership boundary is an owned native shared
state referenced by an opaque handle. [value.hh] offers `ExternalValueBase` and
`nExternal`, so an external task handle is possible without inventing a new
`ValueType`. However, a virtual destructor alone does not arrange GC finalization
of native `shared_ptr`s, file descriptors or threads. Handle rooting, finalizers,
explicit release, state isolation and shutdown still need implementation and
lifetime tests **before** exposing real tasks. Plain integer handles plus an
unbounded global registry would leak and permit forgery; they are not an adequate
default.

Native completions must be synchronized and terminal transitions must happen
once. Cancellation must compete with completion through the same synchronization;
it cannot free resources still in use. No callback/finalizer may force Nix values,
join workers while holding a task mutex, or depend on the evaluator remaining
alive. Any future worker creation also needs a Boehm thread-registration audit
for the chosen build. None of this task machinery is claimed by Phase 0.

## Feasibility verdict

| Objective | Plugin-only verdict for this Nix release |
| --- | --- |
| `builtins.async` attribute namespace | Implemented and exercised in unmodified Nix. |
| Start native work and immediately return a handle | Feasible; runtime and handle lifetime not implemented yet. |
| Multiple pending native timers/I/O operations | Feasible with a separate native runtime; not demonstrated yet. |
| `await` a native result | Feasible as a blocking PrimOp on the evaluator thread. |
| `all` / `race` over native task handles | Feasible composition; not implemented yet. |
| `spawn` an arbitrary existing Nix closure on shared-state workers | Not supported safely by this interface. |
| Suspend/resume arbitrary evaluation without blocking the evaluator | No continuation/suspension facility in this interface. |
| `async {}` / new `await` syntax | No parser extension mechanism here; out of scope. |

For genuine overlap, a native start operation must finish scheduling and return
a handle **without waiting**. Nix let-bindings are lazy: writing two bindings does
not start two operations. An `all` implementation must first force/schedule every
input handle, then wait, rather than force-and-await each input sequentially.
Native completion converts to a Nix value only when consumed on the evaluator
thread. A future `spawn` name must mean a defined native operation/task descriptor,
not silently promise parallel arbitrary Nix lambdas.

Objective A (async native work) therefore does not require a fork. Objective B
(suspending and concurrently evaluating the existing Nix expression graph) would
require evaluator-level changes: resumable forcing/call frames, safe thunk
ownership and synchronization, rooting suspended environments, cancellation and
exception semantics. Plugins may add native values/functions; they do not replace
the evaluator's forcing protocol. Blocking a PrimOp, running another independent
evaluator, or calling a Nix closure from a worker is not an implementation of B.
No fork has been introduced to conceal this limitation.

## Runtime implications, not implemented features

The native runtime can remain independent of Nix headers and GC. A Linux event
loop plus a bounded executor for genuinely blocking work is a plausible hybrid:
`epoll` for readiness, `eventfd` for wake-ups and a monotonic timer queue (possibly
driven by `timerfd`). `poll` offers a simpler portable readiness backend but scans
the descriptor set; `io_uring` adds a completion model and kernel/operation support
constraints that are unnecessary to prove the initial boundary. No backend or
external async dependency is implemented in Phase 0, and `std::async` is not used.

Before native I/O, impurity/restricted-evaluation policy and string/store-path
contexts need explicit handling; the plugin loading mechanism does not enforce
them on custom operations. The probe is deterministic and allowed in pure mode.
Native task/cancellation/shutdown tests and deterministic overlap tests belong
with the actual runtime, not fabricated Phase 0 abstractions.

## Validation scope

CTest covers explicit loading, namespacing, integer arguments/boundaries, forcing,
an unforced failing binding, type errors, thrown/recoverable errors, function reuse
under observed GC cycles, the example, and absence without plugin loading. ELF
checks require an exported C entry point and no direct `libnix*` dependency. Each
evaluation runs in a fresh process with a timeout, isolated Nix configuration and
a dummy store; tests need neither networking nor a daemon.

The normal debug build, plugin-only UBSan build and sandboxed flake package build
were executed successfully. UBSan does not instrument the host evaluator/GC.
ASan/TSan have not been validated. GC pressure is not proof of native handle
finalization, and independent test processes are not evidence of a concurrent
runtime. Phase 0 makes no async performance claim.

[package definition]: https://github.com/NixOS/nixpkgs/blob/02c087f3fe673c9e317f206a627cb6b910336dbe/pkgs/tools/package-management/nix/default.nix#L194-L208
[libmain/plugin.cc]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/src/libmain/plugin.cc#L36-L126
[upstream plugin test]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/tests/functional/plugins/plugintest.cc
[eval.hh]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/src/libexpr/include/nix/expr/eval.hh
[primops.hh]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/src/libexpr/include/nix/expr/primops.hh
[createBaseEnv]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/src/libexpr/primops.cc#L5423-L5457
[addPrimOp]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/src/libexpr/eval.cc#L456-L554
[value.hh]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/src/libexpr/include/nix/expr/value.hh#L29-L162
[forceValue]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/src/libexpr/include/nix/expr/eval-inline.hh#L110-L138
[callFunction]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/src/libexpr/eval.cc#L1645-L1719
[C API manual]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/doc/manual/source/c-api.md
[C evaluator API]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/src/libexpr-c/nix_api_expr.h
[C value API]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/src/libexpr-c/nix_api_value.h#L174-L267
[C callback wrapper]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/src/libexpr-c/nix_api_value.cc#L77-L137
[eval-gc.cc]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/src/libexpr/eval-gc.cc#L49-L77
[allocation]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/src/libexpr/include/nix/expr/eval-inline.hh#L15-L108
[C GC implementation]: https://github.com/NixOS/nix/blob/5fb7eb7d64ba7b78819aa3ac6a7fdd21354910fc/src/libexpr-c/nix_api_expr.cc#L219-L293
