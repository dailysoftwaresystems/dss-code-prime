add_library(tokenizer OBJECT
    source_reader.cpp
    tokenizer.cpp
    token_stream.cpp
)

target_include_directories(tokenizer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:tokenizer> at
# src/CMakeLists.txt — same pattern as core/. DSS_BUILDING_DLL must live
# here (not on the SHARED target) so DSS_EXPORT decorations resolve to
# dllexport when these .obj files are produced on MSVC.
target_compile_definitions(tokenizer PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# tokenizer depends only on `core`'s public surface (GrammarSchema,
# SourceBuffer, Token, DiagnosticReporter). No JSON, no GoogleTest.
target_link_libraries(tokenizer PRIVATE core)
