add_library(mir_lowering OBJECT
    hir_to_mir.cpp
)

target_include_directories(mir_lowering PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Aggregated into dsscp-lib via $<TARGET_OBJECTS:mir_lowering> at
# src/CMakeLists.txt — same pattern as hir/lowering. DSS_BUILDING_DLL must
# live here (not on the SHARED target) so DSS_EXPORT decorations resolve to
# dllexport when these .obj files are produced on MSVC.
target_compile_definitions(mir_lowering PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# ML2 consumes HIR + the CU's type interner; targets MIR's builder.
target_link_libraries(mir_lowering PRIVATE core hir hir_lowering mir)

# FC17.9(b) (D-FULLC-STDBIT, audit I4) — the "enumerator not handled in a
# no-`default` switch = BUILD ERROR" gate that used to be scoped to
# hir_to_mir.cpp here is now PROJECT-WIDE (root CMakeLists.txt). The
# BuiltinCall switch over BuiltinLowering is still covered: a new enumerator
# added without a matching arm still fails the build rather than silently
# reaching the runtime `unsupported(...)` fail-loud. As before it bites only
# no-`default` switches, so this TU's many subset-with-`default` switches are
# unaffected. Consolidated under
# D-BUILD-CLANG-ONLY-WARNINGS-INVISIBLE-TO-THE-MSVC-AND-GCC-LEGS, which
# measured what five hand-listed copies of one gate had cost.
