# Each test gets a fresh, bounded evaluator process. No daemon or real store is
# needed. Ignore ambient Nix configuration, including any user-installed plugins.
set(ENV{NIX_CONF_DIR} "${CMAKE_CURRENT_BINARY_DIR}/no-nix-conf")
set(ENV{NIX_USER_CONF_FILES} "/dev/null")
set(ENV{NIX_CONFIG} "")
set(ENV{NIX_PATH} "")
set(ENV{NIX_SHOW_STATS} "0")
set(ENV{NIX_SHOW_STATS_PATH} "-")
if(GC_STRESS)
    # Override Nix's large initial heap so the retained function is exercised
    # across collections, and require the evaluator to report actual GC cycles.
    set(ENV{GC_INITIAL_HEAP_SIZE} "1048576")
    set(ENV{NIX_SHOW_STATS} "1")
endif()
set(command "${NIX_EXECUTABLE}" --extra-experimental-features nix-command)
if(NOT WITHOUT_PLUGIN)
    # plugin-files must precede the subcommand: Nix loads DSOs before eval.
    list(APPEND command --option plugin-files "${PLUGIN}")
endif()
list(APPEND command eval --store dummy:// --json)
if(IMPURE)
    list(APPEND command --impure)
endif()
execute_process(
    COMMAND ${command} --expr "${EXPRESSION}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    TIMEOUT 15
)
if(NOT "${status}" MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Evaluator failed to run or timed out: ${status}\n${error}")
endif()
if(EXPECT_FAILURE)
    if(status EQUAL 0)
        message(FATAL_ERROR "Expected evaluation failure, got: ${output}")
    endif()
    string(FIND "${error}" "${EXPECTED}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "Expected error containing '${EXPECTED}', got:\n${error}")
    endif()
else()
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Evaluation exited ${status}:\n${error}")
    endif()
    if(NOT output STREQUAL EXPECTED)
        message(FATAL_ERROR "Expected '${EXPECTED}', got '${output}'")
    endif()
endif()
if(GC_STRESS)
    string(REGEX MATCH "\"cycles\": *([0-9]+)" cycles "${error}")
    # Nix performs one final collection when printing stats; require more than
    # that to exercise collections during evaluation, not just at process exit.
    if(NOT cycles OR CMAKE_MATCH_1 LESS 2)
        message(FATAL_ERROR "Expected GC cycles during evaluation:\n${error}")
    endif()
endif()
