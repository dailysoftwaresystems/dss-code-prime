# Driver / program-layer tests (plan 14 LK10 cycle 2).

dss_add_test(NAME program/test_artifact_report
             SOURCES test_artifact_report.cpp)
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
dss_add_test(NAME program/test_system_dirs_cwd_independent
             SOURCES test_system_dirs_cwd_independent.cpp)
dss_add_test(NAME program/test_input_resolver
             SOURCES test_input_resolver.cpp)
dss_add_test(NAME program/test_cross_validate_target_format
             SOURCES test_cross_validate_target_format.cpp)
dss_add_test(NAME program/test_project_config
             SOURCES test_project_config.cpp)

# D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET: the assembly dialect is resolved PER
# TARGET, so one invocation compiling a `.s` for two CPUs parses it under two
# grammars. Driver tier, not tests/examples/, for the structural reason the
# TF-C74 CU-cache pin records: the corpus runner compiles each manifest target
# ROW in its own single-target call, so no example can produce the
# two-targets-one-invocation situation this anchor is about.
dss_add_test(NAME program/test_asm_dialect_per_target
             SOURCES test_asm_dialect_per_target.cpp)

# `--dump-predefined-macros` (FC18 companion): the effective predefined-macro set
# as an INSPECTABLE artifact, plus the sweep asserting no shipped object format
# defines a C23 `__STDC_NO_*` non-support macro.
dss_add_test(NAME program/test_dump_predefined_macros
             SOURCES test_dump_predefined_macros.cpp)

# UCRT-P4 (D-FFI-PE-CRT-UCRT-MIGRATION + D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE): the pe
# program-entry argv spine's REAL-INPUT-PATH witnesses. This tier, not
# tests/examples/, because the subject's real input is the OS COMMAND LINE and the
# examples runner has no argv-passing knob (every example is spawned with no
# arguments, so `argc == 1` is the only observable it can produce).
dss_add_test(NAME program/test_entry_argv_run
             SOURCES test_entry_argv_run.cpp)
