// `--dump-predefined-macros` — the effective predefined-macro set as an
// INSPECTABLE artifact (DSS's `gcc -dM -E`).
//
// WHY THIS SUITE EXISTS, beyond covering a new flag. The project's position that
// DSS never defines a C23 conditional-feature non-support macro
// (`__STDC_NO_THREADS__` / `__STDC_NO_VLA__` / `__STDC_NO_ATOMICS__` /
// `__STDC_NO_COMPLEX__`) rested entirely on four config COMMENTS. The flag makes
// the claim observable and `EveryShippedObjectFormatDefinesNoStdcNoMacro` below
// makes it ENFORCED — table-driven over every `.format.json` on disk, so a 25th
// format cannot land carrying one unnoticed.
//
// Pins:
//   * the dump's list comes from `mergePredefinedMacros` — proven by exercising
//     the per-format `availableObjectFormats` filter and the collision arm
//     THROUGH the dump, neither of which the dump implements;
//   * `constant` prints its spelling verbatim; `date`/`time` print a real value;
//   * `line`/`file` and function-like entries print their KIND and refuse to
//     invent a value;
//   * every origin (language / target / format / command-line) is labelled;
//   * a merge conflict fails loud with `C_ConflictingPredefinedMacro` and emits
//     NO macro line at all — not even for the non-colliding majority;
//   * `--define NAME` with no `=VALUE` contributes the value `1` (the claim
//     `preprocessor.hpp` DOCUMENTED and nothing had MEASURED);
//   * the shipped-format sweep, with a per-dimension FLOOR so a collapsed
//     enumeration cannot report a silent pass.

#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/preprocess_config.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"
#include "program/cli_args.hpp"
#include "program/dump_predefined_macros.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// ── Matchers ─────────────────────────────────────────────────────────────────
//
// ★ ONE matcher, used by BOTH the presence assertions and the absence
// assertions. A red-on-disable arm that proves a witness ABSENT must ask the
// same question the passing arm asked; two matchers that differ by a hair
// (`find(name)` vs `find(" name=" + name + " ")`) is how a mutant "passes"
// while the real subject is broken.
[[nodiscard]] bool hasLineFor(std::string_view dump, std::string_view name) {
    return dump.find(" name=" + std::string{name} + " value=")
           != std::string_view::npos;
}

// The whole rendered line for `name`, or "" when absent. Callers assert on
// substrings of THIS, so a witness can never accidentally match a different
// macro's line.
[[nodiscard]] std::string lineFor(std::string_view dump,
                                  std::string_view name) {
    std::string const needle = " name=" + std::string{name} + " value=";
    auto const at = dump.find(needle);
    if (at == std::string_view::npos) return {};
    auto const begin = dump.rfind('\n', at);
    auto const from  = (begin == std::string_view::npos) ? 0u : begin + 1u;
    auto const end   = dump.find('\n', at);
    return std::string{dump.substr(
        from, (end == std::string_view::npos ? dump.size() : end) - from)};
}

// The `value=` remainder of `name`'s line (everything after `value=`).
[[nodiscard]] std::string valueOf(std::string_view dump,
                                  std::string_view name) {
    std::string const line = lineFor(dump, name);
    auto const at = line.find(" value=");
    if (at == std::string::npos) return {};
    return line.substr(at + 7);
}

// ── Fixtures ────────────────────────────────────────────────────────────────

[[nodiscard]] PredefinedMacroDef constantDef(std::string name, std::string value,
                                             std::vector<std::string> formats = {}) {
    PredefinedMacroDef pm;
    pm.name                   = std::move(name);
    pm.kind                   = PredefinedMacroKind::Constant;
    pm.value                  = std::move(value);
    pm.availableObjectFormats = std::move(formats);
    return pm;
}

[[nodiscard]] PredefinedMacroDef kindDef(std::string name,
                                         PredefinedMacroKind kind) {
    PredefinedMacroDef pm;
    pm.name = std::move(name);
    pm.kind = kind;
    return pm;
}

// A request whose three families are all supplied by the caller. Deliberately
// NOT loaded from shipped config: the collision arm is unreachable through real
// config (no shipped triple collides, by construction), and an arm that can only
// be reached by editing a config file is an arm nobody exercises.
struct Fixture {
    std::vector<PredefinedMacroDef> languageMacros;
    std::vector<PredefinedMacroDef> targetMacros;
    std::vector<PredefinedMacroDef> formatMacros;
    std::vector<std::string>        userDefines;
    std::optional<ObjectFormatKind> activeFormat = ObjectFormatKind::Elf;

    [[nodiscard]] PredefinedMacroDumpRequest request() const {
        PredefinedMacroDumpRequest req;
        req.languageName   = "fixture-lang";
        req.targetName     = "fixture-target";
        req.formatName     = "fixture-format";
        req.languageMacros = languageMacros;
        req.targetMacros   = targetMacros;
        req.formatMacros   = formatMacros;
        req.activeFormat   = activeFormat;
        req.userDefines    = userDefines;
        return req;
    }
};

// The four C23 conditional-feature NON-SUPPORT macros DSS must never define.
// Spelled here and nowhere in the engine: this is a TEST's expectation about
// config, never a compiler behaviour keyed on a macro name.
constexpr std::array<std::string_view, 4> kStdcNoMacros{
    "__STDC_NO_THREADS__",
    "__STDC_NO_VLA__",
    "__STDC_NO_ATOMICS__",
    "__STDC_NO_COMPLEX__",
};

} // namespace

// ── The value column: verbatim, real, or honestly absent ────────────────────

TEST(DumpPredefinedMacros, ConstantPrintsItsSpellingVerbatim) {
    Fixture f;
    f.languageMacros = {constantDef("__STDC_VERSION__", "202311L"),
                        constantDef("__ORDER_LITTLE_ENDIAN__", "1234")};
    auto const r = renderPredefinedMacroDump(f.request());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(valueOf(*r, "__STDC_VERSION__"), "202311L");
    EXPECT_EQ(valueOf(*r, "__ORDER_LITTLE_ENDIAN__"), "1234");
    EXPECT_NE(lineFor(*r, "__STDC_VERSION__").find("kind=constant"),
              std::string::npos);
}

// An EMPTY constant spelling is a real declaration (the pe profile's `__stdcall`
// → empty erase), so the line must still appear with an empty value rather than
// be mistaken for "no value" and routed to the no-single-value marker.
TEST(DumpPredefinedMacros, EmptyConstantSpellingStillPrintsAsAValue) {
    Fixture f;
    f.languageMacros = {constantDef("__stdcall", "")};
    auto const r = renderPredefinedMacroDump(f.request());
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(hasLineFor(*r, "__stdcall"));
    EXPECT_EQ(valueOf(*r, "__stdcall"), "");
    EXPECT_EQ(lineFor(*r, "__stdcall").find("no-single-value"),
              std::string::npos);
}

// ★ THE RULE THIS SUITE EXISTS TO PROTECT: a fabricated value is worse than
// none, because a wrong value is trusted and a missing one is not. `line`/`file`
// are offset-derived, so no single value exists.
TEST(DumpPredefinedMacros, OffsetDerivedKindsPrintTheKindNeverAValue) {
    Fixture f;
    f.languageMacros = {kindDef("__LINE__", PredefinedMacroKind::Line),
                        kindDef("__FILE__", PredefinedMacroKind::File)};
    auto const r = renderPredefinedMacroDump(f.request());
    ASSERT_TRUE(r.has_value());

    EXPECT_EQ(valueOf(*r, "__LINE__"),
              std::string{kNoSingleValueOffsetDerived});
    EXPECT_EQ(valueOf(*r, "__FILE__"),
              std::string{kNoSingleValueOffsetDerived});
    // The KIND is still reported — "we cannot give you a value" must not also
    // mean "we cannot tell you what this is".
    EXPECT_NE(lineFor(*r, "__LINE__").find("kind=line"), std::string::npos);
    EXPECT_NE(lineFor(*r, "__FILE__").find("kind=file"), std::string::npos);
    // And no plausible stand-in leaked in: neither `1` (a line number) nor a
    // quoted file name.
    EXPECT_EQ(valueOf(*r, "__LINE__").find('"'), std::string::npos);
}

TEST(DumpPredefinedMacros, FunctionLikeEntryPrintsItsParamsAndNoExpansion) {
    Fixture f;
    PredefinedMacroDef declspec = constantDef("__declspec", "");
    declspec.isFunctionLike = true;
    declspec.params         = {"x"};
    PredefinedMacroDef nullary = constantDef("__nullary", "0");
    nullary.isFunctionLike = true;   // 0-ary: `params.empty()` must NOT decide
    f.languageMacros = {declspec, nullary};

    auto const r = renderPredefinedMacroDump(f.request());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(valueOf(*r, "__declspec"),
              std::string{kNoSingleValueFunctionLike});
    EXPECT_NE(lineFor(*r, "__declspec").find("form=function(x)"),
              std::string::npos);
    // The 0-ary case: `isFunctionLike` discriminates, so this is `function()`
    // and NOT `object` — and its `0` spelling is NOT printed as a value.
    EXPECT_NE(lineFor(*r, "__nullary").find("form=function()"),
              std::string::npos);
    EXPECT_EQ(valueOf(*r, "__nullary"),
              std::string{kNoSingleValueFunctionLike});
}

// `date`/`time` DO have a real value — computed once per run — so refusing to
// print one would be the mirror-image dishonesty.
TEST(DumpPredefinedMacros, DateAndTimePrintRealQuotedSpellings) {
    Fixture f;
    f.languageMacros = {kindDef("__DATE__", PredefinedMacroKind::Date),
                        kindDef("__TIME__", PredefinedMacroKind::Time)};
    auto const r = renderPredefinedMacroDump(f.request());
    ASSERT_TRUE(r.has_value());

    std::string const date = valueOf(*r, "__DATE__");
    std::string const time = valueOf(*r, "__TIME__");
    // SHAPE, not the instant: C 6.10.8.1's `"Mmm dd yyyy"` (11 chars + 2 quotes)
    // and `"hh:mm:ss"` (8 chars + 2 quotes). Asserting the shape rather than a
    // literal keeps the test from depending on the wall clock while still
    // catching an empty / unquoted / marker value.
    ASSERT_EQ(date.size(), 13u) << "got: " << date;
    ASSERT_EQ(time.size(), 10u) << "got: " << time;
    EXPECT_EQ(date.front(), '"');
    EXPECT_EQ(date.back(), '"');
    EXPECT_EQ(time[3], ':');
    EXPECT_EQ(time[6], ':');
    EXPECT_EQ(valueOf(*r, "__DATE__").find("no-single-value"),
              std::string::npos);
    // The shared owner really is shared: the dumped date is the spelling the
    // preprocessor's own `computeDateTime` stores. (Compared on the DATE only —
    // `time` can legitimately tick between the two reads.)
    EXPECT_EQ(date, '"' + translationTimestamp().date + '"');
}

// ── Origin labelling: what makes this an audit instrument ───────────────────

TEST(DumpPredefinedMacros, EveryOriginIsLabelled) {
    Fixture f;
    f.languageMacros = {constantDef("FROM_LANGUAGE", "1")};
    f.targetMacros   = {constantDef("FROM_TARGET", "2")};
    f.formatMacros   = {constantDef("FROM_FORMAT", "3")};
    f.userDefines    = {"FROM_CLI=4"};

    auto const r = renderPredefinedMacroDump(f.request());
    ASSERT_TRUE(r.has_value());
    EXPECT_NE(lineFor(*r, "FROM_LANGUAGE").find("origin=language"),
              std::string::npos);
    EXPECT_NE(lineFor(*r, "FROM_TARGET").find("origin=target"),
              std::string::npos);
    EXPECT_NE(lineFor(*r, "FROM_FORMAT").find("origin=format"),
              std::string::npos);
    EXPECT_NE(lineFor(*r, "FROM_CLI").find("origin=command-line"),
              std::string::npos);
    EXPECT_EQ(valueOf(*r, "FROM_CLI"), "4");
    // The header states the resolved triple, so a section can never be read as
    // belonging to a target it does not describe.
    EXPECT_NE(r->find("object-format-kind=elf"), std::string::npos);
    EXPECT_NE(r->find("effective=3"), std::string::npos);
}

// ── ★ THE MEASUREMENT: `--define NAME` with no `=VALUE` ─────────────────────
//
// `preprocessor.hpp` DOCUMENTED that the value defaults to `1`. This is the
// first assertion that MEASURES it — through the same `splitUserDefine` the
// `<command-line>` prologue and the include-gating pre-scan both use, so the
// measurement is of the real rule and not of a dump-local restatement.
TEST(DumpPredefinedMacros, BareDefineDefaultsItsValueToOne) {
    Fixture f;
    f.userDefines = {"BARE", "STATED=7", "EMPTY="};
    auto const r = renderPredefinedMacroDump(f.request());
    ASSERT_TRUE(r.has_value());

    EXPECT_EQ(valueOf(*r, "BARE"), "1");
    // …and the dump says the value came from the RULE, not from the operator.
    EXPECT_NE(lineFor(*r, "BARE").find("form=object-default-value"),
              std::string::npos);

    EXPECT_EQ(valueOf(*r, "STATED"), "7");
    EXPECT_NE(lineFor(*r, "STATED").find("form=object "), std::string::npos);

    // `--define NAME=` states an EMPTY value, which is a DIFFERENT macro from
    // `--define NAME`. Folding it onto the default would rewrite the request.
    EXPECT_EQ(valueOf(*r, "EMPTY"), "");
    EXPECT_NE(lineFor(*r, "EMPTY").find("form=object "), std::string::npos);
}

// ★ A `--define` that SHADOWS a config predefine is a build the compile path may
// REFUSE (C 6.10.8.1). Two lines for one name must not read as "both in effect",
// and the dump must not invent the verdict either.
TEST(DumpPredefinedMacros, ADefineShadowingAConfigPredefineIsFlaggedNotAdjudicated) {
    Fixture f;
    f.languageMacros = {constantDef("_MSC_VER", "1943"),
                        constantDef("__STDC__", "1")};
    f.userDefines    = {"_MSC_VER=1", "HARMLESS=1"};

    auto const r = renderPredefinedMacroDump(f.request());
    // NOT a refusal: the dump has no directive handler and therefore no standing
    // to reject a build the compiler might accept.
    ASSERT_TRUE(r.has_value());

    std::string const note = std::string{kPredefinedMacroNoteMarker}
                             + " shadowed-predefine name=_MSC_VER";
    EXPECT_NE(r->find(note), std::string::npos)
        << "a --define colliding with a config predefine was presented as fine";
    // Exactly ONE note, and only for the colliding name.
    EXPECT_EQ(r->find(std::string{kPredefinedMacroNoteMarker}
                      + " shadowed-predefine name=HARMLESS"),
              std::string::npos);
    // The note carries its own marker, so a consumer selecting MACRO lines by
    // prefix cannot mistake it for one.
    EXPECT_EQ(note.find(std::string{kPredefinedMacroLineMarker} + " origin="),
              std::string::npos);
    // And it names BOTH governing clauses + both diagnostic codes, because which
    // one applies routes on `isFunctionLike` inside the directive handler —
    // MEASURED: object-like → P001B (C 6.10.8.1, even for an identical value),
    // function-like → P0014 (C 6.10.3p2).
    EXPECT_NE(r->find("6.10.8.1"), std::string::npos);
    EXPECT_NE(r->find("P001B"), std::string::npos);
    EXPECT_NE(r->find("6.10.3p2"), std::string::npos);
    EXPECT_NE(r->find("P0014"), std::string::npos);
}

// The split's own unit-level pins, at the owner.
TEST(DumpPredefinedMacros, SplitUserDefineIsTheOneOwnerOfTheDefault) {
    auto const bare = splitUserDefine("NAME");
    EXPECT_EQ(bare.name, "NAME");
    EXPECT_EQ(bare.value, "1");
    EXPECT_FALSE(bare.valueWasStated);

    auto const stated = splitUserDefine("NAME=1");
    EXPECT_EQ(stated.value, "1");
    EXPECT_TRUE(stated.valueWasStated);   // same value, DIFFERENT provenance

    // FIRST `=`: a value may contain `=`.
    auto const eqInValue = splitUserDefine("EQ=a == b");
    EXPECT_EQ(eqInValue.name, "EQ");
    EXPECT_EQ(eqInValue.value, "a == b");

    auto const empty = splitUserDefine("NAME=");
    EXPECT_EQ(empty.value, "");
    EXPECT_TRUE(empty.valueWasStated);
}

// ── The single-owner proof: behaviour the dump does not implement ───────────
//
// The dump contains no availability predicate. If its list really is
// `mergePredefinedMacros`'s, then flipping the ACTIVE FORMAT — and nothing else
// — must change which entries appear. This is the strongest available evidence
// that there is no second walk, short of reading the source.
TEST(DumpPredefinedMacros, PerFormatFilterComesFromTheMergeNotFromHere) {
    Fixture f;
    f.languageMacros = {constantDef("_WIN32", "1", {"pe"}),
                        constantDef("__ELF__", "1", {"elf"}),
                        constantDef("__STDC__", "1")};   // universal

    f.activeFormat = ObjectFormatKind::Pe;
    auto const pe = renderPredefinedMacroDump(f.request());
    ASSERT_TRUE(pe.has_value());
    EXPECT_TRUE(hasLineFor(*pe, "_WIN32"));
    EXPECT_FALSE(hasLineFor(*pe, "__ELF__"));
    EXPECT_TRUE(hasLineFor(*pe, "__STDC__"));

    f.activeFormat = ObjectFormatKind::Elf;
    auto const elf = renderPredefinedMacroDump(f.request());
    ASSERT_TRUE(elf.has_value());
    EXPECT_FALSE(hasLineFor(*elf, "_WIN32"));
    EXPECT_TRUE(hasLineFor(*elf, "__ELF__"));
    EXPECT_TRUE(hasLineFor(*elf, "__STDC__"));

    // No active format ⇒ only UNIVERSAL entries survive (the merge's documented
    // LSP / FFI-header-parser configuration).
    f.activeFormat = std::nullopt;
    auto const none = renderPredefinedMacroDump(f.request());
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(hasLineFor(*none, "_WIN32"));
    EXPECT_FALSE(hasLineFor(*none, "__ELF__"));
    EXPECT_TRUE(hasLineFor(*none, "__STDC__"));
    EXPECT_NE(none->find("object-format-kind=<none>"), std::string::npos);
}

// ── Fail loud, and never a partial list ────────────────────────────────────

TEST(DumpPredefinedMacros, ConflictYieldsTheMergesMessagesAndNoMacroLine) {
    Fixture f;
    // A name owned by TWO families. `effective` is documented unusable, so a
    // dump that printed the other 2 entries would be exactly the silent
    // last-writer-wins the collision check exists to prevent.
    f.languageMacros = {constantDef("_WIN32", "1"),
                        constantDef("SURVIVOR_A", "1")};
    f.targetMacros   = {constantDef("_WIN32", "2")};
    f.formatMacros   = {constantDef("SURVIVOR_B", "3")};

    auto const r = renderPredefinedMacroDump(f.request());
    ASSERT_FALSE(r.has_value());
    ASSERT_EQ(r.error().size(), 1u);
    // The merge's OWN message, naming BOTH declaring config paths — not a
    // dump-local restatement that could describe a different rule.
    EXPECT_NE(r.error()[0].find("_WIN32"), std::string::npos);
    EXPECT_NE(r.error()[0].find("<lang>.lang.json"), std::string::npos);
    EXPECT_NE(r.error()[0].find("<arch>.target.json"), std::string::npos);
    // NOTHING partial: not the survivors, not even the header.
    for (std::string const& msg : r.error()) {
        EXPECT_EQ(msg.find(std::string{kPredefinedMacroLineMarker}
                           + " origin="),
                  std::string::npos);
    }
}

// The same conflict THROUGH the CLI entry point: `C_ConflictingPredefinedMacro`
// on stderr, an EMPTY stdout, and rc 1.
TEST(DumpPredefinedMacros, CliPathFailsLoudAndWritesNothingToStdout) {
    // Reached with a REAL triple whose language list is made to collide by
    // asking for a name the shipped pe format already owns is not possible
    // (nothing collides in shipped config, by design), so the CLI-level arms
    // below cover the reachable failures and the conflict wording is pinned at
    // the render level above. What this arm pins is the SHAPE of a CLI failure:
    // rc 1, a coded stderr line, and stdout untouched.
    CliArgs args;
    args.dumpPredefinedMacros = true;
    args.languageName         = "c";
    args.targets              = {"x86_64:this-format-does-not-exist"};

    std::ostringstream out;
    std::ostringstream err;
    EXPECT_EQ(dumpPredefinedMacros(args, out, err), 1);
    EXPECT_EQ(out.str(), "");
    EXPECT_NE(err.str().find("D_SchemaLoadFailed"), std::string::npos);
}

TEST(DumpPredefinedMacros, MissingLanguageOrTargetFailsLoud) {
    {
        CliArgs args;
        args.dumpPredefinedMacros = true;
        args.targets              = {"x86_64:elf64-x86_64-linux-exec"};
        std::ostringstream out, err;
        EXPECT_EQ(dumpPredefinedMacros(args, out, err), 1);
        EXPECT_EQ(out.str(), "");
        EXPECT_NE(err.str().find("--language"), std::string::npos);
    }
    {
        CliArgs args;
        args.dumpPredefinedMacros = true;
        args.languageName         = "c";
        std::ostringstream out, err;
        EXPECT_EQ(dumpPredefinedMacros(args, out, err), 1);
        EXPECT_EQ(out.str(), "");
        EXPECT_NE(err.str().find("--target"), std::string::npos);
    }
}

TEST(DumpPredefinedMacros, MalformedTargetSpecFailsLoudBeforeAnyOutput) {
    CliArgs args;
    args.dumpPredefinedMacros = true;
    args.languageName         = "c";
    args.targets              = {"no-colon-here"};
    std::ostringstream out, err;
    EXPECT_EQ(dumpPredefinedMacros(args, out, err), 1);
    EXPECT_EQ(out.str(), "");
    EXPECT_NE(err.str().find("D_InvalidTargetSpec"), std::string::npos);
    EXPECT_NE(err.str().find("MissingColon"), std::string::npos);
}

// All-or-nothing ACROSS targets: a bad third target must not leave the first two
// on stdout looking like a complete answer.
TEST(DumpPredefinedMacros, AFailureOnALaterTargetPrintsNothingForTheEarlierOnes) {
    CliArgs args;
    args.dumpPredefinedMacros = true;
    args.languageName         = "c";
    args.targets              = {"x86_64:elf64-x86_64-linux-exec",
                                 "x86_64:pe64-x86_64-windows-exec",
                                 "x86_64:not-a-real-format"};
    std::ostringstream out, err;
    EXPECT_EQ(dumpPredefinedMacros(args, out, err), 1);
    EXPECT_EQ(out.str(), "")
        << "the two good sections leaked despite the run failing";
}

// ── The CLI happy path over real shipped config ────────────────────────────

TEST(DumpPredefinedMacros, RealTripleDumpsAndSeparatesTargets) {
    CliArgs args;
    args.dumpPredefinedMacros = true;
    args.languageName         = "c";
    args.targets              = {"x86_64:elf64-x86_64-linux-exec",
                                 "x86_64:pe64-x86_64-windows-exec"};
    args.defines              = {"SQLITE_TEST", "SQLITE_THREADSAFE=0"};

    std::ostringstream out, err;
    ASSERT_EQ(dumpPredefinedMacros(args, out, err), 0) << err.str();
    std::string const dump = out.str();

    // One section per target, each naming its own triple.
    EXPECT_NE(dump.find("format=elf64-x86_64-linux-exec"), std::string::npos);
    EXPECT_NE(dump.find("format=pe64-x86_64-windows-exec"), std::string::npos);
    EXPECT_NE(dump.find("object-format-kind=elf"), std::string::npos);
    EXPECT_NE(dump.find("object-format-kind=pe"), std::string::npos);

    // The command line reached the output, defaults settled.
    EXPECT_EQ(valueOf(dump, "SQLITE_TEST"), "1");
    EXPECT_EQ(valueOf(dump, "SQLITE_THREADSAFE"), "0");

    // A real language constant, a real derived kind, and a real target/format
    // predefine all appear — the three config families are genuinely wired.
    EXPECT_NE(lineFor(dump, "__STDC_VERSION__").find("origin=language"),
              std::string::npos);
    EXPECT_NE(lineFor(dump, "__LINE__").find("kind=line"), std::string::npos);
    EXPECT_NE(lineFor(dump, "__x86_64__").find("origin=target"),
              std::string::npos);

    // FLOOR: a section that enumerated nothing must not read as a pass.
    EXPECT_GE(std::count(dump.begin(), dump.end(), '\n'), 40)
        << "two sections over c's 33 language predefines cannot be "
           "this short — the enumeration collapsed";
}

// `_WIN32` is pe-ONLY in shipped config. Its presence in exactly one of two
// sections is the shipped-config witness that the per-format filter is live end
// to end (the fixture arm above proves the mechanism; this proves the wiring).
TEST(DumpPredefinedMacros, ShippedPeOnlyMacroAppearsOnlyInThePeSection) {
    auto sectionFor = [](std::string const& spec) {
        CliArgs args;
        args.dumpPredefinedMacros = true;
        args.languageName         = "c";
        args.targets              = {spec};
        std::ostringstream out, err;
        EXPECT_EQ(dumpPredefinedMacros(args, out, err), 0) << err.str();
        return out.str();
    };
    std::string const pe  = sectionFor("x86_64:pe64-x86_64-windows-exec");
    std::string const elf = sectionFor("x86_64:elf64-x86_64-linux-exec");
    EXPECT_TRUE(hasLineFor(pe, "_WIN32"));
    EXPECT_FALSE(hasLineFor(elf, "_WIN32"));
    // …and the pe-only function-like predefine reports its params, not a value.
    EXPECT_NE(lineFor(pe, "__declspec").find("form=function(x)"),
              std::string::npos);
    EXPECT_EQ(valueOf(pe, "__declspec"),
              std::string{kNoSingleValueFunctionLike});
}

// ── ★ THE HIGH-VALUE ASSERTION ─────────────────────────────────────────────
//
// For EVERY shipped `.format.json` on disk, the effective set defines NONE of
// the four C23 conditional-feature non-support macros. Table-driven over the
// DIRECTORY, so a 25th format is swept the day it lands rather than the day
// someone remembers to add it here.
//
// Per-dimension FLOORS, because this repo has shipped the exact bug where a
// guard passed while scanning nothing: the format count, the target count, the
// number of (target × format) arms actually merged, and the per-arm effective
// size are all asserted. A collapsed enumeration reds instead of passing.
TEST(DumpPredefinedMacros, EveryShippedObjectFormatDefinesNoStdcNoMacro) {
    auto const formatsDir = dss::test::configRoot() / "object-formats";
    auto const targetsDir = dss::test::configRoot() / "targets";
    ASSERT_TRUE(std::filesystem::is_directory(formatsDir)) << formatsDir;
    ASSERT_TRUE(std::filesystem::is_directory(targetsDir)) << targetsDir;

    auto grammarR = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(grammarR.has_value());
    auto const languageMacros = (*grammarR)->preprocess().predefinedMacros;

    // Every shipped target, by logical name.
    std::vector<std::string> targetNames;
    for (auto const& e : std::filesystem::directory_iterator{targetsDir}) {
        std::string const fn = e.path().filename().string();
        auto const at = fn.find(".target.json");
        if (at == std::string::npos) continue;
        targetNames.push_back(fn.substr(0, at));
    }
    std::sort(targetNames.begin(), targetNames.end());
    ASSERT_GE(targetNames.size(), 2u) << "the target enumeration collapsed";

    std::size_t formatFiles     = 0;   // `.format.json` files seen on disk
    std::size_t formatsLoaded   = 0;   // …that `loadShipped` accepted
    std::size_t armsMerged      = 0;   // (target × format) merges performed
    std::size_t formatsCovered  = 0;   // formats contributing >= 1 arm
    std::vector<std::string> offenders;

    for (auto const& entry : std::filesystem::directory_iterator{formatsDir}) {
        std::string const fn = entry.path().filename().string();
        auto const at = fn.find(".format.json");
        if (at == std::string::npos) continue;
        ++formatFiles;
        std::string const formatName = fn.substr(0, at);

        auto formatR = ObjectFormatSchema::loadShipped(formatName);
        if (!formatR.has_value()) {
            // A format whose schema does not even load cannot contribute a macro
            // to any build. Counted (so the arithmetic below stays closed) and
            // skipped — never silently ignored.
            continue;
        }
        ++formatsLoaded;
        auto const formatMacros = (*formatR)->predefinedMacros();
        auto const kind         = (*formatR)->kind();

        // EVERY target is paired with EVERY format, and the pair is NOT
        // pre-filtered for plausibility: the question "does this format's
        // effective set define one of the four?" is answerable for any pair, and
        // filtering would be an identity branch AND would hand a
        // pairs-with-nothing format a silent skip.
        std::size_t armsForThisFormat = 0;
        for (std::string const& targetName : targetNames) {
            auto targetR = TargetSchema::loadShipped(targetName);
            ASSERT_TRUE(targetR.has_value()) << targetName;

            PredefinedMacroDumpRequest req;
            req.languageName   = "c";
            req.targetName     = targetName;
            req.formatName     = formatName;
            req.languageMacros = languageMacros;
            req.targetMacros   = (*targetR)->predefinedMacros();
            req.formatMacros   = formatMacros;
            req.activeFormat   = kind;

            auto const rendered = renderPredefinedMacroDump(req);
            // A shipped triple must never collide — the merge refusing here IS
            // the finding, so surface it rather than skipping the arm.
            ASSERT_TRUE(rendered.has_value())
                << targetName << ':' << formatName << " -> "
                << (rendered.error().empty() ? std::string{} : rendered.error()[0]);
            ++armsMerged;
            ++armsForThisFormat;

            // FLOOR, per arm: c declares 33 language predefines and the
            // filter removes only the format-gated ones, so an arm reporting a
            // near-empty set has not measured anything.
            MergedPredefinedMacros const merged = mergePredefinedMacros(
                req.languageMacros, req.targetMacros, req.formatMacros, kind);
            ASSERT_GE(merged.effective.size(), 20u)
                << targetName << ':' << formatName
                << " — effective set too small to be a real answer";

            for (std::string_view const banned : kStdcNoMacros) {
                if (hasLineFor(*rendered, banned)) {
                    offenders.push_back(std::string{banned} + " @ " + targetName
                                        + ':' + formatName);
                }
            }
        }
        if (armsForThisFormat > 0) ++formatsCovered;
    }

    // FLOORS. Each names the dimension it protects, so a future collapse says
    // which enumeration went empty.
    EXPECT_GE(formatFiles, 20u)   << "the .format.json enumeration collapsed";
    EXPECT_GE(formatsLoaded, 20u) << "shipped formats stopped loading";
    EXPECT_EQ(formatsCovered, formatsLoaded)
        << "a loadable format contributed no arm — it would be swept silently";
    EXPECT_GE(armsMerged, formatsLoaded * targetNames.size())
        << "fewer (target x format) arms ran than the two enumerations imply";

    EXPECT_TRUE(offenders.empty())
        << "DSS must never define a C23 conditional-feature NON-SUPPORT macro "
           "— the position is to REALIZE each feature, not announce a gap. "
           "Offending (macro @ target:format): "
        << [&] {
               std::string s;
               for (auto const& o : offenders) s += o + "; ";
               return s;
           }();

    // The sweep is only meaningful if the four names could have been FOUND had
    // they been there. Prove the matcher works against this very corpus by
    // asking it for a name the corpus DOES declare.
    {
        auto targetR = TargetSchema::loadShipped(targetNames.front());
        ASSERT_TRUE(targetR.has_value());
        auto formatR = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
        ASSERT_TRUE(formatR.has_value());
        PredefinedMacroDumpRequest req;
        req.languageName   = "c";
        req.targetName     = targetNames.front();
        req.formatName     = "elf64-x86_64-linux-exec";
        req.languageMacros = languageMacros;
        req.targetMacros   = (*targetR)->predefinedMacros();
        req.formatMacros   = (*formatR)->predefinedMacros();
        req.activeFormat   = (*formatR)->kind();
        auto const rendered = renderPredefinedMacroDump(req);
        ASSERT_TRUE(rendered.has_value());
        EXPECT_TRUE(hasLineFor(*rendered, "__STDC_VERSION__"))
            << "the absence matcher cannot find a macro that IS present, so "
               "its `absent` verdicts prove nothing";
    }
}

// ── The CLI surface: help text + the parser contract ───────────────────────
//
// ★ WHY THE HELP TEXT IS PINNED AT ALL. `D-CLI-HELP-OMITS-DEFINE-FLAG` records
// exactly this defect: `--define` was parsed for many cycles while absent from
// the help, so the only discoverable spelling was the compiler's own source. A
// verification instrument nobody can find verifies nothing.
// ★ EVERY WITNESS IS UNIQUE TO THE ONE PLACE IT PINS, and that took two rounds of
// actually RUNNING the failure arm to get right — reading the assertions would
// have found neither miss:
//   · v1 asserted only `find("--dump-predefined-macros")`. The flag name appears
//     in THREE places (usage block, Modes entry, Examples block), so deleting the
//     Modes ENTRY left the other two and the pin stayed GREEN over a help text
//     that no longer explained the flag. FAIL-OPEN.
//   · v2 used `"dsscp --dump-predefined-macros --language"` for the
//     usage block — which the EXAMPLES line also contains, so deleting the usage
//     line stayed GREEN too. FAIL-OPEN again, one layer down.
// v3 keys the usage witness on the `<name> --target <spec>` PLACEHOLDERS (usage
// only) and the examples witness on `c` (examples only). All three
// deletions now red independently — MEASURED, not argued.
TEST(DumpPredefinedMacros, HelpTextDocumentsTheFlagAndItsAbsentAlias) {
    auto const text = cliHelpText();

    // (1) the USAGE block. `<name>`/`<spec>` are placeholders no other line uses.
    EXPECT_NE(text.find("--dump-predefined-macros --language <name> "
                        "--target <spec>"),
              std::string::npos);
    // (2) the MODES entry: what the flag DOES. Unique to that entry.
    EXPECT_NE(text.find("print the EFFECTIVE predefined-macro set"),
              std::string::npos);
    // …and that it needs a TRIPLE. A flag whose help omits its requirements
    // sends the operator to the error message to discover them.
    EXPECT_NE(text.find("Requires --language and at least one --target"),
              std::string::npos);
    // …and that it does NOT compile, which is the whole behavioural contract.
    EXPECT_NE(text.find("exit without compiling"), std::string::npos);
    // …and the deliberate NON-alias, stated so nobody adds one: `-dM` is one
    // letter-cluster of gcc's `-d<CHARS>` family, and claiming that spelling
    // would promise a family DSS does not implement.
    EXPECT_NE(text.find("no `-dM` alias"), std::string::npos);
    // (3) the EXAMPLES block: a runnable invocation, not just a synopsis.
    EXPECT_NE(text.find("--dump-predefined-macros --language c"),
              std::string::npos);
}

namespace {
// argv builder — storage outlives the parse (mirrors tests/program/test_cli_args.cpp).
struct Argv {
    std::vector<std::string> storage;
    std::vector<char*>       ptrs;
    explicit Argv(std::initializer_list<std::string> args) {
        storage.assign(args.begin(), args.end());
        ptrs.reserve(storage.size() + 1);
        for (auto& s : storage) ptrs.push_back(s.data());
        ptrs.push_back(nullptr);
    }
    [[nodiscard]] int    argc() const noexcept { return static_cast<int>(storage.size()); }
    [[nodiscard]] char** argv() noexcept { return ptrs.data(); }
};
} // namespace

TEST(DumpPredefinedMacros, FlagIsAModeRequiringALanguageAndATarget) {
    {   // the happy shape
        Argv a{"dsscp", "--dump-predefined-macros",
               "--language", "c",
               "--target", "x86_64:elf64-x86_64-linux-exec"};
        auto r = parseCliArgs(a.argc(), a.argv());
        ASSERT_TRUE(r.has_value());
        EXPECT_TRUE(r->dumpPredefinedMacros);
        EXPECT_FALSE(r->helpMode);
        EXPECT_FALSE(r->lspMode);
    }
    {   // no --language: the triple is incomplete, so REFUSE at the parser
        Argv a{"dsscp", "--dump-predefined-macros",
               "--target", "x86_64:elf64-x86_64-linux-exec"};
        auto r = parseCliArgs(a.argc(), a.argv());
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().kind, CliArgsError::MissingLanguage);
        // The message must NAME this mode — a mode-list omitting the mode the
        // operator typed reads as "this flag is not supported".
        EXPECT_NE(r.error().detail.find("dump-predefined-macros"),
                  std::string::npos);
    }
    {   // no --target
        Argv a{"dsscp", "--dump-predefined-macros",
               "--language", "c"};
        auto r = parseCliArgs(a.argc(), a.argv());
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().kind, CliArgsError::EmptyTargetList);
        EXPECT_NE(r.error().detail.find("dump-predefined-macros"),
                  std::string::npos);
    }
    {   // MUTUALLY EXCLUSIVE with a compile: "does the compile still happen?"
        // must never be an open question.
        Argv a{"dsscp", "--dump-predefined-macros",
               "--compile", "x.c",
               "--language", "c",
               "--target", "x86_64:elf64-x86_64-linux-exec"};
        auto r = parseCliArgs(a.argc(), a.argv());
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().kind, CliArgsError::DuplicateModeFlag);
    }
    {   // `-dM` is NOT an alias and must stay an unknown flag, so adding one
        // later is a deliberate act rather than a silent accretion.
        Argv a{"dsscp", "-dM", "--language", "c",
               "--target", "x86_64:elf64-x86_64-linux-exec"};
        auto r = parseCliArgs(a.argc(), a.argv());
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().kind, CliArgsError::UnknownFlag);
    }
    {   // --stack-reserve asks for a field in an EMITTED IMAGE; this mode emits
        // none, so the request must be REFUSED rather than silently dropped.
        Argv a{"dsscp", "--dump-predefined-macros",
               "--language", "c",
               "--target", "x86_64:pe64-x86_64-windows-exec",
               "--stack-reserve", "4194304"};
        auto r = parseCliArgs(a.argc(), a.argv());
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().kind, CliArgsError::NoModeSelected);
    }
}

// ── The kind vocabulary has ONE owner ──────────────────────────────────────
//
// `predefinedMacroKindName` prints the same verb `parsePredefinedMacroArray`
// accepts for `"kind"`. Pin the direction that matters: every name the table
// emits round-trips back to its enumerator, so the printed word is always a word
// an operator can write into a config.
TEST(DumpPredefinedMacros, EveryKindNameRoundTripsToItsEnumerator) {
    constexpr std::array<PredefinedMacroKind, 5> kAll{
        PredefinedMacroKind::Line, PredefinedMacroKind::File,
        PredefinedMacroKind::Constant, PredefinedMacroKind::Date,
        PredefinedMacroKind::Time};
    for (PredefinedMacroKind const k : kAll) {
        auto const name = predefinedMacroKindName(k);
        EXPECT_FALSE(name.empty());
        auto const back = predefinedMacroKindFromName(name);
        ASSERT_TRUE(back.has_value()) << name;
        EXPECT_EQ(*back, k) << name;
    }
    // Distinctness: `EnumNameTable::name` falls back to row 0, so two
    // enumerators sharing a spelling would silently collapse a kind's identity.
    EXPECT_NE(predefinedMacroKindName(PredefinedMacroKind::Line),
              predefinedMacroKindName(PredefinedMacroKind::File));
    EXPECT_NE(predefinedMacroKindName(PredefinedMacroKind::Date),
              predefinedMacroKindName(PredefinedMacroKind::Time));
    EXPECT_FALSE(predefinedMacroKindFromName("version").has_value())
        << "`version` is a LOAD-time lowering to Constant, not a runtime kind — "
           "a table row for it would claim a kind the engine cannot hold";
}
