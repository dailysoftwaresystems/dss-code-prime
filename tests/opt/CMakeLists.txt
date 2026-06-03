# OPT2 cycle 1 — optimizer unit tests.

dss_add_test(NAME opt/test_optimizer       SOURCES test_optimizer.cpp)
dss_add_test(NAME opt/test_const_fold      SOURCES test_const_fold.cpp)
dss_add_test(NAME opt/test_pipeline_loader SOURCES test_pipeline_loader.cpp)
