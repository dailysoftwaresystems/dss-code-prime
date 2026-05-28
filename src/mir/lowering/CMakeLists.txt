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
