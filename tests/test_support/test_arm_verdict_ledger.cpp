// Self-tests for the corpus harnesses' per-arm verdict ledger
// (D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT) and the manifest emulator lint
// (D-TEST-MANIFEST-ARM64-ARM-WITHOUT-EMULATOR).
//
// WHY THESE EXIST AS UNIT TESTS RATHER THAN ONLY AS CORPUS RUNS. On a
// windows/x86_64 host every non-Windows arm in the corpus is excluded by
// `runOn` BEFORE the arch gate is reached, so the corpus produces exactly ZERO
// `SkippedEmulatorMissing` verdicts there — the strict-mode guard has no live
// input on the leg most likely to be run. A guard whose only witness lives on
// another machine is the same "code that never runs on the leg we gate on"
// class the ledger itself was built to expose. These tests give every verdict
// class, the strict parse, and both lint rules a witness on EVERY host.
//
// Each assertion is written so that DELETING the behaviour it pins turns it
// red — the mutations are named in the comments beside them.

#include "arm_verdict_ledger.hpp"
#include "host_native_target.hpp"

#include <gtest/gtest.h>

#include <cstddef>   // std::size_t (do not rely on a transitive include)
#include <string>
#include <vector>

using dss::test_support::ArmVerdict;
using dss::test_support::ArmVerdictLedger;
using dss::test_support::armVerdictIsEnvironmentalSkip;
using dss::test_support::armVerdictIsStructuralSkip;
using dss::test_support::armVerdictIsVerified;
using dss::test_support::armVerdictClass;
using dss::test_support::ArmVerdictClass;
using dss::test_support::armVerdictName;
using dss::test_support::DeclaredArm;
using dss::test_support::kAllArmVerdicts;
using dss::test_support::lintDeclaredEmulators;
using dss::test_support::specTargetArch;

namespace {

// The corpus's real shape, in miniature: one manifest declaring a native arm
// and a cross-arch arm, plus the sibling that establishes the emulator
// vocabulary for that (arch, runOn) pair.
[[nodiscard]] std::vector<DeclaredArm> corpusWithConsistentEmulators() {
    return {
        {"c-subset/a", "x86_64:elf64-x86_64-linux-exec", "linux", ""},
        {"c-subset/a", "arm64:elf64-aarch64-linux-exec", "linux", "qemu-aarch64"},
        {"c-subset/b", "x86_64:elf64-x86_64-linux-exec", "linux", ""},
        {"c-subset/b", "arm64:elf64-aarch64-linux-exec", "linux", "qemu-aarch64"},
        // An (arch, runOn) pair for which NO emulator has ever been declared —
        // the arm64-darwin shape. The lint must stay silent here: there is no
        // emulator to name, and demanding one would be demanding a lie.
        {"c-subset/c", "arm64:macho64-arm64-darwin-exec", "darwin", ""},
        {"c-subset/d", "arm64:macho64-arm64-darwin-exec", "darwin", ""},
    };
}

}  // namespace

// ── The verdict vocabulary ─────────────────────────────────────────────────

// The three skip reasons must stay DISTINGUISHABLE. Collapsing any two of them
// back into one status is exactly the defect
// D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT names, so it is pinned rather than
// left to a comment. Red-on-disable: give two verdicts the same name, or make
// `armVerdictIsEnvironmentalSkip` answer true for a structural one.
TEST(ArmVerdict, EveryVerdictHasADistinctNameAndExactlyOneClass) {
    std::vector<std::string> names;
    for (ArmVerdict const v : kAllArmVerdicts) {
        auto const name = std::string{armVerdictName(v)};
        EXPECT_NE(name, "unknown") << "a verdict fell through armVerdictName";
        for (auto const& seen : names) {
            EXPECT_NE(seen, name)
                << "two verdicts share the name '" << name
                << "' — the ledger's whole value is that the skip reasons stay"
                   " distinguishable";
        }
        names.push_back(name);

        // Exactly one class, asked of the ONE chokepoint. Deliberately NOT the
        // old if-chain over named enumerators: that chain answered "no class"
        // for a verdict it had never heard of, which is the silent hole.
        EXPECT_NE(armVerdictClass(v), ArmVerdictClass::Unclassified)
            << "verdict '" << name << "' reached no arm of armVerdictClass()"
               " — it belongs to NO class and would vanish from the ledger's"
               " accounting";
    }
    // Tied to the ENUM, not to the array's length: `kArmVerdictCount` is
    // `ArmVerdict::kCount_`, so this cannot be satisfied by editing the array.
    // (The static_assert in the header already refuses a mismatch at compile
    // time; this pins the runtime walk against the same single source.)
    EXPECT_EQ(names.size(), dss::test_support::kArmVerdictCount)
        << "a verdict was added or removed without updating kAllArmVerdicts";
}

// LAYER 3 of the closed-enum enforcement (layer 1 = the header's static_assert,
// layer 2 = the `default:`-less switch in `armVerdictClass`).
//
// THE PROPERTY: a new enumerator must not be able to vanish from the accounting
// silently. Layer 1 catches "added an enumerator, forgot the array". THIS
// catches the other author mistake — "added the enumerator AND the array,
// forgot to classify it" — which layer 1 cannot see, because the cardinalities
// then agree.
//
// RED-ON-DISABLE (measured): add an 8th enumerator before `kCount_` and add it
// to `kAllArmVerdicts`; the static_assert is satisfied, and this test reds
// because the value classifies `Unclassified` and the buckets stop summing.
TEST(ArmVerdict, EveryVerdictIsAccountedForByExactlyOneReportedBucket) {
    for (ArmVerdict const v : kAllArmVerdicts) {
        auto const name = std::string{armVerdictName(v)};
        int buckets = 0;
        if (armVerdictIsVerified(v)) ++buckets;
        if (dss::test_support::armVerdictIsSkip(v)) ++buckets;
        if (dss::test_support::armVerdictIsFailure(v)) ++buckets;
        EXPECT_EQ(buckets, 1)
            << "verdict '" << name << "' lands in " << buckets
            << " of renderCountsLine()'s three reported buckets (verified /"
               " skipped / failed); it must land in exactly one or the summary"
               " stops summing";
    }
}

// The same property asserted through the LEDGER rather than the predicates: one
// record of every verdict, and the printed breakdown must account for all of
// them. This is the assertion that would have caught a vanished enumerator by
// its OBSERVABLE symptom — `renderCountsLine()` printing numbers that no longer
// add up to its own "(of N declared arms)" denominator.
TEST(ArmVerdictLedgerTest, TheCountsLineAccountsForEveryRecordedVerdict) {
    ArmVerdictLedger ledger;
    for (ArmVerdict const v : kAllArmVerdicts) {
        ledger.record("c-subset/x", "spec", "baseline", v, "");
    }
    ASSERT_EQ(ledger.total(), dss::test_support::kArmVerdictCount);
    EXPECT_EQ(ledger.accountedCount(), ledger.total())
        << "some verdict belongs to no reported class: "
        << ledger.renderCountsLine();
    // The fail-loud marker must be ABSENT on a healthy ledger, or it would be
    // noise nobody reads when it finally matters.
    EXPECT_EQ(ledger.renderCountsLine().find("LEDGER ACCOUNTING HOLE"),
              std::string::npos)
        << ledger.renderCountsLine();
}

// The sentinel is a cardinality pin, NOT a verdict — so it must not masquerade
// as one if it ever leaks into a record. Red-on-disable: give `kCount_` a real
// class arm in `armVerdictClass` and this fails.
TEST(ArmVerdict, TheCardinalitySentinelIsNotAVerdict) {
    EXPECT_EQ(armVerdictClass(ArmVerdict::kCount_),
              ArmVerdictClass::Unclassified);
    EXPECT_FALSE(armVerdictIsVerified(ArmVerdict::kCount_));
    EXPECT_FALSE(dss::test_support::armVerdictIsSkip(ArmVerdict::kCount_));
    EXPECT_FALSE(dss::test_support::armVerdictIsFailure(ArmVerdict::kCount_));
    for (ArmVerdict const v : kAllArmVerdicts) {
        EXPECT_NE(v, ArmVerdict::kCount_)
            << "the sentinel must never be listed in kAllArmVerdicts";
    }
}

// The distinction strict mode acts on. Red-on-disable: move
// SkippedEmulatorMissing (or SkippedBuildInputMissing) into the structural set
// and this fails.
//
// THE THREE ENVIRONMENTAL SKIPS, AND WHY THEY ARE SIBLINGS RATHER THAN ONE.
// `SkippedEmulatorMissing` is "the machine cannot RUN it"; `SkippedLauncher
// PrerequisiteMissing` is "the machine CAN run it and the launcher's own
// declared prerequisites are absent" — the launcher resolves, looks perfectly
// usable, and its sysroot / its ELF interpreter / the program it crosses into a
// distro to reach is not there; `SkippedBuildInputMissing` is "the machine
// cannot BUILD it" — a declared resolve-library binary or a leg's target
// compiler is absent (D-HARNESS-CROSS-HOST-ANY-TARGET). They share a CLASS
// because they share an enforcement (warn by default, red under
// DSS_STRICT_ARM_VERDICTS) and a remedy (install the missing thing), and they
// stay SEPARATE names because a reader must be able to tell which part of the
// pipeline the machine failed to supply. Collapsing the middle one into
// `SkippedEmulatorMissing` would say "missing" of an emulator that is present
// and runs, which is the conflation that let 14 units be charged to the
// compiler after a `which wsl.exe` answered for a `qemu-aarch64` inside it.
TEST(ArmVerdict, TheEnvironmentalSkipsAreExactlyTheMachineSuppliedOnes) {
    EXPECT_TRUE(armVerdictIsEnvironmentalSkip(ArmVerdict::SkippedEmulatorMissing));
    EXPECT_TRUE(armVerdictIsEnvironmentalSkip(
        ArmVerdict::SkippedLauncherPrerequisiteMissing));
    EXPECT_TRUE(armVerdictIsEnvironmentalSkip(ArmVerdict::SkippedBuildInputMissing));
    EXPECT_FALSE(armVerdictIsEnvironmentalSkip(ArmVerdict::SkippedByRunOn));
    EXPECT_FALSE(armVerdictIsEnvironmentalSkip(ArmVerdict::SkippedNoEmulatorDeclared));
    EXPECT_FALSE(armVerdictIsEnvironmentalSkip(ArmVerdict::NotSelectedByRunner));
    EXPECT_FALSE(armVerdictIsEnvironmentalSkip(ArmVerdict::Poisoned));
    EXPECT_TRUE(armVerdictIsStructuralSkip(ArmVerdict::SkippedByRunOn));
    EXPECT_TRUE(armVerdictIsStructuralSkip(ArmVerdict::SkippedNoEmulatorDeclared));
    EXPECT_TRUE(armVerdictIsVerified(ArmVerdict::Ran));
    EXPECT_TRUE(armVerdictIsVerified(ArmVerdict::ExpectErrorAsserted));
    EXPECT_FALSE(armVerdictIsVerified(ArmVerdict::SkippedEmulatorMissing));
    EXPECT_FALSE(armVerdictIsVerified(ArmVerdict::SkippedBuildInputMissing));

    // The counted membership, so a FOURTH environmental skip cannot be added
    // without a deliberate visit here. (It did its job: this number was 2 until
    // `SkippedLauncherPrerequisiteMissing` was added, and the visit is what
    // produced the paragraph above.)
    std::size_t environmental = 0;
    for (ArmVerdict const v : kAllArmVerdicts) {
        if (armVerdictIsEnvironmentalSkip(v)) ++environmental;
    }
    EXPECT_EQ(environmental, 3u)
        << "strict mode acts on exactly the environmental class; a new member"
           " changes what the gate reds on and must be reviewed here";
}

// ── The ledger ─────────────────────────────────────────────────────────────

TEST(ArmVerdictLedgerTest, CountsSeparateVerifiedFromEveryFlavourOfSkip) {
    ArmVerdictLedger ledger;
    ledger.record("c-subset/x", "spec-a", "baseline", ArmVerdict::Ran, "");
    ledger.record("c-subset/x", "spec-a", "release",  ArmVerdict::Ran, "");
    ledger.record("c-subset/y", "spec-b", "expect-error",
                  ArmVerdict::ExpectErrorAsserted, "");
    ledger.record("c-subset/z", "spec-c", "baseline",
                  ArmVerdict::SkippedByRunOn, "runOn=[linux] excludes host=windows");
    ledger.record("c-subset/z", "spec-d", "baseline",
                  ArmVerdict::SkippedNoEmulatorDeclared, "no emulator");
    ledger.record("c-subset/z", "spec-e", "baseline",
                  ArmVerdict::SkippedEmulatorMissing, "qemu-aarch64 not on PATH");
    ledger.record("sqlite-harness/pe64-x86_64", "spec-h", "baseline",
                  ArmVerdict::SkippedBuildInputMissing, "tcl86.dll not found");
    ledger.record("c-subset/z", "spec-f", "cli",
                  ArmVerdict::NotSelectedByRunner, "first-match binding");
    ledger.record("c-subset/w", "spec-g", "baseline", ArmVerdict::Poisoned,
                  "compile failed");

    EXPECT_EQ(ledger.total(), 9u);
    EXPECT_EQ(ledger.verifiedCount(), 3u);
    // Poisoned is NOT a skip: it already failed loudly and must not be filed
    // under "not run" as if nobody had noticed.
    EXPECT_EQ(ledger.skippedCount(), 5u);
    EXPECT_EQ(ledger.count(ArmVerdict::Ran), 2u);
    EXPECT_EQ(ledger.count(ArmVerdict::SkippedEmulatorMissing), 1u);
    EXPECT_EQ(ledger.count(ArmVerdict::SkippedBuildInputMissing), 1u);
    EXPECT_EQ(ledger.count(ArmVerdict::NotSelectedByRunner), 1u);
    EXPECT_EQ(ledger.count(ArmVerdict::Poisoned), 1u);
}

// The list strict mode turns into failures — environmental ONLY. Red-on-disable:
// widen `environmentalSkips()` to any skip and the size assertion fails.
TEST(ArmVerdictLedgerTest, EnvironmentalSkipsAreTheOnlyStrictModeInput) {
    ArmVerdictLedger ledger;
    ledger.record("c-subset/x", "spec-a", "baseline", ArmVerdict::SkippedByRunOn, "a");
    ledger.record("c-subset/x", "spec-b", "baseline",
                  ArmVerdict::SkippedNoEmulatorDeclared, "b");
    ledger.record("c-subset/x", "spec-c", "baseline",
                  ArmVerdict::SkippedEmulatorMissing, "qemu-aarch64 not on PATH");
    ledger.record("c-subset/x", "spec-d", "cli", ArmVerdict::NotSelectedByRunner, "d");
    ledger.record("sqlite-harness/pe64-x86_64", "spec-e", "baseline",
                  ArmVerdict::SkippedBuildInputMissing, "tcl86.dll not found");

    auto const env = ledger.environmentalSkips();
    ASSERT_EQ(env.size(), 2u);
    EXPECT_EQ(env.front().spec, "spec-c");
    EXPECT_EQ(env.front().detail, "qemu-aarch64 not on PATH");
    // Both environmental members, in record order — a strict-mode caller that
    // reported only the first would silently drop the build-side half.
    EXPECT_EQ(env.back().spec, "spec-e");
    EXPECT_EQ(env.back().detail, "tcl86.dll not found");
}

// The counts line must NAME every class. A summary that says "4 skipped"
// re-creates the conflation the ledger exists to end, so the wording is pinned.
TEST(ArmVerdictLedgerTest, CountsLineNamesEverySkipClass) {
    ArmVerdictLedger ledger;
    ledger.record("c-subset/x", "spec-a", "baseline", ArmVerdict::Ran, "");
    ledger.record("c-subset/x", "spec-b", "baseline",
                  ArmVerdict::SkippedEmulatorMissing, "missing");
    auto const line = ledger.renderCountsLine();
    for (char const* needle : {"verified", "ran", "expect-error", "by-runOn",
                               "no-emulator-declared", "emulator-missing",
                               "launcher-prerequisite-missing",
                               "build-input-missing", "not-selected", "poisoned",
                               "declared arms"}) {
        EXPECT_NE(line.find(needle), std::string::npos)
            << "the counts line must name '" << needle << "': " << line;
    }
}

// Detail lines name the arm AND the reason; verified arms are omitted; and the
// structural filter drops only the structural rows, never the counts.
TEST(ArmVerdictLedgerTest, SkipDetailNamesTheArmAndHonoursTheStructuralFilter) {
    ArmVerdictLedger ledger;
    ledger.record("c-subset/x", "spec-ran", "baseline", ArmVerdict::Ran, "");
    ledger.record("c-subset/x", "spec-runon", "baseline",
                  ArmVerdict::SkippedByRunOn, "runOn=[linux] excludes host=windows");
    ledger.record("c-subset/x", "spec-emu", "baseline",
                  ArmVerdict::SkippedEmulatorMissing, "qemu-aarch64 is not on PATH");

    auto const full = ledger.renderSkipDetail("  ", /*includeStructural=*/true);
    EXPECT_EQ(full.find("spec-ran"), std::string::npos)
        << "a verified arm must not appear in the SKIP detail";
    EXPECT_NE(full.find("spec-runon"), std::string::npos);
    EXPECT_NE(full.find("spec-emu"), std::string::npos);
    EXPECT_NE(full.find("qemu-aarch64 is not on PATH"), std::string::npos)
        << "the detail must carry the REASON, not just the verdict name";

    auto const terse = ledger.renderSkipDetail("  ", /*includeStructural=*/false);
    EXPECT_EQ(terse.find("spec-runon"), std::string::npos)
        << "includeStructural=false must drop the by-runOn rows";
    EXPECT_NE(terse.find("spec-emu"), std::string::npos)
        << "includeStructural=false must KEEP the environmental rows";
}

// ── The strict-mode env parse ──────────────────────────────────────────────
//
// Same discipline as DSS_REFRESH_GOLDENS: an unrecognised value is REFUSED, not
// guessed. A typo'd `DSS_STRICT_ARM_VERDICTS=ture` that quietly meant "off"
// would disable the gate in exactly the silent way this whole change exists to
// end. Red-on-disable: make the parser return `on=false` for an unknown value
// and the malformed assertions below fail.
namespace {

// setenv/unsetenv are POSIX; _putenv_s with an empty value removes on Windows.
void setStrictEnv(char const* value) {
#if defined(_WIN32)
    ::_putenv_s(dss::test_support::kStrictArmVerdictsEnv,
                value == nullptr ? "" : value);
#else
    if (value == nullptr) {
        ::unsetenv(dss::test_support::kStrictArmVerdictsEnv);
    } else {
        ::setenv(dss::test_support::kStrictArmVerdictsEnv, value, 1);
    }
#endif
}

}  // namespace

TEST(StrictArmVerdicts, AcceptsTheTruthyVocabularyAndRefusesAnythingElse) {
    for (char const* on : {"1", "true", "TRUE", "yes"}) {
        setStrictEnv(on);
        auto const s = dss::test_support::readStrictArmVerdicts();
        EXPECT_TRUE(s.on) << "'" << on << "' must enable strict mode";
        EXPECT_FALSE(s.malformed);
    }
    for (char const* off : {"0", "false", "FALSE", "no", ""}) {
        setStrictEnv(off);
        auto const s = dss::test_support::readStrictArmVerdicts();
        EXPECT_FALSE(s.on) << "'" << off << "' must not enable strict mode";
        EXPECT_FALSE(s.malformed) << "'" << off << "' is a recognised value";
    }
    for (char const* bad : {"ture", "on", "2", "1 "}) {
        setStrictEnv(bad);
        auto const s = dss::test_support::readStrictArmVerdicts();
        EXPECT_FALSE(s.on);
        EXPECT_TRUE(s.malformed)
            << "'" << bad << "' must be REFUSED, never interpreted";
        EXPECT_EQ(s.raw, bad);
    }
    setStrictEnv(nullptr);
    auto const unset = dss::test_support::readStrictArmVerdicts();
    EXPECT_FALSE(unset.on);
    EXPECT_FALSE(unset.malformed);
    EXPECT_TRUE(unset.raw.empty());
}

// ── The manifest emulator lint ─────────────────────────────────────────────

TEST(EmulatorLint, SilentOnAConsistentCorpus) {
    auto const findings = lintDeclaredEmulators(corpusWithConsistentEmulators());
    EXPECT_TRUE(findings.empty())
        << "the lint fired on a corpus where every (arch, runOn) pair agrees";
}

// THE case this lint was built for, reproduced in miniature: one arm omits the
// emulator its 2 siblings declare for the same (arch, runOn) pair. Red-on-
// disable: delete rule (2) in `lintDeclaredEmulators` and this goes green.
TEST(EmulatorLint, FlagsTheArmThatOmitsItsSiblingsEmulator) {
    auto arms = corpusWithConsistentEmulators();
    arms.push_back({"c-subset/offender", "arm64:elf64-aarch64-linux-exec",
                    "linux", ""});
    auto const findings = lintDeclaredEmulators(arms);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_EQ(findings.front().manifest, "c-subset/offender");
    EXPECT_EQ(findings.front().arch, "arm64");
    EXPECT_EQ(findings.front().runOnOs, "linux");
    // The message must NAME the emulator the arm should have declared —
    // a finding that only says "inconsistent" leaves the reader grepping.
    EXPECT_NE(findings.front().message.find("qemu-aarch64"), std::string::npos)
        << findings.front().message;
}

// Host-independence is the property that makes the lint catch an omission at
// AUTHORING time rather than only on a leg whose arch differs. Same input, same
// answer, on every machine — so the pin is simply that the finding does not
// mention, and is not conditioned on, this host.
TEST(EmulatorLint, IsHostIndependent) {
    auto arms = corpusWithConsistentEmulators();
    arms.push_back({"c-subset/offender", "arm64:elf64-aarch64-linux-exec",
                    "linux", ""});
    // The offending arm is arm64/linux. On an arm64 Linux host a HOST-RELATIVE
    // rule would stay silent (the arm runs natively); this one must not.
    auto const findings = lintDeclaredEmulators(arms);
    EXPECT_EQ(findings.size(), 1u)
        << "the lint's answer must not depend on the host arch/OS";
}

// A pair with no emulator declared ANYWHERE stays silent — the arm64-darwin
// shape, where no emulator exists and demanding one would be demanding a lie.
// Red-on-disable: change rule (2) to fire whenever `emulator` is empty and this
// fails with 2 findings.
TEST(EmulatorLint, SilentWhereNoEmulatorHasEverBeenDeclaredForThePair) {
    std::vector<DeclaredArm> const arms = {
        {"c-subset/c", "arm64:macho64-arm64-darwin-exec", "darwin", ""},
        {"c-subset/d", "arm64:macho64-arm64-darwin-exec", "darwin", ""},
    };
    EXPECT_TRUE(lintDeclaredEmulators(arms).empty());
}

// Two spellings for one (arch, runOn) pair: the harness cannot tell an omitting
// arm what it is missing, so the ambiguity itself is the finding.
TEST(EmulatorLint, FlagsAnAmbiguousEmulatorVocabulary) {
    std::vector<DeclaredArm> const arms = {
        {"c-subset/a", "arm64:elf64-aarch64-linux-exec", "linux", "qemu-aarch64"},
        {"c-subset/b", "arm64:elf64-aarch64-linux-exec", "linux", "qemu-arm64"},
    };
    auto const findings = lintDeclaredEmulators(arms);
    ASSERT_EQ(findings.size(), 1u);
    EXPECT_TRUE(findings.front().manifest.empty())
        << "an ambiguity is a corpus-wide finding, not one manifest's fault";
    EXPECT_NE(findings.front().message.find("qemu-aarch64"), std::string::npos);
    EXPECT_NE(findings.front().message.find("qemu-arm64"), std::string::npos);
}

// The same (arch) under DIFFERENT runOn OSes are different keys: an emulator
// declared for arm64/linux says nothing about arm64/darwin. Red-on-disable: key
// the lint on arch alone and this reports a spurious finding for the darwin arm.
TEST(EmulatorLint, KeysOnTheArchAndTheRunOnOsTogether) {
    std::vector<DeclaredArm> const arms = {
        {"c-subset/a", "arm64:elf64-aarch64-linux-exec", "linux", "qemu-aarch64"},
        {"c-subset/b", "arm64:macho64-arm64-darwin-exec", "darwin", ""},
    };
    EXPECT_TRUE(lintDeclaredEmulators(arms).empty());
}

// ── The two host chokepoints must not drift ────────────────────────────────
//
// `currentHostArch()` and `hostNativeTarget()` both answer "what machine am I
// on". They are separate `#if` ladders, and `host_native_target.hpp` exists
// because that exact duplication was fixed once and its twin was not, twice.
// This pins them together: add a host to one and forget the other, and the
// cross-arch RUN gate would silently start skipping (or wrongly attempting)
// every arm on the new machine.
TEST(ArmVerdictHostIdentity, HostArchMatchesTheHostNativeTargetSpec) {
    auto const fromNativeTarget = specTargetArch(
        std::string{dss::test_support::hostNativeTarget().execTarget});
    EXPECT_EQ(dss::test_support::currentHostArch(), fromNativeTarget)
        << "currentHostArch() and hostNativeTarget() disagree about this host —"
           " one of the two `#if` ladders is missing an arm";
}

// ── The host binding rule ──────────────────────────────────────────────────
//
// D-TEST-INTEGRATED-TESTS-CANNOT-PASS-ON-A-NATIVE-ARM64-LINUX-HOST. The
// CLI-subprocess runner binds ONE target per manifest, and it used to bind the
// FIRST whose `runOn` admits the host. `runOn` names an OS, not a machine, so on
// a native aarch64 Linux box that rule bound the corpus's x86_64 arm — which
// cannot execute there and declares no emulator — and skipped the arm64 arm that
// runs natively. That host verified ZERO runtime behaviour while reporting 4353
// skips, and only [Test 5]'s "no stdout pin was non-empty" guard made it visible.
//
// ★ WHY THESE ARE UNIT TESTS AND NOT ONLY A CORPUS RUN, which is the same
// argument the header of this file makes for the verdict classes and is stronger
// here: the rule's INTERESTING host is one almost nobody runs. On windows,
// darwin and x86_64 linux the old rule and the new one agree by construction, so
// a green suite on any of those three says nothing about the rule at all. These
// tests hand every host the whole (manifest shape × host) table, so a revert to
// first-match reds on the developer's own machine instead of only on the VPS.
namespace {

using dss::test_support::HostBindingCandidate;
using dss::test_support::kNoBoundTarget;
using dss::test_support::selectBoundTargetIndex;

// The corpus's REAL per-OS shapes, in declaration order (✔MEASURED over
// examples/**/expected.json): windows and darwin manifests offer exactly one
// `runOn` match, linux offers two with the x86_64 arm FIRST.
[[nodiscard]] std::vector<HostBindingCandidate> corpusShapeForHostOs(
        std::string const& hostOs) {
    bool const win = hostOs == "windows";
    bool const lin = hostOs == "linux";
    bool const mac = hostOs == "darwin";
    return {
        {"x86_64:pe64-x86_64-windows-exec",   win},
        {"x86_64:elf64-x86_64-linux-exec",    lin},
        {"arm64:elf64-aarch64-linux-exec",    lin},
        {"arm64:macho64-arm64-darwin-exec",   mac},
    };
}

[[nodiscard]] std::string boundSpecFor(std::string const& hostOs,
                                       std::string const& hostArch) {
    auto const candidates = corpusShapeForHostOs(hostOs);
    auto const i          = selectBoundTargetIndex(candidates, hostArch);
    return i == kNoBoundTarget ? std::string{"<none>"} : candidates[i].spec;
}

}  // namespace

// THE NO-REGRESSION PIN for the three hosts that are green today, and the fix
// for the one that is not — one table, so a rule change that helps arm64 Linux
// by moving Windows/WSL/macOS cannot pass this file.
//
// ⚠ Each row runs through a `void` callable: a bare loop of `ASSERT_*` would let
// the FIRST failing host cancel every row after it, and the whole point of the
// table is to report which hosts moved, not just that one did.
TEST(HostTargetBinding, EveryHostBindsTheTargetItCanActuallyRun) {
    struct Row {
        std::string hostOs;
        std::string hostArch;
        std::string expectedSpec;
        std::string why;
    };
    std::vector<Row> const rows = {
        {"windows", "x86_64", "x86_64:pe64-x86_64-windows-exec",
         "one runOn match; old and new rules agree"},
        {"linux", "x86_64", "x86_64:elf64-x86_64-linux-exec",
         "two runOn matches and the FIRST is native — unchanged by the rule"},
        {"linux", "arm64", "arm64:elf64-aarch64-linux-exec",
         "THE FIX: two runOn matches, the native one is declared SECOND"},
        {"darwin", "arm64", "arm64:macho64-arm64-darwin-exec",
         "one runOn match; old and new rules agree"},
        {"darwin", "x86_64", "arm64:macho64-arm64-darwin-exec",
         "no native arm declared ⇒ the first runOn match, exactly as before"},
    };
    auto const checkRow = [](Row const& r) -> void {
        EXPECT_EQ(boundSpecFor(r.hostOs, r.hostArch), r.expectedSpec)
            << "host " << r.hostOs << '/' << r.hostArch << " — " << r.why;
    };
    for (auto const& r : rows) checkRow(r);
}

// ⚠ THE COMMON SHAPE IS NOT THE ONLY SHAPE, and saying "windows/darwin/WSL are
// untouched" while testing only the majority layout would be exactly the
// unstated-scope over-read this project keeps paying for. ✔MEASURED over all
// 602 shipped manifests: the binding moves on 457 for linux/arm64 (the fix), on
// ZERO for windows/x86_64 and darwin/arm64, and on SIX manifests that declare
// their arm64 arm FIRST — `large_frame_arm64` and `large_frame_beyond_16mib`
// (linux, x86_64 declared second as the documented "non-Windows control leg"),
// and four macho manifests that matter only to an INTEL Mac. Those six are the
// shape below, and in every one of them the rule moves the binding from an arm
// this host must emulate to one it executes natively — which is the whole point
// of the rule, not an exception to it.
TEST(HostTargetBinding, TheOutlierShapeMovesFromEmulatedToNative) {
    std::vector<HostBindingCandidate> const largeFrameShape = {
        {"arm64:elf64-aarch64-linux-exec", true},   // declared FIRST
        {"x86_64:elf64-x86_64-linux-exec", true},   // the native control leg
    };
    EXPECT_EQ(selectBoundTargetIndex(largeFrameShape, "x86_64"), 1u)
        << "an x86_64 Linux host must bind its own arm, not the qemu one";
    EXPECT_EQ(selectBoundTargetIndex(largeFrameShape, "arm64"), 0u)
        << "and an arm64 Linux host still binds the arm64 arm";

    std::vector<HostBindingCandidate> const machoShape = {
        {"arm64:macho64-arm64-darwin-exec",  true},  // declared FIRST
        {"x86_64:macho64-x86_64-darwin-exec", true},
    };
    EXPECT_EQ(selectBoundTargetIndex(machoShape, "x86_64"), 1u)
        << "an Intel Mac must bind the x86_64 image, which it can actually run";
    EXPECT_EQ(selectBoundTargetIndex(machoShape, "arm64"), 0u)
        << "an Apple Silicon Mac is unaffected — its arm is already first";
}

// The FALLBACK, on a host this repo has never shipped a target for. It is the
// clause that keeps the rule from being an arch identity check: nothing in it
// knows the names "arm64" or "x86_64", so an unrecognised host still binds the
// first `runOn` match rather than nothing at all.
TEST(HostTargetBinding, FallsBackToTheFirstRunOnMatchWhenNoArmIsNative) {
    auto const candidates = corpusShapeForHostOs("linux");
    EXPECT_EQ(selectBoundTargetIndex(candidates, "riscv64"), 1u)
        << "a host with no native arm must still bind the first runOn match";
}

// The OS gate OUTRANKS the arch preference. Red-on-disable: drop the
// `runsOnHost` guard from the loop and this binds a darwin image on Linux —
// an image this machine's loader cannot even map, chosen because its
// INSTRUCTIONS happen to match.
TEST(HostTargetBinding, NeverBindsATargetWhoseRunOnExcludesThisHost) {
    std::vector<HostBindingCandidate> const candidates = {
        {"arm64:macho64-arm64-darwin-exec", false},   // native arch, wrong OS
        {"arm64:elf64-aarch64-linux-exec",  true},
    };
    EXPECT_EQ(selectBoundTargetIndex(candidates, "arm64"), 1u);
}

// No target admits this host at all ⇒ NO binding, never index 0. Red-on-disable:
// initialise the fallback to `0` instead of the sentinel and this returns a
// target the runner would then compile and try to spawn.
TEST(HostTargetBinding, BindsNothingWhenNoTargetAdmitsThisHost) {
    auto const candidates = corpusShapeForHostOs("plan9");
    EXPECT_EQ(selectBoundTargetIndex(candidates, "x86_64"), kNoBoundTarget);
    EXPECT_EQ(selectBoundTargetIndex({}, "x86_64"), kNoBoundTarget)
        << "a manifest with no targets at all must bind nothing";
}

// The rule must not merely PREFER the native arm — it must find it wherever the
// manifest declared it. Red-on-disable: a `return` after the first runOn match
// (the old rule) passes the first row and reds the other two.
TEST(HostTargetBinding, FindsTheNativeArmAtAnyDeclarationPosition) {
    auto const nativeAt = [](std::size_t position) -> std::size_t {
        std::vector<HostBindingCandidate> candidates = {
            {"x86_64:elf64-x86_64-linux-exec", true},
            {"x86_64:elf64-x86_64-linux-exec", true},
            {"x86_64:elf64-x86_64-linux-exec", true},
        };
        candidates[position].spec = "arm64:elf64-aarch64-linux-exec";
        return selectBoundTargetIndex(candidates, "arm64");
    };
    auto const checkPosition = [&nativeAt](std::size_t position) -> void {
        EXPECT_EQ(nativeAt(position), position)
            << "the native arm was declared at index " << position
            << " and the runner bound something else";
    };
    for (std::size_t position = 0; position < 3; ++position) {
        checkPosition(position);
    }
}
