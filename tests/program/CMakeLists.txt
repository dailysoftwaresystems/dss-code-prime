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

# D-ASM-EXTERN-CALL-CANNOT-BIND-A-LIBRARY +
# D-ASM-RESOLVE-LIBRARY-SILENTLY-IGNORED-ON-ENCODE-TIER: the `encode` tier's
# extern binder. Driver tier because two of its four claims have no example-
# manifest surface at all — `--resolve-library` is a DRIVER flag (a manifest
# declares sources and targets, not link inputs), and the per-format decoration
# gate needs ONE source text to be accepted on elf/pe and refused on Mach-O
# inside one process. The positive end-to-end path (build, RUN, read stdout)
# lives in examples/asm/**.
dss_add_test(NAME program/test_asm_extern_binding
             SOURCES test_asm_extern_binding.cpp)

# D-ASM-DATA-ITEMS-NOT-WIRED-INTO-THE-DRIVER: the `.data`/`.byte`/`.word`/
# `.quad`/`.zero` bytes a `.s` declares must reach the LINKED IMAGE, not merely
# be computed. Driver tier because the corpus runner's only observable is an
# EXIT CODE, so an example can witness data only once assembly can NAME a data
# symbol in an instruction — which is anchored separately
# (D-ASM-SYMBOL-OPERAND-NOT-LOWERED) and not yet reachable.
dss_add_test(NAME program/test_asm_data_directives
             SOURCES test_asm_data_directives.cpp)
# The `.dss-project.json` build-hook RUNNER (`src/program/build_scripts.cpp`) —
# the half `project_config.cpp` deliberately leaves undone (it is a pure parser;
# deciding whether a hook APPLIES needs the host OS and RUNNING it needs a
# working directory). Driver tier, alongside its parser sibling.
#
# ★ It defines its own `main`, and that is load-bearing rather than incidental:
# the test binary re-execs ITSELF as the fixture program these pins need — a
# program that records the argv and the working directory it actually received.
# `main` intercepts the private `--dss-build-script-record=` flag before gtest
# parses argv, exactly as `tests/test_support/test_run_binary_capture.cpp` does
# for its payload flag. Self-spawn (rather than a second `add_executable`) keeps
# the fixture available on every host in the matrix with no PATH assumptions, and
# needs no `$<TARGET_FILE:…>` define, no `add_dependencies` edge, and no
# allowlist row in the two `tools/check-orphan-tests` twins.
dss_add_test(NAME program/test_build_scripts
             SOURCES test_build_scripts.cpp)

# The `runOn` host-token vocabulary (`src/program/platform_token.hpp`) — the ONE
# spelling of windows/linux/darwin, shared by the `.dss-project.json`
# build-script gate, the example manifests and both corpus runners' host
# chokepoint. Driver tier, not test_support: a pre-build script runs on the
# HOST, which is a driver question.
dss_add_test(NAME program/test_platform_token
             SOURCES test_platform_token.cpp)

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
