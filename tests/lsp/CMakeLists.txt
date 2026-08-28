dss_add_test(NAME lsp/test_utf16_column         SOURCES test_utf16_column.cpp)
dss_add_test(NAME lsp/test_json_rpc             SOURCES test_json_rpc.cpp)
dss_add_test(NAME lsp/test_protocol             SOURCES test_protocol.cpp)
dss_add_test(NAME lsp/test_method_dispatcher    SOURCES test_method_dispatcher.cpp)
target_link_libraries(dss_lsp_test_method_dispatcher PRIVATE nlohmann_json::nlohmann_json)
dss_add_test(NAME lsp/test_document_store       SOURCES test_document_store.cpp)
dss_add_test(NAME lsp/test_thread_pool          SOURCES test_thread_pool.cpp)
dss_add_test(NAME lsp/test_stdio_transport      SOURCES test_stdio_transport.cpp)
dss_add_test(NAME lsp/test_schema_cache         SOURCES test_schema_cache.cpp)
dss_add_test(NAME lsp/test_diagnostic_translator SOURCES test_diagnostic_translator.cpp)
dss_add_test(NAME lsp/test_lsp_server_e2e       SOURCES test_lsp_server_e2e.cpp)
dss_add_test(NAME lsp/test_lsp_replay           SOURCES test_lsp_replay.cpp)
dss_add_test(NAME lsp/test_lsp_semantic         SOURCES test_lsp_semantic.cpp)
# D-LSP-ASSEMBLY-DIALECT-UNSERVABLE: the workspace's `*.dss-project.json` is the
# editor's target channel. nlohmann is needed twice here — to build wire
# messages, and to DERIVE the "target with no defaultAssemblyLanguage" fixture
# from the real shipped `x86_64.target.json` rather than hand-typing a stub.
dss_add_test(NAME lsp/test_workspace_project    SOURCES test_workspace_project.cpp)
target_link_libraries(dss_lsp_test_workspace_project PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(dss_lsp_test_lsp_server_e2e PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(dss_lsp_test_lsp_replay     PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(dss_lsp_test_lsp_semantic   PRIVATE nlohmann_json::nlohmann_json)
# lsp_semantic uses semantic_test_fixture.hpp (FIX 4: real SemanticModel
# instead of a reinterpret_cast-fabricated shared_ptr for the stale-model
# test). Mirror the include path the semantic suite already wires up.
target_include_directories(dss_lsp_test_lsp_semantic
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)
dss_add_test(NAME lsp/test_lsp_coordinates      SOURCES test_lsp_coordinates.cpp)
target_include_directories(dss_lsp_test_lsp_coordinates
                           PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# D-LSP-HAS-NO-SYSTEM-INCLUDE-DIRS-AND-DROPS-THE-CU-DRIVER-DIAGNOSTICS: the
# editor and the driver must give the SAME verdict on the same file, in BOTH
# directions. It drives a real `LspServer` AND the real `Program`, so it needs
# nlohmann (to build the `didOpen` payload with correct escaping — the source
# fixtures contain quotes and newlines that a hand-built string literal would
# mis-frame).
dss_add_test(NAME lsp/test_lsp_driver_agreement SOURCES test_lsp_driver_agreement.cpp)
target_link_libraries(dss_lsp_test_lsp_driver_agreement
                      PRIVATE nlohmann_json::nlohmann_json)
