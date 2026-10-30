#include "nix/expr/eval-inline.hh"
#include "nix/expr/primops.hh"
#include "nix/store/globals.hh"

namespace {

// An integer identity operation: only an ABI/forcing/error-propagation probe.
// This is intentionally synchronous and does not claim to implement async work.
void prim_probe(nix::EvalState & state, const nix::PosIdx pos, nix::Value ** args, nix::Value & result)
{
    result.mkInt(state.forceInt(*args[0], pos, "while evaluating the argument to builtins.async.probe"));
}

void prim_async(nix::EvalState & state, const nix::PosIdx, nix::Value **, nix::Value & result)
{
    // Process-lifetime metadata contains no evaluator-owned pointers. Actual
    // values and bindings are allocated separately in each evaluator's memory.
    static nix::PrimOp probe{
        .name = "async.probe",
        .args = {"value"},
        .arity = 1,
        .doc = "Return an integer unchanged. Synchronous nix-async Phase 0 probe.",
        .impl = prim_probe,
        .experimentalFeature = {},
    };

    auto attrs = state.buildBindings(1);
    attrs.alloc("probe").mkPrimOp(&probe);
    result.mkAttrs(attrs);
}

} // namespace

extern "C" void nix_plugin_entry()
{
    // Diagnostic guard only: matching version strings cannot prove ABI
    // compatibility with differently patched/configured Nix builds.
    if (nix::nixVersion != NIX_ASYNC_NIX_VERSION)
        throw nix::Error("nix-async requires Nix %s; host reports %s", NIX_ASYNC_NIX_VERSION, nix::nixVersion);

    // A zero-arity registration becomes a lazy constant in Nix 2.34.9.
    // The '__' prefix leaves only 'async' visible in the builtins attribute set.
    static nix::RegisterPrimOp registration({
        .name = "__async",
        .args = {},
        .arity = 0,
        .doc = "Experimental nix-async namespace. Phase 0: synchronous probe only.",
        .impl = prim_async,
        .experimentalFeature = {},
    });
}
