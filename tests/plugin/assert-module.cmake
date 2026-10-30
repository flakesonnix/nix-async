execute_process(
    COMMAND "${READELF_EXECUTABLE}" --dynamic "${PLUGIN}"
    OUTPUT_VARIABLE dynamic
    COMMAND_ERROR_IS_FATAL ANY
)
if(dynamic MATCHES "NEEDED[^\n]*libnix")
    message(FATAL_ERROR "Plugin must resolve Nix symbols from its host, not link Nix DSOs:\n${dynamic}")
endif()
execute_process(
    COMMAND "${READELF_EXECUTABLE}" --dyn-syms --wide "${PLUGIN}"
    OUTPUT_VARIABLE symbols
    COMMAND_ERROR_IS_FATAL ANY
)
if(NOT symbols MATCHES "FUNC +GLOBAL +DEFAULT +[0-9]+ +nix_plugin_entry")
    message(FATAL_ERROR "Plugin does not export its C entry point")
endif()
