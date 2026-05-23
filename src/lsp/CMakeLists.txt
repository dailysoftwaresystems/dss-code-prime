add_library(lsp OBJECT
    json_rpc.cpp
    protocol.cpp
    utf16_column.cpp
)

target_include_directories(lsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Aggregated into dss-code-prime-lib via $<TARGET_OBJECTS:lsp> at
# src/CMakeLists.txt — same pattern as core/ and tokenizer/.
# DSS_BUILDING_DLL must live here (not on the SHARED target) so
# DSS_EXPORT decorations resolve to dllexport when these .obj files
# are produced on MSVC.
target_compile_definitions(lsp PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# nlohmann/json is PRIVATE — kept out of every lsp/*.hpp per the
# project convention (mirrors core/grammar_schema_json.cpp's
# isolation). Linked in only because the json_rpc.cpp TU needs
# json::parse/dump.
target_link_libraries(lsp
    PRIVATE
        core
        tokenizer
        analysis_syntactic
        nlohmann_json::nlohmann_json
)
