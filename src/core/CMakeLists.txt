add_library(core OBJECT
    compiler.cpp

    # ── types/ — universal in-memory model ──
    types/source_span.cpp
    types/source_buffer.cpp
    types/rule_id.cpp
    types/scope_kind.cpp
    types/parse_diagnostic.cpp
    types/diagnostic_reporter.cpp
    types/grammar_schema.cpp
    types/grammar_schema_json.cpp
    types/tree.cpp
    types/tree_builder.cpp
)

target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# nlohmann/json is included by exactly one translation unit
# (grammar_schema_json.cpp). PRIVATE keeps the include path off the
# transitive consumers — every other file in `core` (and every consumer
# of `core`) sees just our public headers, never <nlohmann/json.hpp>.
target_link_libraries(core PRIVATE nlohmann_json::nlohmann_json)
