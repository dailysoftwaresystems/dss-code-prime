// D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN — the C-symbol decoration rule is
// declared by the OBJECT FORMAT, once, and every format must declare it.
//
// ★ WHAT THE ANCHOR IS ABOUT. Before this axis, ONE per-format fact had TWO
// OWNERS: the closed C++ table `kCManglingRules` (src/ffi/mangling/c_mangle.cpp,
// keyed on `ObjectFormatKind`), AND a duplicate literal encoding in the format
// descriptors themselves — `macho64-*-exec` ships
// `processExit.importMangledName: "_exit"` while its elf/pe siblings ship
// `"exit"`. Two owners, two languages, nothing forcing them to agree. This file
// pins the config-side owner: the vocabulary, the loader's REQUIRED rule, the
// in-memory `validate()` rule, and the declaration on every shipped format.
//
// ★ THE STAKES ARE NOT COSMETIC, and the evidence is MEASURED. On BOTH
// undecorated formats the underscored spelling already names a DIFFERENT
// function:
//   * `nm -D /lib/x86_64-linux-gnu/libc.so.6` (MEASURED 2026-08-06, WSL Ubuntu)
//     → `exit@@GLIBC_2.2.5` AND `_exit@@GLIBC_2.2.5`, i.e. C `exit(3)` (flushes
//     stdio) and POSIX `_exit(2)` (does not);
//   * `objdump -p C:/Windows/System32/ucrtbase.dll` (MEASURED 2026-08-06)
//     → `exit`, `_exit` AND `_Exit`, three distinct exports.
// A format that wrongly claimed `leading-underscore` would therefore produce a
// program that LINKS and RUNS and silently loses buffered output — not a build
// error. That is why the rule is REQUIRED rather than optional-with-default.
//
// ★★ THESE TESTS ARE DESIGNED AGAINST THIS PROJECT'S FOUR MEASURED VACUITY
// SPECIES, because a pin that cannot go red is worse than no pin:
//   (a) a negative fixture SUBSUMED by a later rule — every reject below
//       asserts `countAtPath` on its OWN pointer plus a MEASURED `errorCount`,
//       so a second, unrelated rejection reason reds the pin instead of hiding
//       inside it;
//   (b) a pin driving an arm that cannot produce what it asserts absent — T3
//       exists precisely because T2 structurally CANNOT cover the validate()
//       arm (see T3's own comment);
//   (c) a grep guard satisfied by the comment beside the fix — nothing here
//       greps source text; every assertion runs the real loader/validator;
//   (d) a non-fatal `EXPECT_*` degrading to a vacuous pass — the enumeration in
//       T4 asserts its own population SIZE, so a wrong root path (empty loop)
//       reds instead of passing silently.

#include "core/types/object_format_kind.hpp"
#include "link/object_format_backend.hpp"
#include "link/object_format_schema.hpp"
#include "format_reject_support.hpp"   // countAtPath / errorCount / rejectSummary
#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using dss::link_format::test::countAtPath;
using dss::link_format::test::errorCount;
using dss::link_format::test::rejectSummary;

// ─────────────────────────────────────────────────────────────────────────
// T1 — the closed vocabulary round-trips, and the SENTINEL IS NOT NAMEABLE.
// ─────────────────────────────────────────────────────────────────────────
//
// ★ WHY THE SENTINEL ARM IS THE POINT OF THIS TEST. `object_format_kind.hpp`
// carries a written-down lesson titled "★ THE SENTINEL SPELLS CORRECTLY":
// `ObjectFormatKind::Unknown` DOES carry the name "unknown", so a config site
// that validates a format name only through `objectFormatKindFromName` accepts
// the project's universal invalid sentinel as if it named a real format, and
// every downstream per-kind dispatch then matches NOTHING and takes a default
// arm. That cost the project a rule. `CSymbolDecorationScheme::Unspecified`
// must not repeat it: it has NO spelling in either direction, so the
// required-declaration rules below (which both key on
// `cSymbolDecorationSchemeName(...).empty()`) cannot be satisfied by a
// deliberately-spelled sentinel.
//
// RED LEVER (executed, not merely asserted): add an `Unspecified` arm returning
// "unspecified" to `cSymbolDecorationSchemeName` and a matching row to
// `cSymbolDecorationSchemeFromName`. `SentinelIsNotNameable` goes red on both
// halves.

TEST(CSymbolDecorationVocabulary, SchemeNamesRoundTrip) {
    struct Row { CSymbolDecorationScheme scheme; std::string_view spelling; };
    for (Row const row : {Row{CSymbolDecorationScheme::None, "none"},
                          Row{CSymbolDecorationScheme::LeadingUnderscore,
                              "leading-underscore"}}) {
        EXPECT_EQ(cSymbolDecorationSchemeName(row.scheme), row.spelling);
        auto const back = cSymbolDecorationSchemeFromName(row.spelling);
        ASSERT_TRUE(back.has_value()) << row.spelling;
        EXPECT_EQ(*back, row.scheme) << row.spelling;
    }

    // A typo never falls back to a real scheme — it fails the lookup.
    for (char const* bad : {"leading_underscore", "Leading-Underscore",
                            "underscore", "_", "None", "nOne", "leading"}) {
        EXPECT_FALSE(cSymbolDecorationSchemeFromName(bad).has_value()) << bad;
    }
}

TEST(CSymbolDecorationVocabulary, SentinelIsNotNameable) {
    // Forward: the sentinel has NO spelling. This is the exact property the
    // two required-declaration rules read — both ask
    // `cSymbolDecorationSchemeName(scheme).empty()`.
    EXPECT_TRUE(cSymbolDecorationSchemeName(
                    CSymbolDecorationScheme::Unspecified).empty())
        << "the invalid sentinel must have no spelling, or a schema left at it "
           "would look declared to every rule that keys on the name";

    // Reverse: nothing spells it — not the enumerator's own identifier, not
    // the empty string a `\"scheme\": \"\"` typo produces.
    EXPECT_FALSE(cSymbolDecorationSchemeFromName("unspecified").has_value());
    EXPECT_FALSE(cSymbolDecorationSchemeFromName("").has_value());
    EXPECT_FALSE(cSymbolDecorationSchemeFromName("Unspecified").has_value());

    // And the two REAL schemes are still distinguishable from it, so the
    // emptiness above is a property of the sentinel and not of the helper.
    EXPECT_FALSE(cSymbolDecorationSchemeName(
                     CSymbolDecorationScheme::None).empty());
    EXPECT_FALSE(cSymbolDecorationSchemeName(
                     CSymbolDecorationScheme::LeadingUnderscore).empty());
}

// ─────────────────────────────────────────────────────────────────────────
// T2 — the LOADER rejects a missing key, and rejects it for THAT reason only.
// ─────────────────────────────────────────────────────────────────────────

namespace {

// The shipped-config tree, resolved through the ONE test-side resolver so an
// OUT-OF-TREE build finds it (the `test_shipped_type_consistency.cpp`
// precedent). Empty on a miss, and every caller ASSERTs on it.
[[nodiscard]] fs::path objectFormatsDir() {
    auto const root = dss::test::findConfigRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *root / "object-formats";
}

[[nodiscard]] std::string readFile(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    if (!in) {
        ADD_FAILURE() << "cannot open " << p.string();
        return {};
    }
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

// The minimal ELF document the synthetic pins vary ONE key of, so every
// rejection is attributable to that key and not to a missing sibling — the
// `headerCaseFormatJson` discipline, one axis over.
[[nodiscard]] std::string decorationFormatJson(std::string_view decorationLine) {
    return std::string{R"({
      "dssObjectFormatVersion": 1,
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "cCallingConvention": { "convention": "sysv_amd64" },
      "outputExtension": ".o",
      )"} + std::string{decorationLine} + R"(
      "format": { "name": "csd-stub", "version": "1.0", "kind": "elf" },
      "elf": { "class": "elf64", "data": "lsb", "machine": 62 }
    })";
}

}  // namespace

// ★ THE FIXTURE IS A SHIPPED `-exec` FILE WITH EXACTLY ONE KEY ERASED, and
// that shape is deliberate rather than convenient. `format_reject_support.hpp`
// records what happened the last time a required-declaration rule landed:
// every exec-flavored negative fixture in the tree omitted `processExit`, so
// each was instantly rejected for the NEW reason and went vacuous in place,
// silently, with the suite still green. A fixture COPIED VERBATIM from a
// shipped file cannot drift into that state — the control below proves the
// unmodified document loads clean, so the rejection is attributable to the
// single erased key and to nothing else.
TEST(CSymbolDecorationLoader, MissingKeyRejectedOnAShippedExecMinusOneKey) {
    auto const dir = objectFormatsDir();
    ASSERT_FALSE(dir.empty());

    // Both families, because the two answers are different and a rule that
    // only ever saw `none` would not notice a macho file losing its `_`.
    for (char const* leaf : {"elf64-x86_64-linux-exec.format.json",
                             "macho64-arm64-darwin-exec.format.json"}) {
        SCOPED_TRACE(leaf);
        auto const text = readFile(dir / leaf);
        ASSERT_FALSE(text.empty());

        // CONTROL: unmodified, the shipped document loads with NO diagnostics.
        auto ok = ObjectFormatSchema::loadFromText(text, leaf);
        ASSERT_TRUE(ok.has_value())
            << "the unmodified shipped format must load clean, or the reject "
               "below proves nothing about the erased key: "
            << rejectSummary(ok);

        // Erase exactly ONE key. (An in-memory copy — the file on disk is
        // never reserialized.)
        nlohmann::json doc = nlohmann::json::parse(text);
        ASSERT_TRUE(doc.contains("cSymbolDecoration"))
            << leaf << " must declare cSymbolDecoration (see T4)";
        doc.erase("cSymbolDecoration");

        auto r = ObjectFormatSchema::loadFromText(doc.dump(), leaf);
        ASSERT_FALSE(r.has_value())
            << "a format file that omits cSymbolDecoration must fail at LOAD — "
               "a silent default would re-hide the rule in the engine's C++ "
               "table, which is the two-owner defect the key exists to remove";

        // ★ THE ANTI-SUBSUMPTION HALF. `countAtPath` pins that the rejection
        // NAMES this key, and `errorCount` pins that NOTHING ELSE fired — so
        // deleting the rule under test turns the load green and reds these
        // two lines, instead of the fixture quietly starting to fail for an
        // unrelated reason.
        //
        // ★ THE COUNT IS 2 AND IT IS MEASURED, NOT ASSUMED. One omission
        // raises TWO diagnostics at this pointer because BOTH tiers fire and
        // land in the SAME collector: the loader's `C_MissingField` and then
        // `ObjectFormatData::validate()`'s own arm, which `loadFromText` runs
        // unconditionally before returning. That is exactly why T3 below has
        // to exist as a separate, JSON-free test.
        EXPECT_EQ(countAtPath(r, "/cSymbolDecoration"), 2u) << rejectSummary(r);
        EXPECT_EQ(errorCount(r), 2u)
            << "the fixture must be rejected ONLY for its own defect: "
            << rejectSummary(r);

        bool sawMissingField = false;
        for (auto const& d : r.error()) {
            if (d.code == DiagnosticCode::C_MissingField
                && d.path == "/cSymbolDecoration") sawMissingField = true;
        }
        EXPECT_TRUE(sawMissingField)
            << "the rejection must NAME the missing key with C_MissingField, "
               "not merely fail: " << rejectSummary(r);
    }
}

TEST(CSymbolDecorationLoader, MissingKeyRejectedOnAMinimalSyntheticDocument) {
    // CONTROL: the identical document WITH the key loads clean.
    auto ok = ObjectFormatSchema::loadFromText(
        decorationFormatJson(R"("cSymbolDecoration": { "scheme": "none" },)"));
    ASSERT_TRUE(ok.has_value()) << rejectSummary(ok);
    EXPECT_EQ((*ok)->cSymbolDecoration().scheme, CSymbolDecorationScheme::None);

    auto r = ObjectFormatSchema::loadFromText(decorationFormatJson(""));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(countAtPath(r, "/cSymbolDecoration"), 2u) << rejectSummary(r);
    EXPECT_EQ(errorCount(r), 2u) << rejectSummary(r);
}

TEST(CSymbolDecorationLoader, SchemeIsAClosedVocabularyAndIsPARSEDNotAccepted) {
    // Both legal spellings parse AND are EXPOSED through the accessor — an
    // "accepted but never stored" key is the failure this half rules out.
    auto none = ObjectFormatSchema::loadFromText(
        decorationFormatJson(R"("cSymbolDecoration": { "scheme": "none" },)"));
    ASSERT_TRUE(none.has_value()) << rejectSummary(none);
    EXPECT_EQ((*none)->cSymbolDecoration().scheme,
              CSymbolDecorationScheme::None);

    auto lead = ObjectFormatSchema::loadFromText(decorationFormatJson(
        R"("cSymbolDecoration": { "scheme": "leading-underscore" },)"));
    ASSERT_TRUE(lead.has_value()) << rejectSummary(lead);
    EXPECT_EQ((*lead)->cSymbolDecoration().scheme,
              CSymbolDecorationScheme::LeadingUnderscore);

    // An unknown VALUE fails LOUD at the scheme's own pointer — never a silent
    // degrade to `none`, which on a macho format would bind every C call to an
    // undecorated name libSystem does not export.
    for (char const* bad :
         {R"("cSymbolDecoration": { "scheme": "leading_underscore" },)",
          R"("cSymbolDecoration": { "scheme": "unspecified" },)",
          R"("cSymbolDecoration": { "scheme": "" },)",
          R"("cSymbolDecoration": { "scheme": "_" },)"}) {
        SCOPED_TRACE(bad);
        auto r = ObjectFormatSchema::loadFromText(decorationFormatJson(bad));
        ASSERT_FALSE(r.has_value());
        // MEASURED count: the loader rejects the spelling and therefore stores
        // nothing, so validate() then sees the untouched `Unspecified`
        // sentinel and raises its own arm at the parent pointer. Both carry
        // "/cSymbolDecoration" as a prefix, hence 2.
        EXPECT_EQ(countAtPath(r, "/cSymbolDecoration/scheme"), 1u)
            << rejectSummary(r);
        EXPECT_EQ(errorCount(r), 2u) << rejectSummary(r);
    }

    // A block with no `scheme` at all, and a scalar where the block belongs:
    // both fail loud rather than defaulting.
    for (char const* bad : {R"("cSymbolDecoration": { },)",
                            R"("cSymbolDecoration": "none",)",
                            R"("cSymbolDecoration": "_",)",
                            R"("cSymbolDecoration": true,)"}) {
        SCOPED_TRACE(bad);
        auto r = ObjectFormatSchema::loadFromText(decorationFormatJson(bad));
        EXPECT_FALSE(r.has_value())
            << "a malformed cSymbolDecoration block must fail loud";
        EXPECT_GE(countAtPath(r, "/cSymbolDecoration"), 1u) << rejectSummary(r);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// T3 — `validate()` rejects a HAND-BUILT schema left at the sentinel.
// ─────────────────────────────────────────────────────────────────────────
//
// ★★ THIS IS THE ONLY TEST COVERING THE validate() ARM, AND T2 CANNOT BE. A
// JSON-fed "validate rejects it" test is SPECIES (a) BY CONSTRUCTION: the
// loader's `C_MissingField` fires first and `loadFromText` merges validate()'s
// problems into the SAME collector, so a JSON fixture can never show that the
// validate() arm did any work — delete that arm and the load still fails, for
// the loader's reason. Only the loader-BYPASSING path can tell them apart.
//
// That path is not hypothetical: `ObjectFormatSchema{ObjectFormatData}` is a
// PUBLIC constructor that runs NO validation, so every in-memory producer
// reaches the linker and the walkers without passing the JSON tier at all.
// This arm is the whole defense there.
//
// ⚠ RED LEVER, EXECUTED — AND IT FALSIFIED THE FIRST VERSION OF THIS COMMENT,
// which claimed "this test reds; T2 stays green". MEASURED 2026-08-06 by
// short-circuiting the `/cSymbolDecoration` arm in `ObjectFormatData::
// validate()`: FIVE tests red, both of these AND all three
// `CSymbolDecorationLoader` tests. The corrected claim, which is the one that
// actually matters:
//
//   * deleting this arm does NOT change the LOAD OUTCOME of any JSON fixture —
//     `loadFromText` still rejects, for the loader's own `C_MissingField`. So
//     the NATURAL shape of a JSON-fed "validate rejects it" test, a bare
//     `ASSERT_FALSE(r.has_value())`, STAYS GREEN. That is the species-(a) trap
//     and it is real;
//   * T2 above reds only because it pins the exact diagnostic COUNT (2 → 1).
//     It detects the arm's removal by ARITHMETIC, not by observing the arm do
//     anything the loader was not already doing — so it is evidence the arm
//     EXISTS, never evidence it DEFENDS anything;
//   * T3 is the only test that exercises the arm on the LOADER-BYPASSING path,
//     which is the path the linker and the walkers are actually handed. That
//     is the claim that survived the lever.
TEST(CSymbolDecorationValidate, HandBuiltSchemaLeftAtTheSentinelIsRejected) {
    // A minimal ELF ET_REL schema that is otherwise COMPLETE, so the only
    // possible complaint is the one under test (the `StaticLibraryFormats`
    // hand-built precedent).
    dss::detail::ObjectFormatData data;
    data.name               = "synth-csd";
    // TF-C125: a hand-built `ObjectFormatData` now names its format by
    // resolving the BACKEND, exactly as the loader does. `data.kind` is
    // gone — the field defaulted to `ObjectFormatKind::Elf`, so a
    // default-constructed struct silently claimed an ELF identity with
    // `elf.machine == 0`; the pointer's default is null and fails closed.
    data.backend            = dss::link::objectFormatBackendByConfigName("elf");
    data.dataModel          = DataModel::Lp64;
    data.headerNameMatching = HeaderNameMatching::CaseSensitive;
    data.elf.fileClass      = 2;   // ELFCLASS64
    data.elf.dataEncoding   = 1;   // ELFDATA2LSB
    data.elf.machine        = 62;  // EM_X86_64
    // `cSymbolDecoration` DELIBERATELY LEFT UNSET — the zero value is the
    // invalid sentinel.
    // D-FFI-ABI-CATALOG-SELECTS-CALLING-CONVENTION-BY-FORMAT-IDENTITY: its
    // sibling IS set, and must be, or this pin stops measuring the decoration
    // sentinel and starts measuring two missing fields at once — which is
    // exactly what the "ONLY complaint" assertion below exists to forbid.
    data.cCallingConvention.convention = "sysv_amd64";
    // D-PROGRAM-TIER-RETAINS-FORMAT-IDENTITY-BRANCHES: the artifact
    // NAMING fact, REQUIRED on every format and DISENGAGED is its invalid
    // sentinel (an engaged EMPTY value is a real answer -- a Unix
    // executable). On this hand-built loader-bypassing path validate() is
    // the only enforcement, so leaving it unset would make this fixture
    // report two problems where its assertions expect one.
    data.outputExtension = ".o";

    auto const problems = data.validate();
    std::size_t atKey = 0;
    for (auto const& d : problems) {
        if (d.path == "/cSymbolDecoration") ++atKey;
    }
    EXPECT_EQ(atKey, 1u)
        << "a hand-built ObjectFormatData that never declared its C-symbol "
           "decoration must be rejected BY validate() — the loader is not on "
           "this path";
    EXPECT_EQ(problems.size(), 1u)
        << "and it must be the ONLY complaint, or this pin is measuring some "
           "other missing field";

    // CONTROL, and the machine-checked half of "otherwise complete": declaring
    // EITHER real scheme makes the same schema validate with ZERO problems. So
    // the rejection above is attributable to the sentinel alone.
    for (auto const scheme : {CSymbolDecorationScheme::None,
                              CSymbolDecorationScheme::LeadingUnderscore}) {
        data.cSymbolDecoration.scheme = scheme;
        EXPECT_TRUE(data.validate().empty())
            << "declaring '" << cSymbolDecorationSchemeName(scheme)
            << "' must clear the only complaint";
    }
}

// ★ THE TEETH, ASSERTED RATHER THAN ONLY COMMENTED. The rule's guard is the
// constant `true`: it is not computed from `cSymbolDecoration`, nor from any
// predicate that reads it. Contrast the `processExit ⇒ isExecFlavor()` rule,
// whose ET_DYN arm the tree's own comment calls "a TAUTOLOGY [that] enforces
// nothing" because `elfDynPieShape` counts `processExit.has_value()` as a
// cluster member. A universal predicate cannot be tautological — and this test
// is what keeps it universal: it walks EVERY flavor axis (relocatable, image,
// exec, archive-container, and the two non-native kinds) and requires the same
// rejection from all of them. Gate the rule on any of those and this reds.
TEST(CSymbolDecorationValidate, RuleIsUNCONDITIONALAcrossEveryFlavorAxis) {
    struct Flavor {
        char const* label;
        // TF-C125: the format is named by its CONFIG SPELLING and resolved
        // through the registry, the same path `loadFromText` takes — so this
        // table can no longer name a format the engine does not implement.
        char const* configName;
        // applied after the common fields
        void (*shape)(dss::detail::ObjectFormatData&);
    };

    Flavor const flavors[] = {
        {"elf-rel", "elf",
         [](dss::detail::ObjectFormatData& d) {
             d.elf.fileClass = 2; d.elf.dataEncoding = 1; d.elf.machine = 62;
             d.elf.objectType = ElfObjectType::Rel;
         }},
        {"elf-rel-archive", "elf",
         [](dss::detail::ObjectFormatData& d) {
             d.elf.fileClass = 2; d.elf.dataEncoding = 1; d.elf.machine = 62;
             d.elf.objectType = ElfObjectType::Rel;
             d.container = ObjectFormatContainer::Archive;
         }},
        {"pe-obj", "pe",
         [](dss::detail::ObjectFormatData& d) {
             d.pe.machine = 0x8664; d.pe.objectType = PeObjectType::Obj;
         }},
        {"macho-object", "macho",
         [](dss::detail::ObjectFormatData& d) {
             d.macho.cputype = 0x0100000C;
             d.macho.filetype = MachOObjectType::Object;
         }},
        {"wasm", "wasm", [](dss::detail::ObjectFormatData&) {}},
        {"spirv", "spirv", [](dss::detail::ObjectFormatData&) {}},
    };

    for (auto const& f : flavors) {
        SCOPED_TRACE(f.label);
        dss::detail::ObjectFormatData data;
        data.name               = f.label;
        data.backend            =
            dss::link::objectFormatBackendByConfigName(f.configName);
        ASSERT_NE(data.backend, nullptr)
            << "no backend claims config spelling '" << f.configName
            << "' — the registry FAILS CLOSED, so an unresolved spelling here\n"
               "would silently produce a format with no identity rules at all";
        data.dataModel          = DataModel::Lp64;
        data.headerNameMatching = HeaderNameMatching::CaseSensitive;
        f.shape(data);

        std::size_t atKey = 0;
        for (auto const& d : data.validate()) {
            if (d.path == "/cSymbolDecoration") ++atKey;
        }
        EXPECT_EQ(atKey, 1u)
            << "the required-declaration rule must fire on EVERY flavor — the "
               "moment it is gated on one, it silently stops enforcing on "
               "whatever that gate excludes";

        // And declaring a scheme clears exactly that complaint on every flavor
        // (some of these shapes have OTHER legitimate complaints; this asserts
        // only that this key's is gone, which is the property under test).
        data.cSymbolDecoration.scheme = CSymbolDecorationScheme::None;
        std::size_t stillAtKey = 0;
        for (auto const& d : data.validate()) {
            if (d.path == "/cSymbolDecoration") ++stillAtKey;
        }
        EXPECT_EQ(stillAtKey, 0u);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// T4 — EVERY shipped format declares it, enumerated FROM THE DIRECTORY.
// ─────────────────────────────────────────────────────────────────────────
//
// ★ ENUMERATED, NEVER LISTED BY NAME. A sibling sweep in this tree pins only
// 12 of the 24 shipped formats by hand-written name — species (b): the pin
// drives an arm that cannot report what it never looks at, so a 25th format
// added without the key would sail past it. `fs::directory_iterator` cannot
// miss a file (the `test_shipped_type_consistency.cpp` precedent).
//
// ★ AND THE POPULATION SIZE IS ITSELF ASSERTED — species (d). A wrong root
// path yields an EMPTY loop, and an empty loop passes every per-file
// assertion inside it. `EXPECT_GE(seen, 24u)` is what makes that impossible.
//
// RED LEVER (executed): delete the `cSymbolDecoration` key from any one
// shipped `.format.json`. This test reds naming that file.
TEST(CSymbolDecorationShipped, EveryShippedFormatDeclaresIt) {
    auto const dir = objectFormatsDir();
    ASSERT_FALSE(dir.empty());
    ASSERT_TRUE(fs::exists(dir)) << dir.string();

    std::size_t seen = 0;
    std::map<CSymbolDecorationScheme, std::size_t> byScheme;

    for (auto const& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        if (e.path().filename().string().find(".format.json")
            == std::string::npos) continue;
        auto const leaf = e.path().filename().string();
        SCOPED_TRACE(leaf);
        ++seen;

        // Load through the REAL loader, not a JSON peek: that is what proves
        // the declaration is one the engine accepts, not merely one that is
        // textually present.
        auto r = ObjectFormatSchema::loadFromFile(e.path());
        ASSERT_TRUE(r.has_value())
            << leaf << " must load: " << rejectSummary(r);
        auto const scheme = (*r)->cSymbolDecoration().scheme;
        EXPECT_NE(scheme, CSymbolDecorationScheme::Unspecified)
            << leaf << " reached the engine with the invalid sentinel";
        ++byScheme[scheme];

        // The per-file ANSWER, keyed on the format's own declared kind — the
        // one place this test states what the right answer IS. Mach-O
        // decorates; nothing else shipped does.
        auto const expected = (*r)->kind() == ObjectFormatKind::MachO
                                  ? CSymbolDecorationScheme::LeadingUnderscore
                                  : CSymbolDecorationScheme::None;
        EXPECT_EQ(scheme, expected)
            << leaf << " declares '" << cSymbolDecorationSchemeName(scheme)
            << "' but its kind is '"
            << objectFormatKindName((*r)->kind()) << "'";
    }

    // Species (d) guard: a wrong root path would leave the loop empty and every
    // assertion above vacuously satisfied.
    EXPECT_GE(seen, 24u)
        << "expected at least the 24 shipped object formats under "
        << dir.string() << " — a smaller count means the enumeration missed "
           "the tree and every per-file assertion above ran zero times";

    // MEASURED 2026-08-06: 8 macho (2 arches x {bare, exec, dylib, staticlib})
    // decorate; the other 16 (10 elf + 4 pe + wasm + spirv) do not. Pinned so a
    // file added or dropped WITHOUT a decision shows up here rather than
    // passing silently.
    EXPECT_EQ(byScheme[CSymbolDecorationScheme::LeadingUnderscore], 8u);
    EXPECT_EQ(byScheme[CSymbolDecorationScheme::None], 16u);
}

// The shipped Mach-O family's decoration and its `processExit.importMangledName`
// are the TWO OWNERS the anchor exists to collapse. Until step C4 flips the
// engine onto the schema, they must at least AGREE — and this pins the
// agreement in the direction that matters: the macho exec formats import
// `_exit`, which is `exit` under `leading-underscore`, while their elf/pe
// siblings import the undecorated `exit`. If someone "normalizes" one owner
// without the other, this reds.
TEST(CSymbolDecorationShipped, ImportMangledNameAgreesWithTheDeclaredScheme) {
    auto const dir = objectFormatsDir();
    ASSERT_FALSE(dir.empty());

    std::size_t checked = 0;
    for (auto const& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        auto const leaf = e.path().filename().string();
        if (leaf.find(".format.json") == std::string::npos) continue;

        auto r = ObjectFormatSchema::loadFromFile(e.path());
        ASSERT_TRUE(r.has_value()) << leaf << ": " << rejectSummary(r);
        auto const& pe = (*r)->processExit();
        if (!pe.has_value() || pe->importMangledName.empty()) continue;

        SCOPED_TRACE(leaf);
        ++checked;
        bool const decorated =
            (*r)->cSymbolDecoration().scheme
                == CSymbolDecorationScheme::LeadingUnderscore;
        EXPECT_EQ(pe->importMangledName.front() == '_', decorated)
            << "'" << pe->importMangledName << "' does not match the declared "
               "scheme '"
            << cSymbolDecorationSchemeName((*r)->cSymbolDecoration().scheme)
            << "' — the two owners of one fact have drifted, which is exactly "
               "what D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN exists to end";
    }

    // Species (d) again: `continue` above skips formats without a by-name
    // import, and a bug that skipped ALL of them would leave this vacuous.
    // MEASURED 2026-08-06 (grep of `importMangledName` across the 24 shipped
    // files, confirmed by this assertion): exactly SEVEN declare one —
    // elf64-{aarch64,x86_64}-linux-{exec,pie} (4, importing `exit`),
    // macho64-{arm64,x86_64}-darwin-exec (2, importing `_exit`) and
    // pe64-x86_64-windows-exec (1, importing `exit`). Two of the seven are the
    // decorated arm, so the loop above genuinely exercises both answers.
    EXPECT_EQ(checked, 7u)
        << "the shipped by-name-import population changed — re-derive it rather "
           "than relaxing this number, or the loop above starts asserting less "
           "than it claims";
}
