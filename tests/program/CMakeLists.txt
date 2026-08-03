# Driver / program-layer tests (plan 14 LK10 cycle 2).

dss_add_test(NAME program/test_target_spec
             SOURCES test_target_spec.cpp)
dss_add_test(NAME program/test_compile_pipeline
             SOURCES test_compile_pipeline.cpp)
dss_add_test(NAME program/test_ffi_resolve_library
             SOURCES test_ffi_resolve_library.cpp)
dss_add_test(NAME program/test_static_link
             SOURCES test_static_link.cpp)
dss_add_test(NAME program/test_cli_args
             SOURCES test_cli_args.cpp)
dss_add_test(NAME program/test_include_dirs
             SOURCES test_include_dirs.cpp)
dss_add_test(NAME program/test_input_resolver
             SOURCES test_input_resolver.cpp)
dss_add_test(NAME program/test_cross_validate_target_format
             SOURCES test_cross_validate_target_format.cpp)
dss_add_test(NAME program/test_project_config
             SOURCES test_project_config.cpp)
