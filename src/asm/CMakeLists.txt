add_library(asm OBJECT
    asm.cpp
    format/fixed32.cpp
    format/x86_variable.cpp
)

target_include_directories(asm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:asm>. DSS_BUILDING_DLL
# must live here so DSS_EXPORT decorates with dllexport on MSVC for the .obj
# files produced here.
target_compile_definitions(asm PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

target_link_libraries(asm PRIVATE core lir mir)
