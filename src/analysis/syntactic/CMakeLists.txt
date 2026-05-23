add_library(analysis_syntactic OBJECT
    parser.cpp
)

target_include_directories(analysis_syntactic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:analysis_syntactic>
# at src/CMakeLists.txt — same pattern as core/ and tokenizer/.
# DSS_BUILDING_DLL must live here (not on the SHARED target) so
# DSS_EXPORT decorations resolve to dllexport when these .obj files
# are produced on MSVC.
target_compile_definitions(analysis_syntactic PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# Parser depends only on `core`'s public surface (Tree/TreeBuilder/
# GrammarSchema/SchemaWalker) and `tokenizer`'s TokenStream.
target_link_libraries(analysis_syntactic PRIVATE core tokenizer)
