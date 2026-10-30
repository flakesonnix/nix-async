function(add_runtime_tests suite)
    add_executable(runtime_${suite}_test ${suite}_test.cc)
    target_link_libraries(runtime_${suite}_test PRIVATE nix_async_runtime)
    target_compile_options(runtime_${suite}_test PRIVATE -Wall -Wextra)
    foreach(case IN LISTS ARGN)
        add_test(NAME runtime.${suite}.${case} COMMAND runtime_${suite}_test ${case})
        set_tests_properties(runtime.${suite}.${case} PROPERTIES TIMEOUT 20)
    endforeach()
endfunction()

add_runtime_tests(future result waiting errors broken_promise move_ownership
    concurrent_consumers competing_producers void_and_move_only construction_failure)
add_runtime_tests(task initial abandoned empty)
add_runtime_tests(executor execution overlap fifo shutdown destructor_drains exceptions repeated
    concurrent_submission_shutdown capture_lifetime worker_shutdown_during_join)
add_runtime_tests(scheduler lifecycle failed result_failure rejection concurrent_inspection duplicate_submission)
add_runtime_tests(when_all empty ordering already_ready_and_duplicates failure broken_input invalid
    void_inputs no_worker_waiting concurrent_completion lifetime overlap copy_failure nested)
add_test(NAME runtime.example COMMAND nix_async_example)
set_tests_properties(runtime.example PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "42")
