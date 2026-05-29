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
    types/target_schema.cpp
    types/target_schema_json.cpp
    types/lexer_mode.cpp
    types/schema_walker.cpp
    types/string_style.cpp
    types/tree.cpp
    types/tree_builder.cpp
    types/tree_cursor.cpp
    types/type_lattice/type_lattice.cpp
)

target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# These OBJECT-lib sources are the actual compilation units that get folded
# into dss-code-prime-lib (SHARED) via $<TARGET_OBJECTS:core>. Setting the
# defines on the SHARED target would not reach them — $<TARGET_OBJECTS> only
# copies finished .o files, not usage requirements — so DSS_BUILDING_DLL has
# to live here for __declspec(dllexport) in core/export.hpp to fire on MSVC.
target_compile_definitions(core PRIVATE DSS_SHARED_BUILD DSS_BUILDING_DLL)

# nlohmann/json is included by exactly one translation unit
# (grammar_schema_json.cpp). PRIVATE keeps the include path off the
# transitive consumers — every other file in `core` (and every consumer
# of `core`) sees just our public headers, never <nlohmann/json.hpp>.
target_link_libraries(core PRIVATE nlohmann_json::nlohmann_json)
