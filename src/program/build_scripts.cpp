#include "program/build_scripts.hpp"

#include "core/substrate/process_spawn.hpp"  // spawnAndWaitInherit — the ONE process-creation call
#include "core/types/parse_diagnostic.hpp"
#include "program/platform_token.hpp"        // currentHostOs — the closed windows/linux/darwin vocabulary

#include <string>
#include <string_view>

namespace dss {

namespace {

// Does this entry apply to the machine we are running on?
//
// TWO cases, and the EMPTY one is the important one. `ScriptEntry::runOn` is
// empty EXACTLY when the manifest member was ABSENT (the loader rejects a
// present-but-empty `[]` outright, precisely so that "absent" has one
// unambiguous meaning here), and absent means UNFILTERED — run everywhere.
// Reading empty as "matches nothing" would invert the default and silently
// disable every unfiltered hook in the corpus, which is the failure mode with
// no diagnostic attached to it.
//
// The non-empty case is exact, CASE-SENSITIVE membership against
// `currentHostOs()`. Both sides come from `program/platform_token.hpp`, so the
// spelling the manifest was validated against and the spelling the host probe
// returns cannot drift apart — a mismatch there would make every gate evaluate
// to "never run", again silently.
[[nodiscard]] bool appliesToHost(std::vector<std::string> const& runOn) {
    if (runOn.empty()) return true;  // member absent ⇒ every host
    std::string_view const host = currentHostOs();
    for (std::string const& token : runOn) {
        if (token == host) return true;
    }
    return false;
}

// Emit an Error-severity driver diagnostic. Same shim shape as
// `project_config.cpp`'s `emitProjectError`, minus the source-label prefix:
// by the time a script RUNS the manifest is long parsed, and the useful
// identifier is the program being spawned, which every message below names.
void emitScriptError(DiagnosticReporter& rep,
                     DiagnosticCode      code,
                     std::string         detail) {
    report(rep, code, DiagnosticSeverity::Error, std::move(detail));
}

} // namespace

bool runBuildScripts(std::vector<ScriptEntry> const& scripts,
                     std::filesystem::path const&    cwd,
                     DiagnosticReporter&             rep) {
    // Manifest order, deliberately: `for` over the vector as given, with no
    // sort, no partition of applicable-vs-skipped, and no parallelism. Build
    // hooks are a SEQUENCE — entry 2 consumes what entry 1 produced — so the
    // order the author wrote is the only correct order, and the filter below
    // must not be allowed to reorder what survives it.
    for (ScriptEntry const& entry : scripts) {
        // Skipped entries produce NOTHING: no diagnostic, no marker, no
        // trace. A `runOn` that does not name this host is the manifest
        // working as designed, not a problem to report. (This is also why the
        // filter test asserts `errorCount() == 0` on the skip side — a "skip"
        // that reported would make every cross-platform project fail on two
        // hosts out of three.)
        if (!appliesToHost(entry.runOn)) continue;

        // DEFENCE IN DEPTH, not a live path. `parseProjectConfig` rejects an
        // empty `run` array, so a manifest cannot get here — but this function
        // takes a plain `std::vector<ScriptEntry>` from any caller, and
        // indexing `run[0]` on an empty vector is undefined behaviour, i.e. the
        // one failure mode that produces no message at all. Total function,
        // fail-loud: report the same "nothing was spawned" code the OS-refusal
        // path uses, and name the reason precisely.
        if (entry.run.empty()) {
            emitScriptError(
                rep, DiagnosticCode::D_ScriptSpawnFailed,
                "build script entry has an EMPTY argv: there is no program to "
                "spawn. The manifest loader rejects this shape (`run` must be a "
                "non-empty array of non-empty strings), so a ScriptEntry that "
                "reaches here with no argv was constructed in code — fix the "
                "caller, not the manifest.");
            return false;
        }

        std::string const& argv0 = entry.run.front();

        // THE process creation. `entry.run` goes through WHOLE — argv[0] is the
        // program, argv[1..] are its arguments, byte-for-byte as the manifest
        // spelled them. No join, no split, no expansion, no shell. `cwd` is the
        // child's working directory (see the header note on why it is a
        // parameter). stdio is inherited; there is no timeout.
        substrate::SpawnResult const result =
            substrate::spawnAndWaitInherit(entry.run, cwd);

        // ── THE FORK: was there ever a process? ─────────────────────────────
        //
        // `spawned == false` means the OS refused before the child existed, so
        // `exitCode` describes nothing and must not be read. These two arms are
        // separate CODES rather than one "script failed" because the operator's
        // next action differs completely — see the allocation note at
        // `D_ScriptSpawnFailed` in `core/types/parse_diagnostic.hpp`. Collapsing
        // them would make the single most common CI failure (a missing
        // interpreter) indistinguishable from a genuine script reject.
        if (!result.spawned) {
            emitScriptError(
                rep, DiagnosticCode::D_ScriptSpawnFailed,
                "build script '" + argv0 + "' could not be spawned: "
                + result.diagnostic
                + ". The process was never created, so it has no exit status — "
                  "check that '" + argv0 + "' exists, is executable, and (for a "
                  "bare name) is on PATH. argv[0] is spawned directly, never "
                  "through a shell, so shell builtins and shell-only spellings "
                  "do not resolve.");
            return false;
        }

        if (result.exitCode != 0) {
            // ── THE STATUS IS NOT ALWAYS THE SCRIPT'S OWN VERDICT ───────────
            //
            // On the spawned arm `SpawnResult::diagnostic` is EMPTY for a
            // clean `exit(n)` and NON-EMPTY for exactly the states where the
            // number is a stand-in rather than a choice: a POSIX child killed
            // by a signal (`exitCode = 128 + signo`), a `waitpid` that failed,
            // a `WaitForSingleObject` that did not signal, a
            // `GetExitCodeProcess` that failed (the last three report -1).
            // That emptiness is the discriminator `D_ScriptExitedNonZero`'s
            // allocation note in `core/types/parse_diagnostic.hpp` promises
            // ("the spawn layer's status prose discriminates"), and
            // `process_spawn.hpp` makes the same promise to callers — so
            // DROPPING the field here would break a contract kept by the
            // substrate, and would tell an operator whose codegen hook just
            // SEGV'd that the hook deliberately returned 139. Loud but wrong
            // is still wrong.
            //
            // The two arms therefore give OPPOSITE remediation. Empty ⇒ read
            // the script's output. Non-empty ⇒ the prose names what actually
            // happened, and "check the script's output" is a wild goose chase.
            bool const isOwnVerdict = result.diagnostic.empty();
            emitScriptError(
                rep, DiagnosticCode::D_ScriptExitedNonZero,
                "build script '" + argv0 + "' ran and exited with status "
                + std::to_string(result.exitCode)
                + (isOwnVerdict
                       ? ". The program was found and executed, so this is "
                         "the script's own verdict, not an environment "
                         "problem — fix what it reported on this build's "
                         "inherited stdout/stderr and re-run."
                       : ", which is NOT a status the script chose: "
                         + result.diagnostic
                         + ". The program was found and executed, so this is "
                           "not a missing-interpreter problem either — the "
                           "run itself did not complete normally.")
                + " Non-zero stops the build: a hook that failed must not be "
                  "followed by a compile of stale inputs.");
            return false;
        }

        // Success: fall through to the next entry. Nothing is reported on the
        // happy path — a build that ran its hooks correctly says nothing about
        // them, exactly as it says nothing about a successful compile.
    }
    return true;
}

} // namespace dss
