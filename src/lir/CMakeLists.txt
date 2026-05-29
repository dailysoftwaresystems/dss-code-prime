add_library(lir OBJECT
    lir.cpp
    lir_literal_pool.cpp
    lir_liveness.cpp
    lir_regalloc.cpp
    lir_verifier.cpp
)

target_include_directories(lir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:lir>. DSS_BUILDING_DLL
# must live here so DSS_EXPORT decorates with dllexport on MSVC for the .obj
# files produced here.
target_compile_definitions(lir PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

target_link_libraries(lir PRIVATE core)

# ML5 cycle 3: MIR→LIR isel pass lives in `lowering/`; separate OBJECT lib
# so the LIR substrate itself stays MIR-free (the LIR module is target-
# blind output; the lowerer is the dependency-direction bridge).
add_subdirectory(lowering)
