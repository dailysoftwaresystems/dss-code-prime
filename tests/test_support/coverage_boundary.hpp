#pragma once

// ★★★ THE COVERAGE BOUNDARY BETWEEN THE TWO CORPUS RUNNERS — STATED, EMITTED
// AND JUDGED IN ONE PLACE.
//
// D-TEST-INTEGRATED-RUNNER-BUILDS-ONLY-THE-HOST-RUNNABLE-SPEC-SO-ONE-RUNNER-SEES-A-CAPABILITY
//
// ── THE BOUNDARY, AS A SENTENCE ──────────────────────────────────────────────
//
//   BOTH runners COMPILE the target this host can execute, and both RUN it.
//   Only the IN-PROCESS runner (tests/examples/examples_runner) compiles a
//   target whose `runOn` excludes this host. NEITHER runner spawns a binary the
//   host cannot execute.
//
// ⇒ **the cross-target half of every capability is witnessed by the in-process
// runner ALONE.** That is the sentence this header exists to make checkable.
//
// ── WHY A HEADER AND NOT A COMMENT ───────────────────────────────────────────
//
// The rule this project holds — a capability change must reach BOTH example
// runners, because one enforcing while its sibling shrugs is a silent harness
// bug ([[D-EXAMPLES-RUNNER-TWO-RUNNERS-MUST-AGREE]]) — has a MEASURED exception
// that was never written down. ✔MEASURED by the P30 asm lane and re-measured
// here: a red-on-disable mutant that dropped arm64's width-view letter was
// caught by one runner and not the other, because the CLI-subprocess runner
// never compiles an arm64 spec on a Windows host. The asymmetry is INTENDED —
// the CLI runner's own header and `integrated_tests/CMakeLists.txt` both call it
// a user invariant of 2026-06-02, "always against the CURRENT host platform" —
// and a CLI-subprocess harness that pretended to exec a foreign-arch binary
// would be worse than one that declines.
//
// ⚠ AND THE COMPLEMENT *WAS* WRITTEN DOWN — this file does not claim otherwise.
// ✔MEASURED 2026-08-24: `examples/README.md` tells corpus authors that "the
// in-process runner COMPILES every declared target on every host" while "the CLI
// runner instead binds … and ledgers the rest". What was missing is that NOTHING
// CHECKED IT, that neither RUNNER's own source said it in compile-vs-run terms,
// and that the shared ledger below could not express it. A boundary that is
// described but unchecked is how a capability ships half-tested while both
// suites read green — prose rots, and this one already has: the same README
// still states the CLI runner's SUPERSEDED binding rule ("the FIRST target whose
// runOn matches"), replaced by `selectBoundTargetIndex` on 2026-08-17.
//
// ★★ AND THE SHARED VOCABULARY HID IT. Both runners record a `runOn`-excluded
// arm as `ArmVerdict::SkippedByRunOn`, and the token means two DIFFERENT things:
// in-process it means *compiled, artifact produced, not spawned*; on the CLI it
// means *never compiled at all*. The one instrument that spans both harnesses
// could not tell the two apart, which is exactly why nothing noticed. This
// header adds the missing fact — WAS IT COMPILED — as a first-class observation
// each runner emits about itself.
//
// ── WHAT EACH RUNNER EMITS ───────────────────────────────────────────────────
//
//   [coverage-boundary] runner=<r> example=<id> declared=<set>
//                       compiled=<set> spawned=<set> ran=<set>
//
// (ONE physical line; wrapped here only so this comment fits. ⚠ The field list
// and its ORDER are the parser's closed set — `kKeys` below is the authority,
// and a reader who needs the grammar should read THAT, not this sentence.)
//
// One line per example, from each runner, in its own voice. The EMITTER and the
// PARSER are both here, so the two runners cannot drift into two grammars — the
// same reason `stage_tree.hpp` and `arm_verdict_ledger.hpp` exist, and the
// reason a "two copies held byte-equal" lint is the wrong shape for this.
//
// AGNOSTIC: nothing here names an arch, an OS, a format or an emulator. A spec
// is an opaque token the MANIFEST chose; the host-excluded set is computed by
// the caller from the manifest's own `runOn` lists.
//
// SELF-CONTAINED ON PURPOSE, for the same reason `arm_verdict_ledger.hpp` is:
// its two consumers are binaries with disjoint link sets — `dss_examples_runner`
// links the compiler library plus GTest, `integrated_tests` links
// nlohmann_json alone and drives the compiler as a subprocess — so a shared
// home that needed either of those would not be shared. Standard library only.

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace dss::test_support {

// The line marker. ONE definition: a runner writes it, the guard looks for it.
inline constexpr std::string_view kCoverageBoundaryPrefix = "[coverage-boundary]";

// The two runner names, spelled once. A report whose `runner` is neither is
// REFUSED by the parser rather than accepted as some third harness nobody
// wired — an unknown author would let a guard compare a line against itself.
inline constexpr std::string_view kCoverageRunnerInProcess = "in-process";
inline constexpr std::string_view kCoverageRunnerCli       = "cli-subprocess";

// ⚠ THE EMPTY SET IS SPELLED, NOT OMITTED. An absent field and an empty one
// must not look alike: a parser that accepted `compiled=` would read a runner
// that built NOTHING exactly like a runner whose line was truncated mid-write.
inline constexpr std::string_view kCoverageEmptySet = "-";

struct CoverageReport {
    std::string runner;     // which harness OBSERVED this — the line's author
    std::string exampleId;  // `<lang>/<name>`, the id both runners already print
    // Every target spec the MANIFEST declares. Both runners read the same
    // manifest, so a disagreement here is an instrument fault, not a finding.
    std::vector<std::string> declared;
    // Specs for which this runner PRODUCED AN ARTIFACT. This is the fact the
    // arm-verdict vocabulary could not express, and the whole point of the file.
    std::vector<std::string> compiled;
    // Specs whose artifact this runner ATTEMPTED to spawn — the attempt, not
    // its outcome.
    //
    // ★★★ THIS SET EXISTS BECAUSE A CLAUSE THAT KEYED ON `ran` COULD NOT FIRE,
    // AND THAT WAS MEASURED, NOT REASONED. Red-on-disable arm M4 deleted the
    // in-process runner's `runOn` gate — the "let's just run everything" change
    // clause C4 exists to catch — and the guard stayed GREEN: on a Windows host
    // the foreign-format artifact cannot be spawned at all, so the deleted gate
    // produced a FAILED spawn (`Poisoned`) rather than a run, and `ran` never
    // grew. ⇒ the boundary's real claim is about the ATTEMPT. A harness that
    // tries to exec a foreign binary does not fail cleanly; it produces a corpus
    // of unattributable exit codes, which is the damage, whether or not any
    // single attempt happens to succeed.
    std::vector<std::string> spawned;
    // Specs whose artifact this runner spawned AND which actually ran.
    std::vector<std::string> ran;
};

[[nodiscard]] inline std::string
renderCoverageSet(std::vector<std::string> const& specs) {
    if (specs.empty()) return std::string{kCoverageEmptySet};
    std::string out;
    for (auto const& s : specs) {
        if (!out.empty()) out += ',';
        out += s;
    }
    return out;
}

[[nodiscard]] inline std::string renderCoverageLine(CoverageReport const& r) {
    return std::string{kCoverageBoundaryPrefix} + " runner=" + r.runner
         + " example=" + r.exampleId
         + " declared=" + renderCoverageSet(r.declared)
         + " compiled=" + renderCoverageSet(r.compiled)
         + " spawned=" + renderCoverageSet(r.spawned)
         + " ran=" + renderCoverageSet(r.ran);
}

namespace detail {

[[nodiscard]] inline std::vector<std::string> splitOn(std::string_view s,
                                                      char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

[[nodiscard]] inline bool contains(std::vector<std::string> const& v,
                                   std::string const& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// The set difference `a \ b`, order-preserving so a message names specs in the
// order the manifest declared them.
[[nodiscard]] inline std::vector<std::string>
minus(std::vector<std::string> const& a, std::vector<std::string> const& b) {
    std::vector<std::string> out;
    for (auto const& x : a) {
        if (!contains(b, x)) out.push_back(x);
    }
    return out;
}

[[nodiscard]] inline std::string join(std::vector<std::string> const& v) {
    return renderCoverageSet(v);
}

}  // namespace detail

// ── THE PARSER, WHICH IS ALSO THE INSTRUMENT'S OWN HONESTY CHECK ─────────────
//
// ★ IT REFUSES A MALFORMED LINE RATHER THAN SALVAGING ONE. Every rule below
// exists because its violation would let a guard compare something that is not
// an observation:
//   * a missing or reordered field  -> a torn write read as a smaller coverage;
//   * an UNKNOWN field              -> a producer that has moved on from this
//                                      grammar while the consumer keeps parsing
//                                      the prefix it still recognises;
//   * an unknown `runner`           -> a line whose author nobody wired;
//   * `compiled` outside `declared` -> a runner reporting coverage of a spec its
//                                      manifest never declared;
//   * `spawned` outside `compiled`  -> a spawn with no build behind it, which is
//                                      the one shape that cannot be true;
//   * `ran` outside `spawned`       -> an outcome from an attempt nobody made.
//
// ⚠ THE FIELD LIST IS CLOSED AND ORDERED. Both halves matter: a closed set
// catches a producer that grew a field, and a fixed order makes a truncated
// line fail on the count instead of on a coincidence.
[[nodiscard]] inline bool parseCoverageLine(std::string_view line,
                                            CoverageReport& out,
                                            std::string& why) {
    // Tolerate a trailing CR: the guard reads a file a child process wrote, and
    // on Windows that text arrives with CRLF.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    if (line.rfind(kCoverageBoundaryPrefix, 0) != 0) {
        why = "line does not start with " + std::string{kCoverageBoundaryPrefix};
        return false;
    }
    line.remove_prefix(kCoverageBoundaryPrefix.size());
    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);

    auto const fields = detail::splitOn(line, ' ');
    static constexpr std::string_view kKeys[] = {"runner", "example", "declared",
                                                 "compiled", "spawned", "ran"};
    constexpr std::size_t kFieldCount = sizeof(kKeys) / sizeof(kKeys[0]);
    if (fields.size() != kFieldCount) {
        why = "expected " + std::to_string(kFieldCount) + " fields, read "
            + std::to_string(fields.size()) + " — a torn or extended line";
        return false;
    }
    std::string values[kFieldCount];
    for (std::size_t i = 0; i < kFieldCount; ++i) {
        auto const eq = fields[i].find('=');
        if (eq == std::string::npos) {
            why = "field " + std::to_string(i) + " ('" + fields[i]
                + "') is not <key>=<value>";
            return false;
        }
        std::string const key = fields[i].substr(0, eq);
        if (key != kKeys[i]) {
            why = "field " + std::to_string(i) + " is '" + key + "', expected '"
                + std::string{kKeys[i]}
                + "' — the coverage grammar is CLOSED and ORDERED";
            return false;
        }
        values[i] = fields[i].substr(eq + 1);
        if (values[i].empty()) {
            why = "field '" + key
                + "' is empty; an empty SET is spelled '"
                + std::string{kCoverageEmptySet} + "'";
            return false;
        }
    }

    auto readSet = [&](std::string const& v, std::vector<std::string>& dst,
                       char const* what) {
        dst.clear();
        if (v == kCoverageEmptySet) return true;
        for (auto& s : detail::splitOn(v, ',')) {
            if (s.empty()) {
                why = std::string{what} + " holds an empty spec";
                return false;
            }
            if (detail::contains(dst, s)) {
                why = std::string{what} + " names spec '" + s + "' twice";
                return false;
            }
            dst.push_back(std::move(s));
        }
        return true;
    };

    out = CoverageReport{};
    out.runner    = values[0];
    out.exampleId = values[1];
    if (out.runner != kCoverageRunnerInProcess
        && out.runner != kCoverageRunnerCli) {
        why = "runner='" + out.runner + "' is neither '"
            + std::string{kCoverageRunnerInProcess} + "' nor '"
            + std::string{kCoverageRunnerCli} + "'";
        return false;
    }
    if (!readSet(values[2], out.declared, "declared")) return false;
    if (!readSet(values[3], out.compiled, "compiled")) return false;
    if (!readSet(values[4], out.spawned, "spawned")) return false;
    if (!readSet(values[5], out.ran, "ran")) return false;

    if (out.declared.empty()) {
        why = "declared is empty — a manifest with no target declares no work,"
              " so nothing below could be judged";
        return false;
    }
    if (auto const stray = detail::minus(out.compiled, out.declared);
        !stray.empty()) {
        why = "compiled names " + detail::join(stray)
            + ", which the manifest never declared";
        return false;
    }
    if (auto const stray = detail::minus(out.spawned, out.compiled);
        !stray.empty()) {
        why = "spawned names " + detail::join(stray)
            + ", which this runner never compiled — a spawn with no build"
              " behind it";
        return false;
    }
    if (auto const stray = detail::minus(out.ran, out.spawned);
        !stray.empty()) {
        why = "ran names " + detail::join(stray)
            + ", which this runner never attempted to spawn — an outcome from"
              " an attempt nobody made";
        return false;
    }
    return true;
}

// Find the ONE coverage line a runner emitted for `exampleId`, in a captured
// output stream.
//
// ⚠ "EXACTLY ONE" IS THE POINT, not a convenience. Zero lines means the runner
// did not report — the instrument observed nothing, and the safe reading of an
// instrument that observed nothing is that it did not run. Two lines means the
// stream holds two examples' observations and picking either would be a guess.
[[nodiscard]] inline bool findCoverageReport(std::string const& body,
                                             std::string const& exampleId,
                                             CoverageReport& out,
                                             std::string& why) {
    std::size_t found = 0;
    std::size_t at    = 0;
    std::string firstParseFailure;
    while (at <= body.size()) {
        auto const nl = body.find('\n', at);
        std::string_view const line{body.data() + at,
                                    (nl == std::string::npos ? body.size() : nl)
                                        - at};
        at = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        if (line.find(kCoverageBoundaryPrefix) == std::string_view::npos) {
            continue;
        }
        CoverageReport r;
        std::string    lineWhy;
        if (!parseCoverageLine(line, r, lineWhy)) {
            // A malformed line carrying the marker is a HARD refusal, never a
            // skipped candidate: silently stepping over it is how a producer
            // that broke its own grammar reads as a producer that said nothing.
            why = "malformed coverage line (" + lineWhy + "): "
                + std::string{line};
            return false;
        }
        if (r.exampleId != exampleId) continue;
        ++found;
        out = std::move(r);
    }
    if (found == 0) {
        why = "no coverage line for example '" + exampleId
            + "' — the runner reported nothing, and an instrument that observed"
              " nothing must not be read as one that observed a small coverage";
        return false;
    }
    if (found > 1) {
        why = std::to_string(found) + " coverage lines name example '"
            + exampleId + "' — refusing to guess which observation is the run";
        return false;
    }
    return true;
}

// ── THE CLAUSES ──────────────────────────────────────────────────────────────

struct ClauseVerdict {
    std::string clause;  // the boundary half this verdict is about
    bool        ok = false;
    std::string detail;  // ALWAYS populated, pass or fail — a passing clause
                         // that says what it saw is what makes a vacuous pass
                         // visible instead of silent.
};

// Judge one example's two observations against the boundary stated at the top of
// this file. `hostExcluded` is the set of declared specs whose `runOn` does NOT
// admit this host — a manifest fact the caller computes, never inferred here.
//
// ★ WHAT THIS DELIBERATELY DOES NOT FORBID: it never requires the CLI runner to
// DECLINE anything. Teaching that runner to compile every declared spec would
// leave every clause below green — the guard forbids a LOSS of coverage, not a
// gain. A guard that fired on an improvement would be a ratchet pointing the
// wrong way.
[[nodiscard]] inline std::vector<ClauseVerdict>
judgeCoverageBoundary(CoverageReport const& inproc, CoverageReport const& cli,
                      std::vector<std::string> const& hostExcluded) {
    std::vector<ClauseVerdict> out;

    // C1 — the two runners must be looking at the SAME declared work. Every
    // clause below compares one runner's coverage against the other's; if they
    // disagree about what the manifest says, none of those comparisons means
    // anything, so this is judged FIRST and its failure explains the rest.
    {
        ClauseVerdict v{"C1 SAME-DECLARED-WORK", true, {}};
        if (inproc.exampleId != cli.exampleId) {
            v.ok = false;
            v.detail = "the two reports name different examples ('"
                     + inproc.exampleId + "' vs '" + cli.exampleId + "')";
        } else {
            auto const onlyIn  = detail::minus(inproc.declared, cli.declared);
            auto const onlyCli = detail::minus(cli.declared, inproc.declared);
            if (!onlyIn.empty() || !onlyCli.empty()) {
                v.ok = false;
                v.detail = "the runners disagree about the manifest's targets:"
                           " in-process-only=" + detail::join(onlyIn)
                         + " cli-only=" + detail::join(onlyCli);
            } else {
                v.detail = "both runners read the same "
                         + std::to_string(inproc.declared.size())
                         + " declared target spec(s): "
                         + detail::join(inproc.declared);
            }
        }
        out.push_back(std::move(v));
    }

    // C2 — the CLI runner's coverage is a SUBSET of its sibling's. This is the
    // boundary's shape: the CLI runner reaches a subset of the specs, never a
    // spec its sibling misses. It fails the moment the in-process runner stops
    // compiling (or running) something the CLI runner still does — which is the
    // direction in which a capability silently loses its only witness.
    {
        auto const compiledOnlyCli = detail::minus(cli.compiled, inproc.compiled);
        auto const ranOnlyCli      = detail::minus(cli.ran, inproc.ran);
        ClauseVerdict v{"C2 CLI-COVERAGE-IS-A-SUBSET", true, {}};
        if (!compiledOnlyCli.empty() || !ranOnlyCli.empty()) {
            v.ok = false;
            v.detail = "the CLI runner reached specs the in-process runner did"
                       " not: compiled-only=" + detail::join(compiledOnlyCli)
                     + " ran-only=" + detail::join(ranOnlyCli)
                     + ". The in-process runner is the SUPERSET half of the"
                       " boundary; a spec only it used to cover is now covered"
                       " by the CLI runner alone";
        } else {
            v.detail = "cli compiled=" + detail::join(cli.compiled)
                     + " ran=" + detail::join(cli.ran)
                     + ", both inside in-process compiled="
                     + detail::join(inproc.compiled)
                     + " ran=" + detail::join(inproc.ran);
        }
        out.push_back(std::move(v));
    }

    // C3 — UNION COMPLETENESS, the clause the whole file exists for. A declared
    // spec that NEITHER runner compiles has no witness anywhere, and both suites
    // still read green. The message names the covering runner for each spec,
    // because "which half covers this" is the question the boundary answers.
    {
        std::vector<std::string> uncovered;
        for (auto const& s : inproc.declared) {
            if (!detail::contains(inproc.compiled, s)
                && !detail::contains(cli.compiled, s)) {
                uncovered.push_back(s);
            }
        }
        ClauseVerdict v{"C3 EVERY-DECLARED-SPEC-IS-COMPILED-BY-SOMEONE", true, {}};
        if (!uncovered.empty()) {
            v.ok = false;
            v.detail = "no runner compiled " + detail::join(uncovered)
                     + " on this host. A capability's behaviour on those"
                       " spec(s) is now witnessed by NOBODY, with both corpus"
                       " suites still green — the exact failure this boundary is"
                       " checked to prevent";
        } else {
            auto const inprocOnly = detail::minus(inproc.compiled, cli.compiled);
            v.detail = "all " + std::to_string(inproc.declared.size())
                     + " declared spec(s) compiled; "
                     + std::to_string(inprocOnly.size())
                     + " by the in-process runner ALONE ("
                     + detail::join(inprocOnly) + ")";
        }
        out.push_back(std::move(v));
    }

    // C4 — neither runner even ATTEMPTS to spawn what this host cannot execute.
    // The other half of the boundary, and the half a well-meant "let's run
    // everything" change would break.
    //
    // ⚠ IT KEYS ON `spawned`, NOT ON `ran`, AND THE DIFFERENCE WAS MEASURED
    // RATHER THAN REASONED: keyed on `ran`, this clause could not fire on a
    // Windows host at all, because a foreign-format artifact fails to spawn and
    // so never becomes a run (see the `spawned` field's note). A clause that
    // cannot fire is worse than no clause — it reads as coverage.
    {
        std::vector<std::string> spawnedAnyway;
        for (auto const& s : hostExcluded) {
            if (detail::contains(inproc.spawned, s)) {
                spawnedAnyway.push_back("in-process:" + s);
            }
            if (detail::contains(cli.spawned, s)) {
                spawnedAnyway.push_back("cli:" + s);
            }
        }
        ClauseVerdict v{"C4 SPAWN-STAYS-HOST-LOCAL", true, {}};
        if (!spawnedAnyway.empty()) {
            v.ok = false;
            v.detail = "a runner ATTEMPTED to spawn a spec whose runOn excludes"
                       " this host: " + detail::join(spawnedAnyway)
                     + ". A harness that execs a foreign binary does not fail"
                       " cleanly — it produces exit codes nobody can attribute";
        } else {
            v.detail = std::to_string(hostExcluded.size())
                     + " declared spec(s) excluded by runOn ("
                     + detail::join(hostExcluded)
                     + "); neither runner tried to spawn any of them";
        }
        out.push_back(std::move(v));
    }

    // C5 — the OVERLAP is non-empty, so C2 compared something. This is the
    // clause that keeps the whole judgement from passing vacuously: a manifest
    // no runner could execute here would satisfy C1-C4 while proving nothing
    // about "both runners for the host target".
    {
        std::vector<std::string> const shared =
            detail::minus(cli.ran, detail::minus(cli.ran, inproc.ran));
        ClauseVerdict v{"C5 BOTH-RUNNERS-RAN-THE-HOST-TARGET", true, {}};
        if (shared.empty()) {
            v.ok = false;
            v.detail = "no spec was RUN by both runners (in-process ran "
                     + detail::join(inproc.ran) + ", cli ran "
                     + detail::join(cli.ran)
                     + "). With an empty overlap the subset clause above holds"
                       " trivially and this judgement asserts nothing";
        } else {
            v.detail = "both runners built and spawned " + detail::join(shared);
        }
        out.push_back(std::move(v));
    }

    return out;
}

}  // namespace dss::test_support
