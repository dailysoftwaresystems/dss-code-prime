add_library(lir OBJECT
    lir.cpp
)

target_include_directories(lir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:lir>. DSS_BUILDING_DLL
# must live here so DSS_EXPORT decorates with dllexport on MSVC for the .obj
# files produced here.
target_compile_definitions(lir PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# ML5 cycle 1 sits on the SP1 arena substrate (core). No MIR dependency yet
# — the MIR→LIR isel pass (ML5 cycle 2) lives in its own library and links
# both mir and lir.
target_link_libraries(lir PRIVATE core)
