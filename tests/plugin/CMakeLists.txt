function(add_eval_test name expression expected)
    add_test(NAME "plugin.${name}"
        COMMAND "${CMAKE_COMMAND}"
            "-DNIX_EXECUTABLE=${NIX_EXECUTABLE}"
            "-DPLUGIN=$<TARGET_FILE:nix_async>"
            "-DEXPRESSION=${expression}"
            "-DEXPECTED=${expected}"
            ${ARGN}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/assert-eval.cmake"
    )
    set_tests_properties("plugin.${name}" PROPERTIES TIMEOUT 20)
endfunction()

add_eval_test(namespace "builtins.attrNames builtins.async" "[\"probe\"]")
add_eval_test(probe "builtins.async.probe 42" "42")
add_eval_test(forces_thunk "builtins.async.probe (20 + 22)" "42")
add_eval_test(negative "builtins.async.probe (-42)" "-42")
add_eval_test(integer_max "builtins.async.probe 9223372036854775807" "9223372036854775807")
add_eval_test(integer_min "builtins.async.probe (-9223372036854775807 - 1)" "-9223372036854775808")
add_eval_test(lazy_unused "let unused = builtins.async.probe (throw \"must stay lazy\"); in 42" "42")
add_eval_test(no_global_probe "builtins ? probe || builtins ? \"async.probe\" || builtins ? __async" "false")
add_eval_test(no_runtime_claim "builtins.async ? sleep || builtins.async ? spawn || builtins.async ? await" "false")
add_eval_test(type_error "builtins.async.probe \"wrong\"" "expected an integer" -DEXPECT_FAILURE=ON)
add_eval_test(thunk_error "builtins.async.probe (throw \"probe argument failed\")" "probe argument failed" -DEXPECT_FAILURE=ON)
add_eval_test(recoverable_error "(builtins.tryEval (builtins.async.probe (throw \"expected\"))).success" "false")
add_eval_test(retained_function
    "let probe = builtins.async.probe; in builtins.foldl' (sum: i: sum + probe i) 0 (builtins.genList (i: i) 30000)"
    "449985000" -DGC_STRESS=ON)
add_eval_test(example "import ${PROJECT_SOURCE_DIR}/examples/basic.nix" "42" -DIMPURE=ON)
add_eval_test(not_loaded "builtins ? async" "false" -DWITHOUT_PLUGIN=ON)

find_program(READELF_EXECUTABLE readelf REQUIRED)
add_test(NAME plugin.module_contract
    COMMAND "${CMAKE_COMMAND}"
        "-DREADELF_EXECUTABLE=${READELF_EXECUTABLE}"
        "-DPLUGIN=$<TARGET_FILE:nix_async>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/assert-module.cmake"
)
set_tests_properties(plugin.module_contract PROPERTIES TIMEOUT 20)
