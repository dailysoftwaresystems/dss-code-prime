# Driver / program-layer tests (plan 14 LK10 cycle 2).

dss_add_test(NAME program/test_target_spec
             SOURCES test_target_spec.cpp)
dss_add_test(NAME program/test_compile_pipeline
             SOURCES test_compile_pipeline.cpp)
dss_add_test(NAME program/test_cli_args
             SOURCES test_cli_args.cpp)
dss_add_test(NAME program/test_input_resolver
             SOURCES test_input_resolver.cpp)
