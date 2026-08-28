// OPT11 — the PER-TU MODULE SUMMARY (plan 22 §0.2), unit-tested at the MIR
// tier with HAND-BUILT modules (no SemanticModel, no driver).
//
// A summary replaces a TU's MIR for the whole-program DECISION, so every pin
// here asks the same question in a different place: **is the fact still there
// after the body is gone?** A field that silently comes out wrong does not
// fail a build — it makes the index decide with a lie, which is the shape of
// the miscompiles this arc has to not have.
//
// STRICT pins:
//   * SummaryRecordsCallEdgesByName / …DistinguishesIndirectCalls — the call
//     graph survives, and an indirect call is not mistaken for an edge.
//   * SummaryCountsInstructionsForTheCostModel — the ONE integer that replaces
//     a body for `inlineLegalityGate` rule 6.
//   * SummaryCarriesLinkageAndDirectives — binding / visibility / noinline /
//     always_inline / no-optimize.
//   * SummaryDetectsFrameBoundBody / …ComputedGoto — the gate's rule-5 shape
//     refusals, pre-computed. RED if a refusal the gate makes goes unrecorded.
//   * SummaryEscapeSetSeesAnAddressTakenFunction + …DoesNotFlagAPureCallTarget
//     — the WHOLE-PROGRAM fact behind gate rule 4, and its complement, which
//     is the half that would silently forfeit every inline if it over-fired.
//   * SummaryRecordsGlobalInitSymbolRefs — the `scanLiveSymbols` phase-3 edge
//     set, INCLUDING a target nested in an aggregate literal.
//   * SummaryMarksInlineDefinition — C99 6.7.4p7.
//   * SummaryEncodeIsDeterministic / …RoundTrips — the determinism half of the
//     bar, at the wire format.
//   * SummaryDecodeRejects{Magic,Version,Truncation,TrailingBytes,Binding} —
//     fail-loud on every malformed input, because a misread summary is a
//     miscompile and guessing is never the safe option.

#include "core/types/extern_import.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/summary/mir_summary.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::mirsum;

namespace {

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

std::function<std::string(SymbolId)>
namerOf(std::unordered_map<std::uint32_t, std::string> table) {
    return [table = std::move(table)](SymbolId s) -> std::string {
        auto const it = table.find(s.v);
        return it == table.end() ? std::string{} : it->second;
    };
}

[[nodiscard]] SummaryFunction const*
findFn(ModuleSummary const& s, std::string const& name) {
    for (SummaryFunction const& f : s.functions) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

[[nodiscard]] bool hasName(std::vector<std::string> const& v,
                           std::string const& n) {
    return std::find(v.begin(), v.end(), n) != v.end();
}

// `int f(void) { return 7; }` + `int main(void) { return f(); }`, one module.
// SymbolId 50 = f, 100 = main.
Mir buildCallerCallee(TypeInterner& in) {
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const fnSig = in.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const fEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(fEntry);
    mb.addReturn(mb.addConst(i32Lit(7), i32));

    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(mEntry);
    MirInstId const calleeAddr = mb.addGlobalAddr(SymbolId{50}, fnSig);
    MirInstId const ops[]      = {calleeAddr};
    MirInstId const call       = mb.addInst(MirOpcode::Call, ops, i32);
    mb.addReturn(call);
    return std::move(mb).finish();
}

SummaryCuInput inputFor(Mir const& mir,
                        std::unordered_map<std::uint32_t, std::string> names,
                        std::span<ExternImport const> imports = {}) {
    SummaryCuInput cu;
    cu.mir            = &mir;
    cu.nameOf         = namerOf(std::move(names));
    cu.externImports  = imports;
    cu.moduleDigest   = "digest-A";
    cu.targetIdentity = "x86_64:elf64";
    return cu;
}

} // namespace

TEST(MirSummary, SummaryRecordsCallEdgesByName) {
    TypeInterner in{CompilationUnitId{1}};
    Mir const mir = buildCallerCallee(in);
    ModuleSummary const s =
        buildModuleSummary(inputFor(mir, {{50, "f"}, {100, "main"}}));

    ASSERT_EQ(s.functions.size(), 2u);
    SummaryFunction const* main = findFn(s, "main");
    ASSERT_NE(main, nullptr);
    ASSERT_EQ(main->calls.size(), 1u);
    // The edge is keyed on the DECLARED NAME — the same cross-CU key
    // `mergeCuMirs` uses. A symbol id here would be meaningless in another TU.
    EXPECT_EQ(main->calls[0].calleeName, "f");
    EXPECT_TRUE(main->calls[0].direct);

    SummaryFunction const* f = findFn(s, "f");
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(f->calls.empty());
    EXPECT_FALSE(f->hasIndirectCall);
}

TEST(MirSummary, SummaryDistinguishesIndirectCalls) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const fnSig = in.fnSig({}, i32, CallConv::CcSysV);
    TypeId const ptr   = in.pointer(i32);

    // `int main(int (*p)(void)) { return p(); }` — the callee operand is an
    // Arg, not a GlobalAddr, so there is NO call-graph edge to record.
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirInstId const p    = mb.addArg(0, ptr);
    MirInstId const ops[] = {p};
    MirInstId const call = mb.addInst(MirOpcode::Call, ops, i32);
    mb.addReturn(call);
    Mir const mir = std::move(mb).finish();

    ModuleSummary const s = buildModuleSummary(inputFor(mir, {{100, "main"}}));
    SummaryFunction const* main = findFn(s, "main");
    ASSERT_NE(main, nullptr);
    ASSERT_EQ(main->calls.size(), 1u);
    EXPECT_FALSE(main->calls[0].direct);
    EXPECT_TRUE(main->calls[0].calleeName.empty());
    // ★ The fact that makes the whole-program escape set load-bearing rather
    // than optional: this function's call graph is INCOMPLETE.
    EXPECT_TRUE(main->hasIndirectCall);
}

TEST(MirSummary, SummaryCountsInstructionsForTheCostModel) {
    TypeInterner in{CompilationUnitId{1}};
    Mir const mir = buildCallerCallee(in);
    ModuleSummary const s =
        buildModuleSummary(inputFor(mir, {{50, "f"}, {100, "main"}}));

    // `f` is Const + Return; `main` is GlobalAddr + Call + Return. The count is
    // the WHOLE input to `inlineLegalityGate` rule 6 — one integer standing in
    // for a body — so it is pinned exactly, not as a bound.
    EXPECT_EQ(findFn(s, "f")->instCount, 2u);
    EXPECT_EQ(findFn(s, "main")->instCount, 3u);
    EXPECT_EQ(findFn(s, "f")->blockCount, 1u);
}

TEST(MirSummary, SummaryCarriesLinkageAndDirectives) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const fnSig = in.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    // Weak + hidden + noinline + always_inline + no-optimize, all at once, so
    // a field that is dropped or crossed with its neighbour shows up here.
    mb.addFunction(fnSig, SymbolId{50}, SymbolBinding::Weak,
                   SymbolVisibility::Hidden, /*noInline=*/true,
                   /*alwaysInline=*/true, /*noOptimize=*/true);
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    mb.addReturn(mb.addConst(i32Lit(7), i32));
    Mir const mir = std::move(mb).finish();

    ModuleSummary const s = buildModuleSummary(inputFor(mir, {{50, "f"}}));
    SummaryFunction const* f = findFn(s, "f");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->binding, SymbolBinding::Weak);
    EXPECT_EQ(f->visibility, SymbolVisibility::Hidden);
    EXPECT_TRUE(f->noInline);
    EXPECT_TRUE(f->alwaysInline);
    EXPECT_TRUE(f->noOptimize);
}

TEST(MirSummary, SummaryDetectsFrameBoundBody) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const fnSig = in.fnSig({}, i32, CallConv::CcSysV);
    TypeId const ptr   = in.pointer(i32);

    // A va_start register-save-area leaf. `inlineLegalityGate` refuses this
    // callee because the leaf lowers to `lea reg, [sp + off]` against the
    // CALLEE's own variadic prologue; spliced, it reads the caller's frame.
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    (void)mb.addInst(MirOpcode::VaRegSaveAreaAddr, {}, ptr);
    mb.addReturn(mb.addConst(i32Lit(7), i32));
    Mir const mir = std::move(mb).finish();

    ModuleSummary const s = buildModuleSummary(inputFor(mir, {{50, "f"}}));
    SummaryFunction const* f = findFn(s, "f");
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(f->frameBound);
    EXPECT_FALSE(f->hasComputedGoto);
    EXPECT_FALSE(f->hasSeh);
    EXPECT_TRUE(f->hasReturn);
}

TEST(MirSummary, SummaryDetectsComputedGoto) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const fnSig = in.fnSig({}, i32, CallConv::CcSysV);
    TypeId const ptr   = in.pointer(i32);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const t = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(e);
    (void)mb.addBlockAddress(t, ptr);
    mb.addBr(t);
    mb.beginBlock(t);
    mb.addReturn(mb.addConst(i32Lit(7), i32));
    Mir const mir = std::move(mb).finish();

    ModuleSummary const s = buildModuleSummary(inputFor(mir, {{50, "f"}}));
    EXPECT_TRUE(findFn(s, "f")->hasComputedGoto);
}

TEST(MirSummary, SummaryEscapeSetSeesAnAddressTakenFunction) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const fnSig = in.fnSig({}, i32, CallConv::CcSysV);
    TypeId const ptr   = in.pointer(i32);

    // `main` passes `&f` as an ARGUMENT of a call to `g` — a passed function
    // pointer, which is an escape, NOT a call target.
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const fe = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(fe);
    mb.addReturn(mb.addConst(i32Lit(7), i32));

    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const me = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(me);
    MirInstId const gAddr = mb.addGlobalAddr(SymbolId{60}, fnSig);
    MirInstId const fAddr = mb.addGlobalAddr(SymbolId{50}, ptr);
    MirInstId const ops[] = {gAddr, fAddr};
    MirInstId const call  = mb.addInst(MirOpcode::Call, ops, i32);
    mb.addReturn(call);
    Mir const mir = std::move(mb).finish();

    ModuleSummary const s = buildModuleSummary(
        inputFor(mir, {{50, "f"}, {60, "g"}, {100, "main"}}));

    // ★★★ THE WHOLE-PROGRAM FACT. Without it a TU that only CALLS `f` would
    // inline it and its out-of-line body could then be dropped, while the
    // indirect call through this pointer still reaches for it.
    EXPECT_TRUE(hasName(s.escapedSymbolNames, "f"));
    // ★ AND ITS COMPLEMENT, which is the half that matters for quality: `g`
    // appears ONLY as operand[0] of the Call — a pure call target. If the scan
    // flagged it, every ordinary callee in the program would become
    // un-inlinable and the 2922-splice bar would be unreachable.
    EXPECT_FALSE(hasName(s.escapedSymbolNames, "g"));
}

TEST(MirSummary, SummaryRecordsGlobalInitSymbolRefs) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const fnSig = in.fnSig({}, i32, CallConv::CcSysV);
    TypeId const ptr   = in.pointer(i32);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{50});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    mb.addReturn(mb.addConst(i32Lit(7), i32));

    // A function-pointer TABLE: `void* tbl[] = { &f };` — the symbol-address
    // leaf is NESTED INSIDE an aggregate literal. `scanLiveSymbols` phase 3
    // exists because a function reachable only through this data relocation is
    // otherwise wrongly deleted, and the aggregate arm exists because a
    // scalar-only scan misses exactly this shape.
    MirLiteralValue leaf;
    leaf.value = MirSymbolAddrValue{/*symbol=*/50, /*addend=*/0};
    MirAggregateValue agg;
    agg.fields.push_back(leaf);
    MirLiteralValue table;
    table.value = agg;
    table.core  = TypeKind::Array;
    std::uint32_t const idx = mb.literalPoolAdd(table);
    mb.addGlobal(ptr, SymbolId{200}, idx, MirFuncId{}, SymbolBinding::Global,
                 SymbolVisibility::Default, /*isConst=*/true,
                 MirThreadStorage::Shared, /*alignment=*/0);
    Mir const mir = std::move(mb).finish();

    ModuleSummary const s =
        buildModuleSummary(inputFor(mir, {{50, "f"}, {200, "tbl"}}));
    ASSERT_EQ(s.globals.size(), 1u);
    EXPECT_EQ(s.globals[0].name, "tbl");
    EXPECT_TRUE(s.globals[0].isConst);
    EXPECT_TRUE(hasName(s.globals[0].initSymbolRefs, "f"));
}

TEST(MirSummary, SummaryMarksInlineDefinition) {
    TypeInterner in{CompilationUnitId{1}};
    Mir const mir = buildCallerCallee(in);

    // C99 6.7.4p7: `f` is DEFINED here and also declared by an import row, so
    // this definition is an inline definition — importable, but not the owner
    // of the out-of-line symbol.
    ExternImport row;
    row.symbol      = SymbolId{50};
    row.mangledName = "f";
    row.libraryPath = "libc.so.6";
    std::vector<ExternImport> const imports{row};

    ModuleSummary const s =
        buildModuleSummary(inputFor(mir, {{50, "f"}, {100, "main"}}, imports));
    EXPECT_TRUE(findFn(s, "f")->isInlineDefinition);
    EXPECT_FALSE(findFn(s, "main")->isInlineDefinition);
    ASSERT_EQ(s.imports.size(), 1u);
    // ⚠ Import identity is the TRIPLE, never the name alone — and this must be
    // byte-for-byte `mergeCuMirs::ffiImportKey`, or the index and the merge
    // would disagree about what "one import" means.
    EXPECT_EQ(summaryImportKey(s.imports[0]), summaryImportKey(row));
}

TEST(MirSummary, SummaryEncodeIsDeterministic) {
    TypeInterner in{CompilationUnitId{1}};
    Mir const mir = buildCallerCallee(in);
    auto const cu = inputFor(mir, {{50, "f"}, {100, "main"}});

    // Half the determinism bar for this arc, at the wire format: same input,
    // byte-identical output, twice, from two independently-built summaries.
    std::vector<std::uint8_t> const a = encodeModuleSummary(buildModuleSummary(cu));
    std::vector<std::uint8_t> const b = encodeModuleSummary(buildModuleSummary(cu));
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a.empty());
}

TEST(MirSummary, SummaryRoundTrips) {
    TypeInterner in{CompilationUnitId{1}};
    Mir const mir = buildCallerCallee(in);
    ExternImport row;
    row.symbol      = SymbolId{300};
    row.mangledName = "puts";
    row.libraryPath = "libc.so.6";
    row.version     = "GLIBC_2.2.5";
    std::vector<ExternImport> const imports{row};

    ModuleSummary const s = buildModuleSummary(
        inputFor(mir, {{50, "f"}, {100, "main"}, {300, "puts"}}, imports));
    std::vector<std::uint8_t> const bytes = encodeModuleSummary(s);
    std::optional<ModuleSummary> const back = decodeModuleSummary(bytes);
    ASSERT_TRUE(back.has_value());

    // Re-encoding the DECODED summary must reproduce the same bytes. That is
    // stronger than field-by-field equality: it also catches a field the
    // decoder silently drops, because a dropped field would re-encode to a
    // different image.
    EXPECT_EQ(encodeModuleSummary(*back), bytes);
    EXPECT_EQ(back->moduleDigest, "digest-A");
    EXPECT_EQ(back->targetIdentity, "x86_64:elf64");
    ASSERT_EQ(back->imports.size(), 1u);
    EXPECT_EQ(back->imports[0].version, "GLIBC_2.2.5");
    ASSERT_NE(findFn(*back, "main"), nullptr);
    ASSERT_EQ(findFn(*back, "main")->calls.size(), 1u);
    EXPECT_EQ(findFn(*back, "main")->calls[0].calleeName, "f");
}

TEST(MirSummary, SummaryDecodeRejectsMalformedInput) {
    TypeInterner in{CompilationUnitId{1}};
    Mir const mir = buildCallerCallee(in);
    std::vector<std::uint8_t> const good = encodeModuleSummary(
        buildModuleSummary(inputFor(mir, {{50, "f"}, {100, "main"}})));
    ASSERT_TRUE(decodeModuleSummary(good).has_value());

    // Every arm below is FAIL-LOUD by design: a summary drives codegen
    // decisions, so a reader that guesses at a field it did not understand
    // produces wrong bytes rather than an error.
    EXPECT_FALSE(decodeModuleSummary({}).has_value()) << "empty buffer";

    {   // bad magic
        std::vector<std::uint8_t> b = good;
        b[0] ^= 0xFFu;
        EXPECT_FALSE(decodeModuleSummary(b).has_value());
    }
    {   // unknown version — a future writer's format must not be half-read
        std::vector<std::uint8_t> b = good;
        b[7] = static_cast<std::uint8_t>(b[7] + 1u);
        EXPECT_FALSE(decodeModuleSummary(b).has_value());
    }
    {   // truncation, at every length
        for (std::size_t cut = 1; cut < good.size(); ++cut) {
            std::vector<std::uint8_t> const b(good.begin(),
                                              good.begin() + static_cast<long>(cut));
            EXPECT_FALSE(decodeModuleSummary(b).has_value())
                << "truncated to " << cut << " bytes decoded successfully";
        }
    }
    {   // trailing bytes — reader and writer disagree about the layout
        std::vector<std::uint8_t> b = good;
        b.push_back(0);
        EXPECT_FALSE(decodeModuleSummary(b).has_value());
    }
}

TEST(MirSummary, SummaryDecodeRejectsOutOfRangeBinding) {
    TypeInterner in{CompilationUnitId{1}};
    Mir const mir = buildCallerCallee(in);
    std::vector<std::uint8_t> b = encodeModuleSummary(
        buildModuleSummary(inputFor(mir, {{50, "f"}, {100, "main"}})));

    // Find the first function row's binding byte: magic(7) + version(4) +
    // digest + target + funcCount(4) + nameLen(4) + name + symbol(4).
    std::size_t at = 7 + 4;
    auto skipStr = [&] {
        std::uint32_t const n = static_cast<std::uint32_t>(b[at])
            | (static_cast<std::uint32_t>(b[at + 1]) << 8)
            | (static_cast<std::uint32_t>(b[at + 2]) << 16)
            | (static_cast<std::uint32_t>(b[at + 3]) << 24);
        at += 4 + n;
    };
    skipStr();            // moduleDigest
    skipStr();            // targetIdentity
    at += 4;              // function count
    skipStr();            // functions[0].name
    at += 4;              // functions[0].symbol
    ASSERT_LT(at, b.size());

    // ★ A raw cast of this byte would mint a `SymbolBinding` no switch handles
    // — and it feeds cross-TU symbol RESOLUTION, so a garbage value is a
    // link-time miscompile, not a display bug.
    b[at] = 9;
    EXPECT_FALSE(decodeModuleSummary(b).has_value());
}
