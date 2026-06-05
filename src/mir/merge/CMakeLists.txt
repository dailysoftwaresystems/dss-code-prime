# Cross-module MIR merge (Cycle 25, Stage B). A LEAF library consumed only by the
# driver (Stage C) + its unit tests, so it may depend on BOTH `mir` and `link`
# (the latter owns `resolveCrossCuDefs`) without forming a cycle: `link` already
# depends on `mir`, and nothing in `mir` / `link` / `core` depends back on
# `mir_merge`.
add_library(mir_merge OBJECT
    mir_merge.cpp
)

target_include_directories(mir_merge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# DSS_BUILDING_DLL on the OBJECT lib so DSS_EXPORT decorates with dllexport on
# MSVC for the .obj files produced here (same pattern as mir/ , link/).
target_compile_definitions(mir_merge PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# Depends on core (reinternType), mir (Mir / MirBuilder / MirVerifier), and link
# (resolveCrossCuDefs + LinkedSymbolKey). No back-edge from those into here.
target_link_libraries(mir_merge PRIVATE core mir link)
