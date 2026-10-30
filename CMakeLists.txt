cmake_minimum_required(VERSION 3.24)
project(nix-async VERSION 0.0.1 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Phase 0 supports Linux only")
endif()

# Nix's C++ plugin interface is unstable; do not imply cross-version support.
set(NIX_ASYNC_NIX_VERSION "2.34.9")
find_package(PkgConfig REQUIRED)
# Only query compilation flags. FindPkgConfig's pkg_check_modules also queries
# static linking dependencies, which plugins neither need nor should link.
execute_process(
    COMMAND "${PKG_CONFIG_EXECUTABLE}" --cflags "nix-expr = ${NIX_ASYNC_NIX_VERSION}"
    OUTPUT_VARIABLE nix_cflags
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
separate_arguments(nix_cflags UNIX_COMMAND "${nix_cflags}")
find_package(Boost REQUIRED CONFIG)

add_library(nix_async MODULE src/plugin.cc)
set_target_properties(nix_async PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_compile_options(nix_async PRIVATE ${nix_cflags} -Wall -Wextra)
target_compile_definitions(nix_async PRIVATE
    NIX_ASYNC_NIX_VERSION="${NIX_ASYNC_NIX_VERSION}"
)
target_link_libraries(nix_async PRIVATE Boost::headers)
# Deliberately do not link Nix libraries: the plugin-files contract requires
# Nix symbols to resolve from the host process, not a second set of libraries.

include(GNUInstallDirs)
install(TARGETS nix_async LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/nix-async)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/nix-async)

include(CTest)
if(BUILD_TESTING)
    find_program(NIX_EXECUTABLE nix REQUIRED)
    execute_process(
        COMMAND "${NIX_EXECUTABLE}" --version
        OUTPUT_VARIABLE nix_version
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE nix_status
    )
    if(NOT nix_status EQUAL 0 OR
       NOT nix_version STREQUAL "nix (Nix) ${NIX_ASYNC_NIX_VERSION}")
        message(FATAL_ERROR "Tests require Nix ${NIX_ASYNC_NIX_VERSION}; found '${nix_version}'")
    endif()
    add_subdirectory(tests/plugin)
endif()
