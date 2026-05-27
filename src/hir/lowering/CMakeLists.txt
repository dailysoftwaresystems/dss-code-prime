add_library(hir_lowering OBJECT
    cst_to_hir.cpp
    hir_literal_pool.cpp
)

target_include_directories(hir_lowering PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# DSS_BUILDING_DLL must live here (not on the SHARED target) so DSS_EXPORT
# decorations resolve to dllexport when these .obj files are produced on MSVC.
target_compile_definitions(hir_lowering PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# The lowering engine is the one HIR component that consumes the front-end:
# `hir` (HirBuilder / Hir / HirVerifier), `analysis_semantic` (SemanticModel),
# and `analysis_compilation_unit` (CompilationUnit / schema). This dependency
# chain is acyclic — semantic / CU never include `hir`. Keeping it in a SEPARATE
# object library (not folded into `hir`) preserves `hir`'s core-only footprint
# for the HR1–HR7 substrate.
target_link_libraries(hir_lowering PRIVATE core hir analysis_semantic analysis_compilation_unit)
