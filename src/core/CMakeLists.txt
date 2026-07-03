add_library(core OBJECT
    compiler.cpp

    # ── substrate/ — host/OS utilities (the first non-header-only one) ──
    substrate/large_stack_call.cpp
    substrate/phase_timers.cpp

    # ── types/ — universal in-memory model ──
    types/source_span.cpp
    types/source_buffer.cpp
    types/rule_id.cpp
    types/scope_kind.cpp
    types/parse_diagnostic.cpp
    types/diagnostic_reporter.cpp
    types/unsuppressable_codes.cpp
    types/config_path_walk.cpp
    types/include_path_resolve.cpp
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
    types/type_lattice/type_layout.cpp
    types/type_lattice/type_reintern.cpp
    types/aggregate_abi.cpp

    # ── crypto/ — self-contained hashing primitives (codesign foundation) ──
    crypto/sha256.cpp
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

# Big-object assembler flag for the large auto-generated grammar-schema TU.
# grammar_schema_json.cpp is a big generated translation unit that, under the
# MinGW/binutils toolchain, overflows the WINDOWS COFF object-section count on a
# from-scratch build ("file too big"). `-Wa,-mbig-obj` is the standard binutils
# remedy (it raises the COFF section limit). CRUCIAL: `-mbig-obj` is a COFF-ONLY
# assembler option — the Linux/macOS GNU `as` REJECTS it ("unrecognized option
# -mbig-obj"). GNU-on-Linux (ELF) is the SAME CXX_COMPILER_ID:GNU as MinGW, so a
# bare GNU scope wrongly breaks the Linux-GCC legs. The flag is therefore scoped
# THREE ways: (1) per-FILE via set_source_files_properties — only this one TU,
# every other `core` source byte-identical; (2) GNU AND (3) WINDOWS, via the
# $<$<AND:$<CXX_COMPILER_ID:GNU>,$<PLATFORM_ID:Windows>>:...> generator expression —
# so it is applied ONLY to a MinGW/GNU-on-Windows (COFF) build and expands to
# NOTHING for MSVC, Clang, and Linux/macOS GCC, whose compile lines provably cannot
# change. A toolchain-object-size accommodation, not a language/CPU/format branch.
set_source_files_properties(types/grammar_schema_json.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<AND:$<CXX_COMPILER_ID:GNU>,$<PLATFORM_ID:Windows>>:-Wa,-mbig-obj>")

# D-DIAG-CODENAME-EXHAUSTIVE-WARN (2026-06-09): enforce EXHAUSTIVE
# DiagnosticCode / DiagnosticSeverity dispatch in parse_diagnostic.cpp. Both
# switches (`diagnosticCodeName`, `severityName`) are written with NO
# `default:` arm — one explicit case per enumerator + a post-switch
# out-of-range backstop `return` — so the "no-default + missing case = error"
# flavor (GCC/Clang `-Werror=switch`; MSVC `/we4062`) turns a future
# DiagnosticCode enumerator added without a matching name arm into a BUILD
# ERROR, on local MSVC too. Closes the gap this gate was born from: 12 codes
# (the X_* optimizer/pipeline family + A_FunctionEncodeAborted /
# K_AbsolutePointerRelocMissing / S_NotAComposite / S_NotAPointer) had drifted
# unhandled, surfacing only as a `-Wswitch` warning on the macOS / linux-clang
# CI legs and INVISIBLE on local MSVC. Mirrors the sibling gates in
# src/asm/CMakeLists.txt (D-AS-ENCODINGSLOT-EXHAUSTIVE-WARN) and
# src/opt/CMakeLists.txt (D-OPT-RUNPASS-EXHAUSTIVE-WARN). Per-file scope is
# load-bearing: parse_diagnostic.cpp's only two switches are both no-default
# exhaustive; do NOT spread this to grammar_schema.cpp / target_schema.cpp etc.,
# whose switches legitimately use subset-with-`default` forms.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set_source_files_properties(types/parse_diagnostic.cpp PROPERTIES
        COMPILE_OPTIONS "-Werror=switch")
elseif(MSVC)
    set_source_files_properties(types/parse_diagnostic.cpp PROPERTIES
        COMPILE_OPTIONS "/we4062")
endif()
