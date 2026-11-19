// The printf/scanf FORMAT predefines — `__<T>_FMT<c>__` (`type-format`) — and the
// language table they are built from, `preprocess.typeFormatModifiers`.
// P69, lane `lm`, M4 of D-FFI-INTTYPES-H-SHIPS-FOUR-FORMAT-MACROS.
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// ✔MEASURED 2026-09-30 (`-dM -E -x c`): Apple clang 21.0.0 predefines 120 of these
// names on both arches, clang 18.1.3 on Linux the 90 without the C23 `b`/`B`
// conversions, gcc 13.3.0, mingw-w64 13.2.0 and MSVC 19.51 none. DSS defined none.
// They are the natural single owner of <inttypes.h>'s PRI/SCN macros, which a
// program uses to print a typedef without knowing what it is — so a wrong one is a
// silent printf ABI mismatch (a `long long` read where a `long` was passed).
//
// ★ ONE FACT PER TYPE, the P68 round 9 rule: a row names the SHIPPED TYPEDEF and a
// conversion letter, and the value is the language's length modifier for whatever
// type that typedef is on the pair, then the letter. `__INT64_FMTd__` is `"ld"` on
// glibc and `"lld"` on the UCRT and Apple because `int64_t` is `long` there and `long
// long` here — nothing states a width.
//
// ⚖ THE MEANING FORK, decided as `__INT_FAST16_TYPE__` was: clang 18 on Linux spells
// `__INT_FAST16_FMTd__` "hd" while glibc's own `int_fast16_t` is `long` — the
// shipped typedef wins ("ld"; the modifier a `%` conversion of that typedef needs).
//
// Pins: (1) the family is exactly the references' union, as LANGUAGE rows naming
// shipped typedefs; (2) on every executable pair every value is the modifier of the
// REFERENCE's own type for that typedef (the table below is written out from the
// measurements, never read from DSS's documents) plus the letter; (3) the loader
// refuses every malformed row and table entry; (4) the merge refuses, loud, a type
// the pair realizes and the table cannot format.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/preprocess_config.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// ── THE FAMILY: every name, the typedef it formats, and its header ────────────
struct Family {
    std::string_view stem;      // `__<stem>_FMT<c>__`
    std::string_view typedefName;
    std::string_view header;
    bool             isSigned;
};

constexpr Family kFamilies[] = {
    {"INT8", "int8_t", "stdint.h", true},           {"INT16", "int16_t", "stdint.h", true},
    {"INT32", "int32_t", "stdint.h", true},         {"INT64", "int64_t", "stdint.h", true},
    {"INT_LEAST8", "int_least8_t", "stdint.h", true},
    {"INT_LEAST16", "int_least16_t", "stdint.h", true},
    {"INT_LEAST32", "int_least32_t", "stdint.h", true},
    {"INT_LEAST64", "int_least64_t", "stdint.h", true},
    {"INT_FAST8", "int_fast8_t", "stdint.h", true}, {"INT_FAST16", "int_fast16_t", "stdint.h", true},
    {"INT_FAST32", "int_fast32_t", "stdint.h", true},
    {"INT_FAST64", "int_fast64_t", "stdint.h", true},
    {"INTMAX", "intmax_t", "stdint.h", true},       {"INTPTR", "intptr_t", "stdint.h", true},
    {"PTRDIFF", "ptrdiff_t", "stddef.h", true},
    {"UINT8", "uint8_t", "stdint.h", false},        {"UINT16", "uint16_t", "stdint.h", false},
    {"UINT32", "uint32_t", "stdint.h", false},      {"UINT64", "uint64_t", "stdint.h", false},
    {"UINT_LEAST8", "uint_least8_t", "stdint.h", false},
    {"UINT_LEAST16", "uint_least16_t", "stdint.h", false},
    {"UINT_LEAST32", "uint_least32_t", "stdint.h", false},
    {"UINT_LEAST64", "uint_least64_t", "stdint.h", false},
    {"UINT_FAST8", "uint_fast8_t", "stdint.h", false},
    {"UINT_FAST16", "uint_fast16_t", "stdint.h", false},
    {"UINT_FAST32", "uint_fast32_t", "stdint.h", false},
    {"UINT_FAST64", "uint_fast64_t", "stdint.h", false},
    {"UINTMAX", "uintmax_t", "stdint.h", false},    {"UINTPTR", "uintptr_t", "stdint.h", false},
    {"SIZE", "size_t", "stddef.h", false},
};
constexpr std::string_view kSignedConversions[]   = {"d", "i"};
constexpr std::string_view kUnsignedConversions[] = {"o", "u", "x", "X", "b", "B"};

struct Member {
    std::string name;
    Family      family;
    std::string conversion;
};

[[nodiscard]] std::vector<Member> const& members() {
    static std::vector<Member> const all = [] {
        std::vector<Member> out;
        for (Family const& f : kFamilies) {
            auto const& convs = f.isSigned ? std::span<std::string_view const>{kSignedConversions}
                                           : std::span<std::string_view const>{kUnsignedConversions};
            for (std::string_view const c : convs) {
                out.push_back(Member{std::format("__{}_FMT{}__", f.stem, c), f, std::string{c}});
            }
        }
        return out;
    }();
    return all;
}

// ── THE REFERENCES' TYPES, PER PLATFORM ────────────────────────────────────────
// Written out from the measurements (P68 round 9's `_Generic` + `sizeof` runs, the
// same table test_type_derived_predefines pins the `__*_TYPE__` family against),
// never read from DSS's own documents. The pe pair presents mingw-w64's identity.
std::map<std::string_view, std::string_view> const kCommon{
    {"int8_t", "signed char"},    {"int16_t", "short"},          {"int32_t", "int"},
    {"uint8_t", "unsigned char"}, {"uint16_t", "unsigned short"}, {"uint32_t", "unsigned int"},
    {"int_least8_t", "signed char"},    {"int_least16_t", "short"},
    {"int_least32_t", "int"},           {"uint_least8_t", "unsigned char"},
    {"uint_least16_t", "unsigned short"}, {"uint_least32_t", "unsigned int"},
    {"int_fast8_t", "signed char"},     {"uint_fast8_t", "unsigned char"},
};
std::map<std::string_view, std::string_view> const kGlibc{
    {"int64_t", "long"},  {"uint64_t", "unsigned long"},
    {"int_least64_t", "long"}, {"uint_least64_t", "unsigned long"},
    {"int_fast16_t", "long"},  {"int_fast32_t", "long"},  {"int_fast64_t", "long"},
    {"uint_fast16_t", "unsigned long"}, {"uint_fast32_t", "unsigned long"},
    {"uint_fast64_t", "unsigned long"},
    {"intptr_t", "long"}, {"uintptr_t", "unsigned long"},
    {"intmax_t", "long"}, {"uintmax_t", "unsigned long"},
    {"ptrdiff_t", "long"}, {"size_t", "unsigned long"},
};
std::map<std::string_view, std::string_view> const kMingw{
    {"int64_t", "long long"}, {"uint64_t", "unsigned long long"},
    {"int_least64_t", "long long"}, {"uint_least64_t", "unsigned long long"},
    {"int_fast16_t", "short"}, {"int_fast32_t", "int"},
    {"int_fast64_t", "long long"}, {"uint_fast16_t", "unsigned short"},
    {"uint_fast32_t", "unsigned int"}, {"uint_fast64_t", "unsigned long long"},
    {"intptr_t", "long long"}, {"uintptr_t", "unsigned long long"},
    {"intmax_t", "long long"}, {"uintmax_t", "unsigned long long"},
    {"ptrdiff_t", "long long"}, {"size_t", "unsigned long long"},
};
std::map<std::string_view, std::string_view> const kDarwin{
    {"int64_t", "long long"}, {"uint64_t", "unsigned long long"},
    {"int_least64_t", "long long"}, {"uint_least64_t", "unsigned long long"},
    {"int_fast16_t", "short"}, {"int_fast32_t", "int"},
    {"int_fast64_t", "long long"}, {"uint_fast16_t", "unsigned short"},
    {"uint_fast32_t", "unsigned int"}, {"uint_fast64_t", "unsigned long long"},
    {"intptr_t", "long"}, {"uintptr_t", "unsigned long"},
    {"intmax_t", "long"}, {"uintmax_t", "unsigned long"},
    {"ptrdiff_t", "long"}, {"size_t", "unsigned long"},
};

// C 7.23.6.1's length modifier for a C integer type — the test's own statement of
// the standard, independent of the language document's table.
[[nodiscard]] std::string_view modifierOf(std::string_view cType) {
    if (cType == "signed char" || cType == "unsigned char") return "hh";
    if (cType == "short" || cType == "unsigned short") return "h";
    if (cType == "int" || cType == "unsigned int") return "";
    if (cType == "long" || cType == "unsigned long") return "l";
    if (cType == "long long" || cType == "unsigned long long") return "ll";
    ADD_FAILURE() << "no C length modifier for '" << cType << "'";
    return "?";
}

struct Pair {
    std::string_view target;
    std::string_view formatDoc;
    std::map<std::string_view, std::string_view> const* types;
};

[[nodiscard]] std::vector<Pair> const& pairs() {
    static std::vector<Pair> const all{
        {"x86_64", "elf64-x86_64-linux-exec", &kGlibc},
        {"arm64", "elf64-aarch64-linux-exec", &kGlibc},
        {"x86_64", "pe64-x86_64-windows-exec", &kMingw},
        {"arm64", "macho64-arm64-darwin-exec", &kDarwin},
        {"x86_64", "macho64-x86_64-darwin-exec", &kDarwin},
    };
    return all;
}

[[nodiscard]] std::string_view referenceType(Pair const& p, std::string_view typedefName) {
    if (auto const it = p.types->find(typedefName); it != p.types->end()) return it->second;
    if (auto const it = kCommon.find(typedefName); it != kCommon.end()) return it->second;
    ADD_FAILURE() << "no reference type for " << typedefName;
    return {};
}

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        return loaded.has_value() ? *loaded : std::shared_ptr<GrammarSchema const>{};
    }();
    return schema;
}

// The format rows the merge REALIZES on `facts` for `language`, by name.
[[nodiscard]] std::map<std::string, std::string>
realized(PredefinedTypeFacts const* facts, GrammarSchema const* language, ObjectFormatKind format,
         std::vector<std::string>* conflicts = nullptr) {
    auto const& rows = (language != nullptr ? *language : *cLanguage()).preprocess().predefinedMacros;
    auto const m = mergePredefinedMacros(rows, {}, {}, format, {}, facts, language);
    if (conflicts != nullptr) *conflicts = m.conflicts;
    else EXPECT_TRUE(m.conflicts.empty()) << (m.conflicts.empty() ? "" : m.conflicts.front());
    std::map<std::string, std::string> out;
    for (auto const& pm : m.effective) {
        if (pm.kind == PredefinedMacroKind::TypeFormat) out[pm.name] = pm.value;
    }
    return out;
}

[[nodiscard]] std::string shippedCText() {
    std::ifstream in(dss::test::configRoot() / "sources" / "c.lang.json", std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The shipped document with `from` (found after `anchor`) replaced by `to`.
[[nodiscard]] std::string shippedCEdited(std::string_view anchor, std::string_view from,
                                         std::string_view to) {
    std::string text = shippedCText();
    auto const at = text.find(anchor);
    if (at == std::string::npos) {
        ADD_FAILURE() << "the shipped c config no longer carries " << anchor;
        return {};
    }
    auto const pos = text.find(from, at);
    if (pos == std::string::npos) {
        ADD_FAILURE() << "no '" << from << "' after " << anchor;
        return {};
    }
    text.replace(pos, from.size(), to);
    return text;
}

void refusedNaming(std::string const& text, std::string_view needle) {
    if (text.empty()) return;
    auto r = GrammarSchema::loadFromText(text, "<edited-c>");
    if (r.has_value()) {
        ADD_FAILURE() << "the edited document LOADED — it had to be refused";
        return;
    }
    bool named = false;
    for (auto const& d : r.error()) {
        named |= d.message.find(needle) != std::string::npos
                 || d.path.find(needle) != std::string::npos;
    }
    EXPECT_TRUE(named) << "no diagnostic names '" << needle << "'; first: "
                       << (r.error().empty() ? "<none>" : r.error()[0].message);
}

constexpr std::string_view kInt64d  = "\"__INT64_FMTd__\"";
constexpr std::string_view kModsKey = "\"typeFormatModifiers\"";

}  // namespace

// ── (1) THE FAMILY IS THE REFERENCES' UNION, AS LANGUAGE ROWS NAMING TYPEDEFS ──
TEST(TypeFormatPredefines, TheFamilyIsTheReferencesUnionAsLanguageRowsNamingTypedefs) {
    ASSERT_NE(cLanguage(), nullptr);
    auto const& rows = cLanguage()->preprocess().predefinedMacros;
    std::size_t formatRows = 0;
    for (auto const& pm : rows) formatRows += pm.kind == PredefinedMacroKind::TypeFormat ? 1u : 0u;
    ASSERT_EQ(members().size(), 120u);
    EXPECT_EQ(formatRows, members().size())
        << "the language declares a format row this pin does not know, or lost one";
    for (Member const& m : members()) {
        auto const it = std::ranges::find(rows, m.name, &PredefinedMacroDef::name);
        ASSERT_NE(it, rows.end()) << m.name << " is not a row of the C language";
        EXPECT_EQ(it->kind, PredefinedMacroKind::TypeFormat) << m.name;
        EXPECT_TRUE(it->value.empty()) << m.name << ": a format row states no value";
        EXPECT_EQ(it->sizedType.source, PredefinedTypeSource::ShippedTypedef) << m.name;
        EXPECT_EQ(it->sizedType.spelled, m.family.typedefName) << m.name;
        EXPECT_EQ(it->sizedType.header, m.family.header) << m.name;
        EXPECT_EQ(it->formatConversion, m.conversion) << m.name;
        EXPECT_TRUE(it->availableObjectFormats.empty())
            << m.name << ": the TYPEDEF decides the value, never a format list";
    }
}

// ── (2) EVERY EXECUTABLE PAIR: THE REFERENCE'S TYPE, FORMATTED ────────────────
TEST(TypeFormatPredefines, EveryValueIsTheReferenceTypesModifierOnEveryPair) {
    ASSERT_NE(cLanguage(), nullptr);
    for (Pair const& p : pairs()) {
        SCOPED_TRACE(std::string{p.target} + ":" + std::string{p.formatDoc});
        auto targetR = TargetSchema::loadShipped(p.target);
        ASSERT_TRUE(targetR.has_value()) << p.target;
        auto formatR = ObjectFormatSchema::loadShipped(p.formatDoc);
        ASSERT_TRUE(formatR.has_value()) << p.formatDoc;
        PredefinedTypeFacts const facts = predefinedTypeFactsFor(**targetR, **formatR);
        auto const values = realized(&facts, cLanguage().get(), (*formatR)->kind());
        EXPECT_EQ(values.size(), members().size()) << "every name realizes on every pair";
        for (Member const& m : members()) {
            auto const it = values.find(m.name);
            if (it == values.end()) {
                ADD_FAILURE() << m.name << " did not realize";
                continue;
            }
            std::string const want = "\"" + std::string{modifierOf(referenceType(p, m.family.typedefName))}
                                     + m.conversion + "\"";
            EXPECT_EQ(it->second, want) << m.name << " (" << m.family.typedefName << " is "
                                        << referenceType(p, m.family.typedefName) << ")";
        }
    }
}

// The measured spellings, pinned as they read, so a table edit that happens to agree
// with the derivation above still has to agree with the reference dumps: Apple clang
// 21 (the only compiler defining all 120) on Mach-O, and the glibc typedef's answer
// on ELF where clang 18 and glibc disagree (the RULED fork).
TEST(TypeFormatPredefines, TheMeasuredSpellingsAndTheRuledFork) {
    ASSERT_NE(cLanguage(), nullptr);
    auto facts = [](std::string_view target, std::string_view format) {
        auto t = TargetSchema::loadShipped(target);
        auto f = ObjectFormatSchema::loadShipped(format);
        EXPECT_TRUE(t.has_value() && f.has_value());
        return std::pair{predefinedTypeFactsFor(**t, **f), (*f)->kind()};
    };
    auto const [mach, machKind] = facts("arm64", "macho64-arm64-darwin-exec");
    auto const onMach = realized(&mach, cLanguage().get(), machKind);
    EXPECT_EQ(onMach.at("__INT64_FMTd__"), "\"lld\"");
    EXPECT_EQ(onMach.at("__INTMAX_FMTd__"), "\"ld\"") << "Apple: intmax_t is long, not `j`";
    EXPECT_EQ(onMach.at("__INT_FAST16_FMTd__"), "\"hd\"");
    EXPECT_EQ(onMach.at("__UINT8_FMTB__"), "\"hhB\"");
    EXPECT_EQ(onMach.at("__SIZE_FMTx__"), "\"lx\"");
    EXPECT_EQ(onMach.at("__UINT32_FMTb__"), "\"b\"");
    auto const [elf, elfKind] = facts("x86_64", "elf64-x86_64-linux-exec");
    auto const onElf = realized(&elf, cLanguage().get(), elfKind);
    EXPECT_EQ(onElf.at("__INT64_FMTd__"), "\"ld\"");
    EXPECT_EQ(onElf.at("__INT_FAST16_FMTd__"), "\"ld\"")
        << "glibc's int_fast16_t is long — the RULED fork against clang 18's \"hd\"";
    auto const [pe, peKind] = facts("x86_64", "pe64-x86_64-windows-exec");
    auto const onPe = realized(&pe, cLanguage().get(), peKind);
    EXPECT_EQ(onPe.at("__INT64_FMTd__"), "\"lld\"");
    EXPECT_EQ(onPe.at("__SIZE_FMTu__"), "\"llu\"");
}

// No language, or no pair: none of them — never a guess.
TEST(TypeFormatPredefines, NoPairOrNoLanguageRealizesNothing) {
    ASSERT_NE(cLanguage(), nullptr);
    auto t = TargetSchema::loadShipped("x86_64");
    auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(t.has_value() && f.has_value());
    PredefinedTypeFacts const facts = predefinedTypeFactsFor(**t, **f);
    EXPECT_TRUE(realized(&facts, nullptr, ObjectFormatKind::Elf).empty());
    EXPECT_TRUE(realized(nullptr, cLanguage().get(), ObjectFormatKind::Elf).empty());
}

// ── (3) THE LOADER ──────────────────────────────────────────────────────────
TEST(TypeFormatPredefinesLoader, ARowWithoutItsConversionIsRefused) {
    refusedNaming(shippedCEdited(kInt64d, ", \"conversion\": \"d\"", ""), "requires 'conversion'");
}

TEST(TypeFormatPredefinesLoader, AnUnknownConversionIsRefusedWithTheAcceptedSet) {
    refusedNaming(shippedCEdited(kInt64d, "\"conversion\": \"d\"", "\"conversion\": \"f\""),
                  "unknown integer conversion");
}

TEST(TypeFormatPredefinesLoader, AConversionOnAnotherKindIsRefused) {
    refusedNaming(shippedCEdited("\"__INT64_TYPE__\"", "\"kind\": \"type-name\",",
                                 "\"kind\": \"type-name\", \"conversion\": \"d\","),
                  "valid only on a 'type-format'");
}

TEST(TypeFormatPredefinesLoader, AnUnknownTableKeyIsRefused) {
    refusedNaming(shippedCEdited(kModsKey, "\"modifier\": \"hh\"",
                                 "\"modifier\": \"hh\", \"modifer\": \"hh\""),
                  "modifer");
}

TEST(TypeFormatPredefinesLoader, ATableTypeThatIsNotAnIntegerIsRefused) {
    refusedNaming(shippedCEdited(kModsKey, "\"types\": [\"long\", \"unsigned long\"]",
                                 "\"types\": [\"long\", \"unsigned long\", \"double\"]"),
                  "not an integer type");
}

TEST(TypeFormatPredefinesLoader, OneTypeInTwoEntriesIsRefused) {
    refusedNaming(shippedCEdited(kModsKey, "\"types\": [\"int\", \"unsigned int\"]",
                                 "\"types\": [\"int\", \"unsigned int\", \"long\"]"),
                  "a type has one length modifier");
}

// ── (4) THE MERGE REFUSES, LOUD, A TYPE THE PAIR REALIZES AND THE TABLE CANNOT FORMAT ──
TEST(TypeFormatPredefines, ATypeTheTableCannotFormatIsAConflict) {
    // `long` dropped from its entry: no vocabulary row names it, so the document loads
    // — and the ELF pair's `int64_t` (`long`) then has no modifier. The merge must
    // refuse, naming the macro, never guess one.
    std::string const text = shippedCEdited(kModsKey, "\"types\": [\"long\", \"unsigned long\"]",
                                            "\"types\": [\"unsigned long\"]");
    auto edited = GrammarSchema::loadFromText(text, "<edited-c>");
    ASSERT_TRUE(edited.has_value()) << (edited.error().empty() ? "" : edited.error()[0].message);
    auto t = TargetSchema::loadShipped("x86_64");
    auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(t.has_value() && f.has_value());
    PredefinedTypeFacts const facts = predefinedTypeFactsFor(**t, **f);
    std::vector<std::string> conflicts;
    auto const out = realized(&facts, edited->get(), ObjectFormatKind::Elf, &conflicts);
    bool named = false;
    for (auto const& c : conflicts) {
        named |= c.find("__INT64_FMTd__") != std::string::npos
                 && c.find("names no length modifier") != std::string::npos;
    }
    EXPECT_TRUE(named) << "the conflict must name __INT64_FMTd__ and why";
}
