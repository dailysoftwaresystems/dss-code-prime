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
