# Optimizer unit tests.

dss_add_test(NAME opt/test_optimizer       SOURCES test_optimizer.cpp)
dss_add_test(NAME opt/test_const_fold      SOURCES test_const_fold.cpp)
dss_add_test(NAME opt/test_pipeline_loader SOURCES test_pipeline_loader.cpp)
dss_add_test(NAME opt/test_dce             SOURCES test_dce.cpp)
dss_add_test(NAME opt/test_dce_linkage     SOURCES test_dce_linkage.cpp)
dss_add_test(NAME opt/test_compile_options SOURCES test_compile_options.cpp)
