add_library(mir_lowering OBJECT
    hir_to_mir.cpp
)

target_include_directories(mir_lowering PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:mir_lowering> at
# src/CMakeLists.txt — same pattern as hir/lowering. DSS_BUILDING_DLL must
# live here (not on the SHARED target) so DSS_EXPORT decorations resolve to
# dllexport when these .obj files are produced on MSVC.
target_compile_definitions(mir_lowering PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# ML2 consumes HIR + the CU's type interner; targets MIR's builder.
target_link_libraries(mir_lowering PRIVATE core hir hir_lowering mir)

# FC17.9(b) (D-FULLC-STDBIT, audit I4): promote "enumerator not handled in a
# no-`default` switch" to a BUILD ERROR (GCC/Clang `-Werror=switch`; MSVC
# `/we4062`) so a future BuiltinLowering enumerator added without a matching arm
# in the hir_to_mir BuiltinCall switch fails the build instead of silently
# reaching the runtime `unsupported(...)` fail-loud. Mirrors src/core's per-file
# gate on parse_diagnostic.cpp. Only bites NO-`default` switches that miss a
# case; the many subset-with-`default` switches in this TU are unaffected.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set_source_files_properties(hir_to_mir.cpp PROPERTIES
        COMPILE_OPTIONS "-Werror=switch")
elseif(MSVC)
    set_source_files_properties(hir_to_mir.cpp PROPERTIES
        COMPILE_OPTIONS "/we4062")
endif()
