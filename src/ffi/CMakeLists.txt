add_subdirectory(abi)
add_subdirectory(mangling)

add_library(ffi OBJECT
    binary_reader.cpp
    binary_readers/elf_reader.cpp
    binary_readers/macho_reader.cpp
    binary_readers/pe_reader.cpp
    c_header_parser.cpp
    ingest.cpp
)

target_include_directories(ffi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# The LANGUAGE-NEUTRAL shipped-lib descriptor reader (neutral shipped-lib
# descriptor, 2026-06-06) lives in a SEPARATE OBJECT lib because it is consumed
# by `hir_lowering` (`cst_to_hir.cpp` synthesizes the shipped externs at the
# lowering stage). The main `ffi` lib links `hir_lowering` (for
# `c_header_parser.cpp`'s frontend orchestration), so folding the reader into
# `ffi` would form an `ffi`↔`hir_lowering` link cycle. The reader is
# frontend-free — it depends only on `core` (diagnostics + TypeId) and `hir`
# (the one `parseTypeFromText` type decoder) — so `hir_lowering -> ffi_shipped`
# is acyclic. Keeping it under `src/ffi/` (namespace `dss::ffi`) keeps the FFI
# surface cohesive; only the CMake target placement differs.
add_library(ffi_shipped OBJECT
    shipped_lib_descriptor.cpp
)

target_include_directories(ffi_shipped PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_compile_definitions(ffi_shipped PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# nlohmann_json is linked DIRECTLY (not via core, which keeps it PRIVATE) —
# the descriptor reader parses JSON, mirroring `opt`'s optimizer_json.cpp.
target_link_libraries(ffi_shipped PRIVATE core hir nlohmann_json::nlohmann_json)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:ffi>.
target_compile_definitions(ffi PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# Declared deps reflect actual include surface: c_header_parser.cpp
# orchestrates the c-subset frontend (analysis_compilation_unit +
# analysis_semantic) into HIR (hir + hir_lowering). Documented per
# architect Item A post-FF2 fold — every OBJECT lib in this repo
# already PUBLIC-exposes its includes via target_include_directories,
# so undeclared deps compile by accident; declaring keeps the
# dependency graph honest against future refactors.
target_link_libraries(ffi PRIVATE
    core
    analysis_compilation_unit
    analysis_semantic
    hir
    hir_lowering
    ffi_abi
    ffi_mangling
    link
)
