// D-LIR-AAPCS64-CALL-MIXING-LONG-DOUBLE-AND-DOUBLE-ARGS-REFUSED
//
// An AArch64 call passing a binary128 `long double` AND a `float`/`double` was
// REFUSED at the caller: the F128 argument was marshalled into its physical
// argument register by MIR→LIR and left out of the call's operand list, while
// every other argument was placed by `lir_callconv`'s walk after allocation —
// two placers, two cursors, and the second one handed the F128's register to
// the next `double`. The refusal printed "MIR opcode '<deferred>' is not yet
// lowered" (the opcode-name table had no `Call` row). ✔MEASURED at `7df54cc1`:
// six mixes refused at debug and release; aarch64-linux-gnu-gcc 13.3.0 and
// clang 18.1.3 run all six to 42 at -O0 and -O2 under qemu-aarch64.
//
// ★★ THE FIX IS ONE PLACER: the marshalled F128 keeps its POSITION in the
// call's operand list, as the physical register it was loaded into, so
// `lir_callconv`'s ONE `ArgCursors` walk advances over it and hands every later
// FP argument the next register; the MIR→LIR cursor walks the same object the
// same way (`next(class)` per argument, `exhaust` per stacked carrier). And the
// marshal loads carry a register constraint naming every marshalled register,
// so a `double` live across them cannot be allocated the register they write.
//
// THE ARMS, over hand-built MIR (the shared C fixture declares no long-double
// format, so a `long double` never reaches this tier as F128 through it):
//   (A) the operand list: the F128's physical register at the F128's position,
//       the double's register after it — both orders;
//   (B) the marshal's constraint: each burst load names its register written;
//   (C) the EMITTED call site, through wide-call lowering, allocation, rewrite,
//       legalize and callconv: the F128's register last written by its 128-bit
//       load and the double's register by a move of the double — both orders,
//       and an interleaved mix with a float and integer arguments;
//   (D) a binary128 argument past the last FP argument register is STORED ON
//       THE STACK, 16-aligned and 16 wide, by the one outgoing cursor — it was
//       refused by name until P68 round 8
//       (D-LIR-AAPCS64-LONG-DOUBLE-ARG-PAST-V7-REFUSED); plus the cursor's own
//       arithmetic for a scalar wider than the slot;
//   (E) the callee of one reads it by an `arg` stating 128 bits;
//   (F) a `_Complex long double` result is captured whole from q0 AND q1
//       (D-LIR-AAPCS64-COMPLEX-LONG-DOUBLE-RETURN-PIECE-REFUSED);
//   (G) a result piece that still cannot be read is refused naming the
//       register it needs — never the reasonless "is not yet lowered".
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run
// through ctest and never as a bare `.exe`.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lir_wide_call_args.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"

#include <gtest/gtest.h>

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

[[nodiscard]] bool anyTextContains(DiagnosticReporter const& r, std::string_view s) {
    for (auto const& d : r.all()) {
        if (d.actual.find(s) != std::string::npos) return true;
    }
    return false;
}

// The argument kinds a probe passes, in order.
enum class A : std::uint8_t { F128, F64, F32, I32, I64 };

[[nodiscard]] TypeId typeOf(TypeInterner& in, A a) {
    switch (a) {
        case A::F128: return in.primitive(TypeKind::F128);
        case A::F64:  return in.primitive(TypeKind::F64);
        case A::F32:  return in.primitive(TypeKind::F32);
        case A::I32:  return in.primitive(TypeKind::I32);
        case A::I64:  return in.primitive(TypeKind::I64);
    }
    return InvalidType;
}

// `void f(T0 *p0, T1 *p1, …) { g(*p0, *p1, …); }` — every argument LOADED
// from its own pointer parameter, so each one is a real value computed before
// the call (an F128 one is its home, per the LD model), then one direct call to
// symbol 2.
[[nodiscard]] Mir buildCall(TypeInterner& in, std::vector<A> const& args) {
    TypeId const voidT = in.primitive(TypeKind::Void);
    std::vector<TypeId> argTys, ptrTys;
    for (A const a : args) {
        argTys.push_back(typeOf(in, a));
        ptrTys.push_back(in.pointer(argTys.back()));
    }
    TypeId const calleeSig = in.fnSig(argTys, voidT, CallConv::CcSysV);
    TypeId const sig = in.fnSig(ptrTys, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    mb.beginBlock(mb.createBlock(StructCfMarker::EntryBlock));
    std::vector<MirInstId> vals;
    for (std::size_t i = 0; i < args.size(); ++i) {
        MirInstId const p = mb.addArg(static_cast<std::uint32_t>(i), ptrTys[i]);
        std::array<MirInstId, 1> const ld{p};
        vals.push_back(mb.addInst(MirOpcode::Load, ld, argTys[i]));
    }
    std::vector<MirInstId> callOps;
    callOps.push_back(mb.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig)));
    callOps.insert(callOps.end(), vals.begin(), vals.end());
    (void)mb.addInst(MirOpcode::Call, callOps, voidT);
    mb.addReturn(std::nullopt);
    return std::move(mb).finish();
}

struct Lowered {
    TypeInterner       interner{CompilationUnitId{1}};
    DiagnosticReporter reporter;
    MirToLirResult     result;
};

[[nodiscard]] MirToLirResult lowerArm64(Mir const& m, TargetSchema const& t,
                                        TypeInterner const& in,
                                        DiagnosticReporter& rep) {
    // The arm64 softcall library ACTIVE, as the compile pipeline threads it —
    // the F128 memory-resident model this call boundary belongs to.
    return lowerToLir(m, t, in, rep, std::vector<ExternImport>{},
                      ExternCallDispatch::DirectPlt,
                      /*dataImportBinding=*/std::nullopt,
                      /*tlsAccess=*/std::nullopt, /*sehScopes=*/{},
                      /*wideFloatSoftcallLibrary=*/std::optional<std::string>(
                          "libgcc_s.so.1"));
}

// The ONE call instruction in the function's first block (its index).
[[nodiscard]] std::optional<std::uint32_t>
callIndex(Lir const& lir, TargetSchema const& t) {
    auto const callOp = t.opcodeByMnemonic("call");
    if (!callOp.has_value()) return std::nullopt;
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    std::optional<std::uint32_t> found;
    for (std::uint32_t i = 0; i < lir.blockInstCount(bb); ++i) {
        if (lir.instOpcode(lir.blockInstAt(bb, i)) == *callOp) found = i;
    }
    return found;
}

// The LAST instruction before the call (same block) whose result is physical
// register `ord` of class FPR — what that argument register holds at the call.
[[nodiscard]] std::optional<LirInstId>
lastWriterBeforeCall(Lir const& lir, std::uint32_t callIdx, std::uint16_t ord) {
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    std::optional<LirInstId> w;
    for (std::uint32_t i = 0; i < callIdx; ++i) {
        LirInstId const li = lir.blockInstAt(bb, i);
        LirReg const r = lir.instResult(li);
        if (r.valid() && r.isPhysical != 0u && r.regClass() == LirRegClass::FPR
            && r.id == ord) {
            w = li;
        }
    }
    return w;
}

// The emitted stream of `m`: the compile pipeline's own order from MIR→LIR to
// the materialized call site, under the arm64 primary convention (aapcs64).
[[nodiscard]] std::optional<Lir> emitted(Mir const& m, TargetSchema const& t,
                                         TypeInterner const& in,
                                         DiagnosticReporter& rep) {
    auto lowered = lowerArm64(m, t, in, rep);
    if (!lowered.ok) return std::nullopt;
    auto wide = lowerWideCallArgs(lowered.lir, t, 0, rep);
    if (!wide.ok) return std::nullopt;
    auto const liveness = analyzeLiveness(wide.lir);
    auto const alloc = allocateRegisters(wide.lir, t, liveness, 0, rep);
    if (!alloc.ok()) return std::nullopt;
    auto rewritten = rewriteWithAllocation(wide.lir, t, alloc, rep);
    if (!rewritten.ok) return std::nullopt;
    auto legal = legalizeTwoAddress(rewritten.lir, t, rep);
    if (!legal.ok()) return std::nullopt;
    auto cc = materializeCallingConvention(legal.lir, t, alloc, rep);
    if (!cc.ok()) return std::nullopt;
    return std::move(cc.lir);
}

struct Arm64Regs {
    std::uint16_t v[8]{};
    std::uint16_t fldur = 0;
};

[[nodiscard]] Arm64Regs regsOf(TargetSchema const& t) {
    Arm64Regs r;
    for (int i = 0; i < 8; ++i) {
        auto const o = t.registerByName("v" + std::to_string(i));
        EXPECT_TRUE(o.has_value());
        r.v[i] = o.value_or(0);
    }
    auto const f = t.opcodeByMnemonic("fldur");
    EXPECT_TRUE(f.has_value());
    r.fldur = f.value_or(0);
    return r;
}

// Assert argument register `slot` holds, at the call, the F128 marshal (a
// 128-bit `fldur`) or — for a `float`/`double` — something OTHER than a
// 128-bit load (the double's own move or reload).
void expectSlotHolds(Lir const& lir, std::uint32_t callIdx, Arm64Regs const& r,
                     int slot, bool f128, std::string const& what) {
    auto const w = lastWriterBeforeCall(lir, callIdx, r.v[slot]);
    ASSERT_TRUE(w.has_value())
        << what << ": nothing writes v" << slot << " before the call";
    bool const is128Load = lir.instOpcode(*w) == r.fldur
        && lirInstWidthBits(lir.instFlags(*w)) == 128;
    if (f128) {
        EXPECT_TRUE(is128Load)
            << what << ": v" << slot << " must hold the binary128 argument, "
               "loaded at 128 bits from its home";
    } else {
        EXPECT_FALSE(is128Load)
            << what << ": v" << slot << " holds a 128-bit load where the "
               "float/double argument belongs — the two placers handed one "
               "register to two arguments";
    }
}

} // namespace

// ── (A) + (B): the operand list and the marshal's constraint ────────────────
TEST(Aapcs64MixedFpCall, TheLongDoubleKeepsItsPositionInTheCallsOperands) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    Arm64Regs const r = regsOf(t);
    for (bool f128First : {true, false}) {
        Lowered L;
        Mir const m = buildCall(L.interner, f128First
                                                ? std::vector<A>{A::F128, A::F64}
                                                : std::vector<A>{A::F64, A::F128});
        L.result = lowerArm64(m, t, L.interner, L.reporter);
        ASSERT_TRUE(L.result.ok)
            << "a call mixing a binary128 and a double must LOWER — both "
               "references run it: " << inventory(L.reporter);
        Lir const& lir = L.result.lir;
        auto const ci = callIndex(lir, t);
        ASSERT_TRUE(ci.has_value());
        LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
        LirInstId const call = lir.blockInstAt(bb, *ci);
        auto const ops = lir.instOperands(call);
        ASSERT_EQ(ops.size(), 3u) << "callee + two arguments, in order";
        std::size_t const f128Pos = f128First ? 1u : 2u;
        std::size_t const f64Pos  = f128First ? 2u : 1u;
        int const f128Slot = f128First ? 0 : 1;
        ASSERT_EQ(ops[f128Pos].kind, LirOperandKind::Reg);
        EXPECT_TRUE(ops[f128Pos].reg.isPhysical != 0u
                    && ops[f128Pos].reg.id == r.v[f128Slot])
            << "the binary128 argument must stand in the operand list at ITS "
               "position, as the register it was marshalled into (v"
            << f128Slot << ") — left out, `lir_callconv`'s walk hands that "
               "register to the next double";
        ASSERT_EQ(ops[f64Pos].kind, LirOperandKind::Reg);
        EXPECT_EQ(ops[f64Pos].reg.isPhysical, 0u)
            << "the double is placed after allocation, by the one walk";
        // (B) the marshal load right before the call names its register written
        LirInstId const marshal = lir.blockInstAt(bb, *ci - 1);
        EXPECT_EQ(lir.instOpcode(marshal), r.fldur);
        auto const* c = lir.instRegConstraints(marshal);
        ASSERT_NE(c, nullptr)
            << "the marshal load must carry a register constraint — a double "
               "live across it could otherwise be allocated the register it "
               "overwrites";
        bool names = false;
        for (auto const o : c->outputOrdinals) names = names || o == r.v[f128Slot];
        EXPECT_TRUE(names) << "the constraint must name v" << f128Slot
                           << " as written";
    }
}

// ── (C) the emitted call site ───────────────────────────────────────────────
TEST(Aapcs64MixedFpCall, EveryArgumentReachesItsOwnRegisterAtTheCall) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    Arm64Regs const r = regsOf(t);
    struct Case {
        std::vector<A> args;
        std::vector<std::pair<int, bool>> slots;   // (FP register, is F128)
        char const* what;
    };
    std::vector<Case> const cases{
        {{A::F128, A::F64}, {{0, true}, {1, false}}, "second(long double, double)"},
        {{A::F64, A::F128}, {{0, false}, {1, true}}, "first(double, long double)"},
        {{A::F128, A::F32, A::I32, A::F64, A::F128, A::I64, A::F64},
         {{0, true}, {1, false}, {2, false}, {3, true}, {4, false}},
         "mix(long double, float, int, double, long double, long, double)"},
    };
    for (auto const& c : cases) {
        TypeInterner in{CompilationUnitId{1}};
        DiagnosticReporter rep;
        Mir const m = buildCall(in, c.args);
        auto const lir = emitted(m, t, in, rep);
        ASSERT_TRUE(lir.has_value()) << c.what << ": " << inventory(rep);
        auto const ci = callIndex(*lir, t);
        ASSERT_TRUE(ci.has_value()) << c.what;
        for (auto const& [slot, isF128] : c.slots) {
            expectSlotHolds(*lir, *ci, r, slot, isF128, c.what);
        }
    }
}

// ── (D) past the last FP argument register: ON THE STACK, 16-aligned, 16 wide ─
//
// ★★ P68 round 8 (D-LIR-AAPCS64-LONG-DOUBLE-ARG-PAST-V7-REFUSED). This arm
// used to pin the REFUSAL ("belongs on the STACK — a placement this lowering
// does not realize"); aarch64-linux-gnu-gcc 13.3.0 and clang 18.1.3 run all
// three probes of that shape to 42, so the refusal is now the placement. Each
// case reads the `store_outgoing_arg`s `lowerWideCallArgs` emits — the byte
// offset and the width the ONE outgoing cursor chose.
TEST(Aapcs64MixedFpCall, ABinary128PastTheLastFpArgumentRegisterIsStoredOnTheStack) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    auto const storeOut = t.opcodeByMnemonic("store_outgoing_arg");
    ASSERT_TRUE(storeOut.has_value());
    std::vector<A> const eight(8, A::F64);
    struct Placed { std::uint32_t offset; std::uint32_t widthBits; };
    struct Case { std::vector<A> args; std::vector<Placed> stores; char const* what; };
    auto with = [&eight](std::vector<A> tail) {
        std::vector<A> v = eight;
        v.insert(v.end(), tail.begin(), tail.end());
        return v;
    };
    std::vector<Case> const cases{
        {with({A::F128}), {{0, 128}}, "eight doubles, then a long double"},
        {with({A::F128, A::F64}), {{0, 128}, {16, 64}},
         "a stacked double AFTER a stacked long double skips its 16 bytes"},
        {with({A::F64, A::F128}), {{0, 64}, {16, 128}},
         "a stacked long double after one 8-byte slot is aligned up to 16"},
        {std::vector<A>(9, A::F128), {{0, 128}}, "nine long doubles"},
    };
    for (auto const& c : cases) {
        TypeInterner in{CompilationUnitId{1}};
        DiagnosticReporter rep;
        Mir const m = buildCall(in, c.args);
        auto lowered = lowerArm64(m, t, in, rep);
        ASSERT_TRUE(lowered.ok) << c.what << ": " << inventory(rep);
        EXPECT_FALSE(anyTextContains(rep, "<deferred>")) << c.what;
        auto wide = lowerWideCallArgs(lowered.lir, t, 0, rep);
        ASSERT_TRUE(wide.ok) << c.what << ": " << inventory(rep);
        std::vector<Placed> got;
        LirBlockId const bb = wide.lir.funcBlockAt(wide.lir.funcAt(0), 0);
        for (std::uint32_t i = 0; i < wide.lir.blockInstCount(bb); ++i) {
            LirInstId const li = wide.lir.blockInstAt(bb, i);
            if (wide.lir.instOpcode(li) != *storeOut) continue;
            got.push_back({wide.lir.instPayload(li),
                           lirInstWidthBits(wide.lir.instFlags(li))});
        }
        ASSERT_EQ(got.size(), c.stores.size()) << c.what << ": " << inventory(rep);
        for (std::size_t k = 0; k < got.size(); ++k) {
            EXPECT_EQ(got[k].offset, c.stores[k].offset)
                << c.what << ": stacked argument " << k << " at the wrong byte";
            EXPECT_EQ(got[k].widthBits, c.stores[k].widthBits)
                << c.what << ": stacked argument " << k << " stored at the wrong "
                   "width (a binary128 stored at 64 bits leaves half of it behind)";
        }
        // The whole chain materializes it (the stores placed, the frame sized).
        DiagnosticReporter rep2;
        auto const lir = emitted(m, t, in, rep2);
        EXPECT_TRUE(lir.has_value()) << c.what << ": " << inventory(rep2);
    }
}

// The cursor itself: a 16-byte scalar is aligned by the convention's SCALAR
// cap, occupies 16 bytes, and is read and written at 128 bits.
TEST(Aapcs64MixedFpCall, TheStackCursorPlacesASixteenByteScalarAlignedAndWhole) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto const* cc = (**target).callingConvention(0);
    ASSERT_NE(cc, nullptr);
    StackArgCursor c{*cc, 8};
    auto const a = c.placeNamedScalar(8);
    auto const b = c.placeNamedScalar(16);
    auto const d = c.placeNamedScalar(8);
    EXPECT_EQ(a.byteOffset, 0u);
    EXPECT_EQ(b.byteOffset, 16u) << "aligned up to 16 after one 8-byte slot";
    EXPECT_EQ(lirInstWidthBits(b.widthFlags), 128u) << "moved whole";
    EXPECT_EQ(d.byteOffset, 32u) << "the next argument follows ALL 16 bytes";
    // THE CONTROL: an 8-byte scalar keeps the classic slot and the default width.
    EXPECT_EQ(lirInstWidthBits(a.widthFlags), 64u);
}

// ── (E) the CALLEE of a stacked binary128 ───────────────────────────────────
//
// `void f(double ×8, long double l, long double *out) { *out = l; }` — `l` is
// the ninth FP argument, so it arrives on the stack. Its MIR `Arg` carries FP
// ordinal 8 (past the pool), and the LIR `arg` that reads it must state the
// datum's own width — 128 bits — because that width is what the incoming
// cursor places it by (aligned 16, 16 bytes) and what the read moves.
// ✔MEASURED before the fix: the callee refused with "MIR opcode 'Arg' is not
// yet lowered to target 'arm64'".
namespace {
[[nodiscard]] Mir buildStackedF128Callee(TypeInterner& in) {
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const f64 = in.primitive(TypeKind::F64);
    TypeId const f128 = in.primitive(TypeKind::F128);
    TypeId const p128 = in.pointer(f128);
    std::vector<TypeId> params(8, f64);
    params.push_back(f128);
    params.push_back(p128);
    TypeId const sig = in.fnSig(params, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    mb.beginBlock(mb.createBlock(StructCfMarker::EntryBlock));
    for (std::uint32_t i = 0; i < 8; ++i) (void)mb.addArg(i, f64);
    MirInstId const l = mb.addArg(8, f128);      // FP ordinal 8: past v7
    MirInstId const out = mb.addArg(0, p128);    // GPR ordinal 0: x0
    std::array<MirInstId, 2> const st{l, out};
    (void)mb.addInst(MirOpcode::Store, st, InvalidType);
    mb.addReturn(std::nullopt);
    return std::move(mb).finish();
}
} // namespace

TEST(Aapcs64MixedFpCall, AStackedBinary128ParameterIsReadWholeFromItsSlot) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    auto const argOp = t.opcodeByMnemonic("arg");
    ASSERT_TRUE(argOp.has_value());
    TypeInterner in{CompilationUnitId{1}};
    DiagnosticReporter rep;
    Mir const m = buildStackedF128Callee(in);
    auto lowered = lowerArm64(m, t, in, rep);
    ASSERT_TRUE(lowered.ok)
        << "a callee taking a binary128 past v7 must LOWER — both references "
           "run it: " << inventory(rep);
    EXPECT_FALSE(anyTextContains(rep, "not yet lowered")) << inventory(rep);
    std::optional<LirInstId> stacked;
    LirBlockId const bb = lowered.lir.funcBlockAt(lowered.lir.funcAt(0), 0);
    for (std::uint32_t i = 0; i < lowered.lir.blockInstCount(bb); ++i) {
        LirInstId const li = lowered.lir.blockInstAt(bb, i);
        if (lowered.lir.instOpcode(li) == *argOp && lowered.lir.instPayload(li) == 8u
            && lowered.lir.instResult(li).regClass() == LirRegClass::FPR) {
            stacked = li;
        }
    }
    ASSERT_TRUE(stacked.has_value())
        << "the ninth FP argument must be read by an `arg` of the FP class";
    EXPECT_EQ(lirInstWidthBits(lowered.lir.instFlags(*stacked)), 128u)
        << "the stacked binary128 must be read at its own width — at the "
           "width-default the cursor would place it as an 8-byte slot and the "
           "read would move half of it";
    // The whole chain materializes it (the incoming read placed by the cursor).
    DiagnosticReporter rep2;
    auto const lir = emitted(m, t, in, rep2);
    ASSERT_TRUE(lir.has_value()) << inventory(rep2);
}

// ── (F) a `_Complex long double` RESULT captured from q0 AND q1 ──────────────
//
// A two-member binary128 HFA returns in v0:v1. MIR states it as the call's own
// F128 result (piece 0) plus `ReturnPiece(call, 1)`; the caller must store v1
// into a home at 128 bits right after the call. ✔MEASURED before the fix: the
// piece was refused as "MIR opcode 'returnpiece' is not yet lowered".
namespace {
[[nodiscard]] Mir buildComplexF128Caller(TypeInterner& in) {
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const f128 = in.primitive(TypeKind::F128);
    TypeId const p128 = in.pointer(f128);
    TypeId const calleeSig = in.fnSig(std::vector<TypeId>{}, f128, CallConv::CcSysV);
    TypeId const sig = in.fnSig(std::vector<TypeId>{p128, p128}, voidT,
                                CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    mb.beginBlock(mb.createBlock(StructCfMarker::EntryBlock));
    MirInstId const re = mb.addArg(0, p128);
    MirInstId const im = mb.addArg(1, p128);
    std::array<MirInstId, 1> const callOps{
        mb.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig))};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, f128);
    MirInstId const piece = mb.addReturnPiece(call, 1, TargetRegClass::FPR, f128);
    std::array<MirInstId, 2> const st0{call, re};
    (void)mb.addInst(MirOpcode::Store, st0, InvalidType);
    std::array<MirInstId, 2> const st1{piece, im};
    (void)mb.addInst(MirOpcode::Store, st1, InvalidType);
    mb.addReturn(std::nullopt);
    return std::move(mb).finish();
}
} // namespace

TEST(Aapcs64MixedFpCall, AComplexBinary128ResultIsCapturedFromBothQRegisters) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    Arm64Regs const r = regsOf(t);
    auto const fstur = t.opcodeByMnemonic("fstur");
    ASSERT_TRUE(fstur.has_value());
    TypeInterner in{CompilationUnitId{1}};
    DiagnosticReporter rep;
    Mir const m = buildComplexF128Caller(in);
    auto lowered = lowerArm64(m, t, in, rep);
    ASSERT_TRUE(lowered.ok)
        << "a caller of a function returning `_Complex long double` must "
           "LOWER — both references run it: " << inventory(rep);
    bool v0Captured = false, v1Captured = false;
    LirBlockId const bb = lowered.lir.funcBlockAt(lowered.lir.funcAt(0), 0);
    for (std::uint32_t i = 0; i < lowered.lir.blockInstCount(bb); ++i) {
        LirInstId const li = lowered.lir.blockInstAt(bb, i);
        if (lowered.lir.instOpcode(li) != *fstur) continue;
        if (lirInstWidthBits(lowered.lir.instFlags(li)) != 128u) continue;
        auto const ops = lowered.lir.instOperands(li);
        if (ops.empty() || ops[0].kind != LirOperandKind::Reg
            || ops[0].reg.isPhysical == 0u) continue;
        if (ops[0].reg.id == r.v[0]) v0Captured = true;
        if (ops[0].reg.id == r.v[1]) v1Captured = true;
    }
    EXPECT_TRUE(v0Captured) << "the real half (q0) must be stored whole";
    EXPECT_TRUE(v1Captured)
        << "the imaginary half (q1, return piece 1) must be stored whole at "
           "128 bits — a scalar piece capture reads 64";
    DiagnosticReporter rep2;
    auto const lir = emitted(m, t, in, rep2);
    ASSERT_TRUE(lir.has_value()) << inventory(rep2);
}

// ── (G) a refusal names what failed — never "is not yet lowered" ────────────
//
// The class of message the '<deferred>' name belonged to (P68 round 8,
// D-LIR-REFUSAL-SAYS-AN-OPCODE-IS-NOT-LOWERED-AND-NAMES-NOTHING, which closed
// by making every such refusal name what failed). A piece that names a return
// register the convention does not declare is refused, and rightly: there is
// no register to read it from. What that row's closure changed is the message,
// which now says WHICH register of WHICH convention (✔MEASURED 2026-09-22 by
// this test).
TEST(Aapcs64MixedFpCall, ARefusedResultPieceNamesTheRegisterItNeeds) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    TargetSchema const& t = **target;
    auto const* cc = t.callingConvention(0);
    ASSERT_NE(cc, nullptr);
    TypeInterner in{CompilationUnitId{1}};
    TypeId const voidT = in.primitive(TypeKind::Void);
    TypeId const f128 = in.primitive(TypeKind::F128);
    TypeId const p128 = in.pointer(f128);
    TypeId const calleeSig = in.fnSig(std::vector<TypeId>{}, f128, CallConv::CcSysV);
    TypeId const sig = in.fnSig(std::vector<TypeId>{p128}, voidT, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{1});
    mb.beginBlock(mb.createBlock(StructCfMarker::EntryBlock));
    MirInstId const out = mb.addArg(0, p128);
    std::array<MirInstId, 1> const callOps{
        mb.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig))};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, f128);
    // a piece ordinal past every FP return register the convention declares
    auto const past = static_cast<std::uint32_t>(cc->returnFprs.size());
    MirInstId const piece = mb.addReturnPiece(call, past, TargetRegClass::FPR, f128);
    std::array<MirInstId, 2> const st{piece, out};
    (void)mb.addInst(MirOpcode::Store, st, InvalidType);
    mb.addReturn(std::nullopt);
    Mir const m = std::move(mb).finish();
    DiagnosticReporter rep;
    auto lowered = lowerArm64(m, t, in, rep);
    EXPECT_FALSE(lowered.ok);
    EXPECT_TRUE(anyTextContains(rep, "FP return register"))
        << "the refusal must name the register it needed: " << inventory(rep);
    EXPECT_FALSE(anyTextContains(rep, "not yet lowered"))
        << "never the reasonless form: " << inventory(rep);
}
