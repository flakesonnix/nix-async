# Phase 0 only: verifies the native plugin, not asynchronous execution.
let
  async = builtins.async;
in
async.probe (20 + 22)
