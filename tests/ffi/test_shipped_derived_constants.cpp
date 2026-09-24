// The LATTICE-DERIVED shipped constant — a `constants` row that names a TYPE and a
// LIMIT instead of a number (P68 round 9, D-C-LIMITS-H-DEFINES-NINE-OF-THE-STANDARD-MACROS).
//
//     { "name": "LONG_MAX", "of": "long", "limit": "max" }
//
// C 5.2.5.3.2 gives each `<limits.h>` macro its type's PROMOTED type and VALUE, and
// both are per (target, format) pair: `long` is 32 bits on pe and 64 on ELF/Mach-O,
// plain `char` is unsigned on aarch64 Linux alone. So the row is realized per pair,
// by the ONE constants decode, from `ffi::ShippedPairFacts` — the consuming
// language (its type-name resolver and its integer promotions), the data model and
// plain `char`'s signedness. These pins drive the reader directly, through BOTH
// seams (the interner-free preprocessor read and the semantic read), on
// hand-written fixtures and on the shipped `limits.json`:
//   (1) the value AND the type follow the pair — `long` by the data model, `char`
//       by the signedness, `max`/`min` typed as the promotion (`USHRT_MAX` is an
//       `int`), `width` typed as the promotion floor;
//   (2) `of` names this descriptor's own typedef first (`int64_t`), then the
//       language vocabulary;
//   (3) with NO pair, a row the missing fact decides is NOT realized — never
//       borrowed from a default — while one it cannot change (`int`) still is;
//   (4) every malformed shape is REFUSED, loudly, on every read;
//   (5) the shipped `limits.json` realizes all 32 rows under every data model and
//       both signednesses, with no diagnostic (the typo guard for its `of` names).
// The per-pair C-level pins (every macro against gcc/clang/mingw/MSVC through the
// driver's two halves) are `analysis/preprocess/test_limits_h_lattice`.

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test::configRootDiagnostic;
using dss::test::findConfigRoot;
using dss::test_support::Location;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema =
        []() -> std::shared_ptr<GrammarSchema const> {
        auto loaded = GrammarSchema::loadShipped("c");
        if (!loaded.has_value()) {
            ADD_FAILURE() << "the shipped c language does not load";
            return std::shared_ptr<GrammarSchema const>{};
        }
        return *loaded;
    }();
    return schema;
}

[[nodiscard]] fs::path writeTemp(ScratchDir const& dir, std::string const& name,
                                 std::string const& content) {
    fs::path const p = dir.path() / name;
    std::ofstream(p, std::ios::binary) << content;
    return p;
}

[[nodiscard]] bool anyDiagMentions(DiagnosticReporter const& rep,
                                   std::string_view needle) {
    for (auto const& d : rep.all()) {
        if (d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
}

// The preprocessor seam's view of one constant, or nullptr when not realized.
[[nodiscard]] ShippedPpConstant const*
ppRow(std::vector<ShippedPpConstant> const& rows, std::string_view name) {
    for (auto const& r : rows) {
        if (r.name == name) return &r;
    }
    return nullptr;
}

// The semantic seam's view of one constant, or nullptr when not realized.
[[nodiscard]] ShippedConstant const*
semRow(ShippedLibDescriptor const& d, std::string_view name) {
    for (auto const& c : d.constants) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

constexpr char const* kLongFixture = R"JSON({
    "header": "lim.h",
    "constants": [
      { "name": "LONG_MAX",   "of": "long",          "limit": "max"   },
      { "name": "LONG_MIN",   "of": "long",          "limit": "min"   },
      { "name": "ULONG_MAX",  "of": "unsigned long", "limit": "max"   },
      { "name": "LONG_WIDTH", "of": "long",          "limit": "width" }
    ]
})JSON";

}  // namespace

// ── (1) `long` FOLLOWS THE DATA MODEL, AT BOTH SEAMS ─────────────────────────
TEST(ShippedDerivedConstants, LongFollowsTheDataModelAtBothSeams) {
    ASSERT_NE(cLanguage(), nullptr);
    ScratchDir dir{Location::Temp, "derived-constants"};
    auto const path = writeTemp(dir, "lim.json", kLongFixture);
    struct Want {
        DataModel     model;
        std::int64_t  longMax;
        std::int64_t  longMin;
        std::uint64_t ulongMax;
        std::int64_t  width;
        TypeKind      longCore;
        TypeKind      ulongCore;
    };
    for (Want const w : {Want{DataModel::Lp64, INT64_MAX, INT64_MIN,
                              0xFFFFFFFFFFFFFFFFull, 64, TypeKind::I64, TypeKind::U64},
                         Want{DataModel::Llp64, 2147483647, -2147483647 - 1,
                              0xFFFFFFFFull, 32, TypeKind::I32, TypeKind::U32}}) {
        SCOPED_TRACE(std::string{dataModelName(w.model)});
        ShippedPairFacts const pair{cLanguage().get(), w.model, false};
        // The PREPROCESSOR seam.
        DiagnosticReporter rep;
        auto const pp = readShippedLibConstants(path, rep, std::nullopt, std::nullopt, &pair);
        ASSERT_TRUE(pp.has_value());
        EXPECT_FALSE(rep.hasErrors());
        auto const* lmax = ppRow(*pp, "LONG_MAX");
        auto const* lmin = ppRow(*pp, "LONG_MIN");
        auto const* umax = ppRow(*pp, "ULONG_MAX");
        auto const* lw   = ppRow(*pp, "LONG_WIDTH");
        ASSERT_TRUE(lmax && lmin && umax && lw);
        EXPECT_EQ(lmax->value, w.longMax);
        EXPECT_EQ(lmax->core, w.longCore);
        EXPECT_EQ(lmax->vocabularyName, "long") << "LONG_MAX is a `long`, not the anonymous core";
        EXPECT_FALSE(lmax->isUnsigned);
        EXPECT_EQ(lmin->value, w.longMin);
        EXPECT_EQ(static_cast<std::uint64_t>(umax->value), w.ulongMax);
        EXPECT_EQ(umax->core, w.ulongCore);
        EXPECT_EQ(umax->vocabularyName, "unsigned long");
        EXPECT_TRUE(umax->isUnsigned);
        EXPECT_EQ(lw->value, w.width);
        EXPECT_EQ(lw->core, TypeKind::I32) << "a width is typed as the promotion floor, int";
        EXPECT_EQ(lw->vocabularyName, "");
        // The SEMANTIC seam — the same value and the same identity.
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter srep;
        auto const desc = readShippedLibDescriptor(path, interner, typeReg, srep, w.model,
                                                   std::nullopt, std::nullopt, {}, nullptr,
                                                   &pair);
        ASSERT_TRUE(desc.has_value());
        auto const* s = semRow(*desc, "LONG_MAX");
        ASSERT_NE(s, nullptr);
        EXPECT_EQ(s->value, w.longMax);
        EXPECT_EQ(interner.kind(s->type), w.longCore);
        EXPECT_EQ(interner.vocabularyName(s->type), "long");
        EXPECT_TRUE(s->preprocessorVisible);
    }
}

// ── (1) plain `char` FOLLOWS THE PAIR'S SIGNEDNESS; the promotions type the rows ─
TEST(ShippedDerivedConstants, CharFollowsTheSignednessAndPromotionTypesTheRows) {
    ASSERT_NE(cLanguage(), nullptr);
    ScratchDir dir{Location::Temp, "derived-constants"};
    auto const path = writeTemp(dir, "chr.json", R"JSON({
        "header": "chr.h",
        "constants": [
          { "name": "CHAR_MIN",   "of": "char",           "limit": "min"   },
          { "name": "CHAR_MAX",   "of": "char",           "limit": "max"   },
          { "name": "CHAR_BIT",   "of": "char",           "limit": "width" },
          { "name": "UCHAR_MAX",  "of": "unsigned char",  "limit": "max"   },
          { "name": "USHRT_MAX",  "of": "unsigned short", "limit": "max"   },
          { "name": "UINT_MAX",   "of": "unsigned int",   "limit": "max"   },
          { "name": "BOOL_MAX",   "of": "bool",           "limit": "max"   },
          { "name": "BOOL_WIDTH", "of": "bool",           "limit": "width" }
        ]
    })JSON");
    for (bool const unsignedChar : {false, true}) {
        SCOPED_TRACE(unsignedChar ? "unsigned char" : "signed char");
        ShippedPairFacts const pair{cLanguage().get(), DataModel::Lp64, unsignedChar};
        DiagnosticReporter rep;
        auto const pp = readShippedLibConstants(path, rep, std::nullopt, std::nullopt, &pair);
        ASSERT_TRUE(pp.has_value());
        EXPECT_FALSE(rep.hasErrors());
        auto const* cmin = ppRow(*pp, "CHAR_MIN");
        auto const* cmax = ppRow(*pp, "CHAR_MAX");
        ASSERT_TRUE(cmin && cmax);
        EXPECT_EQ(cmin->value, unsignedChar ? 0 : -128);
        EXPECT_EQ(cmax->value, unsignedChar ? 255 : 127);
        // C 5.2.5.3.2: the PROMOTED type — char promotes to int.
        EXPECT_EQ(cmin->core, TypeKind::I32);
        EXPECT_EQ(cmin->vocabularyName, "");
        EXPECT_EQ(ppRow(*pp, "CHAR_BIT")->value, 8);
        auto const* ucmax = ppRow(*pp, "UCHAR_MAX");
        auto const* usmax = ppRow(*pp, "USHRT_MAX");
        auto const* uimax = ppRow(*pp, "UINT_MAX");
        auto const* bmax  = ppRow(*pp, "BOOL_MAX");
        auto const* bw    = ppRow(*pp, "BOOL_WIDTH");
        ASSERT_TRUE(ucmax && usmax && uimax && bmax && bw);
        EXPECT_EQ(ucmax->value, 255);
        EXPECT_EQ(ucmax->core, TypeKind::I32) << "unsigned char promotes to int";
        EXPECT_EQ(usmax->value, 65535);
        EXPECT_EQ(usmax->core, TypeKind::I32) << "unsigned short promotes to int";
        EXPECT_FALSE(usmax->isUnsigned) << "so `#if -1 < USHRT_MAX` is TRUE, as in gcc";
        EXPECT_EQ(static_cast<std::uint64_t>(uimax->value), 4294967295ull);
        EXPECT_EQ(uimax->core, TypeKind::U32) << "unsigned int does not promote";
        EXPECT_TRUE(uimax->isUnsigned);
        EXPECT_EQ(bmax->value, 1);
        EXPECT_EQ(bmax->core, TypeKind::I32);
        EXPECT_EQ(bw->value, 1) << "C23 5.2.5.3.2 fn 15: BOOL_WIDTH is exactly 1";
    }
}

// ── (2) `of` NAMES THIS DESCRIPTOR'S OWN TYPEDEF FIRST ───────────────────────
// `INT64_MAX` is `of: "int64_t"`: the typedef's per-pair identity (`long` on elf,
// `long long` on macho and pe) is the macro's, so the two can never disagree.
TEST(ShippedDerivedConstants, OfNamesThisDescriptorsTypedefFirst) {
    ASSERT_NE(cLanguage(), nullptr);
    ScratchDir dir{Location::Temp, "derived-constants"};
    auto const path = writeTemp(dir, "si.json", R"JSON({
        "header": "si.h",
        "typedefs": [
          { "name": "my64_t", "variants": [
            { "when": { "dataModel": "LP64",  "format": "elf"   }, "type": "i64 \"long\"" },
            { "when": { "dataModel": "LP64",  "format": "macho" }, "type": "i64 \"long long\"" },
            { "when": { "dataModel": "LLP64", "format": "pe"    }, "type": "i64 \"long long\"" } ] }
        ],
        "constants": [
          { "name": "MY64_MAX", "of": "my64_t", "limit": "max" },
          { "name": "MY64_MIN", "of": "my64_t", "limit": "min" }
        ]
    })JSON");
    struct Case { DataModel model; ObjectFormatKind format; std::string_view vocab; };
    for (Case const c : {Case{DataModel::Lp64, ObjectFormatKind::Elf, "long"},
                         Case{DataModel::Lp64, ObjectFormatKind::MachO, "long long"},
                         Case{DataModel::Llp64, ObjectFormatKind::Pe, "long long"}}) {
        SCOPED_TRACE(std::string{objectFormatKindName(c.format)});
        ShippedPairFacts const pair{cLanguage().get(), c.model, false};
        DiagnosticReporter rep;
        auto const pp = readShippedLibConstants(path, rep, std::nullopt, c.format, &pair);
        ASSERT_TRUE(pp.has_value());
        EXPECT_FALSE(rep.hasErrors());
        auto const* mx = ppRow(*pp, "MY64_MAX");
        auto const* mn = ppRow(*pp, "MY64_MIN");
        ASSERT_TRUE(mx && mn);
        EXPECT_EQ(mx->value, INT64_MAX);
        EXPECT_EQ(mn->value, INT64_MIN);
        EXPECT_EQ(mx->core, TypeKind::I64);
        EXPECT_EQ(mx->vocabularyName, c.vocab)
            << "the macro's identity is the typedef's on THIS pair";
    }
    // A pair the typedef selects no variant for: the row is NOT realized there
    // (it names a typedef the descriptor declares — not an unknown name).
    ShippedPairFacts const noModel{cLanguage().get(), std::nullopt, false};
    DiagnosticReporter rep;
    auto const pp = readShippedLibConstants(path, rep, std::nullopt, ObjectFormatKind::Elf,
                                            &noModel);
    ASSERT_TRUE(pp.has_value());
    EXPECT_FALSE(rep.hasErrors()) << "a typedef not selected here is not an error";
    EXPECT_EQ(ppRow(*pp, "MY64_MAX"), nullptr)
        << "no data model selects no `{dataModel, format}` typedef variant, so the "
           "row it derives from is not realized — never LP64's answer borrowed";
}

// ── (3) NO PAIR: REALIZED ONLY WHERE THE MISSING FACT CANNOT CHANGE IT ───────
TEST(ShippedDerivedConstants, WithoutAPairOnlyThePairIndependentRowsAreRealized) {
    ASSERT_NE(cLanguage(), nullptr);
    ScratchDir dir{Location::Temp, "derived-constants"};
    auto const path = writeTemp(dir, "np.json", R"JSON({
        "header": "np.h",
        "constants": [
          { "name": "INT_MAX",   "of": "int",   "limit": "max" },
          { "name": "LLONG_MAX", "of": "long long", "limit": "max" },
          { "name": "LONG_MAX",  "of": "long",  "limit": "max" },
          { "name": "CHAR_MIN",  "of": "char",  "limit": "min" }
        ]
    })JSON");
    {
        ShippedPairFacts const noPair{cLanguage().get(), std::nullopt, std::nullopt};
        DiagnosticReporter rep;
        auto const pp = readShippedLibConstants(path, rep, std::nullopt, std::nullopt, &noPair);
        ASSERT_TRUE(pp.has_value());
        EXPECT_FALSE(rep.hasErrors());
        EXPECT_NE(ppRow(*pp, "INT_MAX"), nullptr) << "int is 32 bits under every data model";
        EXPECT_NE(ppRow(*pp, "LLONG_MAX"), nullptr) << "so is long long's 64";
        EXPECT_EQ(ppRow(*pp, "LONG_MAX"), nullptr) << "long's width is the pair's to say";
        EXPECT_EQ(ppRow(*pp, "CHAR_MIN"), nullptr) << "plain char's sign is the pair's to say";
    }
    // No language at all: nothing derived is realized, and nothing is refused.
    {
        DiagnosticReporter rep;
        auto const pp = readShippedLibConstants(path, rep);
        ASSERT_TRUE(pp.has_value());
        EXPECT_FALSE(rep.hasErrors());
        EXPECT_TRUE(pp->empty());
    }
}

// ── (4) EVERY MALFORMED SHAPE IS REFUSED, LOUDLY, WITH OR WITHOUT A PAIR ─────
TEST(ShippedDerivedConstants, MalformedDerivedRowsAreRefused) {
    ASSERT_NE(cLanguage(), nullptr);
    ScratchDir dir{Location::Temp, "derived-constants"};
    struct Bad {
        char const*      label;
        char const*      row;
        std::string_view needle;
        bool             needsLanguage;   // refused only once a vocabulary can judge the name
    };
    Bad const cases[] = {
        {"unknown limit",       R"({ "name": "X", "of": "int", "limit": "maximum" })", "unknown 'limit'", false},
        {"non-string limit",    R"({ "name": "X", "of": "int", "limit": 3 })",         "unknown 'limit'", false},
        {"limit without of",    R"({ "name": "X", "limit": "max" })",                  "no 'of'",         false},
        {"of without limit",    R"({ "name": "X", "of": "int" })",                     "no 'limit'",      false},
        {"empty of",            R"({ "name": "X", "of": "", "limit": "max" })",        "non-empty type",  false},
        {"value beside of",     R"({ "name": "X", "of": "int", "limit": "max", "value": 1 })",
                                "Remove 'value'", false},
        {"type beside of",      R"({ "name": "X", "of": "int", "limit": "max", "type": "i32" })",
                                "Remove 'type'", false},
        {"variants beside of",  R"({ "name": "X", "of": "int", "limit": "max", "variants": [] })",
                                "Remove 'variants'", false},
        {"unknown of",          R"({ "name": "X", "of": "lonk", "limit": "max" })",
                                "neither a typedef", true},
        {"non-integer of",      R"({ "name": "X", "of": "double", "limit": "max" })",
                                "not an integer type", true},
        {"uncarriable range",   R"({ "name": "X", "of": "unsigned __int128", "limit": "max" })",
                                "64-bit constant carrier", true},
    };
    for (Bad const& b : cases) {
        SCOPED_TRACE(b.label);
        auto const path = writeTemp(dir, "bad.json",
                                    std::string{R"({ "header": "bad.h", "constants": [ )"}
                                        + b.row + " ] }");
        ShippedPairFacts const pair{cLanguage().get(), DataModel::Lp64, false};
        // Both seams refuse, the semantic one with the pair...
        {
            TypeInterner interner{CompilationUnitId{1}};
            TypeRegistry typeReg;
            DiagnosticReporter rep;
            auto const desc = readShippedLibDescriptor(path, interner, typeReg, rep,
                                                       DataModel::Lp64, std::nullopt,
                                                       std::nullopt, {}, nullptr, &pair);
            EXPECT_FALSE(desc.has_value());
            EXPECT_TRUE(anyDiagMentions(rep, b.needle))
                << "no diagnostic names '" << b.needle << "'";
        }
        // ...and the preprocessor one.
        {
            DiagnosticReporter rep;
            EXPECT_FALSE(readShippedLibConstants(path, rep, std::nullopt, std::nullopt, &pair)
                             .has_value());
            EXPECT_TRUE(anyDiagMentions(rep, b.needle));
        }
        // A SHAPE defect is refused even with no pair at all — it cannot lurk
        // until some target realizes the row. A NAME defect needs a vocabulary
        // to judge it, and with none the row is simply not realized.
        {
            DiagnosticReporter rep;
            auto const r = readShippedLibConstants(path, rep);
            EXPECT_EQ(r.has_value(), b.needsLanguage);
            EXPECT_EQ(rep.hasErrors(), !b.needsLanguage);
        }
    }
}

// ── (5) THE SHIPPED limits.json REALIZES ALL 32 ROWS ON EVERY PAIR SHAPE ─────
// The typo guard for its `of` names: every one must resolve through the C
// language under every data model and both plain-`char` signednesses, with no
// diagnostic, at both seams.
TEST(ShippedDerivedConstants, TheShippedLimitsHeaderRealizesEveryRowOnEveryPairShape) {
    ASSERT_NE(cLanguage(), nullptr);
    auto const root = findConfigRoot();
    ASSERT_TRUE(root.has_value()) << configRootDiagnostic();
    auto const limits = *root / "shippedLibs" / "limits.json";
    constexpr std::size_t kDerivedRows = 31;   // the 32 names minus MB_LEN_MAX (declared per format)
    for (auto const& [model, modelName] : kDataModelTable.rows) {
        for (bool const unsignedChar : {false, true}) {
            SCOPED_TRACE(std::string{modelName} + (unsignedChar ? " unsigned" : " signed"));
            ShippedPairFacts const pair{cLanguage().get(), model, unsignedChar};
            DiagnosticReporter rep;
            auto const pp = readShippedLibConstants(limits, rep, std::nullopt, std::nullopt,
                                                    &pair);
            ASSERT_TRUE(pp.has_value()) << (rep.all().empty() ? "" : rep.all().front().actual);
            EXPECT_FALSE(rep.hasErrors());
            EXPECT_EQ(pp->size(), kDerivedRows)
                << "with no format, every derived row and no per-format one";
            TypeInterner interner{CompilationUnitId{1}};
            TypeRegistry typeReg;
            DiagnosticReporter srep;
            auto const desc = readShippedLibDescriptor(limits, interner, typeReg, srep, model,
                                                       std::nullopt, std::nullopt, {}, nullptr,
                                                       &pair);
            ASSERT_TRUE(desc.has_value())
                << (srep.all().empty() ? "" : srep.all().front().actual);
            EXPECT_EQ(desc->constants.size(), kDerivedRows);
        }
    }
}
