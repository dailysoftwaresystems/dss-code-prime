// A `__try` FILTER WHOSE EXPRESSION HAS A BRANCH.
//
// `__except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION && g == 1)` and
// `__except (c ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)` — the
// shape the vendor's documentation writes a filter in — are filters of more than
// one basic block: `&&`, `||` and `?:` evaluate an operand on one path only, and
// the lowering emits a diamond joined by a `Phi` for them wherever they stand.
// The region's skeleton used to say "the filter is ONE block ending in
// `SehFilterReturn`", in the MIR verifier and in the pass that makes a function of
// the filter, and every such program was refused (`error[I_SehStructure]: … filter
// block #3 must terminate in SehFilterReturn`), at baseline and in release.
//
// What a filter is now has ONE statement, `readSehFilterRegion`
// (mir/seh_filter_region.hpp): a sub-graph with one way in and one way out. This
// binary pins the statement itself, the verifier that holds every module to it,
// and `synthesizeSehFunclets`, which copies the whole sub-graph into the funclet
// and leaves the parent exactly one block in its place.
//
// A binary of its own: its cases are about one construct, and the two binaries
// they would otherwise join (the verifier's and the merge tier's) are each the
// claim runner of many mutation arms, whose case counts a new case moves.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/object_format_kind.hpp"   // SehPersonality, RuntimeLibraryRole
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"   // TypeKind, CallConv
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/merge/synth_seh_funclets.hpp"
#include "mir/mir.hpp"
#include "mir/mir_dom.hpp"              // mirBuildPredecessors
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"   // rederiveStructCfMarkers
#include "mir/mir_verifier.hpp"
#include "mir/seh_filter_region.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace dss;

namespace {

constexpr std::uint32_t kParentSym = 100;

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

[[nodiscard]] std::optional<MirFuncId> findFuncBySymbol(Mir const& mir, SymbolId sym) {
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v == sym.v) return f;
    }
    return std::nullopt;
}

[[nodiscard]] std::string allDiagText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) { out += '['; out += d.actual; out += ']'; }
    return out;
}

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) {
        if (d.code == code) ++n;
    }
    return n;
}

SehPersonality peSehPersonality() {
    SehPersonality p;
    p.role        = RuntimeLibraryRole::UnwindPersonality;
    p.libraryPath = "ucrtbase.dll";
    p.mangledName = "__C_specific_handler";
    return p;
}

// How many instructions of `op` function `fn` holds.
[[nodiscard]] std::size_t countIn(Mir const& mir, MirFuncId fn, MirOpcode op) {
    std::size_t n = 0;
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(fn); ++bi) {
        MirBlockId const b = mir.funcBlockAt(fn, bi);
        for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
            if (mir.instOpcode(mir.blockInstAt(b, ii)) == op) ++n;
        }
    }
    return n;
}

// The first instruction of `op` in block `b`, if it holds one.
[[nodiscard]] std::optional<MirInstId> firstIn(Mir const& mir, MirBlockId b, MirOpcode op) {
    for (std::uint32_t ii = 0; ii < mir.blockInstCount(b); ++ii) {
        MirInstId const id = mir.blockInstAt(b, ii);
        if (mir.instOpcode(id) == op) return id;
    }
    return std::nullopt;
}

[[nodiscard]] bool isBlockOf(Mir const& mir, MirFuncId fn, MirBlockId b) {
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(fn); ++bi) {
        if (mir.funcBlockAt(fn, bi).v == b.v) return true;
    }
    return false;
}

[[nodiscard]] std::int64_t constValue(Mir const& mir, MirInstId c) {
    return std::get<std::int64_t>(mir.literalValue(mir.constLiteralIndex(c)).value);
}

// ── The fixture ─────────────────────────────────────────────────────────────
//
// The parent every case starts from, as the C lowering emits a guarded region:
//   entry   : slot = alloca ; want = alloca ; sum = 3 + 4 ;
//             SehTryBegin(0) → [body, filter entry]
//   body    : load slot ; SehTryEnd(0) ; Br(join)
//   filter  : whatever the case writes, from the open filter entry on
//   handler : Const 1000 (a tag) ; Br(join)
//   join    : return 0
// The blocks a filter adds are created AFTER these five, as the lowering creates
// them (the four blocks of a region exist before its filter is lowered).
struct Ctx {
    MirBuilder&   mb;
    TypeInterner& in;
    TypeId        i32, u32, boolTy, pI32;
    MirBlockId    body, filterEntry, handler, join;
    MirInstId     slot;   // the parent local the guarded body loads
    MirInstId     want;   // a second parent local
    MirInstId     sum;    // 3 + 4, computed before the region
    std::uint32_t region = 0;

    [[nodiscard]] MirBlockId block() const { return mb.createBlock(StructCfMarker::Linear); }
    [[nodiscard]] MirInstId constant(std::int64_t v) const {
        return mb.addConst(i32Lit(v), i32);
    }
    // `GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION`, in the open block.
    [[nodiscard]] MirInstId isAccessViolation() const {
        MirInstId const code = mb.addInst(MirOpcode::SehExceptionCode, {}, u32);
        MirLiteralValue av;
        av.value = std::int64_t{0xC0000005};
        av.core  = TypeKind::U32;
        MirInstId const avc = mb.addConst(std::move(av), u32);
        return mb.addInst(MirOpcode::ICmpEq, std::array<MirInstId, 2>{code, avc}, boolTy);
    }
    [[nodiscard]] MirInstId loadWant() const {
        return mb.addInst(MirOpcode::Load, std::array<MirInstId, 1>{want}, i32);
    }
    [[nodiscard]] MirInstId phi(MirInstId a, MirBlockId fromA, MirInstId b,
                                MirBlockId fromB) const {
        std::array<MirPhiIncoming, 2> const inputs{
            MirPhiIncoming{a, fromA}, MirPhiIncoming{b, fromB}};
        return mb.addPhi(i32, inputs);
    }
    void answer(MirInstId v) const { mb.addSehFilterReturn(v, handler, region); }
};

struct Parent {
    Mir        mir;
    MirBlockId body{}, filterEntry{}, handler{};
};

// `bodyAlsoBranchesTo`, when the case sets it, is a block the GUARDED BODY
// branches to as well — the one malformed shape that needs the body's help.
template <typename EmitFilter>
[[nodiscard]] Parent buildParent(TypeInterner& in, EmitFilter emitFilter,
                                 bool bodyBranchesToAFilterBlock = false) {
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const u32    = in.primitive(TypeKind::U32);
    TypeId const boolTy = in.primitive(TypeKind::Bool);
    TypeId const pI32   = in.pointer(i32);
    MirBuilder mb;
    mb.stateSelfContainedSymbolIds();   // a hand-built module is its own table
    mb.addFunction(in.fnSig({}, i32, CallConv::CcMS64), SymbolId{kParentSym});
    MirBlockId const entry   = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const body    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const filter  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const handler = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const join    = mb.createBlock(StructCfMarker::Linear);
    // The block a malformed case shares between the body and the filter.
    MirBlockId const shared  = bodyBranchesToAFilterBlock
        ? mb.createBlock(StructCfMarker::Linear) : MirBlockId{};

    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, pI32, 4);
    MirInstId const want = mb.addInst(MirOpcode::Alloca, {}, pI32, 4);
    MirInstId const k3   = mb.addConst(i32Lit(3), i32);
    MirInstId const k4   = mb.addConst(i32Lit(4), i32);
    MirInstId const sum  = mb.addInst(MirOpcode::Add, std::array<MirInstId, 2>{k3, k4}, i32);
    mb.addSehTryBegin(body, filter, 0);

    mb.beginBlock(body);
    MirInstId const v = mb.addInst(MirOpcode::Load, std::array<MirInstId, 1>{slot}, i32);
    mb.addInst(MirOpcode::SehTryEnd, {}, InvalidType, 0);
    if (bodyBranchesToAFilterBlock) {
        MirInstId const zero = mb.addConst(i32Lit(0), i32);
        MirInstId const cnd  = mb.addInst(MirOpcode::ICmpNe,
                                          std::array<MirInstId, 2>{v, zero}, boolTy);
        mb.addCondBr(cnd, shared, join);
    } else {
        mb.addBr(join);
    }

    Ctx c{mb, in, i32, u32, boolTy, pI32, body, filter, handler, join, slot, want, sum};
    mb.beginBlock(filter);
    emitFilter(c, shared);

    mb.beginBlock(handler);
    (void)mb.addConst(i32Lit(1000), i32);
    mb.addBr(join);
    mb.beginBlock(join);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir mir = std::move(mb).finish();
    // The blocks are created `Linear`; a module's stored markers are the derived
    // ones (the verifier compares them), so derive them as every producer does.
    rederiveStructCfMarkers(mir);
    return Parent{std::move(mir), body, filter, handler};
}

// `code == AV && want == 1`:
//   entry : isAv ; no = 0 ; CondBr(isAv, rhs, merge)
//   rhs   : eq = (load want == 1) ; Br(merge)
//   merge : Phi[(no, entry), (eq, rhs)] ; SehFilterReturn
void shortCircuit(Ctx& c, MirBlockId) {
    MirBlockId const rhs = c.block(), merge = c.block();
    MirInstId const isAv = c.isAccessViolation();
    MirInstId const no   = c.constant(0);
    c.mb.addCondBr(isAv, rhs, merge);
    c.mb.beginBlock(rhs);
    MirInstId const eq = c.mb.addInst(
        MirOpcode::ICmpEq, std::array<MirInstId, 2>{c.loadWant(), c.constant(1)}, c.i32);
    c.mb.addBr(merge);
    c.mb.beginBlock(merge);
    c.answer(c.phi(no, c.filterEntry, eq, rhs));
}

// `code == AV ? 1 : 0`:
//   entry : isAv ; CondBr(isAv, yes, no)
//   yes   : 1 ; Br(merge)        no : 0 ; Br(merge)
//   merge : Phi[(1, yes), (0, no)] ; SehFilterReturn
void conditional(Ctx& c, MirBlockId) {
    MirBlockId const yes = c.block(), no = c.block(), merge = c.block();
    c.mb.addCondBr(c.isAccessViolation(), yes, no);
    c.mb.beginBlock(yes);
    MirInstId const one = c.constant(1);
    c.mb.addBr(merge);
    c.mb.beginBlock(no);
    MirInstId const zero = c.constant(0);
    c.mb.addBr(merge);
    c.mb.beginBlock(merge);
    c.answer(c.phi(one, yes, zero, no));
}

// `(code == AV ? want : want) == sum` — BOTH arms read one parent local, and the
// block after them names a value the parent computed before the region:
//   entry : isAv ; CondBr(isAv, yes, no)
//   yes   : a = load want ; Br(merge)      no : b = load want ; Br(merge)
//   merge : Phi[(a, yes), (b, no)] == sum ; SehFilterReturn
void bothArmsReadALocal(Ctx& c, MirBlockId) {
    MirBlockId const yes = c.block(), no = c.block(), merge = c.block();
    c.mb.addCondBr(c.isAccessViolation(), yes, no);
    c.mb.beginBlock(yes);
    MirInstId const a = c.loadWant();
    c.mb.addBr(merge);
    c.mb.beginBlock(no);
    MirInstId const b = c.loadWant();
    c.mb.addBr(merge);
    c.mb.beginBlock(merge);
    MirInstId const picked = c.phi(a, yes, b, no);
    c.answer(c.mb.addInst(MirOpcode::ICmpEq,
                          std::array<MirInstId, 2>{picked, c.sum}, c.i32));
}

struct Synthesized {
    bool                      ok = false;
    std::vector<MirSehScope>  scopes;
    std::string               text;
};

[[nodiscard]] Synthesized synthesize(Mir& mir, TypeInterner& in) {
    Synthesized s;
    std::vector<ExternImport> ext;
    DiagnosticReporter        rep;
    s.ok = synthesizeSehFunclets(mir, in, ext, peSehPersonality(),
                                 CSymbolDecorationScheme::None,
                                 "pe64-x86_64-windows-exec", s.scopes, rep);
    s.text = allDiagText(rep);
    return s;
}

// Builds the module, runs the pass and LEAVES THE PROCESS: 0 when the pass
// finished and accepted the module, 3 when it refused it. A pass that ABORTS
// leaves a third way, which is what the caller's `EXPECT_EXIT` reads — so every
// case that runs the pass runs it in a child FIRST, and a pass that dies on the
// case's module fails that case by name instead of taking the binary, and every
// other case's verdict, with it.
template <typename Build>
[[noreturn]] void synthesizeAndLeave(Build build) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = build(in);
    std::exit(synthesize(mir, in).ok ? 0 : 3);
}

// The module of one region whose filter `emitFilter` writes, as a child builds it.
template <typename EmitFilter>
[[nodiscard]] auto moduleOf(EmitFilter emitFilter, bool bodyBranchesToAFilterBlock = false) {
    return [emitFilter, bodyBranchesToAFilterBlock](TypeInterner& in) {
        return buildParent(in, emitFilter, bodyBranchesToAFilterBlock).mir;
    };
}

constexpr char const* kThePassDidNotFinish =
    "the pass did not finish on this module: a child killed by an abort is the "
    "pass meeting a block, an operand or a Phi input it left behind";

// The parent's ONE block in the filter's place: `[Const ; SehFilterReturn]`, its
// successor the handler (the block tagged `Const 1000`). Reports what differs.
void expectTheParentKeepsOneStub(Mir const& mir, MirFuncId parent) {
    std::uint32_t stubs = 0;
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(parent); ++bi) {
        MirBlockId const b = mir.funcBlockAt(parent, bi);
        if (mir.instOpcode(mir.blockTerminator(b)) != MirOpcode::SehFilterReturn) continue;
        ++stubs;
        EXPECT_EQ(mir.blockInstCount(b), 2u) << "the stub is a constant and the marker";
        EXPECT_EQ(mir.instOpcode(mir.blockInstAt(b, 0)), MirOpcode::Const);
        auto const succs = mir.blockSuccessors(b);
        ASSERT_EQ(succs.size(), 1u);
        auto const tag = firstIn(mir, succs[0], MirOpcode::Const);
        ASSERT_TRUE(tag.has_value()) << "the stub's successor is the handler";
        EXPECT_EQ(constValue(mir, *tag), 1000);
    }
    EXPECT_EQ(stubs, 1u) << "one block of the parent stands where the filter stood";
    // Nothing of the filter is left in the parent: its reads, its branch, its join.
    EXPECT_EQ(countIn(mir, parent, MirOpcode::SehExceptionCode), 0u);
    EXPECT_EQ(countIn(mir, parent, MirOpcode::Phi), 0u);
    EXPECT_EQ(countIn(mir, parent, MirOpcode::CondBr), 0u);
    EXPECT_EQ(countIn(mir, parent, MirOpcode::Load), 1u)
        << "the guarded body's load is the parent's only one";
}

} // namespace

// ═══ THE STATEMENT ════════════════════════════════════════════════════════════

// The filter's blocks come back entry first and exit last, a block after every
// block that must run before it — the order a pass copies them in.
TEST(SehFilterRegion, ADiamondIsReadEntryFirstAndExitLast) {
    TypeInterner in{CompilationUnitId{1}};
    Parent const p = buildParent(in, conditional);
    auto const preds = mirBuildPredecessors(p.mir);
    SehFilterRegion const f = readSehFilterRegion(p.mir, preds, p.filterEntry, 0);
    ASSERT_TRUE(f.ok()) << f.problem;
    ASSERT_EQ(f.blocks.size(), 4u) << "the entry, the two arms and the join";
    EXPECT_EQ(f.blocks.front().v, p.filterEntry.v);
    EXPECT_EQ(f.blocks.back().v, f.exit.v);
    EXPECT_EQ(p.mir.instOpcode(p.mir.blockTerminator(f.exit)), MirOpcode::SehFilterReturn);
    EXPECT_EQ(f.handler.v, p.handler.v);
    EXPECT_EQ(f.entry.v, p.filterEntry.v);
}

// A filter of one block is the same statement's smallest case.
TEST(SehFilterRegion, AOneBlockFilterIsItsOwnEntryAndExit) {
    TypeInterner in{CompilationUnitId{1}};
    Parent const p = buildParent(in, [](Ctx& c, MirBlockId) { c.answer(c.constant(1)); });
    auto const preds = mirBuildPredecessors(p.mir);
    SehFilterRegion const f = readSehFilterRegion(p.mir, preds, p.filterEntry, 0);
    ASSERT_TRUE(f.ok()) << f.problem;
    ASSERT_EQ(f.blocks.size(), 1u);
    EXPECT_EQ(f.exit.v, p.filterEntry.v);
    EXPECT_EQ(f.handler.v, p.handler.v);
}

// ═══ THE VERIFIER ═════════════════════════════════════════════════════════════

// The three shapes verify as the lowering emits them. Before the statement existed
// each was refused: "filter block #N must terminate in SehFilterReturn".
TEST(SehFilterVerifier, AFilterWithABranchVerifies) {
    struct Case {
        char const* what;
        void (*emit)(Ctx&, MirBlockId);
    };
    constexpr std::array<Case, 3> cases{{
        {"a && b", &shortCircuit},
        {"c ? 1 : 0", &conditional},
        {"both arms read one parent local", &bothArmsReadALocal},
    }};
    for (Case const& k : cases) {
        SCOPED_TRACE(k.what);
        TypeInterner in{CompilationUnitId{1}};
        Parent const p = buildParent(in, k.emit);
        DiagnosticReporter rep;
        EXPECT_TRUE((MirVerifier{p.mir, &in}.verify(rep))) << allDiagText(rep);
        EXPECT_EQ(countCode(rep, DiagnosticCode::I_SehStructure), 0u) << allDiagText(rep);
    }
}

namespace {

// Each malformed filter, and the sentence that names what it breaks.

// A block of the filter returns from the FUNCTION.
void interiorBlockReturns(Ctx& c, MirBlockId) {
    MirBlockId const out = c.block(), merge = c.block();
    c.mb.addCondBr(c.isAccessViolation(), out, merge);
    c.mb.beginBlock(out);
    c.mb.addReturn(c.constant(7));
    c.mb.beginBlock(merge);
    c.answer(c.constant(1));
}

// Both arms answer: two blocks end in the region's SehFilterReturn.
void twoExits(Ctx& c, MirBlockId) {
    MirBlockId const yes = c.block(), no = c.block();
    c.mb.addCondBr(c.isAccessViolation(), yes, no);
    c.mb.beginBlock(yes);
    c.answer(c.constant(1));
    c.mb.beginBlock(no);
    c.answer(c.constant(0));
}

// The join answers for region 7; the filter is region 0's.
void answersForAnotherRegion(Ctx& c, MirBlockId) {
    MirBlockId const merge = c.block();
    c.mb.addBr(merge);
    c.mb.beginBlock(merge);
    c.mb.addSehFilterReturn(c.constant(1), c.handler, /*region=*/7);
}

// A block of the filter is ALSO branched to by the guarded body.
void sharesABlockWithTheBody(Ctx& c, MirBlockId shared) {
    MirBlockId const merge = c.block();
    c.mb.addCondBr(c.isAccessViolation(), shared, merge);
    c.mb.beginBlock(shared);
    c.mb.addBr(merge);
    c.mb.beginBlock(merge);
    c.answer(c.constant(1));
}

// The exit names a block of the filter as its handler.
void handlerIsAFilterBlock(Ctx& c, MirBlockId) {
    MirBlockId const side = c.block(), exit = c.block();
    c.mb.addCondBr(c.isAccessViolation(), side, exit);
    c.mb.beginBlock(side);
    c.mb.addBr(exit);
    c.mb.beginBlock(exit);
    c.mb.addSehFilterReturn(c.constant(1), side, c.region);
}

// No path of the filter answers.
void neverAnswers(Ctx& c, MirBlockId) {
    MirBlockId const yes = c.block(), no = c.block();
    c.mb.addCondBr(c.isAccessViolation(), yes, no);
    c.mb.beginBlock(yes);
    c.mb.addUnreachable();
    c.mb.beginBlock(no);
    c.mb.addUnreachable();
}

// A block of the filter branches back to its entry.
void branchesBackToTheEntry(Ctx& c, MirBlockId) {
    MirBlockId const again = c.block(), exit = c.block();
    c.mb.addCondBr(c.isAccessViolation(), again, exit);
    c.mb.beginBlock(again);
    c.mb.addBr(c.filterEntry);
    c.mb.beginBlock(exit);
    c.answer(c.constant(1));
}

struct Malformed {
    char const*      what;
    void           (*emit)(Ctx&, MirBlockId);
    bool             bodyBranchesToAFilterBlock;
    std::string_view sentence;
};

constexpr std::array<Malformed, 6> kMalformed{{
    {"a block of the filter returns from the function", &interiorBlockReturns, false,
     "a filter is left only by returning its value"},
    {"two blocks answer", &twoExits, false,
     "both return the filter's value"},
    {"the join answers for another region", &answersForAnotherRegion, false,
     "returns the filter value of region 7"},
    {"the guarded body branches to a block of the filter", &sharesABlockWithTheBody, true,
     "a filter is entered at its entry and nowhere else"},
    {"the exit names a block of the filter as the handler", &handlerIsAFilterBlock, false,
     "is also a block of the filter"},
    {"no path of the filter answers", &neverAnswers, false,
     "the filter never returns a value"},
}};

} // namespace

// Each is refused under the structure code, in a sentence that names its rule.
TEST(SehFilterVerifier, AFilterWithAnotherWayInOrOutIsRefusedByItsRule) {
    for (Malformed const& m : kMalformed) {
        SCOPED_TRACE(m.what);
        TypeInterner in{CompilationUnitId{1}};
        Parent const p = buildParent(in, m.emit, m.bodyBranchesToAFilterBlock);
        DiagnosticReporter rep;
        EXPECT_FALSE((MirVerifier{p.mir, &in}.verify(rep)));
        std::string const text = allDiagText(rep);
        EXPECT_GE(countCode(rep, DiagnosticCode::I_SehStructure), 1u) << text;
        EXPECT_NE(text.find(m.sentence), std::string::npos) << text;
    }
}

// The pass reads the same statement, so it refuses the same shapes by the same
// sentences — and hands out no record for a module it refused.
TEST(SehFilterFunclet, TheSynthesizerRefusesWhatTheStatementRefuses) {
    for (Malformed const& m : kMalformed) {
        SCOPED_TRACE(m.what);
        EXPECT_EXIT(synthesizeAndLeave(moduleOf(m.emit, m.bodyBranchesToAFilterBlock)),
                    ::testing::ExitedWithCode(3), "")
            << "the pass neither refused this module nor finished: a shape the "
               "statement refuses reached the copy";
        if (HasFailure()) continue;
        TypeInterner in{CompilationUnitId{1}};
        Parent p = buildParent(in, m.emit, m.bodyBranchesToAFilterBlock);
        Synthesized const s = synthesize(p.mir, in);
        EXPECT_FALSE(s.ok);
        EXPECT_NE(s.text.find(m.sentence), std::string::npos) << s.text;
        EXPECT_TRUE(s.scopes.empty()) << "a refused module must hand out no record";
    }
    // The verifier meets a branch back to the entry as a second predecessor of the
    // entry, under its own older sentence; the pass meets it here.
    EXPECT_EXIT(synthesizeAndLeave(moduleOf(&branchesBackToTheEntry)),
                ::testing::ExitedWithCode(3), "")
        << "the pass neither refused a filter that branches back to its entry nor "
           "finished";
    if (HasFailure()) return;
    TypeInterner in{CompilationUnitId{1}};
    Parent p = buildParent(in, branchesBackToTheEntry);
    Synthesized const s = synthesize(p.mir, in);
    EXPECT_FALSE(s.ok);
    EXPECT_NE(s.text.find("branches back to the filter's entry"), std::string::npos)
        << s.text;
}

// ═══ THE FUNCLET ══════════════════════════════════════════════════════════════

// `a && b`: the funclet is the three blocks, the branch goes to the right operand
// when the left one holds, the join's Phi takes one input from each way in, and the
// parent keeps one stub.
TEST(SehFilterFunclet, AShortCircuitFilterBecomesAFuncletOfItsBlocks) {
    EXPECT_EXIT(synthesizeAndLeave(moduleOf(&shortCircuit)),
                ::testing::ExitedWithCode(0), "") << kThePassDidNotFinish;
    if (HasFailure()) return;

    TypeInterner in{CompilationUnitId{1}};
    Parent p = buildParent(in, shortCircuit);
    Mir& mir = p.mir;
    Synthesized const s = synthesize(mir, in);
    ASSERT_TRUE(s.ok) << s.text;
    ASSERT_EQ(s.scopes.size(), 1u);

    auto const funclet = findFuncBySymbol(mir, s.scopes[0].filterFuncletSymbol);
    ASSERT_TRUE(funclet.has_value());
    ASSERT_EQ(mir.funcBlockCount(*funclet), 3u) << "the entry, the right operand, the join";
    MirBlockId const entry = mir.funcBlockAt(*funclet, 0);
    ASSERT_EQ(mir.instOpcode(mir.blockTerminator(entry)), MirOpcode::CondBr);
    auto const targets = mir.blockSuccessors(entry);
    ASSERT_EQ(targets.size(), 2u);
    MirBlockId const rhs = targets[0], merge = targets[1];
    EXPECT_TRUE(firstIn(mir, rhs, MirOpcode::Load).has_value())
        << "the TRUE target is the right operand: it runs when the left one holds";
    ASSERT_EQ(mir.instOpcode(mir.blockTerminator(rhs)), MirOpcode::Br);
    EXPECT_EQ(mir.blockSuccessors(rhs)[0].v, merge.v);

    // The join: a Phi of two inputs, each a value of the funclet arriving from a
    // block of the funclet — a constant from the entry, the comparison from the
    // right operand.
    MirInstId const phi = mir.blockInstAt(merge, 0);
    ASSERT_EQ(mir.instOpcode(phi), MirOpcode::Phi);
    auto const inputs = mir.phiIncomings(phi);
    ASSERT_EQ(inputs.size(), 2u) << "the Phi keeps one input per way into the join";
    std::uint32_t fromEntry = 0, fromRhs = 0;
    for (MirPhiIncoming const& inc : inputs) {
        EXPECT_TRUE(isBlockOf(mir, *funclet, mir.instBlock(inc.value)))
            << "a Phi input names an instruction of another function";
        if (inc.pred.v == entry.v) {
            ++fromEntry;
            EXPECT_EQ(mir.instOpcode(inc.value), MirOpcode::Const);
        } else if (inc.pred.v == rhs.v) {
            ++fromRhs;
            EXPECT_EQ(mir.instOpcode(inc.value), MirOpcode::ICmpEq);
        }
    }
    EXPECT_EQ(fromEntry, 1u);
    EXPECT_EQ(fromRhs, 1u);
    MirInstId const ret = mir.blockTerminator(merge);
    ASSERT_EQ(mir.instOpcode(ret), MirOpcode::Return);
    ASSERT_EQ(mir.instOperands(ret).size(), 1u);
    EXPECT_EQ(mir.instOperands(ret)[0].v, phi.v) << "the funclet returns the join's value";
    EXPECT_EQ(countIn(mir, *funclet, MirOpcode::SehExceptionCode), 0u);
    EXPECT_EQ(countIn(mir, *funclet, MirOpcode::SehFilterReturn), 0u);

    auto const parent = findFuncBySymbol(mir, SymbolId{kParentSym});
    ASSERT_TRUE(parent.has_value());
    EXPECT_EQ(mir.funcBlockCount(*parent), 5u)
        << "entry, body, the stub, handler, join — the filter's other blocks are the funclet's";
    expectTheParentKeepsOneStub(mir, *parent);

    DiagnosticReporter rep;
    EXPECT_TRUE((MirVerifier{mir, &in}.verify(rep))) << allDiagText(rep);
}

// `c ? 1 : 0`: the diamond and its Phi, the branch's TRUE arm the one that holds 1.
TEST(SehFilterFunclet, AConditionalFilterBecomesAFuncletWithTheDiamondAndThePhi) {
    EXPECT_EXIT(synthesizeAndLeave(moduleOf(&conditional)),
                ::testing::ExitedWithCode(0), "") << kThePassDidNotFinish;
    if (HasFailure()) return;

    TypeInterner in{CompilationUnitId{1}};
    Parent p = buildParent(in, conditional);
    Mir& mir = p.mir;
    Synthesized const s = synthesize(mir, in);
    ASSERT_TRUE(s.ok) << s.text;
    ASSERT_EQ(s.scopes.size(), 1u);

    auto const funclet = findFuncBySymbol(mir, s.scopes[0].filterFuncletSymbol);
    ASSERT_TRUE(funclet.has_value());
    ASSERT_EQ(mir.funcBlockCount(*funclet), 4u) << "the entry, the two arms, the join";
    MirBlockId const entry = mir.funcBlockAt(*funclet, 0);
    ASSERT_EQ(mir.instOpcode(mir.blockTerminator(entry)), MirOpcode::CondBr);
    auto const arms = mir.blockSuccessors(entry);
    ASSERT_EQ(arms.size(), 2u);
    auto const yes = firstIn(mir, arms[0], MirOpcode::Const);
    auto const no  = firstIn(mir, arms[1], MirOpcode::Const);
    ASSERT_TRUE(yes.has_value());
    ASSERT_TRUE(no.has_value());
    EXPECT_EQ(constValue(mir, *yes), 1) << "the TRUE arm answers 1";
    EXPECT_EQ(constValue(mir, *no), 0)  << "the FALSE arm answers 0";
    ASSERT_EQ(mir.blockSuccessors(arms[0]).size(), 1u);
    ASSERT_EQ(mir.blockSuccessors(arms[1]).size(), 1u);
    MirBlockId const merge = mir.blockSuccessors(arms[0])[0];
    EXPECT_EQ(mir.blockSuccessors(arms[1])[0].v, merge.v);

    MirInstId const phi = mir.blockInstAt(merge, 0);
    ASSERT_EQ(mir.instOpcode(phi), MirOpcode::Phi);
    auto const inputs = mir.phiIncomings(phi);
    ASSERT_EQ(inputs.size(), 2u);
    for (MirPhiIncoming const& inc : inputs) {
        if (inc.pred.v == arms[0].v) EXPECT_EQ(inc.value.v, yes->v);
        else if (inc.pred.v == arms[1].v) EXPECT_EQ(inc.value.v, no->v);
        else ADD_FAILURE() << "a Phi input arrives from a block that is not an arm";
    }
    EXPECT_EQ(mir.instOpcode(mir.blockTerminator(merge)), MirOpcode::Return);

    auto const parent = findFuncBySymbol(mir, SymbolId{kParentSym});
    ASSERT_TRUE(parent.has_value());
    EXPECT_EQ(mir.funcBlockCount(*parent), 5u);
    expectTheParentKeepsOneStub(mir, *parent);

    DiagnosticReporter rep;
    EXPECT_TRUE((MirVerifier{mir, &in}.verify(rep))) << allDiagText(rep);
}

// What the blocks AFTER the entry take from the parent is made in the funclet's
// ENTRY: a frame slot both arms read, and a value the parent computed before the
// region that only the join names. Made where they are first named, the slot would
// stand in one arm and be read in the other.
TEST(SehFilterFunclet, WhatLaterBlocksTakeFromTheParentIsMadeInTheFuncletsEntry) {
    EXPECT_EXIT(synthesizeAndLeave(moduleOf(&bothArmsReadALocal)),
                ::testing::ExitedWithCode(0), "") << kThePassDidNotFinish;
    if (HasFailure()) return;

    TypeInterner in{CompilationUnitId{1}};
    Parent p = buildParent(in, bothArmsReadALocal);
    Mir& mir = p.mir;
    Synthesized const s = synthesize(mir, in);
    ASSERT_TRUE(s.ok) << s.text;
    ASSERT_EQ(s.scopes.size(), 1u);

    auto const funclet = findFuncBySymbol(mir, s.scopes[0].filterFuncletSymbol);
    ASSERT_TRUE(funclet.has_value());
    ASSERT_EQ(mir.funcBlockCount(*funclet), 4u);
    MirBlockId const entry = mir.funcBlockAt(*funclet, 0);
    EXPECT_EQ(countIn(mir, *funclet, MirOpcode::RecoverParentFrameSlot), 1u)
        << "one parent local, recovered once";
    auto const slot = firstIn(mir, entry, MirOpcode::RecoverParentFrameSlot);
    ASSERT_TRUE(slot.has_value()) << "the frame slot is made in the funclet's entry";
    EXPECT_EQ(countIn(mir, *funclet, MirOpcode::Add), 1u)
        << "the sum is computed again, once";
    EXPECT_TRUE(firstIn(mir, entry, MirOpcode::Add).has_value())
        << "the sum is made in the funclet's entry";
    // Both arms load through that one slot.
    std::uint32_t loadsOfTheSlot = 0;
    for (MirBlockId const arm : mir.blockSuccessors(entry)) {
        auto const load = firstIn(mir, arm, MirOpcode::Load);
        ASSERT_TRUE(load.has_value());
        if (mir.instOperands(*load)[0].v == slot->v) ++loadsOfTheSlot;
    }
    EXPECT_EQ(loadsOfTheSlot, 2u);

    DiagnosticReporter rep;
    EXPECT_TRUE((MirVerifier{mir, &in}.verify(rep))) << allDiagText(rep);
}

// An INNER region whose filter has a branch stands inside an OUTER region's body:
//   entry    : slot ; want ; sum ; SehTryBegin(0) → [oBody, oFilter]
//   oBody    : SehTryBegin(1) → [iBody, iFilter]
//   iBody    : load slot ; SehTryEnd(1) ; Br(iJoin)
//   iFilter  : code == AV && want == 1     (three blocks)      → iHandler
//   iHandler : Const 1001 ; Br(iJoin)
//   iJoin    : SehTryEnd(0) ; Br(join)
//   oFilter  : 1                                               → oHandler
//   oHandler : Const 1000 ; Br(join)
//   join     : return 0
// The outer body's membership holds every block of the inner filter, and the
// parent keeps only the inner filter's entry: the outer run is the five blocks
// that are left, in one piece, and the inner record still comes first.
namespace {

[[nodiscard]] Mir buildAnInnerFilterWithABranch(TypeInterner& in) {
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const u32    = in.primitive(TypeKind::U32);
    TypeId const boolTy = in.primitive(TypeKind::Bool);
    TypeId const pI32   = in.pointer(i32);
    MirBuilder mb;
    mb.stateSelfContainedSymbolIds();   // a hand-built module is its own table
    mb.addFunction(in.fnSig({}, i32, CallConv::CcMS64), SymbolId{kParentSym});
    MirBlockId const entry    = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const oBody    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const oFilter  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const oHandler = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const join     = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const iBody    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const iFilter  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const iHandler = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const iJoin    = mb.createBlock(StructCfMarker::Linear);

    mb.beginBlock(entry);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, pI32, 4);
    MirInstId const want = mb.addInst(MirOpcode::Alloca, {}, pI32, 4);
    MirInstId const k3   = mb.addConst(i32Lit(3), i32);
    MirInstId const k4   = mb.addConst(i32Lit(4), i32);
    MirInstId const sum  = mb.addInst(MirOpcode::Add, std::array<MirInstId, 2>{k3, k4}, i32);
    mb.addSehTryBegin(oBody, oFilter, 0);

    mb.beginBlock(oBody);
    mb.addSehTryBegin(iBody, iFilter, 1);
    mb.beginBlock(iBody);
    (void)mb.addInst(MirOpcode::Load, std::array<MirInstId, 1>{slot}, i32);
    mb.addInst(MirOpcode::SehTryEnd, {}, InvalidType, 1);
    mb.addBr(iJoin);

    Ctx inner{mb, in, i32, u32, boolTy, pI32, iBody, iFilter, iHandler, iJoin, slot, want, sum};
    inner.region = 1;
    mb.beginBlock(iFilter);
    shortCircuit(inner, MirBlockId{});

    mb.beginBlock(iHandler);
    (void)mb.addConst(i32Lit(1001), i32);
    mb.addBr(iJoin);
    mb.beginBlock(iJoin);
    mb.addInst(MirOpcode::SehTryEnd, {}, InvalidType, 0);
    mb.addBr(join);

    mb.beginBlock(oFilter);
    mb.addSehFilterReturn(mb.addConst(i32Lit(1), i32), oHandler, 0);
    mb.beginBlock(oHandler);
    (void)mb.addConst(i32Lit(1000), i32);
    mb.addBr(join);
    mb.beginBlock(join);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir mir = std::move(mb).finish();
    rederiveStructCfMarkers(mir);
    return mir;
}

} // namespace

TEST(SehFilterFunclet, AnInnerFilterWithABranchLeavesTheOuterRunWhole) {
    EXPECT_EXIT(synthesizeAndLeave(&buildAnInnerFilterWithABranch),
                ::testing::ExitedWithCode(0), "") << kThePassDidNotFinish;
    if (HasFailure()) return;

    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildAnInnerFilterWithABranch(in);
    {
        DiagnosticReporter before;
        ASSERT_TRUE((MirVerifier{mir, &in}.verify(before))) << allDiagText(before);
    }

    Synthesized const s = synthesize(mir, in);
    ASSERT_TRUE(s.ok) << s.text;
    ASSERT_EQ(s.scopes.size(), 2u);
    auto const parent = findFuncBySymbol(mir, SymbolId{kParentSym});
    ASSERT_TRUE(parent.has_value());
    EXPECT_EQ(mir.funcBlockCount(*parent), 9u)
        << "eleven blocks went in; the inner filter's two blocks past its entry are "
           "the funclet's";
    auto const position = [&](MirBlockId b) {
        for (std::uint32_t i = 0; i < mir.funcBlockCount(*parent); ++i) {
            if (mir.funcBlockAt(*parent, i).v == b.v) return i;
        }
        return UINT32_MAX;
    };
    auto const tagOf = [&](MirSehScope const& scope) {
        auto const tag = firstIn(mir, scope.handlerBlock, MirOpcode::Const);
        return tag.has_value() ? constValue(mir, *tag) : std::int64_t{-1};
    };
    EXPECT_EQ(tagOf(s.scopes[0]), 1001) << "the inner region's record comes first";
    EXPECT_EQ(tagOf(s.scopes[1]), 1000);
    EXPECT_EQ(position(s.scopes[0].endBlock) - position(s.scopes[0].beginBlock) + 1u, 1u)
        << "the inner body is its one block";
    EXPECT_EQ(position(s.scopes[1].endBlock) - position(s.scopes[1].beginBlock) + 1u, 5u)
        << "the outer body: its entry, the inner body, the inner filter's stub, the "
           "inner handler and the inner join — one run";
    // Two funclets, and the inner one is the three blocks.
    auto const innerFunclet = findFuncBySymbol(mir, s.scopes[0].filterFuncletSymbol);
    ASSERT_TRUE(innerFunclet.has_value());
    EXPECT_EQ(mir.funcBlockCount(*innerFunclet), 3u);
    EXPECT_EQ(countIn(mir, *parent, MirOpcode::Phi), 0u);
    EXPECT_EQ(countIn(mir, *parent, MirOpcode::CondBr), 0u);

    DiagnosticReporter rep;
    EXPECT_TRUE((MirVerifier{mir, &in}.verify(rep))) << allDiagText(rep);
}
