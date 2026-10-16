// P69 fold 3 (lane `xa`) — THE SUMMARY TIER STATES WHAT KIND OF DEFINITION EACH ROW IS.
// D-LK-WEAK-EXTERNAL-BODY-OUTRANKED-A-SELECT-ANY-DEFINITION-BY-LINK-ORDER
// D-C-TENTATIVE-DEFINITIONS-OF-ONE-NAME-IN-TWO-UNITS-REFUSED
//
// The summary index (`--lto=thin`) elects one definition of a name across units, and the election is the link's
// (`linker::resolveCrossCuDefs`). Until this fold a summary row stated a binding and a visibility and nothing more,
// so the index could not tell an overridable weak definition from a select-any one, nor a tentative definition
// from a strong one: it ranked every weak definition alike and refused two units' `int x;` as two strong
// definitions. A row now carries the KIND of its weak definition and whether it is a COMMON, each taken from the
// one statement of it in the MIR (`MirLinkage::statedWeakKind`, `Mir::globalIsCommon`).
//
// THE STORED FORM is version 3: a kind byte after the visibility on every row, and a common flag on a global row
// (version 2), and an ALIAS table at the end (version 3, P69 lane `cs`: each alias row its name, its target's name
// and its own binding, visibility and kind, judged by the same rule as every other row).
// A version-1 image is REFUSED, not read with the new bytes guessed — the reader has one answer for an image it
// does not understand ("not a summary"), and that is the answer here. The codec is reached by tests alone today:
// nothing in `src/` calls `encodeModuleSummary` or `decodeModuleSummary` (READ 2026-10-12, a scan of `src/`).
//
// A row's linkage bytes are judged on the way in by the rule every other producer of a linkage record is judged
// by (`mirLinkageDefect`), so the pins below are of THAT rule reaching the decoder, not of a fourth copy of it.

#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/summary/mir_summary.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::mirsum;

namespace {

using Kind = WeakDefinitionKind;

[[nodiscard]] MirLinkage linkage(SymbolBinding binding, std::optional<Kind> kind, bool tentative = false) {
    MirLinkage l{binding, SymbolVisibility::Default};
    if (kind.has_value()) l.weakKind = *kind;
    l.tentative = tentative;
    return l;
}

[[nodiscard]] std::string nameOf(SymbolId s) { return "n" + std::to_string(s.v); }

[[nodiscard]] SummaryFunction const* fn(ModuleSummary const& s, std::string const& name) {
    for (auto const& f : s.functions) {
        if (f.name == name) return &f;
    }
    return nullptr;
}
[[nodiscard]] SummaryGlobal const* global(ModuleSummary const& s, std::string const& name) {
    for (auto const& g : s.globals) {
        if (g.name == name) return &g;
    }
    return nullptr;
}

// One module holding every shape of definition a row can be.
//   functions: n1 strong, n2 overridable, n3 select-any
//   globals:   n11 strong (initialized), n12 tentative, n13 overridable, n14 select-any,
//              n15 thread-local with no initializer (tentative in C, and NOT a common: the front end builds it
//              without the mark, which the rule refuses on a thread-local global)
[[nodiscard]] Mir everyShape(TypeInterner& in) {
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const fnSig = in.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder   b;
    auto const   body = [&] {
        MirBlockId const e = b.createBlock(StructCfMarker::EntryBlock);
        b.beginBlock(e);
        MirLiteralValue v;
        v.value = std::int64_t{7};
        v.core  = TypeKind::I32;
        b.addReturn(b.addConst(v, i32));
    };
    b.addFunction(fnSig, SymbolId{1}, linkage(SymbolBinding::Global, std::nullopt));
    body();
    b.addFunction(fnSig, SymbolId{2}, linkage(SymbolBinding::Weak, Kind::Overridable));
    body();
    b.addFunction(fnSig, SymbolId{3}, linkage(SymbolBinding::Weak, Kind::SelectAny));
    body();
    auto const literal = [&] {
        MirLiteralValue v;
        v.value = std::int64_t{5};
        v.core  = TypeKind::I32;
        return b.literalPoolAdd(v);
    };
    (void)b.addGlobal(i32, SymbolId{11}, literal(), MirFuncId{}, linkage(SymbolBinding::Global, std::nullopt),
                      /*isConst=*/false, mirThreadStorageOf(false));
    (void)b.addGlobal(i32, SymbolId{12}, UINT32_MAX, MirFuncId{},
                      linkage(SymbolBinding::Global, std::nullopt, /*tentative=*/true), false,
                      mirThreadStorageOf(false));
    (void)b.addGlobal(i32, SymbolId{13}, literal(), MirFuncId{}, linkage(SymbolBinding::Weak, Kind::Overridable),
                      false, mirThreadStorageOf(false));
    (void)b.addGlobal(i32, SymbolId{14}, literal(), MirFuncId{}, linkage(SymbolBinding::Weak, Kind::SelectAny),
                      false, mirThreadStorageOf(false));
    (void)b.addGlobal(i32, SymbolId{15}, UINT32_MAX, MirFuncId{}, linkage(SymbolBinding::Global, std::nullopt),
                      false, mirThreadStorageOf(true));
    // aliases: n21 a strong second name of n1, n22 an overridable hidden one of n12.
    b.addAlias(SymbolId{21}, SymbolId{1}, linkage(SymbolBinding::Global, std::nullopt));
    MirLinkage hiddenWeak = linkage(SymbolBinding::Weak, Kind::Overridable);
    hiddenWeak.visibility = SymbolVisibility::Hidden;
    b.addAlias(SymbolId{22}, SymbolId{12}, hiddenWeak);
    return std::move(b).finish();
}

[[nodiscard]] ModuleSummary summaryOfEveryShape() {
    TypeInterner   in{CompilationUnitId{1}};
    Mir const      mir = everyShape(in);
    SummaryCuInput cu;
    cu.mir            = &mir;
    cu.nameOf         = nameOf;
    cu.moduleDigest   = "digest";
    cu.targetIdentity = "one target";
    return buildModuleSummary(cu);
}

// A summary of ONE global row and no function, whose bytes are easy to find: the row's linkage is hand-stated.
[[nodiscard]] ModuleSummary oneGlobalRow(SymbolBinding binding, std::optional<Kind> kind, bool isCommon,
                                         bool isThreadLocal) {
    ModuleSummary s;
    s.moduleDigest   = "d";
    s.targetIdentity = "t";
    SummaryGlobal g;
    g.name          = "g";
    g.symbol        = 1;
    g.binding       = binding;
    g.visibility    = SymbolVisibility::Default;
    g.weakKind      = kind;
    g.isCommon      = isCommon;
    g.isThreadLocal = isThreadLocal;
    s.globals.push_back(std::move(g));
    return s;
}
[[nodiscard]] ModuleSummary oneAliasRow(SymbolBinding binding, std::optional<Kind> kind) {
    ModuleSummary s;
    s.moduleDigest   = "d";
    s.targetIdentity = "t";
    SummaryAlias a;
    a.name       = "a";
    a.symbol     = 2;
    a.targetName = "g";
    a.binding    = binding;
    a.weakKind   = kind;
    s.aliases.push_back(std::move(a));
    return s;
}
[[nodiscard]] ModuleSummary oneFunctionRow(SymbolBinding binding, std::optional<Kind> kind) {
    ModuleSummary s;
    s.moduleDigest   = "d";
    s.targetIdentity = "t";
    SummaryFunction f;
    f.name       = "f";
    f.symbol     = 1;
    f.binding    = binding;
    f.visibility = SymbolVisibility::Default;
    f.weakKind   = kind;
    s.functions.push_back(std::move(f));
    return s;
}

// Where the FIRST row's binding byte stands in an image of `oneGlobalRow` / `oneFunctionRow`: magic (7), version
// (4), the two one-character strings (4 + 1 each), then for a global row the empty function table's count (4),
// then the row table's count (4), the row's one-character name (4 + 1) and its symbol (4).
constexpr std::size_t kVersionAt          = 7;
constexpr std::size_t kFunctionBindingAt  = 7 + 4 + 5 + 5 + 4 + 5 + 4;
constexpr std::size_t kGlobalBindingAt    = 7 + 4 + 5 + 5 + 4 + 4 + 5 + 4;
// An `oneAliasRow` image: the four empty tables' counts (functions, globals, imports, escapes), the alias table's
// count, then the row's one-character name, its symbol and its one-character target name.
constexpr std::size_t kAliasBindingAt     = 7 + 4 + 5 + 5 + 4 + 4 + 4 + 4 + 4 + 5 + 4 + 5;
// After the binding: visibility (+1), the weak kind (+2), and on a global row the common flag (+3), isConst (+4),
// isThreadLocal (+5).
constexpr std::size_t kKindAfterBinding   = 2;
constexpr std::size_t kCommonAfterBinding = 3;
constexpr std::size_t kTlsAfterBinding    = 5;

}  // namespace

TEST(MirSummaryLinkage, ARowStatesTheKindOfItsWeakDefinitionAndWhetherItIsACommon) {
    ModuleSummary const s = summaryOfEveryShape();
    struct WantFn {
        char const*         name;
        SymbolBinding       binding;
        std::optional<Kind> kind;
    };
    for (auto const& w : {WantFn{"n1", SymbolBinding::Global, std::nullopt},
                          WantFn{"n2", SymbolBinding::Weak, Kind::Overridable},
                          WantFn{"n3", SymbolBinding::Weak, Kind::SelectAny}}) {
        SCOPED_TRACE(w.name);
        auto const* row = fn(s, w.name);
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->binding, w.binding);
        EXPECT_EQ(row->weakKind, w.kind);
    }
    struct WantGlobal {
        char const*         name;
        SymbolBinding       binding;
        std::optional<Kind> kind;
        bool                isCommon;
    };
    for (auto const& w : {WantGlobal{"n11", SymbolBinding::Global, std::nullopt, false},
                          WantGlobal{"n12", SymbolBinding::Global, std::nullopt, true},
                          WantGlobal{"n13", SymbolBinding::Weak, Kind::Overridable, false},
                          WantGlobal{"n14", SymbolBinding::Weak, Kind::SelectAny, false},
                          // A thread-local object with no initializer is tentative in C and is NOT a common: no
                          // format has a thread-local common, so its unit defines it outright.
                          WantGlobal{"n15", SymbolBinding::Global, std::nullopt, false}}) {
        SCOPED_TRACE(w.name);
        auto const* row = global(s, w.name);
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->binding, w.binding);
        EXPECT_EQ(row->weakKind, w.kind);
        EXPECT_EQ(row->isCommon, w.isCommon);
    }
    ASSERT_NE(global(s, "n15"), nullptr);
    EXPECT_TRUE(global(s, "n15")->isThreadLocal);
}

TEST(MirSummaryLinkage, TheStoredFormIsVersionThreeAndEveryShapeRoundTrips) {
    EXPECT_EQ(kSummaryFormatVersion, 3u);
    ModuleSummary const             s     = summaryOfEveryShape();
    std::vector<std::uint8_t> const bytes = encodeModuleSummary(s);
    ASSERT_GT(bytes.size(), kVersionAt + 4);
    EXPECT_EQ(bytes[kVersionAt], 3u);
    EXPECT_EQ(bytes[kVersionAt + 1] | bytes[kVersionAt + 2] | bytes[kVersionAt + 3], 0u);

    auto const back = decodeModuleSummary(bytes);
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->functions.size(), s.functions.size());
    for (std::size_t i = 0; i < s.functions.size(); ++i) {
        SCOPED_TRACE(s.functions[i].name);
        EXPECT_EQ(back->functions[i].binding, s.functions[i].binding);
        EXPECT_EQ(back->functions[i].weakKind, s.functions[i].weakKind);
    }
    ASSERT_EQ(back->globals.size(), s.globals.size());
    for (std::size_t i = 0; i < s.globals.size(); ++i) {
        SCOPED_TRACE(s.globals[i].name);
        EXPECT_EQ(back->globals[i].binding, s.globals[i].binding);
        EXPECT_EQ(back->globals[i].weakKind, s.globals[i].weakKind);
        EXPECT_EQ(back->globals[i].isCommon, s.globals[i].isCommon);
        EXPECT_EQ(back->globals[i].isThreadLocal, s.globals[i].isThreadLocal);
    }
    ASSERT_EQ(s.aliases.size(), 2u) << "the module states two aliases, and the summary carries both";
    EXPECT_EQ(s.aliases[0].name, "n21");
    EXPECT_EQ(s.aliases[0].targetName, "n1");
    EXPECT_EQ(s.aliases[1].targetName, "n12");
    EXPECT_EQ(s.aliases[1].weakKind, std::optional<Kind>{Kind::Overridable});
    EXPECT_EQ(s.aliases[1].visibility, SymbolVisibility::Hidden);
    ASSERT_EQ(back->aliases.size(), s.aliases.size());
    for (std::size_t i = 0; i < s.aliases.size(); ++i) {
        SCOPED_TRACE(s.aliases[i].name);
        EXPECT_EQ(back->aliases[i].name, s.aliases[i].name);
        EXPECT_EQ(back->aliases[i].symbol, s.aliases[i].symbol);
        EXPECT_EQ(back->aliases[i].targetName, s.aliases[i].targetName);
        EXPECT_EQ(back->aliases[i].binding, s.aliases[i].binding);
        EXPECT_EQ(back->aliases[i].visibility, s.aliases[i].visibility);
        EXPECT_EQ(back->aliases[i].weakKind, s.aliases[i].weakKind);
    }
    EXPECT_EQ(encodeModuleSummary(*back), bytes) << "what was read writes the same image";
}

TEST(MirSummaryLinkage, AVersionOneImageIsRefused) {
    for (ModuleSummary const& s : {oneGlobalRow(SymbolBinding::Global, std::nullopt, false, false),
                                   oneFunctionRow(SymbolBinding::Global, std::nullopt), summaryOfEveryShape()}) {
        std::vector<std::uint8_t> bytes = encodeModuleSummary(s);
        ASSERT_TRUE(decodeModuleSummary(bytes).has_value()) << "the control: the image as written is read";
        ASSERT_EQ(bytes[kVersionAt], 3u);
        bytes[kVersionAt] = 2;
        EXPECT_FALSE(decodeModuleSummary(bytes).has_value())
            << "an image that says it is version 2 has no alias table: it is not read as if it had";
        bytes[kVersionAt] = 1;
        EXPECT_FALSE(decodeModuleSummary(bytes).has_value())
            << "an image that says it is version 1 has no kind byte and no common flag: it is not read as if it had";
    }
    // A real version-2 image — the current one with the (empty) alias table taken off its end — is short under
    // version 3's number.
    {
        std::vector<std::uint8_t> v2 = encodeModuleSummary(oneGlobalRow(SymbolBinding::Global, std::nullopt, false, false));
        v2.resize(v2.size() - 4);
        EXPECT_FALSE(decodeModuleSummary(v2).has_value()) << "version 2's image under version 3's number";
    }
    // And an image that really HAS version 1's rows — the version 2 image with the kind byte and the common flag
    // taken out — is refused under either version number: version 1 is not read, and under version 2 the row is
    // short.
    std::vector<std::uint8_t> v1 = encodeModuleSummary(oneGlobalRow(SymbolBinding::Global, std::nullopt, false, false));
    v1.erase(v1.begin() + static_cast<std::ptrdiff_t>(kGlobalBindingAt + kKindAfterBinding),
             v1.begin() + static_cast<std::ptrdiff_t>(kGlobalBindingAt + kCommonAfterBinding + 1));
    EXPECT_FALSE(decodeModuleSummary(v1).has_value()) << "version 1's rows under version 3's number";
    v1[kVersionAt] = 1;
    EXPECT_FALSE(decodeModuleSummary(v1).has_value()) << "version 1's rows under version 1's number";
}

// The byte offsets the refusals below patch are the ones the comment above the constants derives; this is the
// control that they are where the test says, on images the reader accepts.
TEST(MirSummaryLinkage, TheLinkageBytesStandWhereTheRefusalPinsPatchThem) {
    {
        auto const bytes = encodeModuleSummary(oneFunctionRow(SymbolBinding::Weak, Kind::SelectAny));
        ASSERT_GT(bytes.size(), kFunctionBindingAt + kKindAfterBinding);
        EXPECT_EQ(bytes[kFunctionBindingAt], static_cast<std::uint8_t>(SymbolBinding::Weak));
        EXPECT_EQ(bytes[kFunctionBindingAt + kKindAfterBinding], static_cast<std::uint8_t>(Kind::SelectAny));
    }
    {
        auto const bytes = encodeModuleSummary(oneGlobalRow(SymbolBinding::Global, std::nullopt, true, false));
        ASSERT_GT(bytes.size(), kGlobalBindingAt + kTlsAfterBinding);
        EXPECT_EQ(bytes[kGlobalBindingAt], static_cast<std::uint8_t>(SymbolBinding::Global));
        EXPECT_EQ(bytes[kGlobalBindingAt + kKindAfterBinding], 0u) << "no kind stated";
        EXPECT_EQ(bytes[kGlobalBindingAt + kCommonAfterBinding], 1u) << "a common";
        EXPECT_EQ(bytes[kGlobalBindingAt + kTlsAfterBinding], 0u) << "not thread-local";
    }
    EXPECT_NE(static_cast<std::uint8_t>(Kind::Overridable), 0u) << "0 on the wire is 'no kind stated'";
    EXPECT_NE(static_cast<std::uint8_t>(Kind::SelectAny), 0u) << "0 on the wire is 'no kind stated'";
}

TEST(MirSummaryLinkage, ARowWhoseLinkageBytesAreNotWellFormedIsRefused) {
    // Each starts from an image the reader ACCEPTS and changes ONE byte.
    struct Case {
        char const*   what;
        ModuleSummary good;
        bool          global;
        std::size_t   afterBinding;
        std::uint8_t  value;
    };
    std::vector<Case> const cases{
        {"a weak function that states no kind", oneFunctionRow(SymbolBinding::Weak, Kind::Overridable), false,
         kKindAfterBinding, 0},
        {"a weak function whose kind byte names no kind", oneFunctionRow(SymbolBinding::Weak, Kind::Overridable),
         false, kKindAfterBinding, 9},
        {"a strong function that states a kind", oneFunctionRow(SymbolBinding::Global, std::nullopt), false,
         kKindAfterBinding, static_cast<std::uint8_t>(Kind::SelectAny)},
        {"a weak global that states no kind", oneGlobalRow(SymbolBinding::Weak, Kind::SelectAny, false, false), true,
         kKindAfterBinding, 0},
        {"a strong global that states a kind", oneGlobalRow(SymbolBinding::Global, std::nullopt, false, false), true,
         kKindAfterBinding, static_cast<std::uint8_t>(Kind::Overridable)},
        {"a weak global that claims to be a common", oneGlobalRow(SymbolBinding::Weak, Kind::Overridable, false, false),
         true, kCommonAfterBinding, 1},
        {"a module-private global that claims to be a common",
         oneGlobalRow(SymbolBinding::Local, std::nullopt, false, false), true, kCommonAfterBinding, 1},
        {"a thread-local global that claims to be a common",
         oneGlobalRow(SymbolBinding::Global, std::nullopt, false, true), true, kCommonAfterBinding, 1},
        {"a common that is then said to be thread-local", oneGlobalRow(SymbolBinding::Global, std::nullopt, true, false),
         true, kTlsAfterBinding, 1},
    };
    // The alias table's rows, judged by the same rule (P69, lane `cs`).
    struct AliasCase {
        char const*   what;
        ModuleSummary good;
        std::size_t   afterBinding;
        std::uint8_t  value;
    };
    std::vector<AliasCase> const aliasCases{
        {"a weak alias that states no kind", oneAliasRow(SymbolBinding::Weak, Kind::SelectAny), kKindAfterBinding, 0},
        {"a strong alias that states a kind", oneAliasRow(SymbolBinding::Global, std::nullopt), kKindAfterBinding,
         static_cast<std::uint8_t>(Kind::Overridable)},
    };
    for (auto const& c : aliasCases) {
        SCOPED_TRACE(c.what);
        std::vector<std::uint8_t> bytes = encodeModuleSummary(c.good);
        ASSERT_TRUE(decodeModuleSummary(bytes).has_value()) << "the control: the image before the one byte changes";
        std::size_t const at = kAliasBindingAt + c.afterBinding;
        ASSERT_LT(at, bytes.size());
        ASSERT_NE(bytes[at], c.value) << "the patch must change the byte";
        bytes[at] = c.value;
        EXPECT_FALSE(decodeModuleSummary(bytes).has_value());
    }
    for (auto const& c : cases) {
        SCOPED_TRACE(c.what);
        std::vector<std::uint8_t> bytes = encodeModuleSummary(c.good);
        ASSERT_TRUE(decodeModuleSummary(bytes).has_value()) << "the control: the image before the one byte changes";
        std::size_t const at = (c.global ? kGlobalBindingAt : kFunctionBindingAt) + c.afterBinding;
        ASSERT_LT(at, bytes.size());
        ASSERT_NE(bytes[at], c.value) << "the patch must change the byte";
        bytes[at] = c.value;
        EXPECT_FALSE(decodeModuleSummary(bytes).has_value());
    }
}
