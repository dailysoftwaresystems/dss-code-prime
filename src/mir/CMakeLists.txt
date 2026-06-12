add_library(mir OBJECT
    mir.cpp
    mir_dom.cpp
    mir_literal_pool.cpp
    mir_struct_markers.cpp
    mir_text.cpp
    mir_verifier.cpp
)

target_include_directories(mir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:mir> at
# src/CMakeLists.txt — same pattern as hir/, core/, etc. DSS_BUILDING_DLL must
# live here (not on the SHARED target) so DSS_EXPORT decorations resolve to
# dllexport when these .obj files are produced on MSVC.
target_compile_definitions(mir PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# ML1 sits on the SP1 arena substrate + the core type lattice (TypeId), both in
# `core`. No HIR dependency: HIR→MIR lowering (ML2) lives in its own library and
# links both hir and mir.
target_link_libraries(mir PRIVATE core)
