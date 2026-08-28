// THE LANGUAGE↔TARGET ARCHITECTURE GATE — closure tests for
// [[D-ISA-LANGUAGE-TARGET-GATE-HAS-NO-BEHAVIOURAL-TEST]]. Subject:
// `src/program/cross_validate_language_target.{hpp,cpp}`, reached from BOTH of
// its landed call sites (the driver's per-target chokepoint in `program.cpp`
// and the `dependsOn` resolver's `gather_` in `dependency_resolver.cpp`).
//
// THE RULE, IN THE OPERATOR'S OWN WORDS (plan 06 §5.1 B.13.5): "if I'm
// importing a project written in x86 assembly, my root project must have an x86
// target only, otherwise this will fail, because x86 assembly only runs in x86,
// it's not portable." Generalized: SOME SOURCE LANGUAGES ARE INHERENTLY BOUND
// TO AN INSTRUCTION-SET ARCHITECTURE; MOST ARE PORTABLE. Compared BY EQUALITY
// ONLY — no subset/superset lattice, because "x86-64 also runs 32-bit x86" is
// exactly the capability claim this axis refuses to make.
//
// ── THE FOUR VERDICTS THIS FILE EXISTS TO PIN ────────────────────────────────
//   1. ISA-bound language + MATCHING target      ⇒ builds, no diagnostic.
//   2. ISA-bound language + MISMATCHED target    ⇒ clean reject, 0xD02A, and
//      the message names BOTH declared ISAs.
//   3. PORTABLE language (declares no `isa`) + ANY target ⇒ builds, always.
//      Zero bookkeeping in the common case — this is what keeps C, T-SQL and
//      every non-assembly language from paying for the axis existing.
//   4. ★ Target declaring NO `isa` + ISA-bound language ⇒ FAIL-CLOSED REJECT,
//      never a permissive skip. THIS IS THE ONE A PLAUSIBLE IMPLEMENTATION GETS
//      WRONG AND NOTHING WOULD NOTICE: `isa` is OPTIONAL on the target side (a
//      required key would break every pre-existing `.target.json`), so the
//      natural-looking `if (target.isa().empty()) return true;` reads as
//      politeness and is how x86 text reaches an AArch64 opcode table. The
//      operator confirmed this arm explicitly and unprompted.
//
// ── WHY THESE PINS ARE IMPOSTOR PINS AND NOT SMOKE TESTS ─────────────────────
// A smoke test that pairs the shipped `asm-x86_64-att` with the shipped
// `x86_64` target and then with `arm64` goes GREEN over at least three wrong
// implementations: one keyed on `target.name()`, one keyed on the format kind,
// and one keyed on the per-format `machine` code. So the fixtures below are
// built to DEFEAT each of those specifically:
//
//   * THE TARGET TWINS ARE IDENTICAL IN `name`, in opcode table, and in every
//     other declaration — they differ in `target.isa` AND NOTHING ELSE, and the
//     verdict INVERTS when that one key flips. A `target.name()` implementation
//     cannot tell them apart, so it must return one answer for both and fail
//     one of the two pins whichever answer it picks.
//   * THE LANGUAGE TWINS are the same construction on the language side: one
//     `GrammarSchema` document, two values of the top-level `isa`.
//   * THE TARGET NAMES ARE DELIBERATELY NOT ISA SPELLINGS (`"twin-alpha"`, not
//     `"arm64"`). That makes the message assertions self-proving: finding
//     `aarch64` in the rendered diagnostic can ONLY have come from the declared
//     `target.isa`, because no other field in the document contains that
//     string. A message that printed the target NAME would not match.
//   * ★ `isa != name` IS A PROPERTY OF SHIPPED CONFIG, NOT ONLY OF A FIXTURE.
//     `arm64.target.json` declares `isa: "aarch64"` — ARM's own psABI spelling,
//     which is also what every `elf64-aarch64-*` format uses — while the target
//     document is NAMED `arm64` (the Apple/Microsoft spelling the
//     `macho64-arm64-*` documents use). The shipped inventory does not agree
//     with itself on an arch spelling, so a name-keyed impostor cannot
//     reproduce the shipped verdicts without inventing a second mapping table.
//     The shipped-config pins below assert that divergence directly, so a
//     future edit that "tidies" the two into agreement fails HERE, loudly,
//     instead of quietly removing this file's power to detect an impostor.
//
// ── WHAT BREAKS SILENTLY WITHOUT THESE PINS ──────────────────────────────────
//   * VERDICT 4 FLIPPED TO A PERMISSIVE SKIP. No shipped target omits `isa`
//     today, so nothing in the corpus, the examples or CI would go red. The
//     first `.target.json` written without the key silently re-opens x86-text-
//     on-AArch64, and the failure surfaces as a SIGILL at USER runtime.
//   * THE RESOLVER ARM DROPPED WHILE THE DRIVER ARM SURVIVES. Both arms emit
//     the same code, so a count-only assertion cannot tell which fired. The
//     resolver pins below therefore assert on the `subject` PREFIX — the
//     dependency's manifest path — which only the resolver arm supplies. The
//     driver arm passes an EMPTY subject, so the two are distinguishable in
//     the one place it matters.
//   * THE GATE MOVED INTO `deriveFormat_`. That is the natural-looking home and
//     it is wrong twice: `deriveFormat_` never runs for `SourceMerge` (so half
//     the feature goes unguarded), and its memo key omits node identity (so one
//     dependency's verdict is served to another). Both resolver composition
//     arms are pinned below for exactly that reason.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "program/cross_validate_language_target.hpp"
#include "program/dependency_resolver.hpp"
#include "program/program.hpp"

#include "diagnostic_count.hpp"
#include "repo_root.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

using namespace dss;
using dss::test_support::countCode;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;

namespace fs = std::filesystem;

namespace {

// ── the synthetic documents ─────────────────────────────────────────────────
//
// `loadFromText` on both sides, for the reason the sibling
// `test_cross_validate_target_format.cpp` uses it: it is the real loader with
// the real key parsing, minus the schema-FILE constraints. The engine reads two
// declared strings, so a fixture that can vary exactly one of them at a time is
// the whole experiment.

// A language document. `declaredIsa` EMPTY ⇒ the `isa` key is OMITTED
// ENTIRELY, which is the portable case — and an omitted key is a genuinely
// different input from a present-but-empty one, so the helper must not emit
// `"isa": ""` as a stand-in.
[[nodiscard]] std::shared_ptr<GrammarSchema const>
makeLanguage(std::string_view declaredIsa,
             std::string_view name = "TwinLang") {
    std::string json = std::string{R"({
      "dssSchemaVersion": 1,
      "language": { "name": ")"}
        + std::string{name} + R"(", "version": "0.1.0" },)";
    if (!declaredIsa.empty()) {
        json += "\n      \"isa\": \"" + std::string{declaredIsa} + "\",";
    }
    json += R"(
      "tokens": { "+": [{ "kind": "PlusOp" }] },
      "shapes": { "root": { "sequence": [ "PlusOp" ] } }
    })";
    auto r = GrammarSchema::loadFromText(json);
    if (!r.has_value()) {
        ADD_FAILURE() << "language load failed: "
                      << (r.error().empty() ? "<no diagnostics>"
                                            : r.error()[0].message);
        return nullptr;
    }
    return *r;
}

// A target document. `declaredIsa` EMPTY ⇒ the `target.isa` key is OMITTED,
// which is verdict 4's whole input: a pre-existing `.target.json` written
// before this axis existed.
[[nodiscard]] std::shared_ptr<TargetSchema const>
makeTarget(std::string_view name, std::string_view declaredIsa) {
    std::string json = std::string{R"({
      "dssTargetVersion": 1,
      "target": {"name": ")"} + std::string{name} + "\"";
    if (!declaredIsa.empty()) {
        json += ", \"isa\": \"" + std::string{declaredIsa} + "\"";
    }
    json += R"(},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })";
    auto r = TargetSchema::loadFromText(json);
    if (!r.has_value()) {
        ADD_FAILURE() << "target load failed: "
                      << (r.error().empty() ? "<no diagnostics>"
                                            : r.error()[0].message);
        return nullptr;
    }
    return *r;
}

[[nodiscard]] bool sawCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return true;
    }
    return false;
}

// The whole rendered text of the FIRST diagnostic carrying `code`. Assertions
// compare CONTENT wherever content is what the reader of the diagnostic
// actually needs to act.
[[nodiscard]] std::string messageFor(DiagnosticReporter const& rep,
                                     DiagnosticCode            code) {
    for (auto const& d : rep.all()) {
        if (d.code == code) return d.contextPrefix + d.actual;
    }
    return "<no diagnostic with that code>";
}

// True when SOME 0xD02A diagnostic's text contains `needle`. The resolver pins
// need this rather than `messageFor`: when the root's language is ALSO
// ISA-bound both call sites can speak, and the pin is about whether the
// DEPENDENCY-attributed one is among them.
[[nodiscard]] bool someIsaMessageContains(DiagnosticReporter const& rep,
                                          std::string_view          needle) {
    for (auto const& d : rep.all()) {
        if (d.code != DiagnosticCode::D_LanguageTargetIsaMismatch) continue;
        if ((d.contextPrefix + d.actual).find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// The two ISA spellings used throughout, and two target NAMES that are
// deliberately NOT ISA spellings (see the header note: this is what makes the
// message assertions prove the ISA was read rather than the name).
constexpr std::string_view kX86  = "x86_64";
constexpr std::string_view kArm  = "aarch64";
constexpr std::string_view kTwin = "twin-alpha";

void writeText(fs::path const& p, std::string_view text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    out << text;
    out.flush();
    ASSERT_TRUE(out) << "could not write fixture file " << p.generic_string();
}

[[nodiscard]] std::string readText(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// VERDICT 1 — ISA-BOUND LANGUAGE + MATCHING TARGET ⇒ BUILDS
// ════════════════════════════════════════════════════════════════════════════
//
// Non-vacuous by asserting THREE things, not one: the predicate is true, the
// gate returns true, and NO diagnostic of this code was emitted. An engine that
// returned true while still reporting would pass a bare `EXPECT_TRUE`.

TEST(CrossValidateLanguageTarget, IsaBoundLanguageOnMatchingTargetBuilds) {
    auto lang   = makeLanguage(kX86);
    auto target = makeTarget(kTwin, kX86);
    ASSERT_TRUE(lang && target);

    EXPECT_TRUE(languageTargetIsaCompatible(*lang, *target));

    DiagnosticReporter rep;
    EXPECT_TRUE(crossValidateLanguageTarget(*lang, "twin-lang", *target,
                                            "twin-alpha:some-format",
                                            /*subject=*/{}, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(sawCode(rep, DiagnosticCode::D_LanguageTargetIsaMismatch))
        << "a MATCHING pair must emit nothing at all; a gate that reports "
           "while returning true would make every assembly build noisy";
}

// ════════════════════════════════════════════════════════════════════════════
// VERDICT 2 — ISA-BOUND LANGUAGE + MISMATCHED TARGET ⇒ CLEAN REJECT
// ════════════════════════════════════════════════════════════════════════════

TEST(CrossValidateLanguageTarget, IsaBoundLanguageOnMismatchedTargetRejects) {
    auto lang   = makeLanguage(kX86);
    auto target = makeTarget(kTwin, kArm);
    ASSERT_TRUE(lang && target);

    EXPECT_FALSE(languageTargetIsaCompatible(*lang, *target));

    DiagnosticReporter rep;
    EXPECT_FALSE(crossValidateLanguageTarget(*lang, "twin-lang", *target,
                                             "twin-alpha:some-format",
                                             /*subject=*/{}, rep));
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_LanguageTargetIsaMismatch), 1u)
        << "exactly one — a reject that fires twice reads to the operator as "
           "two separate problems";
}

// ★ THE MESSAGE MUST NAME BOTH ISAs, and this is the pin that makes the
// diagnostic ACTIONABLE rather than merely present. The remediation is "build
// this source for a target whose `target.isa` is x86_64, or rewrite it in the
// dialect for aarch64" — and neither half of that sentence can be reconstructed
// from a message that names only the language and the target. Note both needles
// are ISA VALUES that appear in no other field of either document, so a message
// built from `target.name()` fails this assertion.
TEST(CrossValidateLanguageTarget, MismatchMessageNamesBothDeclaredIsas) {
    auto lang   = makeLanguage(kX86);
    auto target = makeTarget(kTwin, kArm);
    ASSERT_TRUE(lang && target);

    DiagnosticReporter rep;
    ASSERT_FALSE(crossValidateLanguageTarget(*lang, "twin-lang", *target,
                                             "twin-alpha:some-format",
                                             /*subject=*/{}, rep));

    std::string const m =
        messageFor(rep, DiagnosticCode::D_LanguageTargetIsaMismatch);
    EXPECT_NE(m.find(std::string{kX86}), std::string::npos)
        << "must name the LANGUAGE's declared ISA; got: " << m;
    EXPECT_NE(m.find(std::string{kArm}), std::string::npos)
        << "must name the TARGET's declared ISA; got: " << m;
    EXPECT_NE(m.find("twin-lang"), std::string::npos)
        << "must name the language as the CALLER selected it; got: " << m;
    EXPECT_NE(m.find(std::string{kTwin}), std::string::npos)
        << "must name the target document; got: " << m;
}

// The severity and delivery an operator's flags actually see. `Guaranteed` is
// load-bearing: this is the ONLY statement of why the pairing stopped, so the
// reporter's global cap must not be free to drop it on a diagnostic-heavy
// multi-target build.
TEST(CrossValidateLanguageTarget, MismatchIsErrorAndGuaranteedDelivery) {
    auto lang   = makeLanguage(kX86);
    auto target = makeTarget(kTwin, kArm);
    ASSERT_TRUE(lang && target);

    DiagnosticReporter rep;
    ASSERT_FALSE(crossValidateLanguageTarget(*lang, "twin-lang", *target,
                                             "twin-alpha:some-format",
                                             /*subject=*/{}, rep));
    bool checked = false;
    for (auto const& d : rep.all()) {
        if (d.code != DiagnosticCode::D_LanguageTargetIsaMismatch) continue;
        EXPECT_EQ(d.severity, DiagnosticSeverity::Error);
        EXPECT_EQ(d.delivery, DiagnosticDelivery::Guaranteed);
        checked = true;
    }
    EXPECT_TRUE(checked) << "the loop must have run — otherwise this pin "
                            "asserts nothing at all";
}

// ════════════════════════════════════════════════════════════════════════════
// VERDICT 3 — A PORTABLE LANGUAGE BUILDS FOR EVERY TARGET
// ════════════════════════════════════════════════════════════════════════════
//
// The common case, and the reason the gate is free for C. Pinned against ALL
// THREE target shapes — including the ISA-less one, because "portable language"
// and "undeclared target" meeting each other is the combination where a
// fail-closed implementation could over-reach and start refusing C.

TEST(CrossValidateLanguageTarget, PortableLanguageBuildsForEveryTarget) {
    auto portable = makeLanguage(/*declaredIsa=*/"");
    ASSERT_TRUE(portable);
    ASSERT_TRUE(portable->isa().empty())
        << "the fixture must OMIT the key, not declare an empty one";

    struct Row { std::string_view name; std::string_view isa; };
    for (Row const r : {Row{kTwin, kX86}, Row{kTwin, kArm}, Row{kTwin, ""}}) {
        auto target = makeTarget(r.name, r.isa);
        ASSERT_TRUE(target);
        DiagnosticReporter rep;
        EXPECT_TRUE(languageTargetIsaCompatible(*portable, *target))
            << "portable language refused a target declaring '" << r.isa << "'";
        EXPECT_TRUE(crossValidateLanguageTarget(*portable, "portable-lang",
                                                *target, "twin-alpha:fmt",
                                                /*subject=*/{}, rep));
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_FALSE(sawCode(rep, DiagnosticCode::D_LanguageTargetIsaMismatch))
            << "a portable language must never reach this code at all";
    }
}

// ════════════════════════════════════════════════════════════════════════════
// ★ VERDICT 4 — AN UNDECLARED TARGET IS A REJECT, NOT A SKIP
// ════════════════════════════════════════════════════════════════════════════
//
// THE PIN THIS FILE MOST EXISTS FOR. `isa` is optional on the target side, so
// the permissive `if (target.isa().empty()) return true;` is the shape a
// plausible implementation lands on — it looks like tolerance for older
// documents. It is not: an undeclared target cannot be SHOWN to satisfy a
// binding, and "probably fine" is how x86 text reaches an AArch64 opcode table.
// Nothing shipped omits the key today, so ONLY this pin can catch the flip.

TEST(CrossValidateLanguageTarget, UndeclaredTargetIsaFailsClosed) {
    auto lang     = makeLanguage(kX86);
    auto undecl   = makeTarget(kTwin, /*declaredIsa=*/"");
    ASSERT_TRUE(lang && undecl);
    ASSERT_TRUE(undecl->isa().empty())
        << "the fixture must actually omit `target.isa` — if the loader "
           "defaulted it, this pin would be testing nothing";

    EXPECT_FALSE(languageTargetIsaCompatible(*lang, *undecl))
        << "FAIL-CLOSED: an undeclared target must NOT be treated as "
           "satisfying an ISA binding";

    DiagnosticReporter rep;
    EXPECT_FALSE(crossValidateLanguageTarget(*lang, "twin-lang", *undecl,
                                             "twin-alpha:some-format",
                                             /*subject=*/{}, rep));
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_LanguageTargetIsaMismatch), 1u);

    // The two failure arms must READ DIFFERENTLY. "declares no instruction-set
    // architecture" and "executes 'aarch64'" are different problems with
    // different fixes, and a message that printed an empty string for the first
    // would look like a compiler bug rather than a fact about the document.
    std::string const m =
        messageFor(rep, DiagnosticCode::D_LanguageTargetIsaMismatch);
    EXPECT_NE(m.find("declares NO instruction-set architecture"),
              std::string::npos)
        << "the undeclared-target arm must say so in words; got: " << m;
    EXPECT_NE(m.find("target.isa"), std::string::npos)
        << "and must name the KEY the operator has to add; got: " << m;
    EXPECT_NE(m.find(std::string{kX86}), std::string::npos)
        << "and must still name the language's ISA — that is the value the "
           "operator types into the new key; got: " << m;
}

// The complement, and it is what keeps verdict 4 from being over-broad: an
// ISA-LESS TARGET STILL BUILDS EVERY PORTABLE LANGUAGE. If fail-closed had been
// implemented as "empty target isa ⇒ reject" without first returning on a
// portable language, every C build on a pre-existing target document would have
// started failing. The engine's branch ORDER is what prevents that, and this
// pin is what makes the order observable.
TEST(CrossValidateLanguageTarget, UndeclaredTargetStillBuildsPortableLanguages) {
    auto portable = makeLanguage(/*declaredIsa=*/"");
    auto undecl   = makeTarget(kTwin, /*declaredIsa=*/"");
    ASSERT_TRUE(portable && undecl);

    DiagnosticReporter rep;
    EXPECT_TRUE(languageTargetIsaCompatible(*portable, *undecl));
    EXPECT_TRUE(crossValidateLanguageTarget(*portable, "portable-lang", *undecl,
                                            "twin-alpha:fmt",
                                            /*subject=*/{}, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

// ════════════════════════════════════════════════════════════════════════════
// ★★ THE IMPOSTOR PINS — ONE DECLARATION FLIPS, THE VERDICT INVERTS
// ════════════════════════════════════════════════════════════════════════════

// TWO TARGET DOCUMENTS IDENTICAL IN EVERY FIELD BUT `target.isa`. Same `name`,
// same opcode table, same everything — so `target.name()`, the format kind and
// the per-format `machine` code are all CONSTANT across the pair. Only the ISA
// varies, and the verdict must vary with it. An implementation keyed on any of
// those constants returns the same answer for both rows and fails one of them.
TEST(CrossValidateLanguageTarget, TargetTwinsDifferOnlyInIsaAndVerdictInverts) {
    auto lang = makeLanguage(kX86);
    auto twinMatching  = makeTarget(kTwin, kX86);
    auto twinMismatch  = makeTarget(kTwin, kArm);
    ASSERT_TRUE(lang && twinMatching && twinMismatch);

    // The premise of the experiment, asserted rather than assumed: the two
    // documents really are indistinguishable except on this one axis.
    ASSERT_EQ(twinMatching->name(), twinMismatch->name())
        << "the twins must share a NAME or they are not impostors";
    ASSERT_NE(twinMatching->isa(), twinMismatch->isa())
        << "the twins must differ on the one axis under test";

    DiagnosticReporter repOk;
    EXPECT_TRUE(crossValidateLanguageTarget(*lang, "twin-lang", *twinMatching,
                                            "twin-alpha:fmt", {}, repOk));
    EXPECT_EQ(countCode(repOk, DiagnosticCode::D_LanguageTargetIsaMismatch), 0u);

    DiagnosticReporter repBad;
    EXPECT_FALSE(crossValidateLanguageTarget(*lang, "twin-lang", *twinMismatch,
                                             "twin-alpha:fmt", {}, repBad));
    EXPECT_EQ(countCode(repBad, DiagnosticCode::D_LanguageTargetIsaMismatch), 1u);
}

// The same construction on the LANGUAGE side: one grammar document, two values
// of the top-level `isa`, one fixed target. A `language.name()`-keyed
// implementation cannot separate these either.
TEST(CrossValidateLanguageTarget, LanguageTwinsDifferOnlyInIsaAndVerdictInverts) {
    auto langX86 = makeLanguage(kX86, "TwinLang");
    auto langArm = makeLanguage(kArm, "TwinLang");
    auto target  = makeTarget(kTwin, kArm);
    ASSERT_TRUE(langX86 && langArm && target);

    ASSERT_EQ(langX86->name(), langArm->name())
        << "the language twins must share a NAME or they are not impostors";
    ASSERT_NE(langX86->isa(), langArm->isa());

    DiagnosticReporter repBad;
    EXPECT_FALSE(crossValidateLanguageTarget(*langX86, "twin-lang", *target,
                                             "twin-alpha:fmt", {}, repBad));
    EXPECT_EQ(countCode(repBad, DiagnosticCode::D_LanguageTargetIsaMismatch), 1u);

    DiagnosticReporter repOk;
    EXPECT_TRUE(crossValidateLanguageTarget(*langArm, "twin-lang", *target,
                                            "twin-alpha:fmt", {}, repOk));
    EXPECT_EQ(countCode(repOk, DiagnosticCode::D_LanguageTargetIsaMismatch), 0u);
}

// EQUALITY ONLY — no lattice, no "close enough". `x86_64` and `x86-64` are
// different strings and therefore a REJECT, and that is the design rather than
// an oversight: any normalization table would be the capability claim this axis
// refuses to make, and it would have to be maintained per architecture forever.
TEST(CrossValidateLanguageTarget, ComparisonIsExactEqualityNotNormalized) {
    auto lang = makeLanguage("x86_64");
    ASSERT_TRUE(lang);
    for (std::string_view const spelling : {"x86-64", "X86_64", "amd64", "x86"}) {
        auto target = makeTarget(kTwin, spelling);
        ASSERT_TRUE(target);
        EXPECT_FALSE(languageTargetIsaCompatible(*lang, *target))
            << "'" << spelling << "' must NOT be accepted as equal to "
               "'x86_64' — a normalization table here is the rejected "
               "capability-claim design";
    }
}

// ════════════════════════════════════════════════════════════════════════════
// SHIPPED CONFIG — THE VERDICTS AN OPERATOR ACTUALLY GETS
// ════════════════════════════════════════════════════════════════════════════
//
// The synthetic pins prove the ENGINE; these prove the CONFIG the engine reads
// is declared correctly. Both halves are needed: a perfect engine over a
// `.lang.json` that forgot its `isa` key is a gate that never fires.

// ★ THE LOAD-BEARING SHIPPED PROPERTY. `arm64`'s declared ISA is `aarch64`,
// which is NOT its name. This single row is what makes a name-keyed impostor
// impossible to write without a second mapping table — and if someone ever
// "tidies" these two into agreement, this file quietly loses its power to
// detect one. So it is asserted directly, here, with that reason attached.
TEST(CrossValidateLanguageTarget, ShippedArm64DeclaresIsaDifferentFromItsName) {
    auto arm64 = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(arm64.has_value());
    EXPECT_EQ((*arm64)->name(), "arm64");
    EXPECT_EQ((*arm64)->isa(), "aarch64");
    EXPECT_NE((*arm64)->name(), (*arm64)->isa())
        << "if these ever agree, every impostor pin in this file weakens: a "
           "`target.name()` implementation would start reproducing the shipped "
           "verdicts. Keep the psABI spelling in `isa`.";

    auto x86 = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(x86.has_value());
    EXPECT_EQ((*x86)->isa(), "x86_64");
}

// The operator's own example, on shipped documents: x86 assembly builds for
// x86_64 and is REFUSED on arm64.
TEST(CrossValidateLanguageTarget, ShippedAsmX86OnX86BuildsAndOnArm64Rejects) {
    auto asmX86 = GrammarSchema::loadShipped("asm-x86_64-att");
    auto x86    = TargetSchema::loadShipped("x86_64");
    auto arm64  = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(asmX86.has_value() && x86.has_value() && arm64.has_value());
    ASSERT_EQ((*asmX86)->isa(), "x86_64")
        << "the shipped language must actually declare its binding — without "
           "this key the gate is inert no matter how correct the engine is";

    DiagnosticReporter repOk;
    EXPECT_TRUE(crossValidateLanguageTarget(**asmX86, "asm-x86_64-att", **x86,
                                            "x86_64:elf64-x86_64-linux-exec",
                                            {}, repOk));
    EXPECT_EQ(countCode(repOk, DiagnosticCode::D_LanguageTargetIsaMismatch), 0u);

    DiagnosticReporter repBad;
    EXPECT_FALSE(crossValidateLanguageTarget(**asmX86, "asm-x86_64-att",
                                             **arm64,
                                             "arm64:elf64-aarch64-linux-exec",
                                             {}, repBad));
    EXPECT_EQ(countCode(repBad, DiagnosticCode::D_LanguageTargetIsaMismatch), 1u);
}

// The mirror image, and the one that a name-keyed impostor gets wrong: the
// AArch64 dialect declares `aarch64`, the arm64 target declares `aarch64`, and
// they match ACROSS a name that says `arm64`.
TEST(CrossValidateLanguageTarget, ShippedAsmArm64MatchesArm64TargetAcrossTheNameGap) {
    auto asmArm = GrammarSchema::loadShipped("asm-arm64-gas");
    auto arm64  = TargetSchema::loadShipped("arm64");
    auto x86    = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(asmArm.has_value() && arm64.has_value() && x86.has_value());
    ASSERT_EQ((*asmArm)->isa(), "aarch64");

    DiagnosticReporter repOk;
    EXPECT_TRUE(crossValidateLanguageTarget(**asmArm, "asm-arm64-gas", **arm64,
                                            "arm64:elf64-aarch64-linux-exec",
                                            {}, repOk))
        << "the language declares 'aarch64' and the target is NAMED 'arm64' — "
           "matching them is exactly what a declared-value comparison buys";
    EXPECT_EQ(countCode(repOk, DiagnosticCode::D_LanguageTargetIsaMismatch), 0u);

    DiagnosticReporter repBad;
    EXPECT_FALSE(crossValidateLanguageTarget(**asmArm, "asm-arm64-gas", **x86,
                                             "x86_64:elf64-x86_64-linux-exec",
                                             {}, repBad));
    EXPECT_EQ(countCode(repBad, DiagnosticCode::D_LanguageTargetIsaMismatch), 1u);
}

// The portable languages the repo actually ships must declare NO `isa` — this
// is verdict 3 asserted against config rather than a fixture. A stray `isa` on
// `c` would refuse every cross-architecture C build in the corpus.
TEST(CrossValidateLanguageTarget, ShippedPortableLanguagesDeclareNoIsa) {
    for (std::string_view const stem : {"c", "toy", "tsql-subset"}) {
        auto lang = GrammarSchema::loadShipped(stem);
        ASSERT_TRUE(lang.has_value()) << "could not load " << stem;
        EXPECT_TRUE((*lang)->isa().empty())
            << stem << " must stay PORTABLE — declaring an `isa` here would "
                       "refuse every target that does not match it";

        for (std::string_view const t : {"x86_64", "arm64"}) {
            auto target = TargetSchema::loadShipped(t);
            ASSERT_TRUE(target.has_value());
            DiagnosticReporter rep;
            EXPECT_TRUE(crossValidateLanguageTarget(**lang, stem, **target,
                                                    "spec", {}, rep));
            EXPECT_EQ(rep.errorCount(), 0u);
        }
    }
}

// ── the `subject` prefix: WHOSE pairing was refused ─────────────────────────
//
// One predicate and one message body serve both call sites, so the ONLY thing
// distinguishing a root refusal from a dependency refusal is this prefix. The
// resolver pins below rely on it to prove which arm fired, so its behaviour is
// pinned here first.
TEST(CrossValidateLanguageTarget, SubjectPrefixNamesWhosePairingWasRefused) {
    auto lang   = makeLanguage(kX86);
    auto target = makeTarget(kTwin, kArm);
    ASSERT_TRUE(lang && target);

    DiagnosticReporter repRoot;
    ASSERT_FALSE(crossValidateLanguageTarget(*lang, "twin-lang", *target,
                                             "spec", /*subject=*/{}, repRoot));
    std::string const rootMsg =
        messageFor(repRoot, DiagnosticCode::D_LanguageTargetIsaMismatch);
    EXPECT_EQ(rootMsg.rfind("language '", 0), 0u)
        << "a ROOT refusal starts with the language clause — no prefix; got: "
        << rootMsg;

    DiagnosticReporter repDep;
    ASSERT_FALSE(crossValidateLanguageTarget(
        *lang, "twin-lang", *target, "spec",
        "project 'dependsOn': dependency 'some/path/.dss-project.json'",
        repDep));
    std::string const depMsg =
        messageFor(repDep, DiagnosticCode::D_LanguageTargetIsaMismatch);
    EXPECT_NE(depMsg.find("some/path/.dss-project.json"), std::string::npos)
        << "a DEPENDENCY refusal must point at the manifest that declared the "
           "binding, not at the project the operator invoked; got: " << depMsg;
    // Same body, different head — the proof the two arms cannot drift into
    // disagreeing about the same pair.
    EXPECT_NE(depMsg.find(rootMsg), std::string::npos)
        << "the dependency message must be the root message with a prefix; "
           "two independently-written bodies is the drift this shape prevents";
}

// ── diagnostic identity ─────────────────────────────────────────────────────

TEST(CrossValidateLanguageTarget, DLanguageTargetIsaMismatchNameRoundTrip) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::D_LanguageTargetIsaMismatch),
              "D_LanguageTargetIsaMismatch");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::D_LanguageTargetIsaMismatch),
              "D002A");
}

// ════════════════════════════════════════════════════════════════════════════
// ★★ CALL SITE 2 — THE `dependsOn` RESOLVER ARM
// ════════════════════════════════════════════════════════════════════════════
//
// This is the arm the operator's own example runs through: "if I'm importing a
// project written in x86 assembly, my root project must have an x86 target
// only". It is the more important of the two call sites and it is driven here
// through `Program::compileProject`, the resolver's real input path.
//
// ── WHY A MIRRORED CONFIG ROOT IS NECESSARY, AND WHAT IT DOES *NOT* CHANGE ───
// ✔MEASURED against the shipped inventory: NO shipped language can reach this
// gate as a dependency, because the two ISA-bound documents
// (`asm-x86_64-att`, `asm-arm64-gas`) both declare `artifactProfiles: ["cli"]`,
// and `admitNode_` runs TWO rejects ahead of `gather_`'s ISA gate —
// `enforceArtifactProfile` (the profile must be one the LANGUAGE declares) and
// the `NotConsumable` verdict for `cli`. So a dependency using them is refused
// with 0xD010 or 0xD01B before the ISA question is ever asked.
//
// The honest way to reach the gate is therefore to give the compiler a config
// DIRECTORY in which an ISA-bound language also composes — not to stub the
// resolver. The mirror below copies the WHOLE `src/dss-config` tree and edits
// ONE key of ONE file: `asm-x86_64-att`'s `artifactProfiles`. Its `isa` is
// UNTOUCHED and remains the shipped `x86_64`, so the axis under test is read
// from real shipped config; only the orthogonal profile vocabulary is widened
// so the dependency can compose at all. The whole tree is mirrored (not just
// `sources/`) because `DSS_CONFIG_ROOT` is checked first and a set-but-miss
// FALLS THROUGH to the cwd walk — a partial mirror would silently resolve
// targets from wherever the test binary's cwd happened to sit.

namespace {

// A config tree the compiler can be pointed at, with `asm-x86_64-att` widened
// to compose. Returns false (and says why) rather than aborting, so a copy
// failure reads as a fixture problem instead of a subject failure.
class ComposableAsmConfig {
public:
    explicit ComposableAsmConfig(fs::path const& root) : root_(root) {
        std::error_code ec;
        fs::path const dst = root_ / "src" / "dss-config";
        // ⚠ THE PARENT IS CREATED, THE DESTINATION IS NOT, AND THE DIFFERENCE
        // IS NOT COSMETIC. ✔MEASURED on this tree's MinGW/libstdc++ leg (gcc
        // 13.2): `fs::copy(dir, EXISTING_dir, recursive | overwrite_existing)`
        // fails with `File exists` rather than merging, so a mirror built the
        // pre-create-then-overwrite way never materializes and every pin
        // downstream of it fails in the FIXTURE with no statement about the
        // subject. Copying into a destination that does not yet exist takes
        // the create-and-recurse path, which behaves the same everywhere.
        // (`tests/program/test_dependency_resolver.cpp`'s `ConfigMirror` still
        // uses the other spelling and fails on this leg for exactly this
        // reason — a pre-existing defect in a file this lane does not own.)
        fs::create_directories(root_ / "src", ec);
        if (ec) { why_ = "could not create the mirror parent: " + ec.message(); return; }
        fs::copy(dss::test::configRoot(), dst, fs::copy_options::recursive, ec);
        if (ec) { why_ = "could not mirror the shipped config: " + ec.message(); return; }

        // Widen ONLY the profile vocabulary. `isa` is deliberately not touched.
        fs::path const lang = dst / "sources" / "asm-x86_64-att.lang.json";
        std::string    text = readText(lang);
        constexpr std::string_view kFrom = R"("artifactProfiles": ["cli"],)";
        constexpr std::string_view kTo =
            R"("artifactProfiles": ["cli", "staticlib", "module"],)";
        auto const at = text.find(kFrom);
        if (at == std::string::npos) {
            why_ = "asm-x86_64-att.lang.json no longer contains the expected "
                   "artifactProfiles line; this fixture needs updating";
            return;
        }
        text.replace(at, kFrom.size(), kTo);
        std::ofstream out{lang, std::ios::binary | std::ios::trunc};
        out << text;
        out.flush();
        if (!out) { why_ = "could not rewrite the mirrored language document"; return; }
        ok_ = true;
    }

    [[nodiscard]] bool               ok()  const noexcept { return ok_; }
    [[nodiscard]] std::string const& why() const noexcept { return why_; }
    // `DSS_CONFIG_ROOT` wants the directory CONTAINING `src/dss-config`.
    [[nodiscard]] std::string envValue() const { return root_.string(); }

private:
    fs::path    root_;
    bool        ok_ = false;
    std::string why_;
};

// A root manifest and one `path` dependency written in the ISA-bound language.
// `rootTarget` is the ONLY thing the two resolver pins vary.
struct DepFixture {
    fs::path proj;
    fs::path depManifest;
};

[[nodiscard]] DepFixture writeDepFixture(fs::path const& dir,
                                         std::string_view rootLanguage,
                                         std::string_view depProfile,
                                         std::string_view rootTargetSpec) {
    fs::path const dep = dir / "asmdep";
    writeText(dep / "unit.s", ".text\n");
    writeText(dep / std::string{dss::kDependencyManifestName},
              std::string{R"({
  "language": "asm-x86_64-att",
  "artifactProfile": ")"} + std::string{depProfile} + R"(",
  "targets": ["x86_64:elf64-x86_64-linux-exec"],
  "sources": ["unit.s"]
}
)");

    fs::path const proj = dir / "app.dss-project.json";
    std::string const rootSource =
        rootLanguage == "c" ? "main.c" : "main.s";
    writeText(dir / rootSource,
              rootLanguage == "c" ? "int main(void){ return 0; }\n"
                                         : ".text\n");
    writeText(proj, std::string{R"({
  "language": ")"} + std::string{rootLanguage} + R"(",
  "artifactProfile": "cli",
  "targets": [")" + std::string{rootTargetSpec} + R"("],
  "sources": [")" + rootSource + R"("],
  "dependsOn": [{"path": ")" + dep.generic_string() + R"("}]
}
)");
    return {proj, dep / std::string{dss::kDependencyManifestName}};
}

} // namespace

// ★ THE OPERATOR'S EXAMPLE, END TO END, ON THE ArtifactLink ARM. A root project
// targeting arm64 imports a dependency written in x86 assembly. The build must
// STOP, and the message must point at the DEPENDENCY'S manifest rather than at
// the project the operator invoked — the dependency is where the binding was
// declared and where the fix goes.
TEST(CrossValidateLanguageTargetResolver, X86AsmDependencyRefusedByArm64Consumer) {
    ScratchDir dir{Location::Temp, "isa-gate-link-reject"};
    ComposableAsmConfig cfg{dir.path() / "cfg"};
    ASSERT_TRUE(cfg.ok()) << cfg.why();

    auto const fx = writeDepFixture(dir.path(), "c", "staticlib",
                                    "arm64:elf64-aarch64-linux-exec");

    ScopedEnv const configRoot{"DSS_CONFIG_ROOT", cfg.envValue()};
    Program prog;
    prog.setOutputDir(dir.path() / "out");
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(fx.proj.string(), rep), 0)
        << "x86 assembly must not resolve into an arm64 build";

    ASSERT_GE(countCode(rep, DiagnosticCode::D_LanguageTargetIsaMismatch), 1u)
        << "the RESOLVER arm must fire; without it a dependency is cloned, "
           "hooked and built before anyone notices it cannot target this CPU";
    EXPECT_TRUE(someIsaMessageContains(rep, fx.depManifest.generic_string()))
        << "the message must name the DEPENDENCY'S manifest — that is what "
           "makes this the resolver arm rather than the driver arm, and it is "
           "where the operator's fix goes";
    EXPECT_TRUE(someIsaMessageContains(rep, "x86_64"));
    EXPECT_TRUE(someIsaMessageContains(rep, "aarch64"));
}

// ★★ THE INVERSION, AND IT IS WHAT MAKES THE PIN ABOVE NON-VACUOUS. Identical
// fixtures, identical config, identical dependency — the ONLY difference is the
// root's target spec. If the pin above were passing because resolution died
// early for some unrelated reason, this one would fire too (it does not), and
// if this one were passing because the gate never runs, the pin above would be
// green over an engine that does nothing.
TEST(CrossValidateLanguageTargetResolver, X86AsmDependencyAcceptedByX86Consumer) {
    ScratchDir dir{Location::Temp, "isa-gate-link-accept"};
    ComposableAsmConfig cfg{dir.path() / "cfg"};
    ASSERT_TRUE(cfg.ok()) << cfg.why();

    auto const fx = writeDepFixture(dir.path(), "c", "staticlib",
                                    "x86_64:elf64-x86_64-linux-exec");

    ScopedEnv const configRoot{"DSS_CONFIG_ROOT", cfg.envValue()};
    Program prog;
    prog.setOutputDir(dir.path() / "out");
    DiagnosticReporter rep;
    (void) prog.compileProject(fx.proj.string(), rep);

    // The pin is on the CODE, not on the exit status, and deliberately so: this
    // fixture's assembly is a stub, so the build may still fail further
    // downstream for reasons that have nothing to do with this axis. What must
    // be true is that the ARCHITECTURE GATE did not object.
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_LanguageTargetIsaMismatch), 0u)
        << "an x86 assembly dependency under an x86_64 consumer must pass the "
           "ISA gate untouched; got: "
        << messageFor(rep, DiagnosticCode::D_LanguageTargetIsaMismatch);
}

// ★ THE SourceMerge ARM. This is the half that would be silently unguarded if
// the gate were moved into `deriveFormat_` — that function `continue`s before
// running for a source-merge dependency, so the verdict would simply never be
// taken. The gate sits ABOVE the composition fork precisely so one placement
// covers both, and this pin is what makes that placement enforceable.
//
// The root's language must MATCH the dependency's here (the resolver rejects a
// source-merge dependency whose language differs from the consumer's, ahead of
// this gate), so both are `asm-x86_64-att`. That means the driver arm could
// also speak for this project — which is why the assertion is on the message
// naming the DEPENDENCY'S manifest, a thing only the resolver arm supplies.
TEST(CrossValidateLanguageTargetResolver, SourceMergeArmIsGatedToo) {
    ScratchDir dir{Location::Temp, "isa-gate-merge-reject"};
    ComposableAsmConfig cfg{dir.path() / "cfg"};
    ASSERT_TRUE(cfg.ok()) << cfg.why();

    auto const fx = writeDepFixture(dir.path(), "asm-x86_64-att", "module",
                                    "arm64:elf64-aarch64-linux-exec");

    ScopedEnv const configRoot{"DSS_CONFIG_ROOT", cfg.envValue()};
    Program prog;
    prog.setOutputDir(dir.path() / "out");
    DiagnosticReporter rep;
    EXPECT_NE(prog.compileProject(fx.proj.string(), rep), 0);

    ASSERT_GE(countCode(rep, DiagnosticCode::D_LanguageTargetIsaMismatch), 1u);
    EXPECT_TRUE(someIsaMessageContains(rep, fx.depManifest.generic_string()))
        << "the SOURCE-MERGE arm must be gated as well — a gate living in "
           "`deriveFormat_` would never run for this composition verb and the "
           "dependency's x86 text would be spliced into an arm64 build";
}
