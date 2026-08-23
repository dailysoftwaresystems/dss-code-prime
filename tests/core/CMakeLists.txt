# core/ unit tests. Each test lands as a separate executable so ctest can
# parallelize and pinpoint failures.

dss_add_test(NAME core/test_strong_ids          SOURCES test_strong_ids.cpp)
dss_add_test(NAME core/test_source_span         SOURCES test_source_span.cpp)
dss_add_test(NAME core/test_source_buffer       SOURCES test_source_buffer.cpp)
dss_add_test(NAME core/test_token               SOURCES test_token.cpp)
dss_add_test(NAME core/test_rule_id             SOURCES test_rule_id.cpp)
dss_add_test(NAME core/test_parse_diagnostic    SOURCES test_parse_diagnostic.cpp)
dss_add_test(NAME core/test_config_path_walk    SOURCES test_config_path_walk.cpp)
# The ONE path-identity chokepoint (D-PATH-IDENTITY-8DOT3-SPELLINGS-SPLIT-KEYS).
# Its 8.3 arms CONSTRUCT the hazard and SKIP LOUDLY where the volume cannot
# stage it -- a test that merely hopes to meet a short name is a false green.
dss_add_test(NAME core/test_path_identity       SOURCES test_path_identity.cpp)
dss_add_test(NAME core/test_char_decode         SOURCES test_char_decode.cpp)
dss_add_test(NAME core/test_diagnostic_reporter SOURCES test_diagnostic_reporter.cpp)
dss_add_test(NAME core/test_diagnostic_budget_threading SOURCES test_diagnostic_budget_threading.cpp)
dss_add_test(NAME core/test_unsuppressable_codes SOURCES test_unsuppressable_codes.cpp)
dss_add_test(NAME core/test_header_name_matching SOURCES test_header_name_matching.cpp)
dss_add_test(NAME core/test_grammar_schema      SOURCES test_grammar_schema.cpp)
# Plan 15 CFI slice (D-UNWIND-NO-EH-FRAME-ANY-LANGUAGE-ON-ELF-OR-MACHO): the
# shared PC-keyed unwind representation -- per-PC state, the remember/restore
# stack, and the malformed-stream refusals every format writer relies on.
dss_add_test(NAME core/test_cfi                 SOURCES test_cfi.cpp)
dss_add_test(NAME core/test_target_schema       SOURCES test_target_schema.cpp)
# D-CONFIG-TARGET-LOADER-CONTAINER-KEYS-UNGATED: the container key-gate pins
# mutate the SHIPPED target JSON in memory (nlohmann consumer) — injecting a
# typo key at each gated container and proving the load fails, and injecting a
# `$`-prefixed prose key and proving it does not.
target_link_libraries(dss_core_test_target_schema PRIVATE nlohmann_json::nlohmann_json)
# Two `.target.json` loader gates that close the same shape — a config that
# loads clean and then does nothing:
#   * D-UNWIND-NO-EH-FRAME-…: the psABI DWARF numbering must arrive in BOTH
#     halves, and every register a calling convention names as frame-relevant
#     must be numbered (derived from what the target already declares).
#   * D-CONFIG-VALISTLAYOUT-INERT-CROSS-STRATEGY-KEY: `vaListLayout`'s key set
#     is PER-STRATEGY, so a correctly-spelled key from a different strategy is
#     rejected with a message naming both sides.
# Mutates the SHIPPED JSON in memory (nlohmann consumer), never a parallel
# hand-written file that could drift from the real config.
dss_add_test(NAME core/test_target_dwarf_and_valist_gates
             SOURCES test_target_dwarf_and_valist_gates.cpp)
target_link_libraries(dss_core_test_target_dwarf_and_valist_gates
                      PRIVATE nlohmann_json::nlohmann_json)
dss_add_test(NAME core/test_target_subregister_validation SOURCES test_target_subregister_validation.cpp)
dss_add_test(NAME core/test_number_decode       SOURCES test_number_decode.cpp)
dss_add_test(NAME core/test_bit_int_value       SOURCES test_bit_int_value.cpp)
dss_add_test(NAME core/test_wide_float_value    SOURCES test_wide_float_value.cpp)
dss_add_test(NAME core/test_tree_builder        SOURCES test_tree_builder.cpp)
dss_add_test(NAME core/test_tree_cursor         SOURCES test_tree_cursor.cpp)
dss_add_test(NAME core/test_tree_node           SOURCES test_tree_node.cpp)
dss_add_test(NAME core/test_tree                SOURCES test_tree.cpp)
dss_add_test(NAME core/test_decl_prefix_strip   SOURCES test_decl_prefix_strip.cpp)
dss_add_test(NAME core/test_tree_visitor        SOURCES test_tree_visitor.cpp)
dss_add_test(NAME core/test_tree_attrs          SOURCES test_tree_attrs.cpp)
dss_add_test(NAME core/test_tree_end_to_end     SOURCES test_tree_end_to_end.cpp)
dss_add_test(NAME core/test_c_subset            SOURCES test_c_subset.cpp)
dss_add_test(NAME core/test_operator_table      SOURCES test_operator_table.cpp)
dss_add_test(NAME core/test_schema_cursor       SOURCES test_schema_cursor.cpp)
dss_add_test(NAME core/test_schema_walker       SOURCES test_schema_walker.cpp)
dss_add_test(NAME core/test_contextual_keywords SOURCES test_contextual_keywords.cpp)
dss_add_test(NAME core/test_scope_require       SOURCES test_scope_require.cpp)
dss_add_test(NAME core/test_checkpoint          SOURCES test_checkpoint.cpp)
dss_add_test(NAME core/test_lexer_modes         SOURCES test_lexer_modes.cpp)
dss_add_test(NAME core/test_string_style        SOURCES test_string_style.cpp)
dss_add_test(NAME core/test_tsql_subset         SOURCES test_tsql_subset.cpp)
dss_add_test(NAME core/test_sha256              SOURCES test_sha256.cpp)

# Cross-language `languageReferences` reuse (plan 29 P1+P2). Loads a SYNTHETIC
# host schema built inside the test and reaches the shipped
# `sources/asm.lang.json` through it, so the "asm language is REUSED, zero new
# grammar rules" property has a standing guard that does not depend on
# c-subset — its only real consumer today. Links nlohmann_json because the test
# reads its OWN host document back to prove the host declares none of the rules
# it inherits (a hand-copied list would rot into a tautology).
dss_add_test(NAME core/test_language_references  SOURCES test_language_references.cpp)
target_link_libraries(dss_core_test_language_references PRIVATE nlohmann_json::nlohmann_json)

# D-CONFIG-UNKNOWN-KEY-CHECK-HAND-ROLLED-SITES-REMAIN: every closed-key block in
# the grammar + predefined-macro loaders now routes through the ONE shared
# `dss::detail::rejectUnknownKeys`. The pins MUTATE THE SHIPPED `.lang.json` in
# memory (nlohmann consumer) at a named JSON pointer per block — a stale pointer
# THROWS rather than passing vacuously — and assert three things per block: the
# unmutated document loads, an unknown key is REFUSED by a diagnostic naming that
# block, and a `$…Comment` prose key stays exempt. Plus `impliedSurface`'s
# per-ARM vocabulary, where a sibling-arm key must be refused with the arm it
# belongs to NAMED.
# ⚠ MUST run through ctest, never a bare .exe: `findShippedConfig` walks the cwd
# unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets it.
dss_add_test(NAME core/test_config_closed_key_vocabulary
             SOURCES test_config_closed_key_vocabulary.cpp)
target_link_libraries(dss_core_test_config_closed_key_vocabulary
                      PRIVATE nlohmann_json::nlohmann_json)

# D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET: the sibling class
# one level up — a site whose key (or value) must name an ENUM, whose not-found
# arm IS the report, and whose message used to state the accepted set as a
# literal typed beside the check. Every such message now renders
# `dss::detail::renderAllowedList` over the `EnumNameTable` projection, so the
# message cannot disagree with the check.
# The pins assert three things per site, because no single one carries the claim:
# the shipped corpus really USES the vocabulary (else deleting a table row reds
# nothing), every spelling the message advertises LOADS, and the message names
# every table spelling and no other token.
# ✔The class was NOT merely a hazard: three shipped messages had already drifted
# (operandKinds 3-of-7, resultSlot 8-of-32, terminatorKind 6-of-7).
# ⚠ MUST run through ctest, never a bare .exe: `findShippedConfig` walks the cwd
# unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets it.
dss_add_test(NAME core/test_config_enum_vocabulary_projection
             SOURCES test_config_enum_vocabulary_projection.cpp)
target_link_libraries(dss_core_test_config_enum_vocabulary_projection
                      PRIVATE nlohmann_json::nlohmann_json)

# D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET, the `.target.json`
# loader's half plus the shared predefined-macro parser all three config families
# inherit. Every refusal renders `dss::detail::renderAllowedList` over the
# vocabulary's own table (or its selectable projection), and these pins assert
# the message names EVERY spelling the check accepts and NO other token --
# driven from the table, so adding a row without touching the message reds here.
# The SHAPE sentences (`must be an object { ... }`) are pinned the same way:
# a key list is a closed set and drifts identically.
# The mutations edit the SHIPPED target JSON in memory (nlohmann consumer), never
# a parallel hand-written document that could rot against the real config.
# FOUR of its subjects had ALREADY drifted when the pins were written:
# bitFieldStrategy 1-of-3, implicitRegisters keys 3-of-5, aggregateLayout keys
# 2-of-3, availableObjectFormats 3-of-5 -- plus two refusals that named no set.
# MUST run through ctest, never a bare .exe: `findShippedConfig` walks the cwd
# unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets it.
dss_add_test(NAME core/test_target_vocabulary_projection
             SOURCES test_target_vocabulary_projection.cpp)
target_link_libraries(dss_core_test_target_vocabulary_projection
                      PRIVATE nlohmann_json::nlohmann_json)

# Generalized arena substrate (SP1). The test sources live under substrate/
# and include the shared `substrate/arena_test_types.hpp`, so this directory
# is on their include path.
dss_add_test(NAME core/substrate/test_arena_tag       SOURCES substrate/test_arena_tag.cpp)
dss_add_test(NAME core/substrate/test_arena_container SOURCES substrate/test_arena_container.cpp)
dss_add_test(NAME core/substrate/test_arena_attribute SOURCES substrate/test_arena_attribute.cpp)
dss_add_test(NAME core/substrate/test_arena_concepts  SOURCES substrate/test_arena_concepts.cpp)
dss_add_test(NAME core/substrate/test_mint_monotonic_id SOURCES substrate/test_mint_monotonic_id.cpp)
dss_add_test(NAME core/substrate/test_large_stack_call  SOURCES substrate/test_large_stack_call.cpp)
dss_add_test(NAME core/substrate/test_relocation_table  SOURCES substrate/test_relocation_table.cpp)
target_link_libraries(dss_core_substrate_test_relocation_table PRIVATE nlohmann_json::nlohmann_json)
# The process-spawn substrate (src/core/substrate/process_spawn.{hpp,cpp}) — the
# compiler's FIRST process creation, and the home of the "NO SHELL, EVER"
# contract every `{"run": [...]}` build step and every `git` invocation rides on.
# ★ It defines its own `main` (the test executable IS the spawn fixture: it
# re-invokes itself with `--dss-spawn-fixture` and records its own cwd + argv),
# which is why it must stay a SEPARATE target from its siblings above — the same
# reason tests/test_support/test_run_binary_capture.cpp is one.
dss_add_test(NAME core/substrate/test_process_spawn     SOURCES substrate/test_process_spawn.cpp)
if(WIN32)
    # `CommandLineToArgvW` (<shellapi.h>, shell32) is the ROUND-TRIP ORACLE for
    # buildWindowsCommandLine: the test parses its own output with the very
    # parser the quoting algorithm targets, so a hand-computed expectation is
    # not the only thing standing behind the pins. MSVC does not auto-link
    # shell32 and MinGW's default set is a toolchain detail, so name it.
    target_link_libraries(dss_core_substrate_test_process_spawn PRIVATE shell32)
endif()
target_include_directories(dss_core_substrate_test_arena_tag       PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(dss_core_substrate_test_arena_container PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(dss_core_substrate_test_arena_attribute PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(dss_core_substrate_test_arena_concepts  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Core type lattice + extension registry (SP2).
dss_add_test(NAME core/type_lattice/test_core_type     SOURCES type_lattice/test_core_type.cpp)
dss_add_test(NAME core/type_lattice/test_type_interner SOURCES type_lattice/test_type_interner.cpp)
dss_add_test(NAME core/type_lattice/test_type_registry SOURCES type_lattice/test_type_registry.cpp)
dss_add_test(NAME core/type_lattice/test_type_reintern SOURCES type_lattice/test_type_reintern.cpp)
dss_add_test(NAME core/test_type_layout                SOURCES test_type_layout.cpp)
dss_add_test(NAME core/test_aggregate_abi              SOURCES test_aggregate_abi.cpp)
# D-CSUBSET-BITFIELD-ABI-EXACT: the CI cross-compile-COMPARE witness — builds the
# struct battery with the host's NATIVE C compiler (cl.exe / gcc / clang) and
# asserts dss's computeLayout (host-ABI strategy) is byte-identical. Skips when no
# toolchain. (The macOS leg is the only runtime witness for Apple-arm64.)
dss_add_test(NAME core/test_bitfield_abi_conformance   SOURCES test_bitfield_abi_conformance.cpp)
# D-CSUBSET-PACKED: the packed-struct cross-compile-COMPARE witness — builds the
# packed battery with the host's NATIVE C compiler (cl.exe `#pragma pack(1)` /
# gcc/clang `__attribute__((packed))`) and asserts dss's computeLayout(packed) is
# byte-identical (sizeof + per-field offsetof). Skips when no toolchain.
dss_add_test(NAME core/test_packed_abi_conformance     SOURCES test_packed_abi_conformance.cpp)

# D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET — the two sites of
# the class that live OUTSIDE a config loader.
# (1) `src/ffi/shipped_lib_descriptor.cpp`: its object-format refusals advertised
#     THREE spellings (`"pe"/"elf"/"macho"`) while `objectFormatKindFromName` +
#     `isSelectableObjectFormatKind` accept FIVE — a descriptor author writing
#     `wasm` was told by name it was not allowed. The pins drive the assertion
#     from `kSelectableObjectFormatKindNames` and from `allNames(kDataModelTable)`,
#     never from today's sentence, and assert BOTH directions (the refusal names
#     every accepted spelling AND advertises nothing the reader refuses).
# (2) `src/lir/lir_reg.hpp`: `lirRegClassName`/`lirRegClassFromName` were a
#     hand-written switch + if-chain over the five spellings
#     `kTargetRegClassTable` already owns, and the header's long-standing
#     `static_assert`s pin the two enums' VALUES only — a rename in the target
#     table would split `.target.json` from the `.dsslir` text format in silence.
#     The runtime pin walks the table rows and names the class that lost its
#     spelling (the header now also refuses to BUILD, but a build failure does
#     not say which row broke).
# ⚠ MUST run through ctest, never a bare .exe: the shipped-config resolver walks
# the cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets it.
dss_add_test(NAME core/test_vocabulary_projection_ffi_and_lir
             SOURCES test_vocabulary_projection_ffi_and_lir.cpp)

# D-CONFIG-GRAMMAR-LOADER-KEY-SHAPE-SENTENCES-RETYPE-THEIR-VOCABULARIES +
# D-TEXT-TIER-READERS-KEEP-HAND-WRITTEN-FROMNAME-IF-CHAINS — the two halves of
# the retyped-closed-set class that live in the GRAMMAR loader and in the
# `.dsshir` / `.dssir` text tiers.
#
# ★ NEITHER HALF RETYPES A VOCABULARY. The key half compares a block's SHAPE
#   sentence against the allow-list `rejectUnknownKeys` renders off that block's
#   own closed-key table — both taken out of the real loader, so adding a table
#   row makes the second list grow and reds a sentence that is still a literal.
#   The value half takes the set OUT OF the refusal and feeds every advertised
#   spelling back to the same reader, which is the direction an emit→parse→
#   compare round trip is blind to: `fmtShader`/`fmtTranspile`/`fmtDiag` skip
#   their sentinel values, so three spellings the reader accepts are never
#   emitted by the writer at all.
# ⚠ MUST run through ctest, never a bare .exe: the shipped-config resolver walks
# the cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets it.
dss_add_test(NAME core/test_key_shape_and_text_tier_vocabulary
             SOURCES test_key_shape_and_text_tier_vocabulary.cpp)
# Mutates the SHIPPED language JSON in memory at a named JSON pointer per block
# (nlohmann consumer) — a stale pointer THROWS rather than passing vacuously.
target_link_libraries(dss_core_test_key_shape_and_text_tier_vocabulary
                      PRIVATE nlohmann_json::nlohmann_json)

# D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET — the member of the
# class the CENSUS COULD NOT SEE. `CompositeKind` had no name table and no
# `…FromName`: its three spellings lived in two INLINE `k == "struct"/"union"/
# "enum"` chains in the grammar loader, with five sentences restating them, and
# one of the five had already drifted to 2 of 3 ("must be a string 'struct' or
# 'union'" on the `fieldChildren` TYPE-ERROR arm, above a chain that takes `enum`).
#
# ★ THESE PINS ARE KEYED ON THE JSON PATH, which is the difference that matters.
#   The three arms are three refusals of ONE key, so a per-LOAD search for "some
#   diagnostic naming the whole set" is satisfied by either honest sibling — and
#   the two honest siblings are exactly why the drift went unseen. Each arm now
#   answers for itself at the pointer the loader reports.
# ⚠ MUST run through ctest, never a bare .exe: `findShippedConfig` walks the cwd
# unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets it.
dss_add_test(NAME core/test_composite_kind_vocabulary_projection
             SOURCES test_composite_kind_vocabulary_projection.cpp)
# Mutates the SHIPPED language JSON in memory at pointers found by SEARCHING for
# the rows that carry a compositeKind (nlohmann consumer) — never by index, and a
# stale pointer THROWS rather than passing vacuously.
target_link_libraries(dss_core_test_composite_kind_vocabulary_projection
                      PRIVATE nlohmann_json::nlohmann_json)

# D-CONFIG-GRAMMAR-LOADER-INLINE-CHAIN-VOCABULARIES-REMAIN — the SWEEP of the
# owner shape the retyped-set census could not see, in the one file that held
# all of it: every inline `==` chain in `grammar_schema_json.cpp`. Thirteen
# vocabularies plus the `synthesizedTypes` ROLE ROUTER.
# ★★ THE EXPECTATIONS ARE HAND-WRITTEN, NOT PROJECTED. A pin whose expected set
#    comes off the same table the code renders moves BOTH HALVES TOGETHER: a
#    renamed row re-spells a config key across the tree and every
#    "the-message-names-the-table" assertion stays green through it. This file
#    states each accepted set, and its COUNT, as literals.
# ⚠ MUST run through ctest, never a bare .exe: `findShippedConfig` walks the cwd
# unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets it.
dss_add_test(NAME core/test_grammar_loader_chain_vocabulary_projection
             SOURCES test_grammar_loader_chain_vocabulary_projection.cpp)
# Mutates the SHIPPED language JSON in memory at pointers found by SEARCHING for
# the objects that carry each key (nlohmann consumer) — never by index, and a
# locator that finds nothing fails the arm rather than passing vacuously.
target_link_libraries(dss_core_test_grammar_loader_chain_vocabulary_projection
                      PRIVATE nlohmann_json::nlohmann_json)

# D-TYPEKIND-PASCALCASE-SPELLINGS-HAVE-TWO-OWNERS — `TypeKind`'s forty PascalCase
# spellings had THREE hand-written owners (`lir/lir_text.cpp`'s `.dsslir` `core`
# tag, `type_lattice/type_reintern.cpp`'s abort name, `grammar_schema_json.cpp`'s
# twenty-row config table) and the PRIMITIVE subset had TWO. The first pair had
# already drifted, on `Count_`.
# ★★ THE EXPECTATIONS ARE HAND-WRITTEN, NOT PROJECTED — a set read off the same
#    table the code renders moves BOTH HALVES TOGETHER. Every list and every
#    count in this file is a literal.
# ⚠ MUST run through ctest, never a bare .exe: `findShippedConfig` and
# `TargetSchema::loadShipped` walk the cwd unless `DSS_CONFIG_ROOT` is set, and
# only `dss_add_test` sets it.
dss_add_test(NAME core/test_type_kind_vocabulary_projection
             SOURCES test_type_kind_vocabulary_projection.cpp)
# Mutates the SHIPPED language JSON in memory at a pointer guarded by `at()`,
# which THROWS rather than passing vacuously when it stops resolving (nlohmann
# consumer). The `.dsslir` half needs no JSON — it rewrites the tag in a module
# the emitter produced.
target_link_libraries(dss_core_test_type_kind_vocabulary_projection
                      PRIVATE nlohmann_json::nlohmann_json)

# D-LATTICE-PRIMITIVE-BUILDER-ACCEPTS-A-NON-PRIMITIVE-KIND — `TypeInterner::
# primitive` interned WHATEVER kind it was handed (a fieldless "struct", an
# element-less "complex"), and the only gate on that argument, the config
# loader's `isPrimitiveTypeKind`, guarded ONE of the builder's callers.
# ★ THE FIRING IS THE DELIVERABLE. The gap was ✔MEASURED as not a live defect,
#   so a guard that could not be shown to fire would be strictly worse than
#   none. Twenty per-kind death arms (a loop inside one expectation ends at the
#   FIRST abort and would assert nothing about the other nineteen), plus the
#   INVERSE arm proving all twenty LEAF kinds still intern, plus a partition arm
#   so a new enumerator cannot land in neither list and leave both quiet.
# ★★ EVERY SET AND EVERY COUNT IS A LITERAL — a list read off the same table or
#    the same predicate the code consults moves BOTH HALVES TOGETHER.
# No config, no shipped JSON: the subject is the interner itself.
dss_add_test(NAME core/test_primitive_builder_leaf_kind_gate
             SOURCES test_primitive_builder_leaf_kind_gate.cpp)
