# OPT11 — the cross-CU SUMMARY INDEX (plan 22 §0.2).
#
# A LEAF library, exactly like `mir_merge` beside it and for the same reason:
# it is consumed only by the driver and its unit tests, so it may depend on
# BOTH `mir` and `link` (the latter owns `resolveCrossCuDefs`, the single
# source of truth for cross-CU winner selection that the index and the merge
# MUST share) without forming a cycle — `link` already depends on `mir`, and
# nothing in `mir` / `link` / `core` depends back on this.
#
# ★ NO DEPENDENCY ON `opt` OR ON `program`, deliberately. The per-TU summary is
# a pure function of one module's MIR, and the global pass reads only
# summaries — neither needs the optimizer, and reaching up into the driver for
# `RuntimeObjectKey` would couple a MIR-tier leaf to the build system. The
# Tier-1 digest is therefore carried as an OPAQUE STRING the caller supplies.
# ★ AND STILL NO DEPENDENCY ON `opt` AFTER THE LAZY IMPORT EDGE LANDED
# (D-OPT11-LAZY-IMPORT-EDGE). `lazy_import_optimize.cpp` runs the optimizer, but
# it reaches it through a CALLBACK the driver supplies, so this library still
# does not know what a pass, a pipeline or a compile stage is — and a MIR-tier
# unit test can drive the whole import fixpoint with a hand-written no-op
# optimizer. It does gain `mir_merge`, which IS the subset FunctionCloner: the
# `MergeCuInput::importOnly` filter turns the whole-module clone into a subset
# one rather than standing a second opcode-by-opcode clone switch beside it.
add_library(mir_summary OBJECT
    mir_summary.cpp
    summary_index.cpp
    lazy_import_optimize.cpp
    mir_body_codec.cpp
)

target_include_directories(mir_summary PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# DSS_BUILDING_DLL on the OBJECT lib so DSS_EXPORT decorates with dllexport on
# MSVC for the .obj files produced here (same pattern as mir/, link/, merge/).
target_compile_definitions(mir_summary PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

target_link_libraries(mir_summary PRIVATE core mir link mir_merge)
