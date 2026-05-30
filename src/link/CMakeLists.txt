add_library(link OBJECT
    linker.cpp
    object_format_schema.cpp
    object_format_schema_json.cpp
    format/elf.cpp
    format/pe.cpp
    format/macho.cpp
    format/spirv.cpp
    format/wasm.cpp
    writer.cpp
)

target_include_directories(link PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:link>. DSS_BUILDING_DLL
# must live here so DSS_EXPORT decorates with dllexport on MSVC for the .obj
# files produced here.
target_compile_definitions(link PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

target_link_libraries(link PRIVATE core asm lir mir nlohmann_json::nlohmann_json)
