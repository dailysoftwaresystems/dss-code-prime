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
