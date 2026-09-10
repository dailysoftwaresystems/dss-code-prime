// ★★★ THE `.dsshir` FORMAT HAD A WRITER, A READER, AND NO CONSUMER OF THE
// READER — SO NOTHING EVER ASKED WHETHER THE TWO AGREED.
//
// `emitHir` ships behind `--emit-hir`. `parseHir` ships as a `DSS_EXPORT` for a
// consumer outside this repository. Between them the format claims a contract
// (`hir_text.hpp`): `emitHir(parseHir(emitHir(h)))` reproduces the same bytes.
// Until this file, the only thing that ever exercised that contract over real
// programs was a PERSON — and when one finally did, the corpus gave up THREE
// shipped writer spellings the shipped reader could not read, each failing
// differently and each pre-existing
// (D-HIR-TEXT-NODE-WALK-RECURSES-PER-LEVEL-ON-BOTH-HALVES-AND-THREE-SPELLINGS-DO-NOT-READ-BACK):
//   * a VLA bound `arr<T, -2>` — ABORTED the reading process;
//   * `goto *<expr>` — the reader's lexer had no `*` token at all;
//   * `lit float 18446744073709551616` (2^64) — came back as 0.0 THROUGH A
//     CLEAN REPORTER, the silent one, and therefore the worst.
// All three exited 0 from `--emit-hir`. This file is the run that would have
// caught all three, standing where the person stood.
//
// ── WHY THE PREDICATE IS BYTE IDENTITY AND NOT "IT PARSES" ──────────────────
//
// ⚠⚠ THE CHECK THAT ALREADY EXISTED WAS THE WEAKER ONE, AND IT WOULD HAVE
// PASSED THE SILENT DEFECT. `tests/program/test_emit_hir_mode.cpp` reads every
// artifact back through `parseHir` and asserts `HirParseResult::ok` — which is
// exactly what the 2^64 float satisfied while handing back a different number.
// A successful parse says the reader found no rule it could not apply; it says
// nothing about whether the module it built is the module the writer meant.
// Re-emitting and comparing bytes is the only predicate that can see the
// difference, because it compares the reader's reconstruction against the
// writer's intent one token at a time.
//
// ── WHY IT LOOPS, AND WHY THAT DOES NOT VIOLATE per-example ─────────────────
//
// The claim here is UNIVERSAL over the corpus ("every artifact this compiler
// writes reads back"), and the P24 ruling is that a universal claim is
// per-example because *"a loop reports the first failure and hides the rest"*.
// The concern is the HIDING, not the loop: every arm below is NON-FATAL
// (`ADD_FAILURE` / `EXPECT_`), so one broken spelling names itself and the walk
// continues to name every other. A regression that hits forty examples reports
// forty names, which is what makes the failure diagnosable — and it costs ONE
// front-end run per example instead of one PROCESS per example, over a corpus
// that is already ~800 entries in `tests/examples`.
//
// ── WHY IT DOES ITS OWN ROUND TRIP RATHER THAN TRUSTING rc 0 ───────────────
//
// ★★ `Program::emitHirText` now verifies its OWN artifact and refuses (rc 2) if
// it does not round-trip, which is the production half of this closure. If this
// gate merely asserted rc 0 it would be a check on a check: delete the CALL in
// `emitHirText` and every example is rc 0 again, and this file would go GREEN
// over a broken codec — the disarmed-guard shape this project keeps paying for.
// So the round trip below is ALSO performed here, on the artifact bytes, by
// calling the predicate directly. The gate then fails on its own if the mode
// stops asking.
//
// ⚠ AND THE PREDICATE ITSELF IS PINNED FROM INSIDE THIS GATE, FIRST, BY NAME.
// Sharing one implementation with the product is right — a second copy of the
// round trip in a test is a second thing to drift — but it leaves this walk
// vacuous under one mutation: neuter `hirArtifactRoundTripFailure` to always
// return clean and 800 examples "pass" while measuring nothing. So the first
// arm below is a NEGATIVE CONTROL: a synthetic artifact known to be unreadable,
// which the predicate must REFUSE before a single example is walked. A gate
// that cannot show its own instrument still fires is not a gate.
//
// ── ACCOUNTING, BECAUSE A SKIP IS NOT A PASS ────────────────────────────────
//
// An example whose front end DECLINES (rc 1 — the corpus deliberately carries
// sources that must be diagnosed) produces no artifact, so it makes no claim
// here. That is legitimate, and it is also exactly how this gate could rot into
// measuring nothing, so declines are COUNTED, PRINTED, and floored: the number
// of examples that actually emitted must clear `kEmittedFloor`. And a decline
// can never be a round-trip failure in disguise, because the two have different
// exit codes by construction — that is what rc 2 is for.
//
// ── WHAT IT COSTS, MEASURED, AND WHY IT BELONGS IN THE DEFAULT SUITE ────────
//
// ✔MEASURED 2026-09-09 (Windows, Ninja + mingw-w64 g++ 13.2.0 Debug, build
// `hc`): this entry takes **232.9 s / 252.7 s** across two runs — ~0.3 s per
// example, of which ✔MEASURED ~93% is the FRONT END (preprocess, parse,
// semantic analysis, HIR lowering) that this gate must run to have an artifact
// at all; the two round trips it performs per example (the mode's own, and this
// file's independent one) are ~7% between them.
//
// That is a real cost and it was weighed rather than waved through, TWICE.
//   * AGAINST THE EXISTING TAIL. `analysis/preprocess/test_preprocessor_shuffled`
//     measured 248.3 s and 229.3 s across the same two runs. ⚠ The two entries
//     TRADE PLACES between runs, so the honest statement is NOT "this one is
//     below the long pole" — it is that the two are the same size, and the
//     suite already carries one of them.
//   * AGAINST THE WALL CLOCK, WHICH IS THE NUMBER THAT ACTUALLY DECIDES.
//     ✔MEASURED whole-suite `ctest -j 4`: 1404.6 s over 2154 entries WITHOUT
//     this file, 1279.9 s over 2155 entries WITH it. The delta is NEGATIVE and
//     therefore inside run-to-run variance: a ~250 s entry scheduled among 2155
//     under `-j 4` absorbs into the parallel tail rather than extending it.
// So it belongs in the default set rather than behind a label. A label would
// have made it a check somebody has to remember to run, and the whole subject
// of this file is a property that went unmeasured for want of anyone
// remembering to look.
//
// HOST-INDEPENDENT: the front end runs for every example, but nothing is
// codegen'd, linked, assembled or executed, so every arm is valid on every leg.

#include "program/program.hpp"   // Program::emitHirText, hirArtifactRoundTripFailure
#include "repo_root.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>   // std::sort — the walk order is stable across hosts
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using dss::Program;
using dss::test::repoRoot;

namespace {

// ── The floor that keeps this gate from measuring nothing ───────────────────
//
// ✔MEASURED 2026-09-09 at base `de1e83ef`: 809 manifests, of which 6 drive a
// `.dss-project.json` and are skipped structurally ⇒ 803 cases; **779 emitted
// and round-tripped BYTE FOR BYTE, 24 declined by the front end, 0 refused.**
//
// ★★ AND THE 24 DECLINES ARE NOT AN UNEXPLAINED RESIDUE — THEY ARE EXACTLY THE
// EXAMPLES THE CORPUS ITSELF DECLARES MUST BE DIAGNOSED. ✔MEASURED: every one
// of the 24 carries `expectDiagnostics` in its own manifest, so the set
// difference *declined \ expectDiagnostics* is EMPTY. Nothing is being dropped
// for a reason nobody wrote down. (The converse is not equality and should not
// be: 10 further `expectDiagnostics` examples emit HIR happily, because their
// diagnostic comes from the LINK or the format, and `--emit-hir` stops at the
// front end — the doc's *"a translation unit that would not link is a normal
// input"*, visible here as data.)
//
// The floor sits BELOW the 779 rather than at it, because a new example that
// must be DIAGNOSED is a legitimate addition that leaves the emitted count flat
// while the case count grows. It is close enough that losing a class of
// programs cannot hide.
//
// ⚠ RAISE IT WHEN THE CORPUS GROWS; NEVER LOWER IT TO GET GREEN. A drop below
// this number means either the corpus shrank or `--emit-hir` stopped accepting
// a class of programs, and both are things a person must look at.
constexpr std::size_t kEmittedFloor = 770;

// One example, reduced to the three facts this gate needs.
struct ExampleCase {
    std::string              name;     // the directory name, which is the id
    std::vector<std::string> sources;  // absolute paths, one CU
    std::string              target;   // the spec the example declares first
};

// ⓘ THE TARGET COMES FROM THE MANIFEST, NOT FROM A CONSTANT, AND THAT IS A
// COVERAGE DECISION. HIR is target-dependent by design (docs/hir-text-format.md
// §2.2), so a fixed spec would emit every arch-specific example — inline asm
// above all — against a target it was not written for and collect a decline
// instead of an artifact. The example's own first target is the one whose HIR
// it exists to produce. Every declared spec RESOLVES on every leg (nothing is
// assembled or linked here), so this stays host-independent.
[[nodiscard]] std::vector<ExampleCase> collectExamples() {
    std::vector<ExampleCase> cases;
    fs::path const           root = repoRoot() / "examples" / "c";
    std::vector<fs::path>    dirs;
    for (auto const& e : fs::directory_iterator{root}) {
        if (e.is_directory() && fs::exists(e.path() / "expected.json")) {
            dirs.push_back(e.path());
        }
    }
    // Sorted, so the accounting this test prints is stable across hosts and a
    // diff between two runs is a diff in the SUBJECT, never in the order a
    // directory happened to enumerate in.
    std::sort(dirs.begin(), dirs.end());

    for (auto const& dir : dirs) {
        fs::path const manifest = dir / "expected.json";
        std::ifstream  in{manifest, std::ios::binary};
        // FAIL LOUD: an unreadable manifest is a broken corpus, never a skip.
        if (!in) {
            ADD_FAILURE() << "could not open " << manifest.string();
            continue;
        }
        nlohmann::json j;
        try {
            in >> j;
        } catch (std::exception const& e) {
            ADD_FAILURE() << manifest.string() << ": JSON parse failed: "
                          << e.what();
            continue;
        }
        // PROJECT MODE names a `.dss-project.json` instead of sources. It is a
        // different driver entry (`compileProject`), and `--emit-hir` is a
        // one-TU mode by construction, so there is nothing here to emit. Skipped
        // STRUCTURALLY — counted apart from a front-end decline, because the two
        // mean different things.
        if (j.contains("project")) continue;

        ExampleCase c;
        c.name = dir.filename().string();
        if (j.contains("sources")) {
            for (auto const& s : j.at("sources")) {
                c.sources.push_back((dir / s.get<std::string>()).string());
            }
        } else if (j.contains("source")) {
            c.sources.push_back(
                (dir / j.at("source").get<std::string>()).string());
        }
        if (c.sources.empty()) {
            ADD_FAILURE() << c.name
                          << ": manifest names neither 'source' nor 'sources' "
                             "nor 'project'";
            continue;
        }
        if (!j.contains("targets") || j.at("targets").empty()) {
            ADD_FAILURE() << c.name << ": manifest declares no targets";
            continue;
        }
        c.target = j.at("targets").at(0).at("spec").get<std::string>();
        cases.push_back(std::move(c));
    }
    return cases;
}

// A syntactically well-formed `.dsshir` header over a module body carrying a
// token the grammar has no rule for. Stands in for the historical `goto *<expr>`
// class — a spelling the writer produced and the reader's lexer could not even
// tokenize — and exists so this gate can prove its own instrument still fires.
constexpr std::string_view kUnreadableArtifact =
    "dsshir 3\n"
    "producer \"negative-control\"\n"
    "module \"C\" {\n"
    "  \x01\x02 not a production this grammar has\n"
    "}\n";

} // namespace

// ── The gate ────────────────────────────────────────────────────────────────

TEST(EmitHirRoundTripsEveryExample, EveryArtifactTheWriterProducesReadsBackByteForByte) {
    // ── THE NEGATIVE CONTROL, BEFORE ANY EXAMPLE IS WALKED ──────────────────
    // Everything below this line reports success by NOT finding a difference,
    // which is exactly the shape that reads identically whether the instrument
    // works or has been neutered. So the instrument is made to fire first, on
    // an artifact known to be unreadable. If this passes silently, the ~800
    // green results underneath it mean nothing.
    ASSERT_FALSE(dss::hirArtifactRoundTripFailure(kUnreadableArtifact).empty())
        << "the round-trip predicate accepted an artifact carrying a token the "
           "grammar has no rule for. The instrument is not measuring anything, "
           "so every example this gate reports as green is vacuous.";

    auto const cases = collectExamples();
    ASSERT_FALSE(cases.empty())
        << "the examples corpus enumerated ZERO cases — this gate would have "
           "reported success while measuring nothing";

    std::size_t              emitted = 0;
    std::vector<std::string> declined;    // rc 1: the front end said no
    std::vector<std::string> refused;     // rc 2: the product caught it itself

    for (auto const& c : cases) {
        Program            p;
        std::ostringstream artifact;      // `-` routes the artifact here
        std::ostringstream err;
        int const rc = p.emitHirText(c.sources, "c", c.target, "-", artifact,
                                     err);
        if (rc == 2) {
            // The shipped mode's OWN verification refused. That is the product
            // working — and it is still a red here, because a corpus example
            // whose HIR cannot be serialized is precisely the defect this gate
            // exists to surface. Its message already names the reason.
            refused.push_back(c.name);
            ADD_FAILURE() << c.name << " [" << c.target
                          << "]: `--emit-hir` REFUSED its own artifact (rc 2) — "
                             "the writer produced text this build's reader "
                             "cannot take back.\n"
                          << err.str();
            continue;
        }
        if (rc != 0) {
            // A DECLINE. The corpus carries sources that must be diagnosed, so
            // this is legitimate — but it is counted and printed, never
            // silently dropped, and the floor below is what stops a wholesale
            // decline from reading as success.
            declined.push_back(c.name);
            continue;
        }
        ++emitted;
        std::string const text = artifact.str();
        EXPECT_FALSE(text.empty())
            << c.name << ": rc 0 with an EMPTY artifact — every emission "
                         "carries at least the two header lines";
        if (text.empty()) continue;
        // THE CLAIM, asked here rather than inferred from rc 0 — so this gate
        // still fails if the mode ever stops asking.
        std::string const why = dss::hirArtifactRoundTripFailure(text);
        EXPECT_TRUE(why.empty())
            << c.name << " [" << c.target << "]:\n" << why;
    }

    // The accounting, on stdout, in the shape the corpus harnesses already use.
    std::cout << "[ emit-hir round trip ] cases " << cases.size()
              << " · emitted+round-tripped " << emitted
              << " · declined by the front end " << declined.size()
              << " · refused by the mode's own check " << refused.size()
              << "\n";
    for (auto const& d : declined) {
        std::cout << "[ emit-hir round trip ]   declined: " << d << "\n";
    }

    // ⚠ NON-VACUITY, LAST AND DELIBERATELY NOT FIRST. Everything above can pass
    // while measuring nothing if emission stopped working corpus-wide; this is
    // the assertion that notices.
    EXPECT_GE(emitted, kEmittedFloor)
        << "only " << emitted << " of " << cases.size()
        << " examples emitted HIR — below the floor of " << kEmittedFloor
        << ". Either the corpus shrank or `--emit-hir` stopped accepting a "
           "class of programs; this gate is not measuring what it claims to.";
}
