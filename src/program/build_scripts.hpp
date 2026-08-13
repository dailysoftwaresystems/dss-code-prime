#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "program/project_config.hpp"  // ScriptEntry (the parsed, already-validated manifest row)

#include <filesystem>
#include <vector>

// The build-lifecycle script RUNNER — the half of `preBuildScripts` /
// `postBuildScripts` that `project_config.cpp` deliberately does not do.
//
// THE SPLIT. `parseProjectConfig` is a PURE parser (see the ★ note on its
// declaration): it validates the SHAPE of every `ScriptEntry` — argv non-empty,
// every `runOn` token in the closed `windows`/`linux`/`darwin` vocabulary — and
// then stops, because deciding whether a script APPLIES needs the host OS and
// RUNNING it needs a working directory, neither of which a parser has any
// business knowing. This file is where those two facts arrive, and it is the
// only place in the driver that turns a manifest row into a process.
//
// WHAT IT DOES NOT DO, ON PURPOSE:
//
//   * NO SHELL. `entry.run[0]` is handed to the OS process-creation call
//     directly and the remaining elements cross the exec boundary one-for-one.
//     There is no `system()`, no `cmd /c`, no `sh -c` anywhere beneath this
//     function. The argv-vector type in `ScriptEntry` exists precisely so that
//     nobody has to pick a quoting dialect (see the type's docblock), and
//     re-joining that vector into a command line here would throw away the
//     property the type was chosen for: an argument containing `$HOME`, `;`,
//     `&&`, `*` or a space is DATA, and it must reach the child as the exact
//     bytes the manifest author wrote. `tests/program/test_build_scripts.cpp`
//     pins that byte-for-byte, and it is the load-bearing test of this file.
//
//   * NO TIMEOUT. A pre-build script is arbitrary user work — a code generator,
//     a submodule sync, a download — and there is no defensible number of
//     seconds after which the driver knows better than the operator. A wall
//     clock here would turn a slow-but-correct build into a nondeterministic
//     failure that reproduces only on the loaded CI machine. The operator's
//     existing Ctrl-C and their CI job timeout are the right instruments.
//     (Contrast `tests/test_support/run_binary.hpp`, which DOES time out: there
//     the child is a compiler-emitted binary whose non-termination is the very
//     bug under test, and the budget is derived from measurements.)
//
//   * NO OUTPUT CAPTURE. The child inherits this process's stdio, so the
//     script's own progress and error text stream to the operator's terminal
//     live and interleaved with the build. Capturing would buffer it until the
//     script finished — exactly backwards for the long-running step it usually
//     is — and would then require the driver to decide when to replay it.

namespace dss {

// Run every APPLICABLE entry of `scripts`, in MANIFEST ORDER, stopping at the
// first failure. Returns `true` iff nothing failed (including the vacuous
// cases: an empty list, or a list in which no entry applies to this host).
//
// APPLICABLE means `entry.runOn` is EMPTY (the absent-member default: every
// host) or CONTAINS `dss::currentHostOs()`. A skipped entry is NOT a failure
// and emits NOTHING — `"runOn": ["linux"]` on a Windows host is the manifest
// working as designed, and a diagnostic there would make every cross-platform
// project noisy on two hosts out of three.
//
// `cwd` is the directory the child is spawned in. It is a PARAMETER rather than
// "the process working directory" because this one function serves two callers
// with two different answers: the ROOT manifest's scripts run in the process
// working directory, and a DEPENDENCY manifest's scripts must run in THAT
// dependency's own directory — a `dependsOn` project whose codegen step ran in
// the depender's directory would generate its files into the wrong tree, and
// would do so silently. One function, one parameter; no caller-kind branch.
//
// STOP AT THE FIRST FAILURE. Build hooks are ORDERED work — entry 2 routinely
// consumes what entry 1 produced — so continuing past a failure runs steps
// against inputs that were never generated and buries the one diagnostic that
// mattered under a cascade of derived ones. The `false` return is the caller's
// signal to abandon the build; it is `[[nodiscard]]` so that abandoning cannot
// be forgotten, which would resurrect the silent-success class that
// `D_ScriptExitedNonZero`'s allocation note calls out (a pre-build codegen step
// that failed, followed by a green compile of STALE sources).
//
// FAILURE IS TWO REMEDIATION-DISTINCT CODES, never one:
//   * `D_ScriptSpawnFailed`   (0xD017) — the OS never created the process, so
//     no exit status exists. The message names `run[0]` and quotes the spawn
//     layer's own diagnostic. Fix: the environment or the argv spelling.
//   * `D_ScriptExitedNonZero` (0xD018) — the child RAN and returned non-zero.
//     The message names `run[0]` and the status. Fix: whatever the script
//     itself complained about, on the terminal, a moment ago.
// Exactly ONE diagnostic is emitted per call — the one belonging to the entry
// that stopped the run.
[[nodiscard]] DSS_EXPORT bool
runBuildScripts(std::vector<ScriptEntry> const& scripts,
                std::filesystem::path const&    cwd,
                DiagnosticReporter&             rep);

} // namespace dss
