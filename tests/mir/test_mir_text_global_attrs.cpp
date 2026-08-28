// D-MIRTEXT-GLOBAL-FLAGS-DROPPED-BY-ROUNDTRIP — a global's binding,
// visibility, constness, thread storage and explicit alignment must SURVIVE
// `emitMir` → `parseMir`, not be re-minted as defaults on the way back.
//
// ★★★ WHY THE EXISTING ROUND-TRIP TESTS CANNOT SEE THIS, AND WHY THIS FILE
// ASSERTS FIELDS RATHER THAN TEXT. `test_mir_text.cpp`'s global pin is
// `EXPECT_EQ(rt.firstEmit, rt.secondEmit)` — the byte-identical round-trip
// contract. **A TOTALLY LOSSY ROUND TRIP SATISFIES THAT CONTRACT**: if the
// emitter never prints a field and the parser never reads one, both emits are
// identical and both are wrong. Byte-equality proves the codec is
// SELF-CONSISTENT, never that it is COMPLETE, and the difference between those
// two is this entire defect. So every assertion below reads the PARSED
// MODULE's field, not the text.
//
// ★★ AND EACH ONE IS WRITTEN AS A NEGATIVE. A test that sets an attribute and
// then checks `parsed == expected` is the right shape only by luck; the
// diagnostic form is `parsed != <the default it used to come back as>`,
// because that is the assertion that throws when the field is silently
// dropped. Both are asserted here — the `_NE` says the value did not collapse
// to the default, the `_EQ` says it collapsed to nothing else either.
//
// RED-ON-DISABLE: delete the `appendGlobalAttrs(g)` call in `emitGlobal` (or
// the `parseGlobalAttrs(attrs)` call in `parseGlobal`) → the `_NE` assertions
// fail by NAME, one per lost field.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_text.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// The five values under test, every one deliberately DIFFERENT from the
// default `parseGlobal` used to hard-write. If any of these ever equals its
// default the test silently stops testing that field, so each is asserted
// against its default below rather than trusted here.
constexpr SymbolBinding    kBinding    = SymbolBinding::Weak;
constexpr SymbolVisibility kVisibility = SymbolVisibility::Hidden;
constexpr bool             kIsConst    = true;
constexpr MirThreadStorage kStorage    = MirThreadStorage::PerThread;
constexpr std::uint32_t    kAlign      = 64;

struct ParsedBack {
    std::unique_ptr<MirParseResult> result;
    std::string                     text;
};

// Emit `mir` and parse it straight back, returning both so a test can assert
// on the module AND show the text when it fails.
[[nodiscard]] ParsedBack emitThenParse(Mir const& mir,
                                       TypeInterner const&             interner,
                                       std::vector<std::string> const& names,
                                       DiagnosticReporter&             parseRep) {
    DiagnosticReporter emitRep;
    MirTextContext     ctx{&interner, &names};
    std::string        text = emitMir(mir, ctx, emitRep);
    auto               parsed = parseMir(text, CompilationUnitId{1}, parseRep);
    return ParsedBack{std::move(parsed), std::move(text)};
}

// Every diagnostic, so a failure explains itself instead of only reporting a
// false `ok`.
[[nodiscard]] std::string diagText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) out += "  diag: " + d.actual + "\n";
    if (out.empty()) out = "  (no diagnostics)\n";
    return out;
}

[[nodiscard]] std::vector<std::string> namesWithGlobalAt10() {
    std::vector<std::string> names(11, std::string{});
    names[10] = "g";
    return names;
}

} // namespace

TEST(MirTextGlobalAttrs, EveryNonDefaultFieldSurvivesTheRoundTrip) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32 = ti.primitive(TypeKind::I32);
    MirBuilder   b;
    b.addGlobal(i32, SymbolId{10}, UINT32_MAX, MirFuncId{}, kBinding,
                kVisibility, kIsConst, kStorage, kAlign);
    Mir m = std::move(b).finish();

    auto const names = namesWithGlobalAt10();
    DiagnosticReporter rep;
    auto rt = emitThenParse(m, ti, names, rep);
    ASSERT_TRUE(rt.result->ok) << "emitted text did not parse:\n" << rt.text;
    ASSERT_EQ(rt.result->mir.moduleGlobalCount(), 1u) << rt.text;
    MirGlobalId const g = rt.result->mir.globalAt(0);
    Mir const&        p = rt.result->mir;

    // Each field twice: `_NE` is the pin (it fails when the field was dropped
    // and re-minted as its default), `_EQ` is the completion (it fails when
    // the field survived as some OTHER value).
    EXPECT_NE(p.globalBinding(g), SymbolBinding::Global)
        << "binding collapsed to the default — the round trip dropped it\n"
        << rt.text;
    EXPECT_EQ(p.globalBinding(g), kBinding);

    EXPECT_NE(p.globalVisibility(g), SymbolVisibility::Default)
        << "visibility collapsed to the default\n" << rt.text;
    EXPECT_EQ(p.globalVisibility(g), kVisibility);

    EXPECT_NE(p.globalIsConst(g), false)
        << "constness collapsed to the default — a `const` global came back "
           "mutable\n" << rt.text;
    EXPECT_EQ(p.globalIsConst(g), kIsConst);

    EXPECT_NE(p.globalIsThreadLocal(g), false)
        << "thread storage collapsed to the default — a per-thread object came "
           "back process-SHARED, which is the miscompile this row names\n"
        << rt.text;
    EXPECT_EQ(p.globalIsThreadLocal(g), true);

    EXPECT_NE(p.globalAlignmentBytes(g), 0u)
        << "explicit alignment collapsed to 'no override'\n" << rt.text;
    EXPECT_EQ(p.globalAlignmentBytes(g), kAlign);
}

TEST(MirTextGlobalAttrs, TheAttributeListItselfReachesTheText) {
    // The emitter half on its own. Separate from the field assertions above so
    // a failure says WHICH half broke: this one red and that one red means the
    // printer; this one green and that one red means the parser.
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32 = ti.primitive(TypeKind::I32);
    MirBuilder   b;
    b.addGlobal(i32, SymbolId{10}, UINT32_MAX, MirFuncId{}, kBinding,
                kVisibility, kIsConst, kStorage, kAlign);
    Mir m = std::move(b).finish();

    DiagnosticReporter emitRep;
    auto const         names = namesWithGlobalAt10();
    MirTextContext     ctx{&ti, &names};
    std::string const  text = emitMir(m, ctx, emitRep);

    EXPECT_NE(text.find("weak"), std::string::npos) << text;
    EXPECT_NE(text.find("hidden"), std::string::npos) << text;
    EXPECT_NE(text.find("const"), std::string::npos) << text;
    EXPECT_NE(text.find("threadlocal"), std::string::npos) << text;
    EXPECT_NE(text.find("align=64"), std::string::npos) << text;
}

TEST(MirTextGlobalAttrs, AnOrdinaryGlobalStillEmitsNoAttributeList) {
    // ★ THE BYTE-STABILITY GUARANTEE, and the reason the list is omitted when
    // every field is default: every `.dssir` text ever written for an ordinary
    // global must be unchanged by this row, or the change is a format break
    // rather than a fix.
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32 = ti.primitive(TypeKind::I32);
    MirBuilder   b;
    b.addGlobal(i32, SymbolId{10}, UINT32_MAX, MirFuncId{},
                SymbolBinding::Global, SymbolVisibility::Default,
                /*isConst=*/false, MirThreadStorage::Shared, /*align=*/0);
    Mir m = std::move(b).finish();

    DiagnosticReporter emitRep;
    auto const         names = namesWithGlobalAt10();
    MirTextContext     ctx{&ti, &names};
    std::string const  text = emitMir(m, ctx, emitRep);

    auto const at = text.find("global %10");
    ASSERT_NE(at, std::string::npos) << text;
    auto const eol = text.find('\n', at);
    std::string_view const line{text.data() + at, (eol == std::string::npos
                                                       ? text.size() : eol) - at};
    EXPECT_EQ(line.find('['), std::string_view::npos)
        << "an all-default global must print no attribute list at all: " << line;
}

TEST(MirTextGlobalAttrs, AForwardReferencedInitFuncGlobalKeepsItsAttributes) {
    // ★★ THE FOURTH `addGlobal` CALL SITE — the deferred one in `finalize()`.
    // A global whose `initfunc` names a function declared LATER in the text is
    // parked in `pendingInitFuncGlobals_` and built after the walk, so it is
    // the one path that would keep its attributes only if the PENDING RECORD
    // carries them. Every other case would still pass with that record left
    // alone, which makes this the case most likely to be missed.
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32    = ti.primitive(TypeKind::I32);
    TypeId const voidTy = ti.primitive(TypeKind::Void);
    TypeId const fnSig =
        ti.fnSig(std::span<TypeId const>{}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    MirFuncId const init = b.addFunction(fnSig, SymbolId{11});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    b.addReturn();
    b.addGlobal(i32, SymbolId{10}, UINT32_MAX, init, kBinding, kVisibility,
                kIsConst, kStorage, kAlign);
    Mir m = std::move(b).finish();

    std::vector<std::string> names(12, std::string{});
    names[10] = "g";
    names[11] = "ginit";
    DiagnosticReporter rep;
    auto rt = emitThenParse(m, ti, names, rep);
    ASSERT_TRUE(rt.result->ok) << rt.text << diagText(rep);
    ASSERT_EQ(rt.result->mir.moduleGlobalCount(), 1u) << rt.text;
    MirGlobalId const g = rt.result->mir.globalAt(0);
    Mir const&        p = rt.result->mir;

    EXPECT_NE(p.globalBinding(g), SymbolBinding::Global) << rt.text;
    EXPECT_NE(p.globalVisibility(g), SymbolVisibility::Default) << rt.text;
    EXPECT_TRUE(p.globalIsConst(g)) << rt.text;
    EXPECT_TRUE(p.globalIsThreadLocal(g)) << rt.text;
    EXPECT_EQ(p.globalAlignmentBytes(g), kAlign) << rt.text;
}

// ── fail-loud on malformed attribute text ─────────────────────────────────

namespace {

[[nodiscard]] std::string globalDoc(std::string_view attrs) {
    return std::string{"dssir 1\nmodule {\n  global %10 : i32"} +
           std::string{attrs} + " = zero\n}\n";
}

[[nodiscard]] bool sawMalformed(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::I_TextMalformed) return true;
    }
    return false;
}

} // namespace

TEST(MirTextGlobalAttrs, AnUnknownGlobalAttributeIsRefusedNotIgnored) {
    // A silently ignored attribute is exactly how these five fields went
    // missing for as long as they did, so an unknown name is an error.
    DiagnosticReporter rep;
    auto res = parseMir(globalDoc(" [nosuchattr]"), CompilationUnitId{1}, rep);
    EXPECT_FALSE(res->ok);
    EXPECT_TRUE(sawMalformed(rep));
}

TEST(MirTextGlobalAttrs, ANonPowerOfTwoAlignmentIsRefused) {
    DiagnosticReporter rep;
    auto res = parseMir(globalDoc(" [align=3]"), CompilationUnitId{1}, rep);
    EXPECT_FALSE(res->ok);
    EXPECT_TRUE(sawMalformed(rep));
}

TEST(MirTextGlobalAttrs, AnOverLargeAlignmentIsRefused) {
    // `MirBuilder::addGlobal` documents a ceiling of 256 and ✔MEASURED does NOT
    // enforce it — it stores whatever it is handed. The reader is the only
    // thing between a bad file and the assembler.
    DiagnosticReporter rep;
    auto res = parseMir(globalDoc(" [align=512]"), CompilationUnitId{1}, rep);
    EXPECT_FALSE(res->ok);
    EXPECT_TRUE(sawMalformed(rep));
}

TEST(MirTextGlobalAttrs, AWellFormedAttributeListParsesGreen) {
    // CONTROL for the three refusals above: byte-for-byte the same shape with
    // a legal list. Without it, a parser that refused every attribute list
    // would satisfy all three negatives and be useless.
    DiagnosticReporter rep;
    auto res = parseMir(globalDoc(" [weak, hidden, const, threadlocal, align=64]"),
                        CompilationUnitId{1}, rep);
    ASSERT_TRUE(res->ok);
    ASSERT_EQ(res->mir.moduleGlobalCount(), 1u);
    MirGlobalId const g = res->mir.globalAt(0);
    EXPECT_EQ(res->mir.globalBinding(g), SymbolBinding::Weak);
    EXPECT_EQ(res->mir.globalVisibility(g), SymbolVisibility::Hidden);
    EXPECT_TRUE(res->mir.globalIsConst(g));
    EXPECT_TRUE(res->mir.globalIsThreadLocal(g));
    EXPECT_EQ(res->mir.globalAlignmentBytes(g), 64u);
}
