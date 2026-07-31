add_library(link OBJECT
    cross_cu_resolve.cpp
    entry_trampoline.cpp
    image_request.cpp
    linker.cpp
    object_format_schema.cpp
    object_format_schema_json.cpp
    format/ar.cpp
    format/elf.cpp
    format/elf_object_reader.cpp
    format/pe.cpp
    format/coff_object_reader.cpp
    format/macho.cpp
    format/macho_object_reader.cpp
    format/macho_chained_fixups.cpp
    format/macho_codesign.cpp
    format/spirv.cpp
    format/wasm.cpp
    writer.cpp
)

target_include_directories(link PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:link>. DSS_BUILDING_DLL
# must live here so DSS_EXPORT decorates with dllexport on MSVC for the .obj
# files produced here.
target_compile_definitions(link PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# TF-C97 — the same repo-root VERSION forward `core` does, for the same header.
# `object_format_schema_json.cpp` now includes `predefined_macro_json.hpp` (the
# ONE shared `predefinedMacros` grammar, third consumer after the language and
# target loaders), and that header hard-`#error`s without DSS_PROJECT_VERSION.
# `DSS_VERSION` is the stripped repo-root VERSION already read by the top-level
# CMakeLists — the SAME variable `core` uses, so the version is still written in
# exactly one place. A botched wireup fails the BUILD, never silently.
target_compile_definitions(link PRIVATE DSS_PROJECT_VERSION="${DSS_VERSION}")

target_link_libraries(link PRIVATE core asm lir mir nlohmann_json::nlohmann_json)
