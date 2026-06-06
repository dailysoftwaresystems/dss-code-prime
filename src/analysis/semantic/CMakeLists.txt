add_library(analysis_semantic OBJECT
    scope_tree.cpp
    semantic_analyzer.cpp
    semantic_model.cpp
    symbol_table.cpp
)

target_include_directories(analysis_semantic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:analysis_semantic>
# at src/CMakeLists.txt — same pattern as core/, tokenizer/,
# analysis_compilation_unit/. DSS_BUILDING_DLL must live here (not on the
# SHARED target) so DSS_EXPORT decorations resolve to dllexport when
# these .obj files are produced on MSVC.
target_compile_definitions(analysis_semantic PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# The analyzer consumes the CompilationUnit + the schema's SemanticConfig
# + the type lattice, all of which live in `core` and
# `analysis_compilation_unit`. No parser dependency — Pass 1/2 read the
# parsed Tree only.
#
# ffi_shipped (FF11 neutral-JSON descriptor reader): the FF11 shipped-library
# injection (semantic_analyzer.cpp) reads a resolved descriptor via
# `readShippedLibDescriptor` to mint its extern symbols before Pass 2. The
# reader is frontend-free (depends only on core + hir's parseTypeFromText), so
# analysis_semantic -> ffi_shipped is acyclic (ffi_shipped never reads the
# SemanticModel). Declared to keep the dependency graph honest against the
# include surface (this repo's OBJECT-lib discipline).
target_link_libraries(analysis_semantic PRIVATE core analysis_compilation_unit ffi_shipped)
