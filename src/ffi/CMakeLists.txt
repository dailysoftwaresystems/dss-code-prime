add_subdirectory(abi)
add_subdirectory(mangling)

add_library(ffi OBJECT
    binary_reader.cpp
    c_header_parser.cpp
)

target_include_directories(ffi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

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
)
