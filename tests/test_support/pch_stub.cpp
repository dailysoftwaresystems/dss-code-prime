// PCH anchor translation unit (test executables).
//
// Carries the precompiled-header set (DSS_PCH_HEADERS) plus <gtest/gtest.h>
// for the `dss_test_pch` producer. Every test target reuses that PCH via
// `target_precompile_headers(... REUSE_FROM dss_test_pch)` in dss_add_test.
// The TU emits no code; the static_assert keeps it a non-empty, warning-clean
// translation unit.
static_assert(true, "DSS PCH anchor translation unit (tests)");
