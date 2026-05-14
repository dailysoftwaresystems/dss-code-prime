add_library(core OBJECT
    compiler.cpp

    # ── types/ — universal in-memory model ──
    types/source_span.cpp
    types/source_buffer.cpp
    types/rule_id.cpp
    types/tree.cpp
)

target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
