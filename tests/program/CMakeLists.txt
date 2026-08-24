# Driver / program-layer tests (plan 14 LK10 cycle 2).

dss_add_test(NAME program/test_artifact_report
             SOURCES test_artifact_report.cpp)
dss_add_test(NAME program/test_target_spec
             SOURCES test_target_spec.cpp)
dss_add_test(NAME program/test_compile_pipeline
             SOURCES test_compile_pipeline.cpp)
dss_add_test(NAME program/test_max_diagnostics_reaches_every_tier
             SOURCES test_max_diagnostics_reaches_every_tier.cpp)
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
dss_add_test(NAME program/test_cross_validate_language_target
             SOURCES test_cross_validate_language_target.cpp)
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
# allowlist row in the two `scripts/check-orphan-tests/check-orphan-tests` twins.
dss_add_test(NAME program/test_build_scripts
             SOURCES test_build_scripts.cpp)

# The `.dss-deps` git dependency cache — the acquisition seam
# (`src/program/git_acquire.*`), the lockfile (`dependency_lockfile.*`) and
# B.4's four-outcome state machine (`dependency_cache.*`).
#
# ★ DRIVER TIER, AND NOT tests/examples/, FOR A STRUCTURAL REASON RATHER THAN A
# PREFERENCE. Two of the four outcomes are NETWORK FAILURES, and no corpus
# example can produce one on demand — the corpus compiles real manifests with
# real inputs, and "the network is down" is not an input it can supply. The
# whole file therefore drives an injected fake `IGitRunner`: zero network, zero
# dependence on `git` being installed, and every arm of the machine reachable
# deterministically. (B.7 layer 2 — one offline `file://` end-to-end witness
# through real git, environment-gated — is a separate and complementary thing.)
dss_add_test(NAME program/test_dependency_git_cache
             SOURCES test_dependency_git_cache.cpp)

# `dependsOn` RESOLUTION (`src/program/dependency_resolver.*`) and its wiring
# into `Program::compileProject` — the graph walk, the composition-verb
# dispatch, B.10's consumer-driven format derivation, U-7's dependency-side
# hooks, U-8's absorption rule and U-9's artifact layout.
#
# ★ DRIVER TIER RATHER THAN tests/examples/, and for a structural reason. Half
# of what this file asserts has NO corpus surface at all: a corpus example's
# only observables are an exit code and stdout, while the load-bearing
# properties here are WHICH OBJECT FORMAT a dependency was built with, WHICH
# TARGET its artifact was threaded to, and which of two rejects fired first.
# Two of the six B.10 pins also need a shipped-format inventory the real config
# directory cannot produce (a second candidate, and none), which they obtain by
# pointing DSS_CONFIG_ROOT at a mirrored tree — something no example manifest
# can express.
#
# ★ It defines its own `main`, like `test_build_scripts.cpp` and for the same
# reason: U-7's pins need a hook program that writes a file AT A PATH RELATIVE
# TO ITS OWN WORKING DIRECTORY, and the binary re-execs ITSELF as that fixture.
# Self-spawn keeps the fixture available on every host with no PATH assumption,
# and needs no second target, no `$<TARGET_FILE:…>` define and no allowlist row
# in the two `scripts/check-orphan-tests/check-orphan-tests` twins.
dss_add_test(NAME program/test_dependency_resolver
             SOURCES test_dependency_resolver.cpp)

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

# `--suppress=<a code in kUnsuppressableCodes>` — the refused-request notice
# (`D_SuppressRequestIgnored`). Driver tier because the defect lives in the seam
# between the arg parser that ACCEPTS the flag and the reporter that IGNORES it:
# `parseCliArgs` -> `buildReporterConfig` -> `buildReporter` is the projection
# under test, and only this tier can drive all three.
dss_add_test(NAME program/test_suppress_request_ignored
             SOURCES test_suppress_request_ignored.cpp)

# The COMPILER'S BUILD STAMP (`src/program/dss_build_stamp.hpp`) — the identity
# the runtime object cache keys on. Driver tier because the subject is a
# `src/program/` header whose value is produced by the BUILD SYSTEM, so the only
# way to observe it is to compile against the real generated header.
#
# ★ AND IT IS THE ONLY PLACE THE STAMP'S SHAPE IS OBSERVED. Its production
# consumer — `src/program/runtime_object_cache.cpp` — hashes the stamp into a
# key, which means every defect in this list (empty, whitespace-bearing, missing
# the version) still produces a perfectly valid-looking key over there and is
# invisible from that side. The assertions have to live where the string is,
# not where it is consumed.
dss_add_test(NAME program/test_build_stamp
             SOURCES test_build_stamp.cpp)

# The opt-in (top-level CMakeLists). Wires BOTH the generated header's include
# path and the ordering edge onto this ONE target — no other target can see the
# header, which is what keeps a stamp that changes on every dirty edit from
# rebuilding the world.
dss_use_build_stamp(dss_program_test_build_stamp)

# It REUSES tests/core/native_c_probe.hpp for the reference-compiler probe
# rather than growing a second copy of host-compiler discovery, shell quoting
# and output capture — the same reason tests/conformance adds this path.
target_include_directories(dss_program_test_suppress_request_ignored
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/core)

# The shipped runtime's content-addressed object cache
# (`src/program/runtime_object_cache.{hpp,cpp}` —
# D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF). Driver tier, and not
# tests/examples/, for two structural reasons rather than convenience:
#
#   · The sibling-lookup case must SCAN A DIRECTORY IT CONTROLS. Its
#     load-bearing arm is the >1-candidate refusal, and the shipped
#     `object-formats/` tree deliberately contains no ambiguous pair — so the
#     ambiguity has to be CONSTRUCTED in a scratch directory. A corpus example
#     compiles a source against the shipped tree and has no such knob.
#   · The cache-key cases assert a BYTE-EXACT key document and the ABSENCE of
#     invalidation on an out-of-scope edit. The examples runner's only
#     observable is an exit code, which cannot distinguish "the cache hit" from
#     "the cache missed and recompiled" — the very difference under test.
#
# ⓘ It does NOT call `dss_use_build_stamp`, and that is deliberate: this test
# asserts the key document's SHAPE, and the one term it cannot pin to a literal
# is `compiler=`, whose value is the stamp. Opting in would let the test
# reconstruct the expected value from the same macro the subject reads, which
# asserts only that a macro equals itself — and it would drag a second target
# into the rebuild-on-every-dirty-edit set the stamp's opt-in exists to bound.
# The stamp string's own shape is pinned by `program/test_build_stamp` above.
dss_add_test(NAME program/test_runtime_object_cache
             SOURCES test_runtime_object_cache.cpp)

# THE SHIPPED-RUNTIME COMPILE GATE (D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF)
# — every source named by ANY descriptor's `realization` map must actually
# COMPILE for the object format that declares it, cross, from whatever host runs
# the suite.
#
# ★★★ IT IS A REPLACEMENT, NOT AN ADDITION. A `--compile-runtime` CLI verb was
# designed and then CANCELLED on the explicit understanding that the coverage it
# bought moves into this test. Automatic compilation only ever compiles what a
# build REACHES, so a declared realization no example exercises stays uncompiled
# and its breakage stays invisible until a user hits it. This is what makes every
# declared realization exercised.
#
# Driver tier, and the reason is structural rather than convenience: the gate
# drives `Program::compileFiles` through the real production driver, derives its
# machines from `crossValidateTargetFormat` + `resolveArchiveSiblingFormat`, and
# derives its artifact name from `TargetSpec::outputExtension` — three of its
# four subjects are `src/program/`, and it shares this directory with the object
# cache those same units feed. It reads the descriptor corpus only through
# `src/ffi/shipped_lib_descriptor.hpp`'s own readers, so it needs no JSON link:
# the format keys are discovered by offering the closed `ObjectFormatKind`
# vocabulary to `allShippedSourcesForFormat` and keeping what the corpus answers.
#
# ⓘ Its DECLARATIVE sibling is `tests/ffi/test_shipped_source_realization.cpp`
# (R1 every realization names an existing file; R2 every file is named; R3 no
# format declares both an image and a source). Neither half implies the other — a
# perfectly-declared corpus can name a unit that does not compile — and neither
# restates the other's assertions.
dss_add_test(NAME program/test_shipped_runtime_compiles
             SOURCES test_shipped_runtime_compiles.cpp)

# THE DRIVER'S **SUPPLYING** OF THE PIPELINE KERNEL'S DRIVER-DERIVED ARGUMENTS
# (D-TEST-STATIC-LINK-UNIT-SUITE-CANNOT-WITNESS-A-DRIVER-THREADING-GAP). The
# classification these pins witness lives in a block comment in
# `src/program/compile_pipeline.hpp`, beside the exports it classifies.
#
# ★★★ DRIVER TIER BY DEFINITION, NOT BY PREFERENCE — the tier IS the subject.
# Every entry point pinned here already has thorough unit coverage, and that
# coverage is exactly what cannot help: a unit case CONSTRUCTS the argument the
# driver is supposed to derive, so it tests the callee while assuming the
# caller. Cycle P22 measured the consequence — dropping a newly-threaded
# parameter at ONE driver call site reproduced the original CLI failure while
# the whole in-process suite stayed green at 32/32.
#
# ★★ AND THE PINS ARE PER **ROUTE**. `program.cpp` reaches
# `linkAndWriteWithStaticArchives` by three routes and `optimizeModule` by
# three; a pin on one says nothing about the others, which is how a parameter
# ends up threaded to two call sites out of three.
#
# ⓘ NOT tests/examples/: two of the three parameters have no corpus surface at
# all. `--stack-reserve` is a DRIVER flag (a manifest declares sources and
# targets, not image knobs), and the optimizer STAGE count is invisible to an
# exit code — a build optimized at the unit schedule only still exits 42.
dss_add_test(NAME program/test_driver_argument_supply
             SOURCES test_driver_argument_supply.cpp)

# THE FOUR SOURCE-ARGUMENT SHAPES A USER CAN TYPE
# (D-HARNESS-EXAMPLE-RUNNERS-ALWAYS-COMPILE-AN-ABSOLUTE-SOURCE-PATH).
#
# ★★★ DRIVER TIER BECAUSE THE SHAPE IS A PROPERTY OF THE INVOCATION, NOT OF AN
# EXAMPLE. Both corpus runners construct an ABSOLUTE source path by
# construction, so EVERY manifest exercises exactly one of the four shapes and
# the other three are exercised by nothing. That is not a hypothetical hole: it
# is how `dss --compile main.c` shipped unable to find a header sitting beside
# `main.c` (D-PP-BARE-RELATIVE-MAIN-PATH-DEFEATS-THE-INCLUDER-DIRECTORY-SEARCH)
# behind a green gate.
#
# ⓘ NOT tests/examples/: the manifest vocabulary is a CLOSED key set that both
# runners refuse an unknown key from, so expressing a shape per example would
# mean minting a schema key to serve exactly one manifest — the P24
# invent-a-schema-key anti-pattern — and a new example would witness nothing,
# because both runners would hand it an absolute path like every other one.
#
# ⓘ ITS CLI-SUBPROCESS COUNTERPART is `runSourceArgumentShapePin` on the
# `integrated_tests/cli-surface` entry. A capability that exists on one tier and
# not the other is [[D-EXAMPLES-RUNNER-TWO-RUNNERS-MUST-AGREE]]; these two land
# together.
dss_add_test(NAME program/test_source_argument_shape
             SOURCES test_source_argument_shape.cpp)
