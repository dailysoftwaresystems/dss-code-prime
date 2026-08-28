dss_add_test(NAME ffi/test_binary_reader SOURCES test_binary_reader.cpp)
dss_add_test(NAME ffi/test_binary_reader_pe SOURCES test_binary_reader_pe.cpp)
dss_add_test(NAME ffi/test_binary_reader_macho SOURCES test_binary_reader_macho.cpp)
dss_add_test(NAME ffi/test_binary_reader_macho_roundtrip SOURCES test_binary_reader_macho_roundtrip.cpp)
dss_add_test(NAME ffi/test_binary_reader_ar SOURCES test_binary_reader_ar.cpp)
dss_add_test(NAME ffi/test_c_header_parser SOURCES test_c_header_parser.cpp)
dss_add_test(NAME ffi/test_abi_catalog SOURCES test_abi_catalog.cpp)
dss_add_test(NAME ffi/test_c_mangle SOURCES test_c_mangle.cpp)
dss_add_test(NAME ffi/test_ingest SOURCES test_ingest.cpp)
dss_add_test(NAME ffi/test_shipped_lib_descriptor SOURCES test_shipped_lib_descriptor.cpp)
# D-LANG-TYPE-IDENTITY-VOCABULARY: the EXHAUSTIVE cross-descriptor sweep — it
# enumerates the shipped object-format configs to derive the target matrix, so
# it parses their JSON directly.
dss_add_test(NAME ffi/test_shipped_type_consistency SOURCES test_shipped_type_consistency.cpp)
# D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF: the SHIPPED-SOURCE REALIZATION axis
# — the four refusals (R1 a realization naming a source that is not there; R2 a
# source file no descriptor names; R3 an image AND a source for one format; R4 no
# headers in the runtime tree, folded into R2) plus the tree-shape and
# two-readers-agree invariants. Reads the shipped descriptors' JSON directly, like
# its siblings above, hence the same json link.
dss_add_test(NAME ffi/test_shipped_source_realization
             SOURCES test_shipped_source_realization.cpp)
target_link_libraries(dss_ffi_test_shipped_source_realization
                      PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(dss_ffi_test_shipped_type_consistency
                      PRIVATE nlohmann_json::nlohmann_json)
# D-FFI-PE-CRT-UCRT-MIGRATION: the co-state-group binding invariant — the pe C
# runtime may be mid-migration, but the descriptors that exchange CRT-owned
# objects must never be split across two runtimes. Reads the shipped descriptors'
# JSON directly (like the sweep above), hence the same json link.
dss_add_test(NAME ffi/test_pe_crt_costate_binding SOURCES test_pe_crt_costate_binding.cpp)
target_link_libraries(dss_ffi_test_pe_crt_costate_binding
                      PRIVATE nlohmann_json::nlohmann_json)
# D-FFI-PE-ABORT-BEHAVIOR-NOOP-MACRO-STALE-AFTER-UCRT-FLIP (UCRT-P4):
# `_set_abort_behavior` must be a REAL ucrtbase.dll binding, not the pe-gated
# no-op `(0u)` macro it shipped as under msvcrt. Two axes — the EMITTED pe64
# import table (host-independent: nothing is run, so every leg checks it) and
# the descriptor itself (the partial-revert axis the image cannot see, since a
# restored macro leaves the eager import in place). Reads the shipped
# descriptor's JSON directly, hence the same json link as its two siblings.
dss_add_test(NAME ffi/test_pe_abort_behavior_binding
             SOURCES test_pe_abort_behavior_binding.cpp)
target_link_libraries(dss_ffi_test_pe_abort_behavior_binding
                      PRIVATE nlohmann_json::nlohmann_json)
# POSIX `environ` — the extern DATA-OBJECT row's two load-bearing properties:
# its availability set (eager-import law — widening it to a format whose CRT
# has no such export breaks every including binary's LOAD, silently at build
# time) and the absence of any macro that would shadow an LVALUE data object.
# Reads the shipped descriptors' JSON directly, hence the same json link as its
# three siblings above.
dss_add_test(NAME ffi/test_environ_data_object_binding
             SOURCES test_environ_data_object_binding.cpp)
target_link_libraries(dss_ffi_test_environ_data_object_binding
                      PRIVATE nlohmann_json::nlohmann_json)

# D-FF1-AR-BSD-VARIANT (P31): the BSD `ar` variant -- "__.SYMDEF" /
# "__.SYMDEF SORTED" ranlib tables and "#1/N" inline member names. Split from
# its GNU sibling because it reads REAL committed fixtures (tests/ffi/data/
# libbsdapple.a from Apple ar, libbsdllvm.a from llvm-ar --format=bsd) rather
# than synthesising bytes -- see that file's header for why. Needs no json
# link: it reads binary bytes, not shipped descriptors.
dss_add_test(NAME ffi/test_binary_reader_ar_bsd
             SOURCES test_binary_reader_ar_bsd.cpp)

# D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME (P34): the DARWIN LINK-NAME ORACLE —
# the census that covers every FUTURE symbol, not only the eight that diverge
# today. It enumerates every Mach-O-visible imported function in the shipped
# descriptors (hence the same json link as its four siblings above) and requires
# each to be accounted for by a MEASURED row in tests/ffi/data/darwin-link-names.tsv,
# which records what a real Apple `cc` emits for that identifier per arch. Runs on
# every host: the measurement is DATA, so no Mach-O has to execute.
dss_add_test(NAME ffi/test_darwin_link_name_oracle
             SOURCES test_darwin_link_name_oracle.cpp)
target_link_libraries(dss_ffi_test_darwin_link_name_oracle
                      PRIVATE nlohmann_json::nlohmann_json)

# Shipped-source resolution reports WHY it failed. The pinned property is a NEGATIVE
# one -- a path the filesystem could not be asked about must never be reported as
# absent -- plus its mirror, that a genuine absence is still called an absence, so a
# fix cannot over-correct into "everything is an I/O error".
dss_add_test(NAME ffi/test_shipped_source_lookup
             SOURCES test_shipped_source_lookup.cpp)

# D-FFI-DUPLICATE-SYMBOL-ACROSS-DESCRIPTORS-SILENTLY-ORDER-RESOLVED (P42):
# invariant (C), CROSS-DESCRIPTOR REALIZATION AGREEMENT. Split from its
# type-identity sibling because it asserts a DIFFERENT code
# (F_ShippedCorpusInvariantBroken) and because half of it runs the sweep over
# DELIBERATELY MUTATED COPIES of the shipped corpus, where the sibling's "every
# descriptor must read cleanly" expectation would fire on the mutation itself.
# Enumerates the object-format documents to derive the target matrix, so it parses
# their JSON directly — same json link as its siblings above.
dss_add_test(NAME ffi/test_shipped_realization_consistency
             SOURCES test_shipped_realization_consistency.cpp)
target_link_libraries(dss_ffi_test_shipped_realization_consistency
                      PRIVATE nlohmann_json::nlohmann_json)

# D-FFI-DUPLICATE-SYMBOL-ACROSS-DESCRIPTORS-SILENTLY-ORDER-RESOLVED (P42): the
# THIRD picker — `realizeShippedExternSymbols`, the HAND-DECLARED path. Its
# sibling above pins the RULE; this one pins that the other picker ASKS it, which
# is a different failure (the rule can be perfect and this entry point still
# resolve silently by relPath sort — that was the tree's state when it was
# written). It drives the oracle through `DSS_CONFIG_ROOT`, so it MUST run under
# ctest: a bare .exe takes the cwd walk and reads whichever tree the shell stands
# in, never the mutated copy. Parses the shipped JSON to build the
# REMOVE-direction mutant, hence the same json link as its siblings.
dss_add_test(NAME ffi/test_shipped_realization_oracle
             SOURCES test_shipped_realization_oracle.cpp)
target_link_libraries(dss_ffi_test_shipped_realization_oracle
                      PRIVATE nlohmann_json::nlohmann_json)

# D-FFI-SHIPPED-DESCRIPTOR-PARSE-CACHE-SERVES-A-STALE-DOCUMENT (P42): the
# thread-local descriptor PARSE CACHE must answer about the file on disk NOW — a
# live defect in `dsscp --lsp`, whose persistent ThreadPool worker kept serving
# the document it first read for the whole session. Pins BOTH directions: a
# changed file is re-parsed, and an UNCHANGED one is still served without a
# re-parse (a cache that was simply deleted passes every staleness assertion).
# Needs no json link: it writes its descriptors as text and reads them through
# the shipped reader.
dss_add_test(NAME ffi/test_shipped_descriptor_cache_staleness
             SOURCES test_shipped_descriptor_cache_staleness.cpp)

# D-FFI-ABI-CATALOG-SELECTS-CALLING-CONVENTION-BY-FORMAT-IDENTITY (P44): the
# ABI-resolution pins became CONFIG pins when `kAbiCatalog` was deleted — they
# now build REMOVE-direction mutants of the shipped `.format.json` documents
# (strip the required key; re-point the declared convention) and assert the
# engine follows the document. That needs the JSON reader directly, like the
# descriptor sweeps above it.
target_link_libraries(dss_ffi_test_abi_catalog
                      PRIVATE nlohmann_json::nlohmann_json)
