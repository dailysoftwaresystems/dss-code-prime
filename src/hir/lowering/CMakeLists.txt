add_library(hir_lowering OBJECT
    cst_to_hir.cpp
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

# MSVC: cst_to_hir.cpp's translation unit exceeds the default COFF object
# section limit (C1128) in Debug builds — the per-schema Lowerer accreted enough
# inlined lowering arms (now incl. the D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL
# mutability thread) to cross the cap. /bigobj lifts the section-count format
# limit only — zero codegen/behavior change (same fix as `program`).
if(MSVC)
    target_compile_options(hir_lowering PRIVATE /bigobj)
endif()
