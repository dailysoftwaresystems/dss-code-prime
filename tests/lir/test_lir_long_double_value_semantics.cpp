// A `long double` wider than a register is a VALUE, not a view of the object
// it was read from — pinned at MIR→LIR on both memory-resident models (x87
// F80 on x86_64, binary128 F128 on arm64).
//
// ═══ THE TWO DEFECTS (P68 round 8) ════════════════════════════════════════════
//
// (1) D-LIR-LONG-DOUBLE-LOAD-READ-THE-OBJECT-AFTER-IT-WAS-OVERWRITTEN — a SILENT
//     miscompile. An F80/F128 value is represented by the address of a 16-byte
//     home, and a Load used to define its value AS THE SOURCE ADDRESS ("address
//     propagation"), on the premise that a loaded value is consumed before its
//     object is written. C gives no such promise: `long double x = g; g = 1.0L;
//     use(x);`, a swap through a temporary, `old = g++`, a comma operand, and an
//     argument read before a later argument's call writes the object all read
//     the object AFTER the store. ✔MEASURED 2026-09-19 on the P68 round-7 base:
//     5 of 6 such shapes returned the overwritten value on x86_64 Linux and 4 of
//     6 on aarch64 Linux in the RELEASE pipeline, where mem2reg turns the local
//     that used to hold a copy into an SSA name for the source address; gcc
//     13.3.0 and clang 18.1.3 run all six to 42 at -O0 and -O2.
//     ⇒ A Load now COPIES the datum into a fresh home at the point it executes —
//     unless every read of its value can happen in place: the same contract the
//     optimizer's Load motion rests on (no instruction that may write or fence
//     memory between the Load and the read, every use in the Load's block or an
//     edge out of it, and the Load not `volatile`), which keeps the copy out of
//     the ordinary `*q = *p` and puts it exactly where a read cannot wait.
//
// (2) D-LIR-VOLATILE-LONG-DOUBLE-ACCESS-MOVED-ITS-ADDRESS-INTO-A-FLOAT-REGISTER —
//     a refusal of valid code. Reading or writing a `volatile long double` is a
//     qualifier conversion (C 6.3.2.1p2), which HIR→MIR spells as a SAME-KIND
//     Bitcast; `lowerBitcast` picked the class move by the result type (FPR) and
//     moved the home ADDRESS into an FP register, which the next `fld_m80` /
//     `ldr` then used as a base — refused by the encoder on x86_64 and aarch64
//     Linux, debug and release, while gcc and clang compile and run it.
//     ⇒ A same-kind Bitcast of a memory-resident float is the identity on its
//     home; a Bitcast between one and another kind is refused by name.
//
// ═══ WHAT EACH PIN ASSERTS, AND WHY IT IS A SEMANTIC CHECK ════════════════════
//
// `TheObjectIsNeverReadAfterItIsOverwritten` lowers the SWAP — `x = *p; y = *q;
// *p = y; *q = x;` — and asserts, on the emitted instruction stream, that no
// instruction READS the memory at `p` after the first instruction that WRITES
// it. That is the defect's own definition, independent of how the copy is
// spelled: under address propagation the last store (`*q = x`) read `x` from
// `p`, after `*p = y` had overwritten it. The pin never names a frame slot,
// an order of homes or a register — only reads and writes of one object.
// The five boundary pins after them hold where the copy is and is not: a read
// that nothing can precede with a write takes no home (`*q = *p`), while a
// `volatile` read, a value used in another block, a value read through a
// same-kind Bitcast after a store (the Bitcast names the home, it does not read
// it), and a value that leaves its block through a phi edge after a store each
// take exactly one — counted as the 16-byte frame reservations a copy home is
// made of.
// `AQualifierConversionKeepsTheAddressOutOfTheFloatRegisters` lowers `x = *(volatile
// long double *)p; *q = x` and asserts that NO instruction of the function
// defines a floating-point register: every byte of both models moves through
// x87 memory forms or general registers, so an FP-class result can only be
// the address taking the wrong road. `ABitcastToAnotherKindIsRefusedByName`
// holds the refusal to its words.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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

// One memory-resident `long double` model: the target it lives on, its kind,
// and the mnemonics its bytes move through — READ from memory at operand
// `readBase`, WRITTEN to memory at operand `writeBase`. The x87 model moves an
// F80 with FLD m80 / FSTP m80; the binary128 model moves an F128 as two
// general-register words (`load` / `store`).
struct Model {
    char const*      target;
    TypeKind         kind;
    char const*      readMnemonic;
    std::uint32_t    readBase;
    char const*      writeMnemonic;
    std::uint32_t    writeBase;
    std::optional<std::string> softcallLibrary;
};

constexpr std::array<char const*, 2> kModelNames{"x87 F80 on x86_64",
                                                 "binary128 F128 on arm64"};

[[nodiscard]] std::array<Model, 2> models() {
    return {Model{"x86_64", TypeKind::F80, "fld_m80", 0, "fstp_m80", 0, std::nullopt},
            Model{"arm64", TypeKind::F128, "load", 0, "store", 1,
                  std::string{"libgcc_s.so.1"}}};
}

[[nodiscard]] MirToLirResult lower(Mir const& m, TargetSchema const& t,
                                   TypeInterner const& in, DiagnosticReporter& rep,
                                   Model const& model) {
    return lowerToLir(m, t, in, rep, std::vector<ExternImport>{},
                      ExternCallDispatch::DirectPlt,
                      /*dataImportBinding=*/std::nullopt,
                      /*tlsAccess=*/std::nullopt, /*sehScopes=*/{},
                      model.softcallLibrary);
}

// `void f(T *p, T *q) { T x = *p; T y = *q; *p = y; *q = x; }` — the swap.
[[nodiscard]] Mir buildSwap(TypeInterner& in, TypeKind kind) {
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const ty = in.primitive(kind);
    TypeId const ptr = in.pointer(ty);
    std::vector<TypeId> const params{ptr, ptr};
    TypeId const sig = in.fnSig(params, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    mb.beginBlock(mb.createBlock(StructCfMarker::EntryBlock));
    MirInstId const p = mb.addArg(0, ptr);
    MirInstId const q = mb.addArg(1, ptr);
    std::array<MirInstId, 1> const lp{p}, lq{q};
    MirInstId const x = mb.addInst(MirOpcode::Load, lp, ty);
    MirInstId const y = mb.addInst(MirOpcode::Load, lq, ty);
    std::array<MirInstId, 2> const sp{y, p}, sq{x, q};
    (void)mb.addInst(MirOpcode::Store, sp, InvalidType);
    (void)mb.addInst(MirOpcode::Store, sq, InvalidType);
    mb.addReturn(std::nullopt);
    return std::move(mb).finish();
}

// `void f(T *p, U *q) { *q = (U)*(volatile T *)p; }` — a volatile read, then a
// Bitcast to `to` (the same kind for the qualifier conversion HIR→MIR emits).
[[nodiscard]] Mir buildVolatileReadAndBitcast(TypeInterner& in, TypeKind kind,
                                              TypeKind to) {
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const ty = in.primitive(kind);
    TypeId const vty = in.volatileQualified(ty);
    TypeId const toTy = in.primitive(to);
    TypeId const ptr = in.pointer(vty);
    TypeId const outPtr = in.pointer(toTy);
    std::vector<TypeId> const params{ptr, outPtr};
    TypeId const sig = in.fnSig(params, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    mb.beginBlock(mb.createBlock(StructCfMarker::EntryBlock));
    MirInstId const p = mb.addArg(0, ptr);
    MirInstId const q = mb.addArg(1, outPtr);
    std::array<MirInstId, 1> const lp{p};
    MirInstId const x = mb.addInst(MirOpcode::Load, lp, vty, /*payload=*/0,
                                   MirInstFlags::Volatile);
    std::array<MirInstId, 1> const bc{x};
    MirInstId const c = mb.addInst(MirOpcode::Bitcast, bc, toTy);
    std::array<MirInstId, 2> const st{c, q};
    (void)mb.addInst(MirOpcode::Store, st, InvalidType);
    mb.addReturn(std::nullopt);
    return std::move(mb).finish();
}

// `void f(T *p, T *q) { *q = *p; }` — a read that nothing can precede with a
// write: the Load's only use is the very next instruction.
[[nodiscard]] Mir buildCopyThrough(TypeInterner& in, TypeKind kind) {
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const ty = in.primitive(kind);
    TypeId const ptr = in.pointer(ty);
    std::vector<TypeId> const params{ptr, ptr};
    TypeId const sig = in.fnSig(params, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    mb.beginBlock(mb.createBlock(StructCfMarker::EntryBlock));
    MirInstId const p = mb.addArg(0, ptr);
    MirInstId const q = mb.addArg(1, ptr);
    std::array<MirInstId, 1> const lp{p};
    MirInstId const x = mb.addInst(MirOpcode::Load, lp, ty);
    std::array<MirInstId, 2> const sq{x, q};
    (void)mb.addInst(MirOpcode::Store, sq, InvalidType);
    mb.addReturn(std::nullopt);
    return std::move(mb).finish();
}

// `void f(T *p, T *q) { T x = *p; goto next; next: *q = x; }` — the value is
// used in another block, with no phi and no write anywhere before the use.
[[nodiscard]] Mir buildUseInAnotherBlock(TypeInterner& in, TypeKind kind) {
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const ty = in.primitive(kind);
    TypeId const ptr = in.pointer(ty);
    std::vector<TypeId> const params{ptr, ptr};
    TypeId const sig = in.fnSig(params, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const next = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirInstId const p = mb.addArg(0, ptr);
    MirInstId const q = mb.addArg(1, ptr);
    std::array<MirInstId, 1> const lp{p};
    MirInstId const x = mb.addInst(MirOpcode::Load, lp, ty);
    mb.addBr(next);
    mb.beginBlock(next);
    std::array<MirInstId, 2> const sq{x, q};
    (void)mb.addInst(MirOpcode::Store, sq, InvalidType);
    mb.addReturn(std::nullopt);
    return std::move(mb).finish();
}

// `void f(T *p, T *q) { T x = *p; T y = *q; *p = y; join: *q = phi(x); }` — x
// leaves its block through a phi edge AFTER the store that overwrites `*p`,
// while y is consumed by that store with nothing in between.
[[nodiscard]] Mir buildStoreBeforeThePhiEdge(TypeInterner& in, TypeKind kind) {
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const ty = in.primitive(kind);
    TypeId const ptr = in.pointer(ty);
    std::vector<TypeId> const params{ptr, ptr};
    TypeId const sig = in.fnSig(params, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const join = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(entry);
    MirInstId const p = mb.addArg(0, ptr);
    MirInstId const q = mb.addArg(1, ptr);
    std::array<MirInstId, 1> const lp{p}, lq{q};
    MirInstId const x = mb.addInst(MirOpcode::Load, lp, ty);
    MirInstId const y = mb.addInst(MirOpcode::Load, lq, ty);
    std::array<MirInstId, 2> const sp{y, p};
    (void)mb.addInst(MirOpcode::Store, sp, InvalidType);
    mb.addBr(join);
    mb.beginBlock(join);
    MirInstId const ph = mb.addPhi(ty);
    mb.addPhiIncoming(ph, MirPhiIncoming{x, entry});
    std::array<MirInstId, 2> const sq{ph, q};
    (void)mb.addInst(MirOpcode::Store, sq, InvalidType);
    mb.addReturn(std::nullopt);
    return std::move(mb).finish();
}

// `void f(T *p, T *q) { T x = *p; T c = (T)x; *p = *q; *q = c; }` — the value
// is read through a same-kind Bitcast (the identity on its home) AFTER the
// store that overwrites `*p`; the Bitcast itself sits before that store.
[[nodiscard]] Mir buildReadThroughAQualifierConversion(TypeInterner& in,
                                                       TypeKind kind) {
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const ty = in.primitive(kind);
    TypeId const ptr = in.pointer(ty);
    std::vector<TypeId> const params{ptr, ptr};
    TypeId const sig = in.fnSig(params, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    mb.beginBlock(mb.createBlock(StructCfMarker::EntryBlock));
    MirInstId const p = mb.addArg(0, ptr);
    MirInstId const q = mb.addArg(1, ptr);
    std::array<MirInstId, 1> const lp{p}, lq{q};
    MirInstId const x = mb.addInst(MirOpcode::Load, lp, ty);
    std::array<MirInstId, 1> const bc{x};
    MirInstId const c = mb.addInst(MirOpcode::Bitcast, bc, ty);
    MirInstId const y = mb.addInst(MirOpcode::Load, lq, ty);
    std::array<MirInstId, 2> const sp{y, p}, sq{c, q};
    (void)mb.addInst(MirOpcode::Store, sp, InvalidType);
    (void)mb.addInst(MirOpcode::Store, sq, InvalidType);
    mb.addReturn(std::nullopt);
    return std::move(mb).finish();
}

// `void f(T *p, T *q) { T x = *p; T c = (T)x; *q = c; }` — the same-kind
// Bitcast FORWARDS the read to the store right after it, with nothing between.
[[nodiscard]] Mir buildForwardedThroughAQualifierConversion(TypeInterner& in,
                                                            TypeKind kind) {
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const ty = in.primitive(kind);
    TypeId const ptr = in.pointer(ty);
    std::vector<TypeId> const params{ptr, ptr};
    TypeId const sig = in.fnSig(params, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    mb.beginBlock(mb.createBlock(StructCfMarker::EntryBlock));
    MirInstId const p = mb.addArg(0, ptr);
    MirInstId const q = mb.addArg(1, ptr);
    std::array<MirInstId, 1> const lp{p};
    MirInstId const x = mb.addInst(MirOpcode::Load, lp, ty);
    std::array<MirInstId, 1> const bc{x};
    MirInstId const c = mb.addInst(MirOpcode::Bitcast, bc, ty);
    std::array<MirInstId, 2> const sq{c, q};
    (void)mb.addInst(MirOpcode::Store, sq, InvalidType);
    mb.addReturn(std::nullopt);
    return std::move(mb).finish();
}

// The 16-byte frame reservations of the function — what a copy home (and a
// phi's home or staging slot) is made of.
[[nodiscard]] std::uint32_t sixteenByteHomes(Lir const& lir, TargetSchema const& t) {
    auto const allocaOp = t.opcodeByMnemonic("alloca");
    if (!allocaOp.has_value()) return 0xFFFFFFFFu;
    std::uint32_t n = 0;
    LirFuncId const fn = lir.funcAt(0);
    for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
        LirBlockId const bb = lir.funcBlockAt(fn, b);
        for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
            LirInstId const li = lir.blockInstAt(bb, i);
            if (lir.instOpcode(li) == *allocaOp && lir.instPayload(li) == 16u) ++n;
        }
    }
    return n;
}

// The result register of the function's `arg` with GPR ordinal `ord`.
[[nodiscard]] std::optional<LirReg> argReg(Lir const& lir, TargetSchema const& t,
                                           std::uint32_t ord) {
    auto const argOp = t.opcodeByMnemonic("arg");
    if (!argOp.has_value()) return std::nullopt;
    LirFuncId const fn = lir.funcAt(0);
    for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
        LirBlockId const bb = lir.funcBlockAt(fn, b);
        for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
            LirInstId const li = lir.blockInstAt(bb, i);
            if (lir.instOpcode(li) == *argOp && lir.instPayload(li) == ord
                && lir.instResult(li).regClass() == LirRegClass::GPR) {
                return lir.instResult(li);
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool sameReg(LirReg a, LirReg b) {
    return a.id == b.id && a.isPhysical == b.isPhysical && a.regClass() == b.regClass();
}

// Does `li` (an instruction of mnemonic `op`) address memory through `base` at
// operand index `at`?
[[nodiscard]] bool addresses(Lir const& lir, LirInstId li, std::uint32_t at,
                             LirReg base) {
    auto const ops = lir.instOperands(li);
    return at < ops.size() && ops[at].kind == LirOperandKind::Reg
        && sameReg(ops[at].reg, base);
}

}  // namespace

// ── (1) the object is never read after it is overwritten ────────────────────
TEST(LongDoubleValueSemantics, TheObjectIsNeverReadAfterItIsOverwritten) {
    for (std::size_t mi = 0; mi < 2; ++mi) {
        Model const model = models()[mi];
        SCOPED_TRACE(kModelNames[mi]);
        auto target = TargetSchema::loadShipped(model.target);
        ASSERT_TRUE(target.has_value());
        TargetSchema const& t = **target;
        auto const readOp = t.opcodeByMnemonic(model.readMnemonic);
        auto const writeOp = t.opcodeByMnemonic(model.writeMnemonic);
        ASSERT_TRUE(readOp.has_value() && writeOp.has_value());

        TypeInterner in{CompilationUnitId{1}};
        DiagnosticReporter rep;
        Mir const m = buildSwap(in, model.kind);
        auto lowered = lower(m, t, in, rep, model);
        ASSERT_TRUE(lowered.ok) << inventory(rep);
        Lir const& lir = lowered.lir;
        auto const p = argReg(lir, t, 0);
        ASSERT_TRUE(p.has_value()) << "no `arg` defines the first pointer";

        // Walk the one block in order: the index of the FIRST write to `*p`,
        // and every read of `*p`.
        LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
        std::optional<std::uint32_t> firstWrite;
        std::vector<std::uint32_t> reads;
        for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
            LirInstId const li = lir.blockInstAt(bb, i);
            if (lir.instOpcode(li) == *writeOp
                && addresses(lir, li, model.writeBase, *p) && !firstWrite) {
                firstWrite = i;
            }
            if (lir.instOpcode(li) == *readOp
                && addresses(lir, li, model.readBase, *p)) {
                reads.push_back(i);
            }
        }
        ASSERT_TRUE(firstWrite.has_value())
            << "the swap writes `*p` — no write through p was found, so the "
               "check below would pass over nothing";
        ASSERT_FALSE(reads.empty())
            << "the swap reads `*p` — no read through p was found";
        for (std::uint32_t const r : reads) {
            EXPECT_LT(r, *firstWrite)
                << "instruction " << r << " reads `*p` AFTER instruction "
                << *firstWrite << " overwrote it: the value loaded from `*p` "
                   "is being read from the object instead of from its copy, "
                   "so `*q = x` stores y's value";
        }
    }
}

// ── (2) a qualifier conversion keeps the address out of the FP registers ────
TEST(LongDoubleValueSemantics, AQualifierConversionKeepsTheAddressOutOfTheFloatRegisters) {
    for (std::size_t mi = 0; mi < 2; ++mi) {
        Model const model = models()[mi];
        SCOPED_TRACE(kModelNames[mi]);
        auto target = TargetSchema::loadShipped(model.target);
        ASSERT_TRUE(target.has_value());
        TargetSchema const& t = **target;
        TypeInterner in{CompilationUnitId{1}};
        DiagnosticReporter rep;
        Mir const m = buildVolatileReadAndBitcast(in, model.kind, model.kind);
        auto lowered = lower(m, t, in, rep, model);
        ASSERT_TRUE(lowered.ok) << inventory(rep);
        Lir const& lir = lowered.lir;
        LirFuncId const fn = lir.funcAt(0);
        std::uint32_t seen = 0;
        for (std::uint32_t b = 0; b < lir.funcBlockCount(fn); ++b) {
            LirBlockId const bb = lir.funcBlockAt(fn, b);
            for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
                LirInstId const li = lir.blockInstAt(bb, i);
                ++seen;
                LirReg const r = lir.instResult(li);
                EXPECT_FALSE(r.valid() && r.regClass() == LirRegClass::FPR)
                    << "instruction " << i << " defines a floating-point "
                       "register: a memory-resident `long double` moves only "
                       "through memory forms and general registers, so this "
                       "is its home ADDRESS taking the class move of a value";
            }
        }
        EXPECT_GE(seen, 4u) << "the function lowered to almost nothing";
    }
}

// ── (3) a bit-cast to another kind is refused by name ───────────────────────
TEST(LongDoubleValueSemantics, ABitcastToAnotherKindIsRefusedByName) {
    for (std::size_t mi = 0; mi < 2; ++mi) {
        Model const model = models()[mi];
        SCOPED_TRACE(kModelNames[mi]);
        auto target = TargetSchema::loadShipped(model.target);
        ASSERT_TRUE(target.has_value());
        TargetSchema const& t = **target;
        TypeInterner in{CompilationUnitId{1}};
        DiagnosticReporter rep;
        Mir const m = buildVolatileReadAndBitcast(in, model.kind, TypeKind::F64);
        auto lowered = lower(m, t, in, rep, model);
        EXPECT_FALSE(lowered.ok) << "a bit-cast of a memory-resident `long "
                                    "double` to F64 must be refused";
        bool named = false;
        for (auto const& d : rep.all()) {
            if (d.code == DiagnosticCode::L_UnsupportedLoweringForOpcode
                && d.actual.find("reinterprets a memory-resident `long double`")
                       != std::string::npos
                && d.actual.find(std::string{"'"}
                                 + std::string{typeKindNameOrEmpty(model.kind)} + "'")
                       != std::string::npos
                && d.actual.find("'F64'") != std::string::npos) {
                named = true;
            }
        }
        EXPECT_TRUE(named) << "the refusal must say what it refused and name "
                              "both kinds: " << inventory(rep);
    }
}

// ── (4)-(7) the boundary of the copy ────────────────────────────────────────
// Each arm lowers one shape on both models and counts the 16-byte homes. The
// numbers are the copy's own: a read in place takes none, every copied Load
// takes one, and a phi adds its home and its staging slot (two).
namespace {

struct HomeCase {
    char const*   what;
    Mir (*build)(TypeInterner&, TypeKind);
    std::uint32_t homes;
    char const*   why;
};

void expectHomes(HomeCase const& c) {
    for (std::size_t mi = 0; mi < 2; ++mi) {
        Model const model = models()[mi];
        SCOPED_TRACE(std::string{kModelNames[mi]} + " — " + c.what);
        auto target = TargetSchema::loadShipped(model.target);
        ASSERT_TRUE(target.has_value());
        TargetSchema const& t = **target;
        TypeInterner in{CompilationUnitId{1}};
        DiagnosticReporter rep;
        Mir const m = c.build(in, model.kind);
        auto lowered = lower(m, t, in, rep, model);
        ASSERT_TRUE(lowered.ok) << inventory(rep);
        EXPECT_EQ(sixteenByteHomes(lowered.lir, t), c.homes) << c.why;
    }
}

Mir buildVolatileSameKind(TypeInterner& in, TypeKind kind) {
    return buildVolatileReadAndBitcast(in, kind, kind);
}

}  // namespace

TEST(LongDoubleValueSemantics, AReadNothingCanPrecedeWithAWriteIsTakenInPlace) {
    expectHomes({"*q = *p", &buildCopyThrough, 0u,
                 "the Load's only use is the next instruction and no write lies "
                 "between them, so the store reads `*p` itself — a copy home "
                 "here is a copy the value semantics never needed"});
}

TEST(LongDoubleValueSemantics, AVolatileReadIsCopiedWhereItHappens) {
    expectHomes({"*q = *(volatile T *)p", &buildVolatileSameKind, 1u,
                 "a volatile read happens where the program performs it (C "
                 "6.7.3), so its Load copies even with nothing in between"});
}

TEST(LongDoubleValueSemantics, AValueUsedInAnotherBlockIsCopied) {
    expectHomes({"x = *p; goto next; next: *q = x", &buildUseInAnotherBlock, 1u,
                 "a use in another block is past whatever that block's "
                 "predecessors may write, so the Load copies"});
}

TEST(LongDoubleValueSemantics, AReadThroughAQualifierConversionIsTheLoadsOwnRead) {
    expectHomes({"x = *p; c = (T)x; *p = *q; *q = c",
                 &buildReadThroughAQualifierConversion, 1u,
                 "the Bitcast NAMES x without reading it — c IS x's home — so "
                 "x is read at `*q = c`, after `*p = *q`, and must copy (1); "
                 "the second Load feeds the very next store (0)"});
}

// The FORWARDER's other half. The pin above proves a Bitcast is not a read at
// its own position (the part-3 miscompile's direction); this one proves its uses
// ARE the Load's uses: with nothing between the Load and the store the Bitcast
// forwards to, the value is read in place. ✔MEASURED 2026-09-21 that the pin
// above cannot see this half — the red-on-disable run that switched forwarding
// off (M46) left it and every other pin of this file green, because a use by a
// non-forwarding, non-reading opcode copies, and a copy was already its answer.
TEST(LongDoubleValueSemantics, AReadForwardedThroughAQualifierConversionIsTakenInPlace) {
    expectHomes({"x = *p; c = (T)x; *q = c",
                 &buildForwardedThroughAQualifierConversion, 0u,
                 "the Bitcast names x's home and forwards the read to `*q = c`, "
                 "the next instruction, with no write between — a copy home "
                 "here means the forwarder's uses stopped being the Load's"});
}

TEST(LongDoubleValueSemantics, AValueLeavingThroughAPhiEdgeAfterAStoreIsCopied) {
    expectHomes({"x = *p; y = *q; *p = y; join: *q = phi(x)",
                 &buildStoreBeforeThePhiEdge, 3u,
                 "x's edge copy runs after `*p = y`, so x copies (1); y feeds "
                 "that very store and is read in place (0); the phi takes its "
                 "home and its staging slot (2)"});
}
