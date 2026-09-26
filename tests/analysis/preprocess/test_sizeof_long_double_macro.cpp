// `__SIZEOF_LONG_DOUBLE__` — the C language's `type-size` row, realized on
// every (target, format) pair to the size `sizeof(long double)` has there.
//
// ═══ WHY THIS FILE EXISTS (P68 round 8) ═════════════════════════════════════
//
// gcc and clang predefine `__SIZEOF_LONG_DOUBLE__`; DSS did not
// (D-C-SIZEOF-LONG-DOUBLE-NOT-PREDEFINED). An undefined name in `#if` evaluates
// to 0, so a program that picks a code path on it took its `#else` arm under DSS
// on every target, silently, and one that required it stopped.
//
// ⓘ THE FIRST FIX STATED THE NUMBER; THIS ONE STATES THE TYPE
// (D-C-SIZEOF-PREDEFINED-MACRO-FAMILY-MISSING). It first shipped as 18
// per-format `constant` rows, each spelling the size beside the
// `longDoubleFormat` it followed — one fact written twice per file, held
// together only by this test. It is now ONE row in `c.lang.json`:
// `{ "kind": "type-size", "type": "long double" }`, realized per pair by the
// merge from the same facts `sizeof(long double)` reads:
//
//     format family             longDoubleFormat   sizeof   ✔MEASURED reference
//     elf64-aarch64-linux*      ieee128            16       gcc 16, clang 16
//     elf64-x86_64-linux*       x87-80             16       gcc 16, clang 16
//     macho64-arm64-darwin*     f64                8        clang 8
//     macho64-x86_64-darwin*    x87-80             16       clang 16
//     pe64-x86_64-windows*      f64                8        clang-msvc 8
//
// ★ THE PE ROW IS WHY THE EXPECTATION IS NEVER A TABLE WRITTEN HERE. mingw's gcc
// and clang (`x86_64-w64-windows-gnu`) both say 16 for x86_64 Windows, because
// mingw's `long double` is x87. DSS's PE `long double` is binary64 — MSVC's ABI,
// the one its PE model follows — so the number that is RIGHT for DSS is 8. The
// pin derives the expected size from the C grammar's `long double` specifier row
// (`TypeSpecifierRule::resolveCore`) and the layout the type system uses
// (`scalarByteSize`).
//
// Pins:
//   * the language declares the macro ONCE, as a `type-size` row naming
//     `long double`, resolved at load to the triple the grammar's own
//     `long double` row carries;
//   * on EVERY (target × format) pair the merge realizes it to exactly the size
//     the type has there when the format realizes `long double`, and leaves it
//     UNDEFINED when the format does not — a size for a type the build refuses
//     is a promise a program would branch on. ★ This is also what holds
//     `predefinedTypeSize`'s restated resolution rule equal to
//     `TypeSpecifierRule::resolveCore`'s;
//   * no target or format document states it (the language row is the one
//     owner — a leftover format row would also be a merge conflict);
//   * with no pair at all (no facts) it is not defined;
//   * FLOORS on every enumeration, so a collapsed directory walk reds instead of
//     passing over nothing.
//
// The end-to-end half — `_Static_assert(sizeof(long double) ==
// __SIZEOF_LONG_DOUBLE__)` compiled and run — is the corpus example
// `examples/c/sizeof_long_double_macro`; the whole family's is
// `test_sizeof_macro_family` and `examples/c/sizeof_macro_family`.

#include "analysis/compilation_unit/compilation_unit.hpp"   // predefinedTypeFactsFor
#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/data_model.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/preprocess_config.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

constexpr std::string_view kMacro = "__SIZEOF_LONG_DOUBLE__";

// The shipped C grammar's `long double` row — the NON-complex one; the
// `_Complex` row shares the name and resolves the ELEMENT type. A test's
// expectation about config, never an engine branch on a spelling.
[[nodiscard]] TypeSpecifierRule const* longDoubleRow(GrammarSchema const& c) {
    for (auto const& row : c.semantics().typeSpecifiers) {
        if (row.name == "long double" && !row.complex) return &row;
    }
    return nullptr;
}

// The rows of `macros` named `__SIZEOF_LONG_DOUBLE__`. ONE matcher for the
// presence arms and the absence arms, so an absence verdict asks exactly the
// question a presence verdict answered.
[[nodiscard]] std::vector<PredefinedMacroDef const*>
sizeRows(std::span<PredefinedMacroDef const> macros) {
    std::vector<PredefinedMacroDef const*> out;
    for (auto const& m : macros) {
        if (m.name == kMacro) out.push_back(&m);
    }
    return out;
}

struct ShippedFormat {
    std::string                         name;
    std::shared_ptr<ObjectFormatSchema> schema;
};

// Every `.format.json` on disk, loaded. A document that does not load cannot
// serve a build, so it contributes nothing — but it is COUNTED by the caller's
// floor, never silently dropped.
[[nodiscard]] std::vector<ShippedFormat> shippedFormats(std::size_t& filesSeen) {
    std::vector<ShippedFormat> out;
    filesSeen = 0;
    auto const dir = dss::test::configRoot() / "object-formats";
    for (auto const& e : std::filesystem::directory_iterator{dir}) {
        std::string const fn = e.path().filename().string();
        auto const at = fn.find(".format.json");
        if (at == std::string::npos) continue;
        ++filesSeen;
        std::string const name = fn.substr(0, at);
        auto r = ObjectFormatSchema::loadShipped(name);
        if (!r.has_value()) continue;
        out.push_back(ShippedFormat{name, *r});
    }
    return out;
}

[[nodiscard]] std::vector<std::string> shippedTargetNames() {
    std::vector<std::string> out;
    auto const dir = dss::test::configRoot() / "targets";
    for (auto const& e : std::filesystem::directory_iterator{dir}) {
        std::string const fn = e.path().filename().string();
        auto const at = fn.find(".target.json");
        if (at == std::string::npos) continue;
        out.push_back(fn.substr(0, at));
    }
    return out;
}

}  // namespace

// ── ★ ONE OWNER: the language row, naming the TYPE ──────────────────────────
TEST(SizeofLongDoubleMacro, TheLanguageDeclaresItOnceAsTheSizeOfLongDouble) {
    auto grammarR = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(grammarR.has_value());
    auto const& c = **grammarR;
    TypeSpecifierRule const* const row = longDoubleRow(c);
    ASSERT_NE(row, nullptr) << "the C grammar declares no `long double` row";

    auto const rows = sizeRows(c.preprocess().predefinedMacros);
    ASSERT_EQ(rows.size(), 1u) << "c.lang.json must declare " << kMacro << " once";
    PredefinedMacroDef const& m = *rows.front();
    EXPECT_EQ(m.kind, PredefinedMacroKind::TypeSize)
        << kMacro << " must be DERIVED from the type, never a number";
    EXPECT_EQ(m.sizedType.source, PredefinedTypeSource::Vocabulary);
    EXPECT_EQ(m.sizedType.spelled, "long double");
    ASSERT_TRUE(m.sizedType.resolved) << "the loader left the row unresolved";
    // The load resolved it to the SAME triple `sizeof(long double)` reads.
    EXPECT_EQ(m.sizedType.core, row->core);
    EXPECT_EQ(m.sizedType.coreByDataModel, row->coreByDataModel);
    EXPECT_EQ(m.sizedType.coreByLongDoubleFormat, row->coreByLongDoubleFormat);
    EXPECT_TRUE(m.availableObjectFormats.empty())
        << "ungated: whether it is defined is the PAIR's question, answered by "
           "the type, not by a format filter";

    // And NO target or format document states it — two owners of one fact is
    // what this row replaced, and a same-named row would be a merge conflict.
    for (std::string const& t : shippedTargetNames()) {
        auto tr = TargetSchema::loadShipped(t);
        ASSERT_TRUE(tr.has_value()) << t;
        EXPECT_TRUE(sizeRows((*tr)->predefinedMacros()).empty())
            << t << ".target.json states " << kMacro;
    }
    std::size_t filesSeen = 0;
    for (auto const& f : shippedFormats(filesSeen)) {
        EXPECT_TRUE(sizeRows(f.schema->predefinedMacros()).empty())
            << f.name << ".format.json states " << kMacro
            << " — the language's `type-size` row owns it now";
    }
    EXPECT_GE(filesSeen, 24u) << "the .format.json enumeration collapsed";
}

// ── ★ REALIZED ON EVERY PAIR TO THE SIZE THE TYPE HAS — OR NOT AT ALL ──────
//
// Every target is paired with every format, unfiltered for plausibility (the
// question "what does the effective set say for this pair?" is answerable for
// any pair, and a plausibility filter would be an identity branch handing a
// pair a silent skip).
TEST(SizeofLongDoubleMacro, EveryPairStatesTheSizeLongDoubleHasThereOrNothing) {
    auto grammarR = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(grammarR.has_value());
    auto const& c = **grammarR;
    auto const& pp = c.preprocess();
    TypeSpecifierRule const* const row = longDoubleRow(c);
    ASSERT_NE(row, nullptr);

    auto const targetNames = shippedTargetNames();
    ASSERT_GE(targetNames.size(), 2u) << "the target enumeration collapsed";
    std::size_t filesSeen = 0;
    auto const formats = shippedFormats(filesSeen);
    ASSERT_GE(formats.size(), 24u) << "the format enumeration collapsed";
    EXPECT_EQ(formats.size(), filesSeen)
        << "a shipped format stopped loading — it would be swept silently";

    std::size_t armsWithSize = 0, armsWithout = 0;
    for (std::string const& targetName : targetNames) {
        auto targetR = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(targetR.has_value()) << targetName;
        for (auto const& f : formats) {
            PredefinedTypeFacts const facts =
                predefinedTypeFactsFor(**targetR, *f.schema);
            MergedPredefinedMacros const merged = mergePredefinedMacros(
                pp.predefinedMacros, (*targetR)->predefinedMacros(),
                f.schema->predefinedMacros(), f.schema->kind(),
                pp.mutuallyExclusivePredefinedMacros, &facts);
            ASSERT_TRUE(merged.conflicts.empty())
                << targetName << ':' << f.name << " -> " << merged.conflicts.front();
            auto const seen = sizeRows(merged.effective);
            auto const core = row->resolveCore(f.schema->dataModel(),
                                               f.schema->longDoubleFormat());
            if (!core.has_value()) {
                ++armsWithout;
                EXPECT_TRUE(seen.empty())
                    << targetName << ':' << f.name << " does not realize `long "
                       "double` (no longDoubleFormat), yet defines "
                    << kMacro << " = '" << seen.front()->value << "'";
                continue;
            }
            ++armsWithSize;
            auto const bytes = scalarByteSize(*core, f.schema->dataModel());
            ASSERT_TRUE(bytes.has_value()) << f.name;
            ASSERT_EQ(seen.size(), 1u)
                << targetName << ':' << f.name << " realizes `long double` ("
                << longDoubleFormatName(f.schema->longDoubleFormat())
                << ") and must define " << kMacro << " exactly once";
            EXPECT_EQ(seen.front()->value, std::to_string(*bytes))
                << targetName << ':' << f.name << ": " << kMacro << " says '"
                << seen.front()->value << "' but sizeof(long double) is "
                << *bytes << " there ("
                << longDoubleFormatName(f.schema->longDoubleFormat()) << ", "
                << dataModelName(f.schema->dataModel()) << ")";
        }
    }
    // FLOORS. ✔MEASURED at P68 round 8 part 4: 18 of the 24 formats realize
    // `long double` (the elf `-dyn`/`-pie` four and wasm/spirv do not, until
    // they declare the axis) × every shipped target.
    EXPECT_GE(armsWithSize, 18u * targetNames.size());
    EXPECT_EQ(armsWithSize + armsWithout, formats.size() * targetNames.size());
}

// ── ★ NO PAIR, NO SIZE ──────────────────────────────────────────────────────
//
// A caller that names no (target, format) — the LSP with none, the direct API —
// merges with no facts. The row must then be ABSENT, not sized for some default
// target: "what size is `long double` here?" has no answer when no "here" was
// named, and a borrowed answer is the wrong one on every other target.
TEST(SizeofLongDoubleMacro, WithNoPairTheSizeIsNotDefined) {
    auto grammarR = GrammarSchema::loadShipped("c");
    ASSERT_TRUE(grammarR.has_value());
    auto const& pp = (*grammarR)->preprocess();
    // The control: the row IS there to drop.
    ASSERT_EQ(sizeRows(pp.predefinedMacros).size(), 1u);
    MergedPredefinedMacros const merged = mergePredefinedMacros(
        pp.predefinedMacros, {}, {}, std::nullopt,
        pp.mutuallyExclusivePredefinedMacros, nullptr);
    ASSERT_TRUE(merged.conflicts.empty());
    EXPECT_TRUE(sizeRows(merged.effective).empty())
        << "with no pair the merge must not invent a size";
}
