// A VALUE THIS TIER KEEPS IN MEMORY, BOUND TO AN ASM REGISTER, TRAVELS BY ITS
// HOME — LOADED IN, STORED OUT — AND NEVER AS ITS ADDRESS.
// D-LIR-ASM-MEMORY-RESIDENT-FLOAT-OPERAND-CARRIED-AS-ITS-ADDRESS.
//
// ★★★ THE DEFECT, ✔MEASURED 2026-09-18 THROUGH THE SHIPPED CLI. An F80/F128
// (`long double`) SSA value is the ADDRESS of its 16-byte home at the MIR→LIR
// tier (the LD model). The asm expansion materialised an input with a register
// MOVE from `regForValue(operand)` — i.e. it moved the ADDRESS — so an aarch64
// `long double` bound to `"w"` reached the template as `mov x17, sp; fmov d0,
// x17`, rc=0, and the program returned the bits of a stack address at debug AND
// release. gcc 13.3.0 and clang 18.1.3 both compile and RUN it to the right
// answer. An OUTPUT was refused at encode instead (its register later used as a
// base address), so the `"+w"` round trip did not build at all.
//
// ★★ WHAT IS PINNED HERE IS THE STRUCTURE, AT THE TIER THAT DECIDES IT: the
// register the template READS is defined by the constraint class's LOAD at the
// value's full 128-bit width out of a GPR-held home address, the register the
// template WRITES is STORED at 128 bits into a home, and no cross-file move (the
// address-into-an-FP-register the defect emitted) appears anywhere. A runtime
// pin can be satisfied by an allocation coincidence; this cannot.
//
// ★ EVERY ARM IS A MATCHED PAIR over one bit: the same statement over a
// `double` (register-resident at this tier) must keep its ordinary MOVE and
// store nothing; a value WIDER than the bound register travels as a register
// PAIR (D-ASM-MULTI-REGISTER-OPERAND-BINDING-NOT-REALIZED — these two arms
// pinned its refusal until the pair landed) beside a value of the register's
// own width that is moved; and the shape no reference carries (an x87 value in
// an XMM register) is refused by name beside a double that lowers. A lowering
// that loaded every operand, or refused every long double, fails its control.
//
// ⚠ BUILT BY HAND, ON PURPOSE: the shared C fixture analyzes with no
// long-double format declared, so `long double` never reaches this tier as F128
// through it. The MIR below is the shape `hir_to_mir` emits for these
// statements (outputs as result pieces, a `+` as a tied read half), minus the
// store-backs, which are not what this tier decides.
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run
// through ctest and never as a bare `.exe`.

#include "asm_region_test_support.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_asm_descriptor.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::string inventory(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& d : r.all()) {
        out += diagnosticCodeName(d.code);
        out += ": ";
        out += d.actual;
        out += "\n";
    }
    return out.empty() ? std::string{"(nothing reported)"} : out;
}

[[nodiscard]] bool anyTextContains(DiagnosticReporter const& r,
                                   std::string_view needle) {
    for (auto const& d : r.all()) {
        if (d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
}

[[nodiscard]] MirAsmOperand operand(std::string constraint, TargetRegClass cls,
                                    std::vector<std::string> spellings) {
    MirAsmOperand o;
    o.constraint = std::move(constraint);
    o.regClass   = cls;
    o.spellings  = std::move(spellings);
    return o;
}

// `T f(T a) { T r; __asm__(templ : "=<L>"(r) : "<L>"(a)); return r; }`
[[nodiscard]] Mir buildCopyThroughAsm(TypeInterner& interner, TypeKind kind,
                                      TargetRegClass cls, std::string const& letter,
                                      std::string templ) {
    TypeId const ty = interner.primitive(kind);
    std::array<TypeId, 1> const params{ty};
    TypeId const fnSig = interner.fnSig(params, ty, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a = mb.addArg(0, ty);
    MirAsmDescriptor d;
    d.templateText = std::move(templ);
    d.isExtended   = true;
    d.outputs.push_back(operand("=" + letter, cls, {"%0"}));
    d.inputs.push_back(operand(letter, cls, {"%1"}));
    std::array<MirInstId, 1> const ops{a};
    MirInstId const r = mb.addInlineAsm(std::move(d), ops, ty);
    mb.addReturn(r);
    return std::move(mb).finish();
}

// Two outputs (output 0 the asm's own value, output 1 a RESULT PIECE stored
// through a pointer parameter) and a read-write operand tied to a third output:
// `x = b; y = a; z = z` in one template, every route one statement has.
[[nodiscard]] Mir buildTwoOutputsAndATie(TypeInterner& interner) {
    TypeId const f128 = interner.primitive(TypeKind::F128);
    TypeId const ptr  = interner.pointer(f128);
    std::array<TypeId, 3> const params{f128, f128, ptr};
    TypeId const fnSig = interner.fnSig(params, f128, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    MirInstId const a   = mb.addArg(0, f128);
    MirInstId const b   = mb.addArg(1, f128);
    MirInstId const out = mb.addArg(2, ptr);
    MirAsmDescriptor d;
    d.templateText = "mov %0.16b, %3.16b\n\tmov %1.16b, %4.16b";
    d.isExtended   = true;
    MirAsmOperand x = operand("=&w", TargetRegClass::FPR, {"%0"});
    x.isEarlyClobber = true;
    MirAsmOperand y = operand("=&w", TargetRegClass::FPR, {"%1"});
    y.isEarlyClobber = true;
    MirAsmOperand z = operand("+w", TargetRegClass::FPR, {"%2"});
    z.isReadWrite = true;
    d.outputs.push_back(std::move(x));
    d.outputs.push_back(std::move(y));
    d.outputs.push_back(z);
    d.inputs.push_back(operand("w", TargetRegClass::FPR, {"%3"}));
    d.inputs.push_back(operand("w", TargetRegClass::FPR, {"%4"}));
    MirAsmOperand tied = z;          // GNU's synthesized read half of `+w`
    tied.spellings.clear();
    tied.tiedOutput = 2u;
    d.inputs.push_back(std::move(tied));
    std::array<MirInstId, 3> const ops{b, a, a};
    MirInstId const r0 = mb.addInlineAsm(std::move(d), ops, f128);
    MirInstId const r1 = mb.addReturnPiece(r0, 1, TargetRegClass::FPR, f128);
    MirInstId const r2 = mb.addReturnPiece(r0, 2, TargetRegClass::FPR, f128);
    std::array<MirInstId, 2> const st{r1, out};
    (void)mb.addInst(MirOpcode::Store, st);
    (void)r2;
    mb.addReturn(r0);
    return std::move(mb).finish();
}

// `asm goto` with ONE long double output read on BOTH edges: the value must be
// stored into its home at the head of the fall-through edge AND of the label
// edge, because each landing block's piece reads that home.
[[nodiscard]] Mir buildAsmGotoWithOutput(TypeInterner& interner) {
    TypeId const f128  = interner.primitive(TypeKind::F128);
    TypeId const i64   = interner.primitive(TypeKind::I64);
    TypeId const ptr   = interner.pointer(f128);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    std::array<TypeId, 3> const params{f128, i64, ptr};
    TypeId const fnSig = interner.fnSig(params, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const yes   = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirInstId const a   = mb.addArg(0, f128);
    MirInstId const c   = mb.addArg(1, i64);
    MirInstId const out = mb.addArg(2, ptr);
    MirAsmDescriptor d;
    d.templateText = "mov %0.16b, %1.16b\n\tcmp %2, #0\n\tb.ne %l[yes]";
    d.isExtended   = true;
    d.clobbersConditionCodes = true;
    d.outputs.push_back(operand("=w", TargetRegClass::FPR, {"%0"}));
    d.inputs.push_back(operand("w", TargetRegClass::FPR, {"%1"}));
    d.inputs.push_back(operand("r", TargetRegClass::GPR, {"%2"}));
    d.labelSpellings.push_back({"%l[yes]", "%l3"});
    std::array<MirInstId, 2> const ops{a, c};
    std::array<MirBlockId, 1> const labels{yes};
    auto const res = mb.addInlineAsmGoto(std::move(d), ops, labels);
    for (auto const& e : res.edges) {
        if (!e.split) continue;
        mb.beginBlock(e.successor);
        MirInstId const rp =
            mb.addReturnPiece(res.terminator, 0, TargetRegClass::FPR, f128);
        std::array<MirInstId, 2> const st{rp, out};
        (void)mb.addInst(MirOpcode::Store, st);
        mb.addBr(e.onward);
    }
    mb.beginBlock(yes);
    mb.addReturn();
    mb.beginBlock(res.continuation());
    mb.addReturn();
    return std::move(mb).finish();
}

struct Lowered {
    TypeInterner       interner{CompilationUnitId{1}};
    DiagnosticReporter reporter;
    MirToLirResult     result;
};

// Every instruction of the module with the block it sits in, in block order.
struct Placed { LirInstId id; LirBlockId block; };
[[nodiscard]] std::vector<Placed> allInsts(Lir const& lir) {
    std::vector<Placed> out;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const f = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
            LirBlockId const b = lir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(b); ++ii) {
                out.push_back({lir.blockInstAt(b, ii), b});
            }
        }
    }
    return out;
}

[[nodiscard]] std::uint16_t opOf(TargetSchema const& t, std::string_view m) {
    auto const i = t.opcodeByMnemonic(m);
    EXPECT_TRUE(i.has_value()) << "target declares no '" << m << "'";
    return i.has_value() ? *i : std::uint16_t{0};
}

[[nodiscard]] std::vector<LirInstId> withOpcode(Lir const& lir, std::uint16_t op) {
    std::vector<LirInstId> out;
    for (auto const& p : allInsts(lir)) {
        if (lir.instOpcode(p.id) == op) out.push_back(p.id);
    }
    return out;
}

// The last instruction strictly before `before` whose result is `reg`.
// P68 round 8 part 4 — a TEMPLATE LINE: an instruction of some statement's
// bundle BODY, with the bundle it belongs to (the statement's position in the
// function, for a search that runs before or after it).
struct TemplateLine {
    Lir const* body   = nullptr;
    LirInstId  id{};
    LirInstId  bundle{};
};
[[nodiscard]] std::vector<TemplateLine>
templateWithOpcode(Lir const& lir, std::uint16_t op) {
    std::vector<TemplateLine> out;
    for (LirInstId const b : dss::test_support::asmRegionBundles(lir)) {
        LirAsmRegion const& r = *lir.instAsmRegion(b);
        for (LirInstId const i : dss::test_support::asmRegionBodyInsts(r)) {
            if (r.body.instOpcode(i) == op) out.push_back({&r.body, i, b});
        }
    }
    return out;
}

[[nodiscard]] std::optional<LirInstId>
defBefore(Lir const& lir, LirReg reg, LirInstId before) {
    std::optional<LirInstId> found;
    for (auto const& p : allInsts(lir)) {
        if (p.id.v == before.v) break;
        if (lir.instResult(p.id) == reg) found = p.id;
    }
    return found;
}

// Every store (`storeOp`) at `widthBits` whose VALUE operand is `reg`, with the
// block it sits in.
[[nodiscard]] std::vector<Placed>
storesOf(Lir const& lir, std::uint16_t storeOp, LirReg reg, std::uint32_t widthBits) {
    std::vector<Placed> out;
    for (auto const& p : allInsts(lir)) {
        if (lir.instOpcode(p.id) != storeOp) continue;
        if (lirInstWidthBits(lir.instFlags(p.id)) != widthBits) continue;
        auto const ops = lir.instOperands(p.id);
        if (ops.size() < 2 || ops[0].kind != LirOperandKind::Reg) continue;
        if (!(ops[0].reg == reg)) continue;
        out.push_back(p);
    }
    return out;
}

// One memory access of a carriage: the instruction, the register it loads
// or stores, and its displacement from the base.
struct MemAccess { LirInstId id; LirReg reg; std::int32_t offset; };

[[nodiscard]] std::optional<std::int32_t> memOffset(Lir const& lir, LirInstId i) {
    for (auto const& o : lir.instOperands(i)) {
        if (o.kind == LirOperandKind::MemOffset) return o.offset;
    }
    return std::nullopt;
}

// Every `loadOp` at `widthBits` whose RESULT is a register of `cls`.
[[nodiscard]] std::vector<MemAccess>
loadsOf(Lir const& lir, std::uint16_t loadOp, std::uint32_t widthBits,
        LirRegClass cls) {
    std::vector<MemAccess> out;
    for (auto const& p : allInsts(lir)) {
        if (lir.instOpcode(p.id) != loadOp) continue;
        if (lirInstWidthBits(lir.instFlags(p.id)) != widthBits) continue;
        LirReg const r = lir.instResult(p.id);
        if (!r.valid() || r.regClass() != cls) continue;
        auto const off = memOffset(lir, p.id);
        if (!off.has_value()) continue;
        out.push_back({p.id, r, *off});
    }
    return out;
}

// Every `storeOp` at `widthBits` whose VALUE is a register of `cls`.
[[nodiscard]] std::vector<MemAccess>
storesAt(Lir const& lir, std::uint16_t storeOp, std::uint32_t widthBits,
         LirRegClass cls) {
    std::vector<MemAccess> out;
    for (auto const& p : allInsts(lir)) {
        if (lir.instOpcode(p.id) != storeOp) continue;
        if (lirInstWidthBits(lir.instFlags(p.id)) != widthBits) continue;
        auto const ops = lir.instOperands(p.id);
        if (ops.empty() || ops[0].kind != LirOperandKind::Reg) continue;
        if (ops[0].reg.regClass() != cls) continue;
        auto const off = memOffset(lir, p.id);
        if (!off.has_value()) continue;
        out.push_back({p.id, ops[0].reg, *off});
    }
    return out;
}

} // namespace

// ── ARM 1: THE INPUT IS LOADED OUT OF ITS HOME, THE OUTPUT STORED INTO ONE ───
TEST(LirAsmHomeCarriage, ALongDoubleBoundToAnFpRegisterTravelsByItsHome) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    Lowered L;
    Mir const m = buildCopyThroughAsm(L.interner, TypeKind::F128,
                                      TargetRegClass::FPR, "w",
                                      "mov %0.16b, %1.16b");
    L.result = lowerToLir(m, t, L.interner, L.reporter);
    ASSERT_TRUE(L.result.ok)
        << "a `long double` on `\"w\"` must LOWER — gcc 13.3.0 and clang "
           "18.1.3 both compile and run it: " << inventory(L.reporter);
    Lir const& lir = L.result.lir;

    auto const templ = templateWithOpcode(lir, opOf(t, "move_bytes"));
    ASSERT_EQ(templ.size(), 1u)
        << "the template `mov %0.16b, %1.16b` is ONE 128-bit register move";
    TemplateLine const T = templ[0];
    auto const tops = T.body->instOperands(T.id);
    ASSERT_GE(tops.size(), 1u);
    ASSERT_EQ(tops[0].kind, LirOperandKind::Reg);
    LirReg const in  = tops[0].reg;
    LirReg const out = T.body->instResult(T.id);

    // (a) ★★ THE REGISTER THE TEMPLATE READS HOLDS THE VALUE: it is defined by
    // the SIMD&FP class's load, at the full 128 bits, from a GPR-held address —
    // before the statement's bundle (P68 round 8 part 4).
    auto const def = defBefore(lir, in, T.bundle);
    ASSERT_TRUE(def.has_value())
        << "nothing defines the register the template reads";
    EXPECT_EQ(lir.instOpcode(*def), opOf(t, "fldur"))
        << "the input must be LOADED out of its home — a move here is the "
           "defect: it copied the home's ADDRESS into the template's register";
    EXPECT_EQ(lirInstWidthBits(lir.instFlags(*def)), 128u)
        << "all sixteen bytes of the binary128 value, not the width-default 8";
    auto const dops = lir.instOperands(*def);
    ASSERT_GE(dops.size(), 1u);
    ASSERT_EQ(dops[0].kind, LirOperandKind::Reg);
    EXPECT_EQ(dops[0].reg.regClass(), LirRegClass::GPR)
        << "the load's base is the home's address, an integer register";

    // (b) THE DEFECT'S OWN INSTRUCTION IS GONE: no integer→SIMD&FP file move.
    EXPECT_TRUE(withOpcode(lir, opOf(t, "movq_gpr_to_xmm")).empty())
        << "an integer-to-FP-file move is how the ADDRESS used to reach `%1`";

    // (c) THE OUTPUT LEAVES BY A 128-BIT STORE INTO A HOME.
    auto const stores = storesOf(lir, opOf(t, "fstur"), out, 128);
    ASSERT_EQ(stores.size(), 1u)
        << "the register the template wrote must be stored, once, at 128 bits "
           "— the MIR value it becomes is a home";
    auto const sops = lir.instOperands(stores[0].id);
    ASSERT_GE(sops.size(), 2u);
    EXPECT_EQ(sops[1].reg.regClass(), LirRegClass::GPR);
}

// ── ARM 1′: THE CONTROL — THE SAME STATEMENT OVER A `double` ──────────────────
TEST(LirAsmHomeCarriage, ADoubleBoundToAnFpRegisterKeepsItsMove) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    Lowered L;
    Mir const m = buildCopyThroughAsm(L.interner, TypeKind::F64,
                                      TargetRegClass::FPR, "w",
                                      "mov %0.16b, %1.16b");
    L.result = lowerToLir(m, t, L.interner, L.reporter);
    ASSERT_TRUE(L.result.ok) << inventory(L.reporter);
    Lir const& lir = L.result.lir;
    auto const templ = templateWithOpcode(lir, opOf(t, "move_bytes"));
    ASSERT_EQ(templ.size(), 1u);
    LirReg const in  = templ[0].body->instOperands(templ[0].id)[0].reg;
    LirReg const out = templ[0].body->instResult(templ[0].id);
    auto const def = defBefore(lir, in, templ[0].bundle);
    ASSERT_TRUE(def.has_value());
    EXPECT_EQ(lir.instOpcode(*def), opOf(t, "fmov"))
        << "a double lives in a register at this tier: its operand is MOVED "
           "in, and a load here would mean every FP operand went through memory";
    EXPECT_TRUE(storesOf(lir, opOf(t, "fstur"), out, 128).empty())
        << "a register-resident output is not stored into a home";
}

// ── ARM 2: EVERY ROUTE OF ONE STATEMENT — output 0, a PIECE, and a `+` tie ───
TEST(LirAsmHomeCarriage, EveryOutputRouteStoresIntoAHome) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    Lowered L;
    Mir const m = buildTwoOutputsAndATie(L.interner);
    L.result = lowerToLir(m, t, L.interner, L.reporter);
    ASSERT_TRUE(L.result.ok)
        << "two long double outputs, a piece and a `\"+w\"` must lower — "
           "gcc and clang run the C twin to the right answer: "
        << inventory(L.reporter);
    Lir const& lir = L.result.lir;
    auto const templ = templateWithOpcode(lir, opOf(t, "move_bytes"));
    ASSERT_EQ(templ.size(), 2u) << "two `mov Vd.16b, Vn.16b` lines";
    LirReg const x = templ[0].body->instResult(templ[0].id);
    LirReg const y = templ[1].body->instResult(templ[1].id);
    EXPECT_FALSE(x == y) << "two early-clobber outputs in one register";
    EXPECT_EQ(storesOf(lir, opOf(t, "fstur"), x, 128).size(), 1u)
        << "output 0 (the asm's own value) must be stored into its home";
    EXPECT_EQ(storesOf(lir, opOf(t, "fstur"), y, 128).size(), 1u)
        << "output 1 (read back through a result PIECE) must be stored too — "
           "the piece is that home";
    // The tied read half is LOADED: some 128-bit `fldur` defines a register
    // that is also stored at 128 bits (the `+w` register, read then written).
    bool tieLoadedAndStored = false;
    for (LirInstId const ld : withOpcode(lir, opOf(t, "fldur"))) {
        if (lirInstWidthBits(lir.instFlags(ld)) != 128) continue;
        LirReg const r = lir.instResult(ld);
        if (!r.valid() || r.regClass() != LirRegClass::FPR) continue;
        if (!storesOf(lir, opOf(t, "fstur"), r, 128).empty())
            tieLoadedAndStored = true;
    }
    EXPECT_TRUE(tieLoadedAndStored)
        << "the `+w` operand's ONE register must be loaded from the old home "
           "and stored into the new one";
    EXPECT_TRUE(withOpcode(lir, opOf(t, "movq_gpr_to_xmm")).empty());
}

// ── ARM 3: `asm goto` — the store rides EVERY edge ────────────────────────────
TEST(LirAsmHomeCarriage, AnAsmGotoOutputIsStoredOnEveryEdge) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    Lowered L;
    Mir const m = buildAsmGotoWithOutput(L.interner);
    L.result = lowerToLir(m, t, L.interner, L.reporter);
    ASSERT_TRUE(L.result.ok)
        << "an `asm goto` with a long double output must lower — gcc and "
           "clang run the C twin: " << inventory(L.reporter);
    Lir const& lir = L.result.lir;
    auto const templ = templateWithOpcode(lir, opOf(t, "move_bytes"));
    ASSERT_EQ(templ.size(), 1u);
    LirReg const out = templ[0].body->instResult(templ[0].id);
    auto const stores = storesOf(lir, opOf(t, "fstur"), out, 128);
    ASSERT_EQ(stores.size(), 2u)
        << "one 128-bit store per edge — the fall-through AND the label — "
           "because each landing block reads the output's home";
    EXPECT_FALSE(stores[0].block.v == stores[1].block.v)
        << "the two stores must sit on the two different edges";
}

// ── ARM 4: WIDER THAN ONE REGISTER — a register PAIR ─────────────────────────
// D-ASM-MULTI-REGISTER-OPERAND-BINDING-NOT-REALIZED. ✔MEASURED 2026-09-18:
// aarch64-linux-gnu-gcc 13.3.0 binds a binary128 `long double` on `"r"` to TWO
// x-registers — `%0` the first, `%H0` the second — and runs the read, the write
// and the `+` form to 42 at -O0 and -O2 under qemu-aarch64. This arm used to pin
// the refusal the pair replaced. ★ THE PAIR IS PINNED PIECE BY PIECE: each half
// is loaded at its own displacement (0 and 8) into its own register, the
// template's `%1` reads the FIRST and its `%H1` the SECOND, and each register
// the template writes is stored back at the displacement it came from — a
// carriage that swapped the halves, or moved one register, fails here.
TEST(LirAsmHomeCarriage, ALongDoubleOnAGeneralRegisterTravelsAsAPair) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    Lowered L;
    Mir const m = buildCopyThroughAsm(L.interner, TypeKind::F128,
                                      TargetRegClass::GPR, "r",
                                      "mov %0, %1\n\tmov %H0, %H1");
    L.result = lowerToLir(m, t, L.interner, L.reporter);
    ASSERT_TRUE(L.result.ok)
        << "a `long double` on `\"r\"` must LOWER as a register pair — gcc "
           "13.3.0 runs it: " << inventory(L.reporter);
    Lir const& lir = L.result.lir;

    auto const loads = loadsOf(lir, opOf(t, "load"), 64, LirRegClass::GPR);
    auto const stores = storesAt(lir, opOf(t, "store"), 64, LirRegClass::GPR);
    ASSERT_EQ(loads.size(), 2u)
        << "the input pair is TWO 64-bit loads out of the value's home";
    ASSERT_EQ(stores.size(), 2u)
        << "the output pair is TWO 64-bit stores into its home";
    auto const bundle = dss::test_support::onlyAsmRegion(lir);
    ASSERT_TRUE(bundle.has_value());
    Lir const& body = bundle->region->body;
    for (auto const& s : stores) {
        // the template instruction that wrote this half (a line of the
        // statement's bundle BODY, P68 round 8 part 4) ...
        std::optional<LirInstId> def;
        for (LirInstId const i :
             dss::test_support::asmRegionBodyInsts(*bundle->region)) {
            if (body.instResult(i) == s.reg) def = i;
        }
        ASSERT_TRUE(def.has_value()) << "nothing wrote the half stored at +"
                                     << s.offset;
        auto const dops = body.instOperands(*def);
        ASSERT_FALSE(dops.empty());
        ASSERT_EQ(dops[0].kind, LirOperandKind::Reg);
        // ... read the input half loaded from the SAME displacement.
        bool matched = false;
        for (auto const& l : loads) {
            if (l.reg == dops[0].reg) {
                matched = true;
                EXPECT_EQ(l.offset, s.offset)
                    << "the half stored at +" << s.offset << " was computed "
                       "from the half loaded at +" << l.offset
                    << " — `%0`/`%H0` bound the pair's halves in the wrong order";
            }
        }
        EXPECT_TRUE(matched) << "the half stored at +" << s.offset
                             << " was not computed from a loaded half";
    }
    std::vector<std::int32_t> loadOffsets;
    for (auto const& l : loads) loadOffsets.push_back(l.offset);
    std::sort(loadOffsets.begin(), loadOffsets.end());
    EXPECT_EQ(loadOffsets, (std::vector<std::int32_t>{0, 8}));
    EXPECT_FALSE(loads[0].reg == loads[1].reg)
        << "the two halves of a pair are two registers";

    // CONTROL: a value of the register's own width on the same letter binds
    // ONE register and is moved, not loaded.
    Lowered ok;
    Mir const c = buildCopyThroughAsm(ok.interner, TypeKind::I64,
                                      TargetRegClass::GPR, "r", "mov %0, %1");
    ok.result = lowerToLir(c, t, ok.interner, ok.reporter);
    ASSERT_TRUE(ok.result.ok) << inventory(ok.reporter);
    EXPECT_TRUE(loadsOf(ok.result.lir, opOf(t, "load"), 64,
                        LirRegClass::GPR).empty())
        << "a register-resident operand is moved in, never loaded";

    // AND THE SECOND-REGISTER LETTER IS REFUSED ON A ONE-REGISTER OPERAND, by
    // name, where gcc refuses it too ("invalid operand for '%H'").
    Lowered bad;
    Mir const h = buildCopyThroughAsm(bad.interner, TypeKind::I64,
                                      TargetRegClass::GPR, "r", "mov %0, %H1");
    bad.result = lowerToLir(h, t, bad.interner, bad.reporter);
    EXPECT_FALSE(bad.result.ok);
    EXPECT_TRUE(anyTextContains(bad.reporter,
                                "names the SECOND register of a two-register"))
        << inventory(bad.reporter);
}

// ── ARM 5: x86_64 — the 80-bit x87 value on a PAIR (64 + 16 bits), and on an
// XMM register, where no reference carries it ────────────────────────────────
TEST(LirAsmHomeCarriage, AnX87LongDoubleTravelsAsAPairAndHasNoXmmCarriage) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;

    // ✔MEASURED 2026-09-18: gcc 13.3.0 binds an x87 `long double` on `"r"` to
    // TWO general registers and runs the read, the write and the `+` form to 42
    // at -O0 and -O2 (clang refuses). The format is 64 significand bits plus 16
    // sign/exponent bits, so the second piece is 16 bits wide — the format's
    // own split, never a full second register's worth of the home.
    Lowered pair;
    Mir const m1 = buildCopyThroughAsm(pair.interner, TypeKind::F80,
                                       TargetRegClass::GPR, "r", "movq %1, %0");
    pair.result = lowerToLir(m1, t, pair.interner, pair.reporter);
    ASSERT_TRUE(pair.result.ok)
        << "an x87 long double on `\"r\"` must LOWER as a register pair: "
        << inventory(pair.reporter);
    Lir const& lir = pair.result.lir;
    auto const lo = loadsOf(lir, opOf(t, "load"), 64, LirRegClass::GPR);
    auto const hi = loadsOf(lir, opOf(t, "load"), 16, LirRegClass::GPR);
    ASSERT_EQ(lo.size(), 1u) << "the significand: one 64-bit load";
    ASSERT_EQ(hi.size(), 1u) << "sign and exponent: one 16-bit load";
    EXPECT_EQ(lo[0].offset, 0);
    EXPECT_EQ(hi[0].offset, 8);
    EXPECT_EQ(storesAt(lir, opOf(t, "store"), 64, LirRegClass::GPR).size(), 1u);
    EXPECT_EQ(storesAt(lir, opOf(t, "store"), 16, LirRegClass::GPR).size(), 1u)
        << "the output's second piece is stored at its own 16-bit width";

    // No reference carries an x87 value in an XMM register (gcc 13.3.0
    // "inconsistent operand constraints", clang 18.1.3 "couldn't allocate"),
    // so the target lists no F80 carriage for `fpr` and it is refused BY NAME.
    Lowered view;
    Mir const m2 = buildCopyThroughAsm(view.interner, TypeKind::F80,
                                       TargetRegClass::FPR, "x", "nop");
    view.result = lowerToLir(m2, t, view.interner, view.reporter);
    EXPECT_FALSE(view.result.ok);
    EXPECT_TRUE(anyTextContains(view.reporter, "10-byte 'F80' value"))
        << inventory(view.reporter);
    EXPECT_TRUE(anyTextContains(view.reporter, "`asmValueCarriage`"))
        << inventory(view.reporter);

    // CONTROL: a double on `"x"` is the register's ordinary tenant and lowers.
    Lowered ok;
    Mir const c = buildCopyThroughAsm(ok.interner, TypeKind::F64,
                                      TargetRegClass::FPR, "x", "nop");
    ok.result = lowerToLir(c, t, ok.interner, ok.reporter);
    EXPECT_TRUE(ok.result.ok) << inventory(ok.reporter);
}
