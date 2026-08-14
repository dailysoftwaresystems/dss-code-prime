// `--suppress=<a protected code>` — the refused-request notice
// (`D_SuppressRequestIgnored`, 0xD021).
//
// THE DEFECT THESE PINS CLOSE. `--suppress=<code>` accepted any WELL-FORMED
// name of a REAL diagnostic: `parseSuppressCode` rejects only names resolving
// to `Unknown`/`None`, so a member of `kUnsuppressableCodes` parsed fine,
// landed in `policy.suppress`, and was then discarded by `applyPolicy`'s
// `isUnsuppressable` gate. The user asked for something, got nothing, and was
// told nothing. `buildReporter` now announces every such refusal.
//
// THE SHAPE OF THE CONTRACT, and it is TWO halves — a pin that checks only
// the first is the more dangerous kind of green, because the silent no-op
// already satisfied the second:
//   (1) the refusal is SAID, and
//   (2) NOTHING ELSE CHANGES — the build continues, the exit code is what it
//       was, and the protected code still emits exactly as before.
// So every arm below asserts the control as well as the effect.
//
// Pins:
//   * A protected code in `--suppress` produces the notice AND leaves the
//     run's exit code unchanged (end-to-end, through `Program::transpile`).
//   * The protected code is still reported afterwards — the failed
//     suppression really did nothing to it.
//   * An ORDINARY code in `--suppress` is still suppressed (the negative:
//     the new path must not swallow legitimate suppression).
//   * The notice is itself suppressible, and promotable to an error by
//     `--warnings-as-errors` — both directions, because its own
//     suppressibility is the deliberate design and not an oversight.
//   * A REFERENCE-COMPILER PROBE recording the gcc / clang / MSVC behaviour
//     the warn-and-continue decision rests on, so the conformance claim is
//     MEASURED IN-TREE rather than asserted in a comment.
//
// Every pin drives the SHIPPED projection — `parseCliArgs` (argv) →
// `buildReporterConfig` → `buildReporter` — never a hand-typed `Config`. A
// pin that re-types its subject's input is testing the stub: the whole defect
// lived in the seam between the parser that ACCEPTS the flag and the reporter
// that IGNORES it, and a hand-built config skips exactly that seam.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/unsuppressable_codes.hpp"
#include "program/cli_args.hpp"
#include "program/program.hpp"

#include "native_c_probe.hpp"   // findCompiler, locateMsvcToolchain, captureCmd, tailOf, ExecutedRows
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
namespace native_probe = dss::test_support::native_probe;
namespace fs           = std::filesystem;

namespace {

// gtest-friendly argv builder — mirrors the one in `test_cli_args.cpp`; the
// `std::string` storage must outlive the `parseCliArgs` call.
struct Argv {
    std::vector<std::string> storage;
    std::vector<char*>       ptrs;

    explicit Argv(std::initializer_list<std::string> args) {
        storage.assign(args.begin(), args.end());
        ptrs.reserve(storage.size() + 1);
        for (auto& s : storage) ptrs.push_back(s.data());
        ptrs.push_back(nullptr);
    }
    [[nodiscard]] int   argc() const noexcept { return static_cast<int>(storage.size()); }
    [[nodiscard]] char** argv() noexcept { return ptrs.data(); }
};

// argv → CliArgs → Config → DiagnosticReporter, through the shipped
// projection at every step. This is the subject's REAL input path; nothing
// below constructs a `DiagnosticReporter::Config` by hand.
[[nodiscard]] DiagnosticReporter reporterFor(std::initializer_list<std::string> args) {
    Argv a{args};
    auto parsed = parseCliArgs(a.argc(), a.argv());
    // A `CHECK`-style abort rather than ASSERT_*: this helper returns
    // non-void, so `ASSERT_*` cannot be used here (it expands to a bare
    // `return`), and a half-built reporter would fail the caller's pins for
    // the wrong reason.
    if (!parsed) {
        ADD_FAILURE() << "parseCliArgs rejected the pin's own argv: "
                      << parsed.error().detail;
        return DiagnosticReporter{};
    }
    return buildReporter(buildReporterConfig(*parsed));
}

[[nodiscard]] std::vector<DiagnosticCode> codesOf(DiagnosticReporter const& rep) {
    std::vector<DiagnosticCode> out;
    for (auto const& d : rep.all()) out.push_back(d.code);
    return out;
}

// The subject code used throughout: `D_PlanNotLanded` is a member of
// `kUnsuppressableCodes` AND has a live emit site reachable from a CLI mode
// (`Program::transpile`), which is what lets one pin cover "the notice fires"
// and "the protected code still emits" on the SAME run.
constexpr DiagnosticCode kProtected = DiagnosticCode::D_PlanNotLanded;

// A deliberately ORDINARY code — asserted non-member below rather than
// assumed, so this file's negative arm cannot silently become vacuous if the
// closed table ever adopts it.
constexpr DiagnosticCode kOrdinary = DiagnosticCode::P_DeprecatedSyntax;

// `parseCliArgs` requires at least one `--target` for every compile-producing
// mode, transpile included — MEASURED: without it the parse fails before the
// suppress policy is ever projected, which would make every pin below test
// the argument parser's rejection path instead of the refusal notice. The
// spec is inert here (transpile fails loud before any target is used); it is
// present so the argv is one a real operator could type.
constexpr char const* kTarget = "--target=x86_64:elf64-x86_64-linux";

// Capture `std::cerr` for the duration of a scope. The driver's
// `drainDiagnosticsToStderr` is how a real run shows the operator anything,
// so the end-to-end arm has to read what actually reaches the terminal.
class CerrCapture {
public:
    CerrCapture() : old_(std::cerr.rdbuf(buf_.rdbuf())) {}
    ~CerrCapture() { std::cerr.rdbuf(old_); }
    CerrCapture(CerrCapture const&)            = delete;
    CerrCapture& operator=(CerrCapture const&) = delete;
    [[nodiscard]] std::string str() const { return buf_.str(); }

private:
    std::ostringstream buf_;
    std::streambuf*    old_;
};

} // namespace

// ── The membership decision, pinned as a decision ────────────────────────

TEST(SuppressRequestIgnored, TheNoticeIsDeliberatelyNotItselfProtected) {
    // Not a tautology restating the table — the load-bearing DESIGN claim.
    // `D_SuppressRequestIgnored` fails `kUnsuppressableCodes`' own written
    // criterion in both prongs: suppressing "your suppression request was
    // ignored" ships NO wrong bytes (the protected code it is about still
    // emits — pinned below), and it hides no build failure (the build was
    // continuing either way). If a later cycle adds it to that table, this
    // reds and the two arms of `TheNoticeIsSuppressibleAndPromotable` red
    // with it, which is the intended coupling.
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::D_SuppressRequestIgnored))
        << "D_SuppressRequestIgnored must stay suppressable: it is ABOUT a "
           "protected code, it is not one.";
    EXPECT_TRUE(isUnsuppressable(kProtected))
        << "the pin's subject must be a protected code or every arm here is "
           "vacuous";
    EXPECT_FALSE(isUnsuppressable(kOrdinary))
        << "the pin's negative-arm code must NOT be protected or the "
           "legitimate-suppression arm proves nothing";
}

// ── (1) the refusal is SAID, and (2) nothing else changes ────────────────

TEST(SuppressRequestIgnored, ProtectedCodeWarnsAndTheRunContinuesUnchanged) {
    // END-TO-END, through a real CLI mode. `Program::transpile` emits
    // `D_PlanNotLanded` and returns 1 unconditionally, so it exercises the
    // whole contract on one run: the notice appears, the protected code is
    // STILL reported, and the exit code is the one the control arm produced.
    auto run = [](std::initializer_list<std::string> args) {
        Argv a{args};
        auto parsed = parseCliArgs(a.argc(), a.argv());
        // NOT `EXPECT_TRUE` followed by `*parsed`. A non-fatal assertion in a
        // value-returning lambda keeps going, and dereferencing a failed
        // `std::expected` is UB — MEASURED: the first draft of this pin
        // SEGFAULTED the whole binary on a missing `--target`, taking the
        // other six tests down as collateral and reporting the cause as an
        // exception rather than as the argv mistake it was.
        if (!parsed) {
            ADD_FAILURE() << "parseCliArgs rejected the pin's own argv: "
                          << parsed.error().detail;
            return std::pair<int, std::string>{-1, std::string{}};
        }
        auto const cfg = buildReporterConfig(*parsed);
        Program     p;
        CerrCapture cap;
        int const   rc = p.transpile({"probe.c"}, "c", {}, cfg);
        return std::pair<int, std::string>{rc, cap.str()};
    };

    auto const [rcControl, outControl] =
        run({"dss-code-prime", "--transpile", "probe.c", "--language", "c", kTarget});
    auto const [rcSuppress, outSuppress] =
        run({"dss-code-prime", "--transpile", "probe.c", "--language", "c", kTarget,
             "--suppress=D_PlanNotLanded"});

    // (2) THE EXIT CODE IS UNCHANGED. Asserted against the CONTROL RUN's own
    // rc, not against a typed-in `1` — if `transpile`'s exit convention ever
    // changes, this pin must keep testing "unchanged", not "still 1".
    EXPECT_EQ(rcSuppress, rcControl)
        << "asking to suppress a protected code must refuse the REQUEST, not "
           "the compilation — the exit code has to be exactly what the same "
           "run without the flag produced";

    // (1) THE REFUSAL IS SAID — and only when it was asked for.
    //
    // ⓘ Matched on the code NAME, not on the `D0021` hex prefix, and that is
    // MEASURED rather than stylistic: the driver's buffer-less renderer
    // (`program.cpp`'s `severityName(...) << "[" << diagnosticCodeName(...)`)
    // is the arm every driver-tier `D_*` takes, because it has no source
    // span to point at. `diagnosticCodePrefix`'s `D0021` spelling belongs to
    // `DiagnosticReporter::format`, which only positioned diagnostics reach.
    // The first draft asserted `warning[D0021]` and went RED against a
    // perfectly correct message.
    EXPECT_NE(outSuppress.find("warning[D_SuppressRequestIgnored]"),
              std::string::npos)
        << "no D_SuppressRequestIgnored notice reached stderr. stderr was:\n"
        << outSuppress;
    EXPECT_NE(outSuppress.find("--suppress=D_PlanNotLanded had no effect"),
              std::string::npos)
        << "the notice must name the code the operator actually typed. "
           "stderr was:\n"
        << outSuppress;
    EXPECT_EQ(outControl.find("D_SuppressRequestIgnored"), std::string::npos)
        << "a run that suppressed nothing must not produce the notice. "
           "stderr was:\n"
        << outControl;

    // The rationale is rendered FROM THE TABLE ROW, so the operator is told
    // WHY. Matched against `unsuppressableRationale` rather than a copy of
    // the sentence: a pin carrying its own copy of the text would pass while
    // the message and the table drifted apart, which is the exact failure the
    // promotion-to-data exists to prevent.
    auto const why = unsuppressableRationale(kProtected);
    ASSERT_FALSE(why.empty());
    EXPECT_NE(outSuppress.find(std::string{why}), std::string::npos)
        << "the notice must carry the rationale recorded on the table row.\n"
           "expected substring: "
        << why << "\nstderr was:\n"
        << outSuppress;

    // (2) THE PROTECTED CODE IS STILL EMITTED — in BOTH arms, identically.
    // This is the property the silent no-op accidentally had, and losing it
    // would be a far worse regression than the defect being fixed.
    EXPECT_NE(outControl.find("error[D_PlanNotLanded]"), std::string::npos)
        << "control run lost D_PlanNotLanded; the pin's premise is broken";
    EXPECT_NE(outSuppress.find("error[D_PlanNotLanded]"), std::string::npos)
        << "the refused --suppress must leave the protected code emitting "
           "exactly as before. stderr was:\n"
        << outSuppress;
}

TEST(SuppressRequestIgnored, ProtectedCodeStillLandsInTheReporterAfterTheNotice) {
    // The same second half of the contract at the reporter tier, where it can
    // be asserted EXACTLY rather than by substring: full-sequence equality on
    // the emitted codes, in order, plus the error count.
    auto rep = reporterFor({"dss-code-prime", "--transpile", "probe.c",
                            "--language", "c", kTarget, "--suppress=D_PlanNotLanded"});

    ASSERT_EQ(codesOf(rep),
              (std::vector<DiagnosticCode>{DiagnosticCode::D_SuppressRequestIgnored}))
        << "buildReporter must emit exactly one notice for one refused code";
    EXPECT_EQ(rep.errorCount(), 0u)
        << "the notice is a WARNING — it must not by itself fail the build";
    EXPECT_EQ(rep.warningCount(), 1u);

    // CHARACTER-EXACT on the whole message, not a substring: the format is
    // fully determined here, so the house rule is full equality (substring
    // matching is reserved for implementation-defined output). Note WHICH
    // half is spelled out and which is not — the template is the message
    // contract and belongs in the pin, while the RATIONALE is interpolated
    // from `unsuppressableRationale` rather than copied. A pin carrying its
    // own copy of the reason would go green while the message and the table
    // drifted apart, which is precisely the failure the rationale-as-data
    // promotion exists to prevent; it must read the same source the message
    // reads.
    EXPECT_EQ(rep.all()[0].actual,
              "--suppress=D_PlanNotLanded had no effect: D_PlanNotLanded is a "
              "protected diagnostic and cannot be suppressed: "
                  + std::string{unsuppressableRationale(kProtected)}
                  + ". It will still be reported; the build continues. Use "
                    "--suppress=D_SuppressRequestIgnored to silence this "
                    "notice, or --warnings-as-errors to make it fatal.")
        << "the notice must name the code TWICE (the flag the operator typed, "
           "then the subject), carry the table's reason, state that the code "
           "is still reported and the build continues, and offer BOTH controls";

    // Now the protected code emits, exactly as any tier would emit it.
    report(rep, kProtected, DiagnosticSeverity::Error, "the protected code");

    EXPECT_EQ(codesOf(rep),
              (std::vector<DiagnosticCode>{DiagnosticCode::D_SuppressRequestIgnored,
                                           kProtected}))
        << "the refused suppression must not have removed the protected code "
           "from the stream — that no-op is the ONE thing the old silent "
           "behaviour got right";
    EXPECT_EQ(rep.errorCount(), 1u);
}

// ── The negative: legitimate suppression must still work ─────────────────

TEST(SuppressRequestIgnored, AnOrdinarySuppressStillSuppressesAndSaysNothing) {
    // CONTROL — without the flag the ordinary code lands.
    {
        auto rep = reporterFor({"dss-code-prime", "--transpile", "probe.c",
                                "--language", "c", kTarget});
        ASSERT_TRUE(rep.all().empty())
            << "a run with no --suppress must build a silent reporter";
        report(rep, kOrdinary, DiagnosticSeverity::Warning, "ordinary");
        EXPECT_EQ(codesOf(rep), (std::vector<DiagnosticCode>{kOrdinary}));
    }
    // EFFECT — with the flag it is dropped, and NO notice is produced (the
    // code is not protected, so there is nothing to refuse).
    {
        auto rep = reporterFor({"dss-code-prime", "--transpile", "probe.c",
                                "--language", "c", kTarget,
                                "--suppress=P_DeprecatedSyntax"});
        EXPECT_TRUE(rep.all().empty())
            << "suppressing an ORDINARY code must not produce a notice — only "
               "a REFUSED request does";
        report(rep, kOrdinary, DiagnosticSeverity::Warning, "ordinary");
        EXPECT_TRUE(rep.all().empty())
            << "the new refusal path must not have broken legitimate "
               "suppression";
    }
}

// ── The notice's own controls, BOTH directions ───────────────────────────

TEST(SuppressRequestIgnored, TheNoticeIsSuppressibleAndPromotable) {
    // DIRECTION 1 — an operator who has read it can silence it. Note what is
    // being exercised: `buildReporter` GENERATES the notice for
    // `D_PlanNotLanded` and then `report()` DROPS it under the second
    // `--suppress`. The generation and the policy are separate steps and both
    // have to be right for this to come out empty.
    {
        auto rep = reporterFor({"dss-code-prime", "--transpile", "probe.c",
                                "--language", "c", kTarget, "--suppress=D_PlanNotLanded",
                                "--suppress=D_SuppressRequestIgnored"});
        EXPECT_TRUE(rep.all().empty())
            << "--suppress=D_SuppressRequestIgnored must silence the notice; "
               "an unsilenceable meta-warning would be the same defect one "
               "layer up";
    }
    // DIRECTION 2 — an operator who wants it fatal gets that from the
    // standard mechanism, rather than the compiler deciding for them (the
    // measured clang `-Werror,-Wunknown-warning-option` posture).
    {
        auto rep = reporterFor({"dss-code-prime", "--transpile", "probe.c",
                                "--language", "c", kTarget, "--suppress=D_PlanNotLanded",
                                "--warnings-as-errors"});
        ASSERT_EQ(rep.all().size(), 1u);
        EXPECT_EQ(rep.all()[0].code, DiagnosticCode::D_SuppressRequestIgnored);
        EXPECT_EQ(rep.all()[0].severity, DiagnosticSeverity::Error)
            << "--warnings-as-errors must promote the notice like any other "
               "warning";
        EXPECT_EQ(rep.errorCount(), 1u)
            << "promotion has to reach errorCount, or `--warnings-as-errors` "
               "would report an error that does not fail the build";
    }
}

TEST(SuppressRequestIgnored, EveryRefusedCodeGetsItsOwnNoticeInDeterministicOrder) {
    // N refused codes → N notices, ordered by numeric code value. The order
    // matters because `policy.suppress` is an `unordered_set`: iterating it
    // directly would make this stream vary run to run, which no full-sequence
    // pin could hold and no operator could diff between two builds.
    auto rep = reporterFor({"dss-code-prime", "--transpile", "probe.c",
                            "--language", "c", kTarget,
                            "--suppress=H_VerifierFailure",   // 0xF...
                            "--suppress=D_PlanNotLanded",     // 0xD009
                            "--suppress=P_DeprecatedSyntax",  // ordinary — no notice
                            "--suppress=D_TargetAbiModelMismatch"});  // 0xD00E
    ASSERT_EQ(rep.all().size(), 3u)
        << "one notice per REFUSED code, and none for the ordinary one";
    for (auto const& d : rep.all()) {
        EXPECT_EQ(d.code, DiagnosticCode::D_SuppressRequestIgnored);
    }
    // Ascending by code value, and each names its own subject.
    EXPECT_NE(rep.all()[0].actual.find("--suppress=D_PlanNotLanded"),
              std::string::npos)
        << "first notice was: " << rep.all()[0].actual;
    EXPECT_NE(rep.all()[1].actual.find("--suppress=D_TargetAbiModelMismatch"),
              std::string::npos)
        << "second notice was: " << rep.all()[1].actual;
    EXPECT_NE(rep.all()[2].actual.find("--suppress=H_VerifierFailure"),
              std::string::npos)
        << "third notice was: " << rep.all()[2].actual;
}

// ── The other end of the same defect: a mode with no reporter at all ─────

TEST(SuppressRequestIgnored, PolicyFlagsAreRefusedInModesThatBuildNoReporter) {
    // The refusal notice above answers "a request the reporter DECLINES".
    // This answers the neighbouring case found while building it: a request
    // that reaches NO REPORTER — `--lsp` returns from `Program::run` before
    // the reporter config exists, and `--dump-predefined-macros` is dispatched
    // from `main.cpp` before `Program::run` is entered at all. ✔MEASURED
    // before the fix: both accepted the flags and silently discarded them.
    //
    // ⓘ WHY REJECTION HERE AND WARN-AND-CONTINUE THERE — the two are not in
    // tension. There, the flag reaches a reporter that declines it per-code
    // and the build proceeds. Here there is no reporter to warn THROUGH: the
    // instrument that would carry the notice is precisely what is missing. So
    // parse-time refusal is the only fail-loud option, and it is already what
    // `--max-diagnostics`, `--schema-dir` and `--stack-reserve` do.
    //
    // Asserted over the FULL CROSS PRODUCT of {3 policy flags} × {2 modes}.
    // The gate was originally written for `--max-diagnostics` alone and its
    // two siblings leaked through for exactly that reason — a gate written
    // per-flag is how the next flag gets missed, so the pin is written
    // per-class.
    struct Case { char const* mode; char const* flag; };
    constexpr Case kModes[] = {
        {"--lsp", nullptr},
        {"--dump-predefined-macros", nullptr},
    };
    constexpr char const* kPolicyFlags[] = {
        "--suppress=D_PlanNotLanded",
        "--suppress=P_DeprecatedSyntax",   // ORDINARY too: the mode is the
                                           // problem, not the code
        "--warnings-as-errors",
        "--max-diagnostics=10",
    };
    for (auto const& m : kModes) {
        for (auto const* flag : kPolicyFlags) {
            Argv a{"dss-code-prime", m.mode, "--language", "c-subset",
                   "--target=x86_64:elf64-x86_64-linux", flag};
            auto r = parseCliArgs(a.argc(), a.argv());
            ASSERT_FALSE(r.has_value())
                << m.mode << " accepted " << flag
                << ", which it cannot honour — it would be silently discarded";
            EXPECT_EQ(r.error().kind, CliArgsError::NoModeSelected)
                << m.mode << " + " << flag;
            EXPECT_NE(r.error().detail.find("silently discarded"),
                      std::string::npos)
                << "the refusal must say WHY, not just refuse: " << m.mode
                << " + " << flag << " gave: " << r.error().detail;
        }
    }
    // CONTROL — the same flags in a mode that DOES build a reporter are
    // accepted. Without this the pin above is satisfied by a parser that
    // rejects the flags everywhere, which would be a worse defect than the
    // one being fixed.
    for (auto const* flag : kPolicyFlags) {
        Argv a{"dss-code-prime", "--transpile", "probe.c", "--language", "c",
               "--target=x86_64:elf64-x86_64-linux", flag};
        auto r = parseCliArgs(a.argc(), a.argv());
        EXPECT_TRUE(r.has_value())
            << "a compiling mode must still accept " << flag << ": "
            << (r ? std::string{} : r.error().detail);
    }
}

// ── The reference-compiler probe ─────────────────────────────────────────
//
// WHY THIS TEST EXISTS. "warn and continue" was chosen over "reject at parse
// time" because rejecting would make DSS strictly harsher than gcc, clang and
// MSVC on a flag whose purpose is to be script-driven. That is a CONFORMANCE
// CLAIM about three other compilers, and a conformance claim recorded only in
// a comment is a claim nobody re-measures. This runs the probe.
//
// THE CLAIM UNDER TEST, stated so it is falsifiable: *no reference compiler
// treats an unhonourable SILENCING request as a fatal command-line error.*
// Each arm compiles a trivial TU with the local analogue of
// `--suppress=<something that cannot be silenced>` and asserts the compile
// still SUCCEEDS.
//
// ⚠ THE SILENCING FORM SPECIFICALLY, and the distinction is not pedantic:
// gcc hard-rejects `-W<unknown>` (the ENABLING form) with rc=1 while
// ACCEPTING `-Wno-<unknown>`. A request to silence can be honoured vacuously;
// a request to enable cannot. `--suppress` is the silencing form, so the
// accepting arm is the applicable one — and gcc's rejecting arm is evidence
// that it HAD the option of rejecting here and deliberately did not take it.
//
// SKIPPING IS HONEST, NEVER VACUOUS: a host without the toolchain SKIPS with
// a named verdict, and `ExecutedRows` fails the test if the arms it did reach
// somehow ran zero probes. A silent pass on a compiler-less host would make
// this pin worthless exactly where it looks green.

namespace {

// Compile `source` with `extraFlags` appended and return the process exit
// code, or nullopt when the toolchain is absent. `native_c_probe`'s
// `runNativeCProbe` bakes its flags in and offers no extra-argument seam, so
// this goes one level down to `findCompiler`/`locateMsvcToolchain` — the same
// route `test_coff_object_reader.cpp` and `test_reference_conformance.cpp`
// take.
struct FlagProbe {
    bool        toolPresent = false;
    std::string tool;        // what was actually exercised, for the verdict line
    int         rc          = -1;
    std::string output;
};

// ⚠ EVERY ARM GETS ITS OWN FILENAMES, VIA `tag`. The first version had the
// control and the flag arm both writing `flagprobe.{c,obj,bat,log}` into one
// directory, and it FAILED — ✔MEASURED: the flag arm came back rc=1 with
// "the file is already in use by another process" and an otherwise empty
// compiler log, which read exactly like "MSVC rejects /wd2065" and would have
// been reported as a refutation of the decision this test exists to support.
// It was not: driving the identical flag form by hand against the identical
// compiler (19.51.36252) gives rc=0 plus `Command line warning D9014`, as
// recorded. The rc came from the freshly-written batch/object of the PREVIOUS
// arm still being held when the next one started. Distinct names per arm
// remove the contention and, with it, a flake that produced a false
// measurement rather than a false failure — the more dangerous kind.
[[nodiscard]] FlagProbe probeUnixFlag(fs::path const& work, std::string const& tag,
                                      std::string const& extraFlags) {
    FlagProbe out;
    auto const loc = native_probe::findCompiler(work);
    if (!loc.compiler.has_value()) {
        out.tool = loc.detail;
        return out;
    }
    // ⚠ THE TOOLCHAIN KIND MUST BE CHECKED, AND THE FIRST DRAFT DID NOT —
    // which made this arm VACUOUS ON WINDOWS in the way that looks best.
    // `findCompiler`'s Windows arm returns a `Toolchain::Msvc` compiler whose
    // `buildCmd` emits a generated .bat with the whole `cl` line baked in, so
    // appending `-c -Wno-<unknown>` appends it to the BATCH FILE INVOCATION,
    // not to `cl`. cmd.exe hands the batch two arguments it never reads, the
    // compile succeeds, rc is 0 — and the assertion below passes while
    // reporting that it exercised "the host cc/clang/gcc" and having tested
    // no flag at all. A pin that cannot fail is worse than no pin, because it
    // occupies the slot where a real one would go.
    if (loc.compiler->kind != native_probe::Toolchain::Unix) {
        out.tool = "the host toolchain is MSVC, not a unix-style cc "
                   "(the MSVC arm covers this host)";
        return out;
    }
    fs::path const src = work / (tag + "_probe.c");
    fs::path const exe = work / (tag + "_probe.out");
    {
        std::ofstream f{src};
        f << "int main(void) { return 0; }\n";
    }
    out.toolPresent = true;
    out.tool        = "the host cc/clang/gcc";
    // `-c` (compile only). Whether the driver ACCEPTS the flag is settled
    // before any linking, and stopping at the object leaves no executable
    // behind for a scanner or a lingering child process to hold open — which
    // is what made `ScratchDir` warn it could not clean up on this test's
    // first run.
    std::string const cmd = loc.compiler->buildCmd(src, exe) + " -c " + extraFlags;
    fs::path const    log = work / (tag + "_log.txt");
    out.rc     = std::system(native_probe::captureCmd(cmd, log).c_str());
    out.output = native_probe::tailOf(log, 20);
    return out;
}

[[nodiscard]] FlagProbe probeMsvcFlag(fs::path const& work, std::string const& tag,
                                      std::string const& extraFlags) {
    FlagProbe out;
    auto const msvc = native_probe::locateMsvcToolchain(work);
    if (!msvc.ok()) {
        out.tool = msvc.detail;
        return out;
    }
    fs::path const src = work / (tag + "_probe.c");
    fs::path const obj = work / (tag + "_probe.obj");
    fs::path const bat = work / (tag + "_probe.bat");
    {
        std::ofstream f{src};
        f << "int main(void) { return 0; }\n";
    }
    {
        // `\r\n` and the `call` line mirror `native_c_probe`'s own generated
        // batch: cmd.exe is the interpreter, and vcvars64 is what puts `cl`
        // on PATH at all.
        std::ofstream f{bat, std::ios::binary};
        f << "@echo off\r\n"
          << "call \"" << msvc.vcvars.string() << "\" >nul 2>&1\r\n"
          << "cl /nologo /W3 /c " << extraFlags << " /Fo:\"" << obj.string()
          << "\" \"" << src.string() << "\"\r\n";
    }
    out.toolPresent = true;
    out.tool        = "MSVC cl.exe";
    fs::path const log = work / (tag + "_log.txt");
    out.rc     = std::system(
        native_probe::captureCmd("\"\"" + bat.string() + "\"\"", log).c_str());
    out.output = native_probe::tailOf(log, 20);
    return out;
}

} // namespace

TEST(SuppressRequestIgnored, ReferenceCompilersDoNotRejectAnUnhonourableSilencingRequest) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp,
                                          "suppress-flag-probe"};

    // ⚠ EACH ARM IS A CONTROLLED COMPARISON, NOT A BARE ASSERTION. The ONLY
    // variable between the two runs is the flag under test; everything else
    // — compiler, source, working dir, invocation — is held identical. Without
    // the control arm a toolchain that is broken for some unrelated reason
    // would come back rc!=0 and be reported as "this compiler rejects the
    // silencing form", which is a conclusion about the wrong thing. It matters
    // most for MSVC: `locateMsvcToolchain` only finds `vcvars64.bat`, it never
    // checks that `cl` can compile anything, so the control is the only thing
    // establishing that a non-zero rc means what the assertion says it means.
    auto const unixControl = probeUnixFlag(scratch.path(), "unixctl", "");
    // The gcc/clang analogue: silence a warning option the compiler has never
    // heard of. `findCompiler`'s Windows arm resolves MSVC rather than a
    // MinGW gcc, so on a Windows host this arm reports absent and the MSVC
    // arm below is the one that runs; on Linux/macOS it is the reverse.
    auto const unix_ = probeUnixFlag(scratch.path(), "unixflag",
                                     "-Wno-dss-probe-no-such-warning-option");
    auto const msvcControl = probeMsvcFlag(scratch.path(), "msvcctl", "");
    // The MSVC analogue, and it is the CLOSEST of the three to the DSS
    // question: `/wd2065` is a WELL-FORMED number naming a REAL diagnostic
    // that `/wd` cannot silence, because C2065 is an error and not a warning.
    // Well-formed input naming something that cannot be silenced is exactly
    // `--suppress=<a protected code>`.
    auto const msvc = probeMsvcFlag(scratch.path(), "msvcflag", "/wd2065");

    if (!unix_.toolPresent && !msvc.toolPresent) {
        GTEST_SKIP()
            << "VERDICT: NOT MEASURED ON THIS HOST — neither a unix-style cc "
               "nor an MSVC toolchain is available (cc: "
            << unix_.tool << "; msvc: " << msvc.tool
            << "). The warn-and-continue decision rests on a probe of gcc "
               "13.2/13.3, Apple clang 21 and MSVC 19.51, none of which makes "
               "the silencing form fatal; this host simply cannot re-measure "
               "it. The DSS-side behaviour is pinned unconditionally by the "
               "other tests in this file.";
    }

    // Constructed only after every early exit: a ledger that outlives a
    // legitimate skip turns that skip into a spurious red.
    native_probe::ExecutedRows rows{"reference-compiler silencing-form probes", 1};

    if (unix_.toolPresent) {
        rows.record();
        ASSERT_EQ(unixControl.rc, 0)
            << "CONTROL FAILED, so the flag arm proves nothing: this host's "
               "cc/clang/gcc could not compile `int main(void){return 0;}` "
               "with NO extra flags at all (rc="
            << unixControl.rc << "). Fix the toolchain; do not read the "
               "flag arm below as a verdict about the flag. Output:\n"
            << unixControl.output;
        EXPECT_EQ(unix_.rc, unixControl.rc)
            << "MEASURED REFUTATION: a unix-style host compiler REJECTED a "
               "`-Wno-<unknown>` silencing request (rc="
            << unix_.rc << " against a control rc of " << unixControl.rc
            << "). The warn-and-continue decision was taken because gcc and "
               "clang both accept this form — if that is no longer true on "
               "this toolchain, the decision's premise needs re-examining, "
               "not this assertion relaxing. Compiler output:\n"
            << unix_.output;
    }
    if (msvc.toolPresent) {
        rows.record();
        ASSERT_EQ(msvcControl.rc, 0)
            << "CONTROL FAILED, so the flag arm proves nothing: `cl` could "
               "not compile `int main(void){return 0;}` with NO extra flags "
               "(rc=" << msvcControl.rc
            << "). `locateMsvcToolchain` only locates vcvars64.bat, it never "
               "verifies cl runs, so this is exactly the case the control "
               "exists to separate out. Output:\n"
            << msvcControl.output;
        EXPECT_EQ(msvc.rc, msvcControl.rc)
            << "MEASURED REFUTATION: MSVC REJECTED `/wd2065` — a well-formed "
               "number naming a diagnostic `/wd` cannot silence (rc="
            << msvc.rc << " against a control rc of " << msvcControl.rc
            << "). This is the closest analogue of `--suppress=<a protected "
               "code>` that exists in another compiler, and it was measured "
               "to emit command-line warning D9014 and CONTINUE. Compiler "
               "output:\n"
            << msvc.output;
    }
}
