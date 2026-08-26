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
add_library(mir_summary OBJECT
    mir_summary.cpp
    summary_index.cpp
)

target_include_directories(mir_summary PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# DSS_BUILDING_DLL on the OBJECT lib so DSS_EXPORT decorates with dllexport on
# MSVC for the .obj files produced here (same pattern as mir/, link/, merge/).
target_compile_definitions(mir_summary PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

target_link_libraries(mir_summary PRIVATE core mir link)
