add_library(hir OBJECT
    hir.cpp
    hir_cursor.cpp
    hir_intrinsic_registry.cpp
    hir_kind_registry.cpp
    hir_op_registry.cpp
    hir_text.cpp
    hir_verifier.cpp
)

target_include_directories(hir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:hir> at
# src/CMakeLists.txt — same pattern as core/, analysis_semantic/, etc.
# DSS_BUILDING_DLL must live here (not on the SHARED target) so DSS_EXPORT
# decorations resolve to dllexport when these .obj files are produced on MSVC.
target_compile_definitions(hir PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# HR1 sits on the SP1 arena substrate + the core type lattice (TypeId), both in
# `core`. No semantic/CU dependency yet — CST→HIR lowering arrives at HR8.
target_link_libraries(hir PRIVATE core)
