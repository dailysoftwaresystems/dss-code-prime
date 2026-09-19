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
//   (D) a binary128 argument past the last FP argument register is REFUSED BY
//       NAME (its AAPCS64 stack placement is not realized) — never '<deferred>'.
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

// ── (D) past the last FP argument register: refused by name ──────────────────
TEST(Aapcs64MixedFpCall, ABinary128PastTheLastFpArgumentRegisterIsRefusedByName) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    // eight doubles fill v0..v7; the binary128 after them belongs on the stack
    Lowered L;
    Mir const m = buildCall(L.interner, {A::F64, A::F64, A::F64, A::F64, A::F64,
                                         A::F64, A::F64, A::F64, A::F128});
    L.result = lowerArm64(m, **target, L.interner, L.reporter);
    EXPECT_FALSE(L.result.ok);
    EXPECT_TRUE(anyTextContains(L.reporter, "belongs on the STACK"))
        << inventory(L.reporter);
    EXPECT_FALSE(anyTextContains(L.reporter, "<deferred>"))
        << "a refusal must name its reason: " << inventory(L.reporter);
}
