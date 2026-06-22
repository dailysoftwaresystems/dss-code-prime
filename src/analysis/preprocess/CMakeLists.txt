add_library(analysis_preprocess OBJECT
    preprocessor.cpp
)

target_include_directories(analysis_preprocess PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:analysis_preprocess>
# at src/CMakeLists.txt -- same pattern as analysis_compilation_unit. The
# DSS_BUILDING_DLL define must live here so DSS_EXPORT resolves to dllexport
# when these .obj files are produced on MSVC.
target_compile_definitions(analysis_preprocess PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# The preprocessor drives the tokenizer over its synthesized buffer and reads
# the schema's preprocess() vocabulary, so it depends on core + tokenizer.
target_link_libraries(analysis_preprocess PRIVATE core tokenizer)
