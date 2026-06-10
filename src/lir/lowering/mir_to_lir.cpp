#include "lir/lowering/mir_to_lir.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "mir/mir_opcode.hpp"

#include <array>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace dss {

namespace {

// Symbolic name of a MIR opcode, for diagnostic messages. Centralised so a
// future addition to `MirOpcode` is a one-line update; the lowerer's
// per-opcode dispatch is the authoritative consumer.
[[nodiscard]] std::string_view mirOpcodeName(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::Invalid:       return "Invalid";
        case MirOpcode::Arg:           return "Arg";
        case MirOpcode::Const:         return "Const";
        case MirOpcode::GlobalAddr:    return "GlobalAddr";
        case MirOpcode::Add:           return "Add";
        case MirOpcode::Sub:           return "Sub";
        case MirOpcode::Mul:           return "Mul";
        case MirOpcode::ICmpEq:        return "ICmpEq";
        case MirOpcode::ICmpNe:        return "ICmpNe";
        case MirOpcode::ICmpSlt:       return "ICmpSlt";
        case MirOpcode::ICmpSle:       return "ICmpSle";
        case MirOpcode::ICmpSgt:       return "ICmpSgt";
        case MirOpcode::ICmpSge:       return "ICmpSge";
        case MirOpcode::ICmpUlt:       return "ICmpUlt";
        case MirOpcode::ICmpUle:       return "ICmpUle";
        case MirOpcode::ICmpUgt:       return "ICmpUgt";
        case MirOpcode::ICmpUge:       return "ICmpUge";
        case MirOpcode::Br:            return "Br";
        case MirOpcode::CondBr:        return "CondBr";
        case MirOpcode::Switch:        return "Switch";
        case MirOpcode::Phi:           return "Phi";
        case MirOpcode::Return:        return "Return";
        case MirOpcode::Unreachable:   return "Unreachable";
        default:                       return "<deferred>";
    }
}

// Map a MIR ICmp opcode to the universal `TargetCondCode` carried in the
// LIR `setcc` / `jcc` payload. Target-blind: every register-machine
// backend (x86_64, ARM64, RISC-V) has these conditions natively; the
// assembler maps the enum to the physical encoding.
[[nodiscard]] std::optional<TargetCondCode> condCodeForICmp(MirOpcode op) noexcept {
    switch (op) {
        case MirOpcode::ICmpEq:  return TargetCondCode::Eq;
        case MirOpcode::ICmpNe:  return TargetCondCode::Ne;
        case MirOpcode::ICmpSlt: return TargetCondCode::Slt;
        case MirOpcode::ICmpSle: return TargetCondCode::Sle;
        case MirOpcode::ICmpSgt: return TargetCondCode::Sgt;
        case MirOpcode::ICmpSge: return TargetCondCode::Sge;
        case MirOpcode::ICmpUlt: return TargetCondCode::Ult;
        case MirOpcode::ICmpUle: return TargetCondCode::Ule;
        case MirOpcode::ICmpUgt: return TargetCondCode::Ugt;
        case MirOpcode::ICmpUge: return TargetCondCode::Uge;
        default:                 return std::nullopt;
    }
}

[[nodiscard]] bool isMirTerminator(MirOpcode op) noexcept {
    return op == MirOpcode::Br
        || op == MirOpcode::CondBr
        || op == MirOpcode::Switch
        || op == MirOpcode::Return
        || op == MirOpcode::Unreachable;
}

// Single source of truth for the lowerer's opcode-mnemonic cache. The
// enum entry order MUST match `kMnemonicTable` below; the static_assert
// pins the alignment.
//
// Cycle 3c collapsed the cycle-3a-pattern of three parallel sequences
// (enum / optional<uint16_t> fields / bool array) into one indexable
// array — the type-design agent's recommendation closing the silent-
// drift hazard between the three. Now every new opcode = one enum row
// + one table row, never a missing-bool flag.
enum class MnemonicSlot : std::uint8_t {
    Arg, Mov, Add, Sub, Mul,
    // D-CSUBSET-DIVISION-OP-CODEGEN (cycle 10r split, 2026-06-04):
    // signed/unsigned divide pre+core slots. Cycle 10q used a single
    // compound slot with packed `[REX.W, 0x99, REX.W, 0xF7]` opcode
    // bytes — but the encoder auto-emits ONE REX prefix at the start
    // computed from operand high bits (e.g. REX.B for R14). The
    // embedded literal second 0x48 (REX.W only); per x86 decode the
    // LAST REX prefix before the opcode wins → the auto-REX 0x41 was
    // REX.B → IDIV operand-low-3-bits=6 decodes as RSI (without REX.B)
    // instead of R14 → divide-by-zero trap. Cycle 10r splits into pre
    // (CQO / XOR-RDX) and core (IDIV /7 / DIV /6) so each instruction
    // gets its own auto-REX. FLAG 1 silent-miscompile guard preserved:
    // SDiv routes pre=cqo + core=idiv_op; UDiv routes pre=xor_rdx_zero
    // + core=div_op. ARM64 closure (single 3-addr SDIV/UDIV with no
    // pre + no implicit RDX:RAX) is anchored
    // D-MIR-TO-LIR-DIV-SEQUENCE-AGNOSTIC.
    SDivPre, SDivCore, UDivPre, UDivCore,
    // D-CSUBSET-DIVISION-OP-CODEGEN type-fold (cycle 10r post-7-agent
    // review, 2026-06-04): the four divide slots above are PAIRED
    // (pre + core). The `DivSlotPair` struct below makes the pairing
    // a type-level invariant so callers cannot transpose pre/core
    // arguments to `lowerDiv` (a silent-miscompile vector that would
    // emit IDIV before CQO → reads uninitialized RDX → nondeterministic
    // quotient or trap, with no diagnostic). The two `constexpr`
    // instances `kSDivPair` / `kUDivPair` define the only valid pairs
    // the lowering dispatch can use.
    Cmp, Setcc, Jmp, Jcc,
    Ret, Unreachable,
    Alloca, Load, Store, Lea,        // cycle 3c memory ops
    SExt, ZExt, Trunc,                // cycle 3c cast lowering
    // cycle 3d bitwise + integer Neg
    And, Or, Xor, Shl, ShrL, ShrA, Not, Neg,
    // cycle 3d float arithmetic + casts
    FAdd, FSub, FMul, FDiv, FNeg,
    FpCvt, FpToSi, FpToUi, SiToFp, UiToFp,
    // cycle 3d cross-class move (movq via FPR↔GPR Bitcast)
    MovqXClass,
    // cycle 3e: Call + IntrinsicCall (GlobalAddr reuses Mov via SymbolRef)
    Call, IntrinsicCall,
    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY post-fold (2026-06-02): indirect
    // call through an extern-import IAT/GOT slot. x86_64 PE = `FF 15
    // disp32` (CALL [RIP+disp32]); the linker patches disp32 to the IAT
    // slot RVA. Direct `call` (E8 disp32) would execute the IAT slot's
    // BYTES as code rather than dereferencing it for the callee's
    // address — the 0xC0000005 puts SEGV had two layers: missing
    // shadow space AND wrong call opcode. Same schema opcode the
    // trampoline emitter uses for ExitProcess.
    CallIndirectViaExtern,
    // FC1 (V2-4.X full-C, 2026-06-10): NATIVE result-bearing division /
    // remainder verbs — Rule 1 of the capability-driven div/mod
    // realization (closes D-MIR-TO-LIR-DIV-SEQUENCE-AGNOSTIC). A target
    // that declares one of these mnemonics as a `result: value` opcode
    // gets a single 3-address LIR op (arm64 SDIV/UDIV; a future RISC-V
    // DIV/DIVU/REM/REMU hits all four). A target without them falls
    // back to the implicit-register pair (x86 cqo+idiv_op /
    // xor_rdx_zero+div_op) or — remainder only — the generic
    // rem = n − (n/d)·d expansion over div + mul + sub.
    SDivNative, UDivNative, SModNative, UModNative,
    Count_
};
constexpr std::size_t kMnemonicCount = static_cast<std::size_t>(MnemonicSlot::Count_);

// D-CSUBSET-DIVISION-OP-CODEGEN type-fold (cycle 10r 7-agent review,
// 2026-06-04): named-aggregate enforcing the (pre, core) ordering at
// the type system level. The MIR→LIR Div arm dispatches via these
// constants — there is no path for a caller to pass an arbitrary
// (MnemonicSlot, MnemonicSlot) pair to `lowerDiv`. ARM64 will add its
// own DivSlotPair (likely `{Invalid, Sdiv}` with a pre-op-absent
// sentinel) when the 2nd target lands; the substrate already
// supports that via `lowerDiv`'s preOp-absent fail-loud below — see
// D-MIR-TO-LIR-DIV-SEQUENCE-AGNOSTIC.
struct DivSlotPair {
    MnemonicSlot pre;
    MnemonicSlot core;
};
constexpr DivSlotPair kSDivPair{MnemonicSlot::SDivPre,  MnemonicSlot::SDivCore};
constexpr DivSlotPair kUDivPair{MnemonicSlot::UDivPre,  MnemonicSlot::UDivCore};

struct MnemonicCache {
    std::string_view             mnemonic;
    std::optional<std::uint16_t> id;
    bool                         missingReported = false;
};

// Per-row paired table closing the cycle-3d-anchored swap-silent-
// corruption hazard. Each row pins `slot ↔ mnemonic`; the consteval
// alignment check below enforces `kMnemonicRows[i].slot ==
// MnemonicSlot(i)` at compile time — a transposed entry now FAILS the
// build rather than silently misrouting every subsequent slot.
struct MnemonicRow { MnemonicSlot slot; std::string_view mnemonic; };
constexpr std::array<MnemonicRow, kMnemonicCount> kMnemonicRows{{
    {MnemonicSlot::Arg,           "arg"},
    {MnemonicSlot::Mov,           "mov"},
    {MnemonicSlot::Add,           "add"},
    {MnemonicSlot::Sub,           "sub"},
    {MnemonicSlot::Mul,           "mul"},
    {MnemonicSlot::SDivPre,       "cqo"},
    {MnemonicSlot::SDivCore,      "idiv_op"},
    {MnemonicSlot::UDivPre,       "xor_rdx_zero"},
    {MnemonicSlot::UDivCore,      "div_op"},
    {MnemonicSlot::Cmp,           "cmp"},
    {MnemonicSlot::Setcc,         "setcc"},
    {MnemonicSlot::Jmp,           "jmp"},
    {MnemonicSlot::Jcc,           "jcc"},
    {MnemonicSlot::Ret,           "ret"},
    {MnemonicSlot::Unreachable,   "unreachable"},
    {MnemonicSlot::Alloca,        "alloca"},
    {MnemonicSlot::Load,          "load"},
    {MnemonicSlot::Store,         "store"},
    {MnemonicSlot::Lea,           "lea"},
    {MnemonicSlot::SExt,          "sext"},
    {MnemonicSlot::ZExt,          "zext"},
    {MnemonicSlot::Trunc,         "trunc"},
    {MnemonicSlot::And,           "and"},
    {MnemonicSlot::Or,            "or"},
    {MnemonicSlot::Xor,           "xor"},
    {MnemonicSlot::Shl,           "shl"},
    {MnemonicSlot::ShrL,          "shr_l"},
    {MnemonicSlot::ShrA,          "shr_a"},
    {MnemonicSlot::Not,           "not"},
    {MnemonicSlot::Neg,           "neg"},
    {MnemonicSlot::FAdd,          "fadd"},
    {MnemonicSlot::FSub,          "fsub"},
    {MnemonicSlot::FMul,          "fmul"},
    {MnemonicSlot::FDiv,          "fdiv"},
    {MnemonicSlot::FNeg,          "fneg"},
    {MnemonicSlot::FpCvt,         "fpcvt"},
    {MnemonicSlot::FpToSi,        "fp_to_si"},
    {MnemonicSlot::FpToUi,        "fp_to_ui"},
    {MnemonicSlot::SiToFp,        "si_to_fp"},
    {MnemonicSlot::UiToFp,        "ui_to_fp"},
    {MnemonicSlot::MovqXClass,    "movq_xclass"},
    {MnemonicSlot::Call,           "call"},
    {MnemonicSlot::IntrinsicCall,  "intrinsic_call"},
    {MnemonicSlot::CallIndirectViaExtern, "call_indirect_via_extern"},
    {MnemonicSlot::SDivNative,    "sdiv"},
    {MnemonicSlot::UDivNative,    "udiv"},
    {MnemonicSlot::SModNative,    "smod"},
    {MnemonicSlot::UModNative,    "umod"},
}};
consteval bool kMnemonicRowsAligned() noexcept {
    for (std::size_t i = 0; i < kMnemonicRows.size(); ++i) {
        if (kMnemonicRows[i].slot != static_cast<MnemonicSlot>(i)) return false;
    }
    return true;
}
static_assert(kMnemonicRows.size() == kMnemonicCount,
    "MnemonicSlot enum and kMnemonicRows table drifted in COUNT");
static_assert(kMnemonicRowsAligned(),
    "MnemonicSlot rows out of order — kMnemonicRows[i].slot must equal "
    "MnemonicSlot(i) for every i (cycle-3d swap-silent-corruption hazard).");

// One transient lowerer per `lowerToLir` call. Holds the per-pass state
// the dispatcher methods read/write. Mirrors `Lowerer` in `hir_to_mir.cpp`.
struct Lowerer {
    Mir const&          mir;
    TargetSchema const& target;
    TypeInterner const& interner;
    DiagnosticReporter& reporter;
    LirBuilder          lir;

    // Single table of cached opcode ids. `kMnemonics[i].mnemonic` is the
    // JSON-side name, `cache_[i].id` is the resolved numeric opcode (or
    // nullopt when the target schema omits it), `cache_[i].missingReported`
    // gates the one-shot diagnostic.
    std::array<MnemonicCache, kMnemonicCount> cache_;

    // FC2 Part B: per-(register-class, op) opcode handles resolved from
    // the target's `registerClassOps[]` table (with the GPR universal
    // defaults — see TargetSchema::regClassOpOpcode). `nullopt` = the
    // class has no such operation; consumers fail loud at the use site
    // (a class/op combination a module never emits must not fail the
    // whole lowering at construction). Generalizes the lowerBitcast
    // class-dispatch pattern to EVERY mov/load/store emission.
    std::array<std::array<std::optional<std::uint16_t>, kRegClassOpCount>, 5>
        classOpCache_{};

    // Per-function state. Cleared at the top of `lowerFunction`.
    MirAttribute<LirReg>            valueToReg;
    MirBlockAttribute<LirBlockId>   mirBlockToLirBlock;

    // Module-tier (NOT per-function) reverse-mapping LirInstId.v →
    // MirInstId. Grows as the lowerer emits LIR insts (slot 0 is the
    // arena's invalid sentinel, default-initialized `InvalidMirInst`).
    // The LirVerifier consumes this to cross-reference LIR vregs
    // against MIR types WITHOUT the cycle-3e positional-alignment
    // hazard that silently skipped switch-bearing functions.
    std::vector<MirInstId>          lirToMir;
    // Current MIR inst being lowered. Set by `lowerInst` /
    // `lowerTerminator` / pre-pass methods so the per-inst LIR emit
    // helpers can record the source. `InvalidMirInst` means "no MIR
    // source" (e.g., Switch's synthetic next-compare blocks, phi-edge
    // parallel-copy moves).
    MirInstId                       currentMir{};

    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY post-fold (2026-06-02): set
    // of SymbolIds that name extern imports (populated from the
    // caller-supplied `externImports` vector at construction time).
    // `lowerCall` consults this set when the callee is a GlobalAddr
    // — an extern-targeting call must lower to
    // `CallIndirectViaExtern` (FF 15 disp32 — dereferences the IAT
    // slot) rather than `Call` (E8 disp32 — interprets the IAT slot
    // bytes as code).
    std::unordered_set<std::uint32_t> externSymbols;

    // D-FFI-EXTERN-CALL-DISPATCH: the ACTIVE OBJECT FORMAT's extern-call
    // shape. `lowerCall` reads this to pick `CallIndirectViaExtern`
    // (indirect-slot — PE/Mach-O deref an IAT/__got pointer slot) vs the
    // plain `Call` opcode (direct-plt — ELF direct branch to the linker
    // PLT stub). `std::nullopt` = the format declared none; a module with
    // extern imports under a nullopt dispatch is a fail-loud at ctor (no
    // silent default — the wrong shape miscompiles).
    std::optional<ExternCallDispatch> externCallDispatch_;

    // Whether the running lowering pass added any error-severity
    // diagnostics. Mirrors ML2's delta-on-errorCount; reset by the ctor.
    std::uint32_t baselineErrors = 0;
    bool hadError() const noexcept {
        return reporter.errorCount() != baselineErrors;
    }

    Lowerer(Mir const& m, TargetSchema const& t, TypeInterner const& i,
            DiagnosticReporter& r,
            std::span<ExternImport const> externImports,
            std::optional<ExternCallDispatch> externCallDispatch)
        : mir(m), target(t), interner(i), reporter(r), lir(t),
          valueToReg(m), mirBlockToLirBlock(m.blockArena()),
          externCallDispatch_(externCallDispatch) {
        baselineErrors = reporter.errorCount();
        externSymbols.reserve(externImports.size());
        for (auto const& e : externImports) {
            externSymbols.insert(e.symbol.v);
        }
        // Mnemonics in MnemonicSlot enum-declaration order. The
        // `static_assert` below closes the silent-drift hazard 4
        // review agents converged on: if a developer adds a slot to
        // `MnemonicSlot` but forgets to add its mnemonic here (or
        // vice-versa), compilation fails LOUD rather than silently
        // zero-filling the array and returning `nullopt` from every
        // `opcodeByMnemonic("")` lookup. Each slot is positionally
        // indexed; a single off-by-one would corrupt all subsequent
        // opcode lookups.
        // Cycle 3e: pairing of MnemonicSlot ↔ mnemonic-string is now
        // verified at compile time by `kMnemonicRowsAligned` declared
        // alongside the table at namespace scope (below). The constexpr
        // table iterator gives BOTH count-drift AND row-order-drift
        // protection — the cycle-3d count-only static_assert allowed a
        // transposition to compile silently; this closes that.
        for (auto const& r : kMnemonicRows) {
            auto const i = static_cast<std::size_t>(r.slot);
            cache_[i].mnemonic = r.mnemonic;
            cache_[i].id       = target.opcodeByMnemonic(r.mnemonic);
        }

        // FC2 Part B: resolve the per-class move/load/store handles
        // once (mirrors the mnemonic cache). Unresolvable cells stay
        // nullopt — diagnosed at the emitting site, not here.
        for (std::size_t c = 0; c < classOpCache_.size(); ++c) {
            for (std::size_t o = 0; o < kRegClassOpCount; ++o) {
                classOpCache_[c][o] = target.regClassOpOpcode(
                    static_cast<TargetRegClass>(c),
                    static_cast<RegClassOp>(o));
            }
        }

        // D-FFI-EXTERN-CALL-DISPATCH (was D-LK10-ENTRY-ML7-FRAME-BIAS-
        // UNIFY 2nd-order audit fold): fail loud UPFRONT at construction
        // if the module declares extern imports but the active object
        // FORMAT cannot dispatch an extern call — before any lowering
        // work. The extern-call shape is a property of the FORMAT (its
        // dynamic-import model), NOT the target: the SAME x86_64 target
        // needs `call_indirect_via_extern` (FF 15, deref the IAT slot)
        // under PE but the plain `call` (E8, direct branch to the PLT
        // stub) under ELF — picking the wrong one miscompiles (FF 15
        // through an ELF PLT stub dereferences code as a pointer →
        // SIGSEGV). So the gate is keyed on `externCallDispatch_`:
        //   * nullopt          → the format declared no dispatch model
        //                        (NO silent default to either shape).
        //   * indirect-slot    → requires `call_indirect_via_extern`.
        //   * direct-plt       → requires the universal `call` opcode.
        // Static modules (no externs) skip the gate entirely — they
        // legitimately need neither the dispatch model nor the opcode.
        if (!externImports.empty()) {
            auto const callIndirectMissing = !cache_[static_cast<std::size_t>(
                MnemonicSlot::CallIndirectViaExtern)].id.has_value();
            auto const callMissing =
                !cache_[static_cast<std::size_t>(MnemonicSlot::Call)]
                     .id.has_value();
            if (!externCallDispatch_.has_value()) {
                dss::report(reporter,
                    DiagnosticCode::L_RequiredLirOpcodeMissing,
                    DiagnosticSeverity::Error,
                    "MIR→LIR: module declares extern imports but the active "
                    "object format declares no `externCallDispatch` shape — "
                    "extern calls have no defined call-site form. Declare "
                    "`externCallDispatch` in the format's `.format.json` "
                    "(\"indirect-slot\" for PE IAT / Mach-O __got, "
                    "\"direct-plt\" for ELF PLT). (D-FFI-EXTERN-CALL-DISPATCH.)");
            } else if (*externCallDispatch_ == ExternCallDispatch::IndirectSlot
                       && callIndirectMissing) {
                dss::report(reporter,
                    DiagnosticCode::L_RequiredLirOpcodeMissing,
                    DiagnosticSeverity::Error,
                    "MIR→LIR: the active format uses `indirect-slot` extern "
                    "dispatch but the target schema does not declare a "
                    "`call_indirect_via_extern` opcode — IAT/__got-indirect "
                    "extern calls cannot be lowered. Add the indirect-call "
                    "encoding to the target's `.target.json` `opcodes[]` "
                    "(x86_64 uses `FF 15 disp32`). (D-FFI-EXTERN-CALL-DISPATCH.)");
            } else if (*externCallDispatch_ == ExternCallDispatch::DirectPlt
                       && callMissing) {
                dss::report(reporter,
                    DiagnosticCode::L_RequiredLirOpcodeMissing,
                    DiagnosticSeverity::Error,
                    "MIR→LIR: the active format uses `direct-plt` extern "
                    "dispatch but the target schema does not declare a "
                    "`call` opcode — extern calls lower to a plain direct "
                    "call to the linker-synthesized PLT stub, which needs "
                    "the universal `call` encoding (x86_64 `E8 disp32`, "
                    "ARM64 `BL imm26`). (D-FFI-EXTERN-CALL-DISPATCH.)");
            }
        }
    }

    [[nodiscard]] std::optional<std::uint16_t> opcode(MnemonicSlot s) const {
        return cache_[static_cast<std::size_t>(s)].id;
    }

    // FC2 Part B: the opcode that performs `op` on a value of register
    // class `cls` (the registerClassOps resolution). GPR resolves to
    // the universal mov/load/store; a class with no declared operation
    // returns nullopt — callers MUST route through
    // `reportMissingClassOp` rather than falling back to the GPR
    // handle (the silent class-blind miscompile this table kills).
    [[nodiscard]] std::optional<std::uint16_t>
    classOp(LirRegClass cls, RegClassOp op) const {
        auto const c = static_cast<std::size_t>(cls);
        if (c >= classOpCache_.size()) return std::nullopt;
        return classOpCache_[c][static_cast<std::size_t>(op)];
    }

    void reportMissingClassOp(LirRegClass cls, RegClassOp op,
                              std::string_view context) {
        dss::report(reporter, DiagnosticCode::L_RequiredLirOpcodeMissing,
            DiagnosticSeverity::Error,
            std::format(
                "target '{}' declares no '{}' operation for register "
                "class '{}' (required for lowering {}) — declare it in "
                "the target's `registerClassOps[]` (the universal "
                "mov/load/store bindings cover only the GPR class); "
                "falling back to the GPR instruction forms would "
                "silently mis-encode",
                target.name(), regClassOpName(op),
                targetRegClassName(static_cast<TargetRegClass>(cls)),
                context));
    }

    // Map a MIR `TypeId` to the LIR register class that holds its
    // values. F16/F32/F64/F128 → FPR; Vector/Matrix → VR (SIMD);
    // integer/bool/pointer → GPR; default for aggregates (Struct/
    // Union/Array/Enum/Tuple/Slice) and any future variant arm →
    // GPR with explicit "scoped to ML5; cycle 3e aggregate-flattening
    // will decide the real shape" note. Invalid TypeId is the only
    // genuinely-unhandled case — we fail-loud-deferred those at the
    // narrow-vs-wide gate in `lowerConst`, so reaching here with an
    // invalid TypeId is itself a structural violation; we return GPR
    // as a defensive default but the using-site should have flagged
    // it earlier.
    [[nodiscard]] LirRegClass regClassForType(TypeId ty) const {
        if (!ty.valid()) return LirRegClass::GPR;
        // Delegate to the substrate-tier helper in `target_schema.hpp`
        // so ML6/ML7/LirVerifier share the same mapping. TargetRegClass
        // and LirRegClass are numerically aligned (the static_assert in
        // `lir_reg.hpp` pins it), so the cast is safe by-construction.
        return static_cast<LirRegClass>(regClassForCoreType(interner.kind(ty)));
    }

    [[nodiscard]] LirRegClass regClassFor(MirInstId id) const {
        return regClassForType(mir.instType(id));
    }

    // FC2 Part B width gate (D-TARGET-ENCODING-WIDTH-GUARD): encoding
    // variant guards carry NO operand-width discriminator, so once the
    // F64 SSE rows exist (addsd / cvttsd2si / movsd_load), a non-F64
    // float op lowered through the same mnemonics would SILENTLY
    // encode the 8-byte double instruction forms against narrower
    // values — a miscompile, not a diagnostic. This is the only tier
    // where MIR types are still visible (post-regalloc LIR carries
    // register CLASSES, not widths), so the gate lives here: any
    // FPR-class type flowing into an F64-encoded mnemonic must BE
    // F64. Returns true (no-op) for non-FPR types. Applied exactly
    // where F64 encodings exist this cycle (FAdd, FPToSI source,
    // FPR-class Load); the still-encoding-less float ops keep their
    // assemble-tier A_NoEncodingDeclared fail-loud and gain this gate
    // alongside their encodings.
    [[nodiscard]] bool requireF64Width(MirInstId id, TypeId ty,
                                       std::string_view context) {
        if (!ty.valid()) return true;  // upstream diagnosed the type hole
        if (regClassForType(ty) != LirRegClass::FPR) return true;
        if (interner.kind(ty) == TypeKind::F64) return true;
        dss::report(reporter,
            DiagnosticCode::L_UnsupportedLoweringForOpcode,
            DiagnosticSeverity::Error,
            std::format(
                "{}: float TypeKind ordinal {} is not lowerable to "
                "target '{}' — only F64 has scalar float encodings this "
                "cycle; proceeding would silently select the F64 "
                "(double-width) instruction forms "
                "(D-TARGET-ENCODING-WIDTH-GUARD)",
                context, static_cast<unsigned>(interner.kind(ty)),
                target.name()));
        poisonValue(id);
        return false;
    }

    // ── diagnostics ──────────────────────────────────────────────────

    void reportMissingOpcode(MnemonicSlot slot, std::string_view context) {
        auto& entry = cache_[static_cast<std::size_t>(slot)];
        if (entry.missingReported) return;
        entry.missingReported = true;
        ParseDiagnostic d;
        d.code     = DiagnosticCode::L_RequiredLirOpcodeMissing;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "target '{}' declares no '{}' opcode (required for lowering {})",
            target.name(), entry.mnemonic, context);
        reporter.report(std::move(d));
    }

    void reportUnsupported(MirOpcode op, MirInstId at) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "MIR opcode '{}' is not yet lowered to target '{}' (inst {})",
            mirOpcodeName(op), target.name(), at.v);
        reporter.report(std::move(d));
    }

    void reportDoubleDef(MirInstId at) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "MIR inst %{} would re-define a value already mapped — structural "
            "violation (SSA single-definition broken)",
            at.v);
        reporter.report(std::move(d));
    }

    // ── value/block map plumbing ─────────────────────────────────────

    [[nodiscard]] std::optional<LirReg> regForValue(MirInstId v) {
        if (!v.valid() || !valueToReg.has(v)) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "MIR value %{} used before definition during LIR lowering — "
                "either a structural violation or a deferred-opcode dependency",
                v.v);
            reporter.report(std::move(d));
            return std::nullopt;
        }
        return valueToReg.get(v);
    }

    bool defineValue(MirInstId id, LirReg reg) {
        if (valueToReg.has(id)) {
            reportDoubleDef(id);
            return false;
        }
        valueToReg.set(id, reg);
        return true;
    }

    // ── per-instruction lowering ─────────────────────────────────────

    // Non-terminator dispatcher (terminators flow through `lowerTerminator`
    // and Phi is a pre-pass no-op).
    void lowerInst(MirInstId id) {
        currentMir = id;
        MirOpcode const op = mir.instOpcode(id);
        switch (op) {
            case MirOpcode::Arg:    return lowerArg(id);
            case MirOpcode::Const:  return lowerConst(id);
            case MirOpcode::Add:    return lowerBinaryOp(id, MnemonicSlot::Add);
            case MirOpcode::Sub:    return lowerBinaryOp(id, MnemonicSlot::Sub);
            case MirOpcode::Mul:    return lowerBinaryOp(id, MnemonicSlot::Mul);
            case MirOpcode::SDiv:   return lowerDivLike(id, /*isSigned=*/true,  /*wantRemainder=*/false);
            case MirOpcode::UDiv:   return lowerDivLike(id, /*isSigned=*/false, /*wantRemainder=*/false);
            case MirOpcode::SMod:   return lowerDivLike(id, /*isSigned=*/true,  /*wantRemainder=*/true);
            case MirOpcode::UMod:   return lowerDivLike(id, /*isSigned=*/false, /*wantRemainder=*/true);
            case MirOpcode::ICmpEq: case MirOpcode::ICmpNe:
            case MirOpcode::ICmpSlt: case MirOpcode::ICmpSle:
            case MirOpcode::ICmpSgt: case MirOpcode::ICmpSge:
            case MirOpcode::ICmpUlt: case MirOpcode::ICmpUle:
            case MirOpcode::ICmpUgt: case MirOpcode::ICmpUge: {
                // Even though this dispatch arm gates `op` to the
                // ICmp* set, route through the optional return so a
                // future MirOpcode added to this arm but NOT to
                // `condCodeForICmp`'s switch surfaces as a loud
                // diagnostic instead of UB blind-deref. The poison
                // call prevents downstream `regForValue` from
                // cascading "used before definition" diagnostics.
                auto const cc = condCodeForICmp(op);
                if (!cc.has_value()) {
                    reportUnsupported(op, id);
                    poisonValue(id);
                    return;
                }
                return lowerICmp(id, *cc);
            }
            case MirOpcode::Phi:    return;  // pre-pass-allocated; no body emission
            case MirOpcode::Alloca: return lowerAlloca(id);
            case MirOpcode::Load:   return lowerLoad(id);
            case MirOpcode::Store:  return lowerStore(id);
            case MirOpcode::Gep:    return lowerGep(id);
            case MirOpcode::Trunc:  return lowerCast(id, MnemonicSlot::Trunc, "MIR Trunc");
            case MirOpcode::SExt:   return lowerCast(id, MnemonicSlot::SExt,  "MIR SExt");
            case MirOpcode::ZExt:   return lowerCast(id, MnemonicSlot::ZExt,  "MIR ZExt");
            case MirOpcode::Bitcast:    return lowerBitcast(id);
            case MirOpcode::IntToPtr:
            case MirOpcode::PtrToInt:
                // Identity cast between integer and pointer on x86_64
                // — both are 64-bit GPR-class values. Single `mov`.
                return lowerCast(id, MnemonicSlot::Mov, "MIR IntToPtr/PtrToInt");
            // ── cycle 3d bitwise + Neg ─────────────────────────────
            case MirOpcode::And:    return lowerBinaryOp(id, MnemonicSlot::And);
            case MirOpcode::Or:     return lowerBinaryOp(id, MnemonicSlot::Or);
            case MirOpcode::Xor:    return lowerBinaryOp(id, MnemonicSlot::Xor);
            case MirOpcode::Shl:    return lowerBinaryOp(id, MnemonicSlot::Shl);
            case MirOpcode::LShr:   return lowerBinaryOp(id, MnemonicSlot::ShrL);
            case MirOpcode::AShr:   return lowerBinaryOp(id, MnemonicSlot::ShrA);
            case MirOpcode::Not:    return lowerUnaryOp(id, MnemonicSlot::Not);
            case MirOpcode::Neg:    return lowerUnaryOp(id, MnemonicSlot::Neg);
            // ── cycle 3d float arithmetic (FPR-class result) ───────
            case MirOpcode::FAdd:
                // FC2 Part B: fadd carries the F64 addsd encoding —
                // gate the width before the class-blind lowering.
                if (!requireF64Width(id, mir.instType(id), "MIR FAdd")) return;
                return lowerBinaryOp(id, MnemonicSlot::FAdd);
            case MirOpcode::FSub:   return lowerBinaryOp(id, MnemonicSlot::FSub);
            case MirOpcode::FMul:   return lowerBinaryOp(id, MnemonicSlot::FMul);
            case MirOpcode::FDiv:   return lowerBinaryOp(id, MnemonicSlot::FDiv);
            case MirOpcode::FNeg:   return lowerUnaryOp(id, MnemonicSlot::FNeg);
            // ── cycle 3d float casts (the 6 conversion variants) ───
            case MirOpcode::FPTrunc: return lowerCast(id, MnemonicSlot::FpCvt,   "MIR FPTrunc");
            case MirOpcode::FPExt:   return lowerCast(id, MnemonicSlot::FpCvt,   "MIR FPExt");
            case MirOpcode::FPToSI: {
                // FC2 Part B: fp_to_si carries the F64 cvttsd2si
                // encoding — the SOURCE operand's float width is the
                // encoded one (the result is integer/GPR).
                auto const convOps = mir.instOperands(id);
                if (convOps.size() == 1
                    && !requireF64Width(id, mir.instType(convOps[0]),
                                        "MIR FPToSI (source)")) {
                    return;
                }
                return lowerCast(id, MnemonicSlot::FpToSi,  "MIR FPToSI");
            }
            case MirOpcode::FPToUI:  return lowerCast(id, MnemonicSlot::FpToUi,  "MIR FPToUI");
            case MirOpcode::SIToFP:  return lowerCast(id, MnemonicSlot::SiToFp,  "MIR SIToFP");
            case MirOpcode::UIToFP:  return lowerCast(id, MnemonicSlot::UiToFp,  "MIR UIToFP");
            // ── cycle 3e: Calls + GlobalAddr ───────────────────────
            case MirOpcode::GlobalAddr:    return lowerGlobalAddr(id);
            case MirOpcode::Call:          return lowerCall(id);
            case MirOpcode::IntrinsicCall: return lowerIntrinsicCall(id);
            // ── cycle 3e: aggregate ops (memory-flattening lowering) ──
            case MirOpcode::ExtractValue:  return lowerExtractValue(id);
            case MirOpcode::InsertValue:   return lowerInsertValue(id);
            default: break;
        }
        reportUnsupported(op, id);
    }

    void lowerArg(MirInstId id) {
        if (!opcode(MnemonicSlot::Arg).has_value()) {
            reportMissingOpcode(MnemonicSlot::Arg, "MIR Arg");
            return;
        }
        // Cycle 3d: Arg's result reg class follows the parameter type
        // (F32/F64 → FPR; integer/ptr → GPR). ML7 callconv lowering
        // reads the result class to pick the right arg-passing
        // register per the cycle-2b TargetCallingConvention.
        LirReg const result = lir.newVReg(regClassFor(id));
        emitInst(*opcode(MnemonicSlot::Arg), result, std::span<LirOperand const>{},
                    /*payload=*/mir.argIndex(id));
        defineValue(id, result);
    }

    // Emit a single-operand class-correct register write and define the
    // result vreg. Shared tail of `lowerConst`'s narrow + wide paths
    // (the only difference between them is the operand-kind in `op`).
    // FC2 Part B: the mnemonic comes from the per-register-class table
    // (GPR → the universal `mov`; FPR → the class's declared move, e.g.
    // x86_64 movaps) — a GPR mov against an FPR ordinal mis-encodes.
    // Returns the result reg so callers can chain.
    LirReg emitMovToFresh(MirInstId id, LirOperand op, LirRegClass cls) {
        auto const movOp = classOp(cls, RegClassOp::Move);
        if (!movOp.has_value()) {
            reportMissingClassOp(cls, RegClassOp::Move, "MIR value copy");
            poisonValue(id);
            return valueToReg.get(id);
        }
        LirReg const result = lir.newVReg(cls);
        std::array<LirOperand, 1> ops{op};
        emitInst(*movOp, result, ops);
        defineValue(id, result);
        return result;
    }

    // Define a poisoned-value vreg + return false so the caller can
    // signal "lowering failed but downstream uses get a quiet
    // placeholder, not a cascade of `regForValue` diagnostics". The
    // diagnostic for the ROOT cause is the caller's responsibility.
    void poisonValue(MirInstId id) {
        LirReg const placeholder = lir.newVReg(LirRegClass::GPR);
        valueToReg.set(id, placeholder);
    }

    void lowerConst(MirInstId id) {
        if (!opcode(MnemonicSlot::Mov).has_value()) {
            reportMissingOpcode(MnemonicSlot::Mov, "MIR Const");
            return;
        }
        std::uint32_t const litIdx = mir.constLiteralIndex(id);
        MirLiteralValue const& lit = mir.literalValue(litIdx);

        // ── narrow integer path: inline ImmInt32 ──
        std::int32_t imm32 = 0;
        bool fits = false;
        if (auto const* i = std::get_if<std::int64_t>(&lit.value)) {
            if (*i >= std::numeric_limits<std::int32_t>::min()
             && *i <= std::numeric_limits<std::int32_t>::max()) {
                imm32 = static_cast<std::int32_t>(*i);
                fits  = true;
            }
        } else if (auto const* u = std::get_if<std::uint64_t>(&lit.value)) {
            if (*u <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
                imm32 = static_cast<std::int32_t>(*u);
                fits  = true;
            }
        } else if (auto const* b = std::get_if<bool>(&lit.value)) {
            imm32 = *b ? 1 : 0;
            fits  = true;
        }
        if (fits) {
            // Narrow integer literal — GPR-class regardless of the
            // declared MIR type since the immediate IS an integer.
            // (Bool literals are also routed here as 0/1.)
            emitMovToFresh(id, LirOperand::makeImmInt32(imm32), LirRegClass::GPR);
            return;
        }

        // ── wide-literal path: route through LirLiteralPool ──
        //
        // Routing: int32-fits → ImmInt (above); int64/uint64/string →
        // pool with GPR-class result; monostate / aggregate →
        // unsupported + poison so dependent uses surface ONE root-cause
        // diagnostic rather than a cascade of "used before definition".
        //
        // FC2 Part B (F64 constant materialization): float (FPR-class)
        // constants must NOT reach MIR→LIR as `Const` — HIR→MIR
        // promotes F64 literals to anonymous rodata globals
        // (GlobalAddr + Load, the string-literal shape). The prior
        // double→LiteralPool route was a DEAD END: no encoding variant
        // consumes a LiteralIndex operand, so every FPR literal hit
        // A_NoMatchingEncodingVariant at assemble time. Fail loud HERE
        // (where the contract is visible) on EITHER signal — a double
        // variant arm, or an FPR-class result type (catches a non-
        // double literal hand-typed F64). Non-F64 float widths have
        // no promotion either (D-TARGET-ENCODING-WIDTH-GUARD).
        LirRegClass resultCls = regClassFor(id);
        if (std::holds_alternative<double>(lit.value)
            || resultCls == LirRegClass::FPR) {
            dss::report(reporter,
                DiagnosticCode::L_UnsupportedLoweringForOpcode,
                DiagnosticSeverity::Error,
                std::format(
                    "MIR Const %{} is a float (FPR-class) literal at "
                    "MIR→LIR — float constants must be promoted to "
                    "anonymous rodata globals (GlobalAddr + Load) at "
                    "HIR→MIR; the LirLiteralPool LiteralIndex route has "
                    "no encoder path (FC2 Part B; non-F64 widths: "
                    "D-TARGET-ENCODING-WIDTH-GUARD)", id.v));
            poisonValue(id);
            return;
        }
        LirLiteralValue lirLit;
        lirLit.core = lit.core;
        bool unsupportedVariant = false;
        if (auto const* i = std::get_if<std::int64_t>(&lit.value))       lirLit.value = *i;
        else if (auto const* u = std::get_if<std::uint64_t>(&lit.value)) lirLit.value = *u;
        else if (auto const* s = std::get_if<std::string>(&lit.value))   lirLit.value = *s;
        else {
            // monostate (malformed-source recovery), aggregate (cycle
            // 3e), or any future variant arm: fail loud + poison.
            unsupportedVariant = true;
        }
        if (unsupportedVariant) {
            reportUnsupported(MirOpcode::Const, id);
            poisonValue(id);
            return;
        }
        std::uint32_t const lirLitIdx = lir.literalPoolAdd(std::move(lirLit));
        emitMovToFresh(id, LirOperand::makeLiteralIndex(lirLitIdx), resultCls);
    }

    // ── memory ops (cycle 3c) ────────────────────────────────────────

    void lowerAlloca(MirInstId id) {
        if (!opcode(MnemonicSlot::Alloca).has_value()) {
            reportMissingOpcode(MnemonicSlot::Alloca, "MIR Alloca");
            return;
        }
        std::uint32_t const payload = mir.instPayload(id);
        LirReg const result = lir.newVReg(LirRegClass::GPR);
        emitInst(*opcode(MnemonicSlot::Alloca), result,
                    std::span<LirOperand const>{}, payload);
        defineValue(id, result);
    }

    void lowerLoad(MirInstId id) {
        auto const operands = mir.instOperands(id);
        if (operands.size() != 1) { reportUnsupported(MirOpcode::Load, id); return; }
        std::optional<LirReg> const base = regForValue(operands[0]);
        if (!base.has_value()) return;
        // Cycle 3d FPR-class plumbing closure: a Load of an F64 must
        // produce an FPR-class result, not GPR. Closes the silent-
        // failure-hunter HIGH finding from cycle 3d's review.
        // FC2 Part B: an FPR-class load selects the F64 movsd_load
        // mnemonic (8-byte read) via registerClassOps — gate the
        // width so an F32 load can't silently read 8 bytes.
        if (!requireF64Width(id, mir.instType(id), "MIR Load")) return;
        // FC2 Part B: the load mnemonic follows the RESULT's register
        // class (registerClassOps — GPR `load`, FPR e.g. movsd_load).
        LirRegClass const cls = regClassFor(id);
        auto const loadOp = classOp(cls, RegClassOp::Load);
        if (!loadOp.has_value()) {
            reportMissingClassOp(cls, RegClassOp::Load, "MIR Load");
            poisonValue(id);
            return;
        }
        LirReg const result = lir.newVReg(cls);
        std::array<LirOperand, 3> ops{
            LirOperand::makeReg(*base),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0),
        };
        emitInst(*loadOp, result, ops);
        defineValue(id, result);
    }

    void lowerStore(MirInstId id) {
        auto const operands = mir.instOperands(id);
        if (operands.size() != 2) { reportUnsupported(MirOpcode::Store, id); return; }
        std::optional<LirReg> const value = regForValue(operands[0]);
        std::optional<LirReg> const base  = regForValue(operands[1]);
        if (!value.has_value() || !base.has_value()) return;
        // FC2 Part B: the store mnemonic follows the stored VALUE's
        // register class (x86_64 fpr → movsd_store since the PE-float
        // closure, 2026-06-10). A class with no declared store fails
        // loud here instead of silently GPR-storing 8 bytes of XMM
        // ordinal.
        LirRegClass const cls = value->regClass();
        auto const storeOp = classOp(cls, RegClassOp::Store);
        if (!storeOp.has_value()) {
            reportMissingClassOp(cls, RegClassOp::Store, "MIR Store");
            return;
        }
        std::array<LirOperand, 4> ops{
            LirOperand::makeReg(*value),
            LirOperand::makeReg(*base),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0),
        };
        emitInst(*storeOp, InvalidLirReg, ops);
    }

    void lowerGep(MirInstId id) {
        auto const operands = mir.instOperands(id);
        if (operands.empty()) { reportUnsupported(MirOpcode::Gep, id); return; }
        std::optional<LirReg> const base = regForValue(operands[0]);
        if (!base.has_value()) return;
        // No-index degenerate: emit explicit `mov result, base` rather
        // than `lea result, [base + 0]` so the identity is visible at
        // LIR and reuses ML6's coalescer path.
        if (operands.size() == 1) {
            if (!opcode(MnemonicSlot::Mov).has_value()) {
                reportMissingOpcode(MnemonicSlot::Mov, "MIR Gep (no-index)");
                return;
            }
            emitMovToFresh(id, LirOperand::makeReg(*base), LirRegClass::GPR);
            return;
        }
        // Cycle 3d: dynamic-index Gep with a single index. Emits
        // `lea result, [base + index*1 + 0]` via the 4-operand `lea`
        // form (`[base_reg, index_reg, MemBase(scale=1), MemOffset(0)]`).
        // The scale defaults to 1 for cycle 3d; element-size-driven
        // scales (sizeof(T) for typed-array Gep) co-design with ML6's
        // address-mode synthesis. Multi-index Gep — multiple
        // dereferences — folds into a chain at the optimizer layer.
        if (operands.size() == 2) {
            if (!opcode(MnemonicSlot::Lea).has_value()) {
                reportMissingOpcode(MnemonicSlot::Lea, "MIR Gep (dynamic-index)");
                return;
            }
            std::optional<LirReg> const index = regForValue(operands[1]);
            if (!index.has_value()) return;
            LirReg const result = lir.newVReg(LirRegClass::GPR);
            std::array<LirOperand, 4> ops{
                LirOperand::makeReg(*base),
                LirOperand::makeReg(*index),
                LirOperand::makeMemBase(1),
                LirOperand::makeMemOffset(0),
            };
            emitInst(*opcode(MnemonicSlot::Lea), result, ops);
            defineValue(id, result);
            return;
        }
        // Multi-index Gep (≥3 operands) — defer to cycle 3e (the
        // optimizer or a Gep-flattening pass is the natural consumer
        // since chains of dereferences need address-mode synthesis).
        reportUnsupported(MirOpcode::Gep, id);
    }

    // Single-operand value-producing cast. The result register class
    // follows the MIR result type (FPR for float-result casts; GPR
    // otherwise). Cycle 3d's float casts (FpCvt/FpToSi/FpToUi/SiToFp/
    // UiToFp) all land here. Delegates to the shared lowerNAryOp; the
    // `context` argument is now redundant with `mirOpcodeName` and
    // dropped (simplifier review).
    void lowerCast(MirInstId id, MnemonicSlot slot, std::string_view /*context*/) {
        lowerNAryOp<1>(id, slot);
    }

    // Bitcast lowering — closes the cycle-3c-anchored hazard 3 review
    // agents flagged. If src + dst share a register class (GPR↔GPR or
    // FPR↔FPR), emit a plain `mov` (bit-pattern-preserving). If they
    // differ, emit `movq_xclass` (cycle-3d cross-class move that AS1
    // maps to x86_64 `movq xmm,rax` or `movq rax,xmm`). A regression
    // omitting the cross-class case would silently mis-lower float
    // reinterpretation.
    void lowerBitcast(MirInstId id) {
        auto const operands = mir.instOperands(id);
        if (operands.size() != 1) {
            reportUnsupported(MirOpcode::Bitcast, id);
            return;
        }
        std::optional<LirReg> const src = regForValue(operands[0]);
        if (!src.has_value()) return;
        LirRegClass const srcCls = src->regClass();
        LirRegClass const dstCls = regClassFor(id);
        // FC2 Part B: the same-class arm now routes through the
        // per-register-class move table (this function WAS the
        // class-dispatch pattern the table generalizes) — an FPR↔FPR
        // bitcast copies via the class's move (x86_64 movaps), never
        // the GPR mov. Cross-class keeps the dedicated movq_xclass.
        std::optional<std::uint16_t> copyOp;
        if (srcCls == dstCls) {
            copyOp = classOp(dstCls, RegClassOp::Move);
            if (!copyOp.has_value()) {
                reportMissingClassOp(dstCls, RegClassOp::Move,
                                     "MIR Bitcast (same-class)");
                poisonValue(id);
                return;
            }
        } else {
            if (!opcode(MnemonicSlot::MovqXClass).has_value()) {
                reportMissingOpcode(MnemonicSlot::MovqXClass,
                                    "MIR Bitcast (cross-class)");
                return;
            }
            copyOp = opcode(MnemonicSlot::MovqXClass);
        }
        LirReg const result = lir.newVReg(dstCls);
        std::array<LirOperand, 1> ops{LirOperand::makeReg(*src)};
        emitInst(*copyOp, result, ops);
        defineValue(id, result);
    }

    // ── cycle 3e: Calls + GlobalAddr ─────────────────────────────────

    // MIR GlobalAddr → LIR `lea result, [rip + sym]` (D-LK4-RODATA-
    // PRODUCER 2026-06-02 — closes the prior `mov SymbolRef` gap).
    // The address materialization is a pure 7-byte RIP-relative
    // load-effective-address that emits a `rel32` Relocation
    // against the symbol. Cross-target shape: ARM64 will declare
    // its own LEA variant (ADRP+ADD pair, 2-instruction macro-op
    // — anchored under D-LK10-ENTRY-ARM64).
    //
    // ML7 cycle 2 peephole impact: when this GlobalAddr's result is
    // consumed ONLY by a direct `Call`, the Call lowerer folds the
    // SymbolId straight into the LIR call's SymbolRef operand and
    // never reads this GlobalAddr's emitted vreg. The LEA still
    // emits 7 bytes into the function body as a dead def; regalloc
    // handles the dead vreg, but the bytes remain in the binary.
    // Dead-LEA elimination for direct-call-only GlobalAddrs is
    // anchored at D-ML7-2.9 (carried forward from the prior `mov`
    // shape — same elimination opportunity, same trigger).
    //
    // Pre-D-LK4-RODATA-PRODUCER shape was `mov result, SymbolRef`,
    // which tripped `A_NoMatchingEncodingVariant` at assemble-time
    // because no `mov` variant accepted a SymbolRef operand. The
    // direct-call peephole sidesteps the LEA's vreg (the call's
    // SymbolRef operand is folded from the MIR-level GlobalAddr
    // SymbolId, not from the LEA's result) — but `lowerGlobalAddr`
    // still RUNS in source order, so the LEA's bytes still land in
    // the binary as a dead def. Non-call uses (returning a global's
    // address, loading from a global, etc.) were blocked by the
    // missing `mov` SymbolRef variant pre-D-LK4-RODATA-PRODUCER;
    // the LEA-RIP-rel variant unblocks them.
    void lowerGlobalAddr(MirInstId id) {
        if (!opcode(MnemonicSlot::Lea).has_value()) {
            reportMissingOpcode(MnemonicSlot::Lea, "MIR GlobalAddr");
            return;
        }
        SymbolId const sym = mir.globalAddrSymbol(id);
        LirReg const result = lir.newVReg(regClassFor(id));
        std::array<LirOperand, 1> ops{LirOperand::makeSymbolRef(sym.v)};
        emitInst(*opcode(MnemonicSlot::Lea), result, ops);
        defineValue(id, result);
    }

    // MIR Call → LIR `call callee, args...`. Convention:
    //   operand[0] = callee value (typically a GlobalAddr — direct
    //                call — peepholed into a SymbolRef LIR operand;
    //                anything else routes through the indirect-call
    //                path which is anchored at D-ML7-2.4)
    //   operand[1..N] = argument values
    // ML7 cycle 2 (landed) rewrites this to the explicit
    // mov-to-arg-register + call + mov-from-return-register sequence
    // using the ML5 cycle-2b `TargetCallingConvention` sections
    // (argGprs/argFprs/returnGprs/returnFprs). The LIR shape coming
    // OUT of isel is the abstract `call(callee, args...)` form —
    // target-blind — and the callconv pass rewrites it to the
    // schema-encodable single-SymbolRef form.
    void lowerCall(MirInstId id) {
        // Cycle 3e fix-up: opcode resolution now goes through the
        // MnemonicCache (was ad-hoc `target.opcodeByMnemonic("call")`
        // before — flagged by code-reviewer as MED follow-up). Cached
        // resolution + one-shot diagnostic for missing mnemonic.
        if (!opcode(MnemonicSlot::Call).has_value()) {
            reportMissingOpcode(MnemonicSlot::Call, "MIR Call");
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.empty()) {
            reportUnsupported(MirOpcode::Call, id);
            return;
        }

        // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY post-fold (2026-06-02):
        // pick the right call opcode based on whether the callee is
        // an extern import. Two distinct x86_64 encodings:
        //   * `call` (E8 disp32): direct call; the linker patches
        //     disp32 to the callee's RVA. Correct for module-internal
        //     functions whose bodies live in the same image.
        //   * `call_indirect_via_extern` (FF 15 disp32): indirect
        //     call via [RIP+disp32]; the linker patches disp32 to
        //     the IAT slot RVA. The CPU dereferences the slot to
        //     reach the loader-fixed-up callee address. Correct for
        //     extern imports (puts via msvcrt.dll, ExitProcess via
        //     kernel32.dll, etc.).
        // Using `call` for an extern target would execute the IAT
        // slot's bytes as code — guaranteed SEGV. Detected via the
        // `externSymbols` set populated at Lowerer construction from
        // the caller-supplied `externImports` vector.
        // Cycle 3e fix-up: HOIST all regForValue checks BEFORE any
        // state mutation. The cycle-3e first cut built the operand
        // vector inline; a mid-loop arg-not-mapped bail-out would
        // leave the partial operand list dangling AND skip
        // `defineValue`, producing cascading "used before definition"
        // diagnostics that drown the root cause. Same discipline as
        // `lowerStore`/`lowerInsertValue`.

        // ML7 cycle 2 peephole: if the callee operand is a `GlobalAddr`
        // MIR instruction, fold its SymbolId directly into the LIR
        // call's `SymbolRef` operand (the direct-call form). This
        // matches the target schema's `call` encoding variant guard
        // `["symbol"]` AND lets ML7 cycle 2's callconv materialization
        // pass leave the callee operand intact while just rewriting
        // the args. Indirect-call form (callee is any other MIR
        // instruction — IntToPtr from a function pointer load, etc.)
        // is anchored at D-ML7-2.4 (an indirect-call schema variant
        // + the assembler-side encoder for `call <reg>` must land
        // together; today the assembler trips on it loud).
        MirInstId const calleeMir = operands[0];
        bool const calleeIsGlobalAddr =
            mir.instOpcode(calleeMir) == MirOpcode::GlobalAddr;

        // Determine extern-vs-internal based on the GlobalAddr's
        // SymbolId. Indirect-without-GlobalAddr (function-pointer
        // call) is anchored at D-ML7-2.4 — surfaces loud at
        // materialization for now.
        bool calleeIsExtern = false;
        SymbolId calleeSym{};
        if (calleeIsGlobalAddr) {
            calleeSym = mir.globalAddrSymbol(calleeMir);
            calleeIsExtern = externSymbols.contains(calleeSym.v);
        }

        // D-FFI-EXTERN-CALL-DISPATCH: an extern call's CALL-SITE shape is
        // the ACTIVE OBJECT FORMAT's, not a fixed assumption. Only an
        // `indirect-slot` format (PE IAT / Mach-O __got) dereferences a
        // pointer slot via `call_indirect_via_extern` (FF 15); a
        // `direct-plt` format (ELF) makes a PLAIN DIRECT `call` (E8 / BL)
        // to the linker-synthesized PLT stub — byte-identical to an
        // intra-module call, the linker's symbolVa→stub mapping + the
        // call reloc do the indirection. A non-extern call is always the
        // direct `Call`. (The ctor guard already fail-louded if externs
        // are present under a nullopt / under-equipped format, so an
        // extern reaching here has a valid, opcode-backed dispatch model;
        // a nullopt compares unequal to IndirectSlot → falls to `Call`.)
        bool const useIndirectExtern =
            calleeIsExtern
            && externCallDispatch_.has_value()
            && externCallUsesIndirectShape(*externCallDispatch_);
        MnemonicSlot const callSlot = useIndirectExtern
            ? MnemonicSlot::CallIndirectViaExtern
            : MnemonicSlot::Call;
        if (!opcode(callSlot).has_value()) {
            reportMissingOpcode(callSlot,
                useIndirectExtern
                    ? "MIR Call (extern-import target)"
                    : "MIR Call");
            return;
        }

        std::vector<LirReg> argRegs;
        argRegs.reserve(operands.size());
        // Skip operand[0] (callee) when we fold it; otherwise include it.
        std::size_t const firstArgIdx = calleeIsGlobalAddr ? 1u : 0u;
        for (std::size_t i = firstArgIdx; i < operands.size(); ++i) {
            std::optional<LirReg> const r = regForValue(operands[i]);
            if (!r.has_value()) return;
            argRegs.push_back(*r);
        }

        // Build the LIR operand list.
        // Direct: [SymbolRef(sym), arg0, arg1, ...]
        // Indirect-via-extern: [SymbolRef(sym), arg0, arg1, ...] —
        //     same shape; the SymbolRef tags an extern (resolved to
        //     IAT slot RVA at link time; the opcode encoding
        //     dereferences it).
        // Indirect (fn-pointer): [callee_reg, arg0, arg1, ...]
        //     (anchored D-ML7-2.4; trips loud at materialization).
        std::vector<LirOperand> ops;
        ops.reserve(argRegs.size() + 1);
        if (calleeIsGlobalAddr) {
            ops.push_back(LirOperand::makeSymbolRef(calleeSym.v));
        }
        for (auto const r : argRegs) ops.push_back(LirOperand::makeReg(r));

        // Result class follows MIR's result type. Void-returning calls
        // produce an invalid result vreg (no value).
        TypeId const resultTy = mir.instType(id);
        bool const isVoid = !resultTy.valid()
                         || interner.kind(resultTy) == TypeKind::Void;
        // D-LANG-VARIADIC (step 13.4): forward the MIR Call's
        // variadic-payload bits (isVariadic + fixedArgCount) to the
        // LIR Call so the post-regalloc ML7 materialize pass can
        // emit the platform's variadic-call setup (SysV `mov al,
        // <xmm-arg-count>` etc.). Non-variadic calls keep payload=0.
        std::uint32_t const payload = mir.instPayload(id);
        if (isVoid) {
            emitInst(*opcode(callSlot), InvalidLirReg, ops, payload);
            return;
        }
        LirReg const result = lir.newVReg(regClassFor(id));
        emitInst(*opcode(callSlot), result, ops, payload);
        defineValue(id, result);
    }

    // MIR IntrinsicCall → LIR `intrinsic_call`. Operands are the args;
    // payload carries the intrinsic id. Result is optional (per the
    // schema's `result: "optional"` for the opcode). AS1 maps the
    // intrinsic id to its concrete sequence per target.
    //
    // Cycle 3e fix-up: hoist all regForValue checks BEFORE state
    // mutation (mirrors lowerCall fix). A mid-loop arg-not-mapped
    // bail-out would otherwise leave a partial operand list dangling.
    void lowerIntrinsicCall(MirInstId id) {
        if (!opcode(MnemonicSlot::IntrinsicCall).has_value()) {
            reportMissingOpcode(MnemonicSlot::IntrinsicCall, "MIR IntrinsicCall");
            return;
        }
        auto const operands = mir.instOperands(id);
        std::vector<LirReg> argRegs;
        argRegs.reserve(operands.size());
        for (auto const opnd : operands) {
            std::optional<LirReg> const r = regForValue(opnd);
            if (!r.has_value()) return;
            argRegs.push_back(*r);
        }
        std::vector<LirOperand> ops;
        ops.reserve(argRegs.size());
        for (auto const r : argRegs) ops.push_back(LirOperand::makeReg(r));

        std::uint32_t const intrId = mir.intrinsicId(id);
        TypeId const resultTy = mir.instType(id);
        bool const isVoid = !resultTy.valid()
                         || interner.kind(resultTy) == TypeKind::Void;
        if (isVoid) {
            emitInst(*opcode(MnemonicSlot::IntrinsicCall), InvalidLirReg, ops, intrId);
            return;
        }
        LirReg const result = lir.newVReg(regClassFor(id));
        emitInst(*opcode(MnemonicSlot::IntrinsicCall), result, ops, intrId);
        defineValue(id, result);
    }

    // Cycle 3e fix-up: shared helper for aggregate-op index validation.
    // The cycle-3e first cut only accepted `int64_t` zero — the
    // `uint64_t` and `bool` variants of `MirLiteralValue` silently fell
    // through to `reportUnsupported` even when the value WAS zero.
    // Type-design + silent-failure-hunter flagged this as a real latent
    // bug since MIR text round-trip accepts both `lit int N` and
    // `lit uint N` variants. Now accepts int64==0 OR uint64==0 OR bool
    // false (the three literal variants `lowerConst` produces).
    [[nodiscard]] bool isZeroIntegerLiteral(MirInstId idxOp) const {
        if (mir.instOpcode(idxOp) != MirOpcode::Const) return false;
        std::uint32_t const litIdx = mir.constLiteralIndex(idxOp);
        MirLiteralValue const& lit = mir.literalValue(litIdx);
        if (auto const* i = std::get_if<std::int64_t>(&lit.value))  return *i == 0;
        if (auto const* u = std::get_if<std::uint64_t>(&lit.value)) return *u == 0;
        if (auto const* b = std::get_if<bool>(&lit.value))          return !*b;
        return false;
    }

    // ── cycle 3e: aggregate ops (memory-flattening lowering) ─────────
    //
    // MIR ExtractValue/InsertValue operate on aggregate values directly
    // (D5.6 substrate). The cycle-3e LIR lowering routes them through
    // memory: each aggregate value is materialized via an Alloca, and
    // ExtractValue/InsertValue lower as Load/Store at the appropriate
    // byte offset. This is correct + simple but adds extra memory
    // traffic; ML6's optimizer (plan 11) will promote-to-registers
    // where the alloca doesn't escape.
    //
    // Cycle 3e ships the DEGENERATE shape: zero offset (the first
    // field). Real field-offset computation needs the MIR type's
    // layout from the interner, which is co-designed with ML6's
    // frame-layout phase. Until that lands, ExtractValue/InsertValue
    // with a non-zero index defers to ML6 frame-layout.

    void lowerExtractValue(MirInstId id) {
        // MIR operands: [aggregate, index_const_1, index_const_2, ...].
        // The aggregate's value flows from a ConstructAggregate (ML2
        // cycle 6) which is built via alloca + insertvalue chain in
        // MIR. For cycle 3e: only the zero-index path lowers; non-zero
        // indices defer (would need type-layout lookup).
        auto const operands = mir.instOperands(id);
        if (operands.size() < 2) {
            reportUnsupported(MirOpcode::ExtractValue, id);
            return;
        }
        // Every index must be a zero-valued Const-literal for cycle
        // 3e's degenerate path. Non-zero indices defer to ML6 frame-
        // layout co-design (need type-layout-driven field offsets).
        for (std::size_t k = 1; k < operands.size(); ++k) {
            if (!isZeroIntegerLiteral(operands[k])) {
                reportUnsupported(MirOpcode::ExtractValue, id);
                return;
            }
        }
        // Degenerate first-field path: emit `load` from base. FC2
        // Part B: class-routed like lowerLoad (an F64 first field
        // would otherwise GPR-load into an FPR vreg).
        std::optional<LirReg> const base = regForValue(operands[0]);
        if (!base.has_value()) return;
        if (!requireF64Width(id, mir.instType(id), "MIR ExtractValue")) return;
        LirRegClass const cls = regClassFor(id);
        auto const loadOp = classOp(cls, RegClassOp::Load);
        if (!loadOp.has_value()) {
            reportMissingClassOp(cls, RegClassOp::Load, "MIR ExtractValue");
            poisonValue(id);
            return;
        }
        LirReg const result = lir.newVReg(cls);
        std::array<LirOperand, 3> ops{
            LirOperand::makeReg(*base),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0),
        };
        emitInst(*loadOp, result, ops);
        defineValue(id, result);
    }

    void lowerInsertValue(MirInstId id) {
        // MIR operands: [aggregate, value, index_const_1, ...].
        // Cycle 3e: degenerate zero-index → emit `store value, base`
        // and return the (modified) aggregate base. Multi-byte offsets
        // defer to ML6 frame-layout.
        auto const operands = mir.instOperands(id);
        if (operands.size() < 3) {
            reportUnsupported(MirOpcode::InsertValue, id);
            return;
        }
        for (std::size_t k = 2; k < operands.size(); ++k) {
            if (!isZeroIntegerLiteral(operands[k])) {
                reportUnsupported(MirOpcode::InsertValue, id);
                return;
            }
        }
        std::optional<LirReg> const base  = regForValue(operands[0]);
        std::optional<LirReg> const value = regForValue(operands[1]);
        if (!base.has_value() || !value.has_value()) return;
        // FC2 Part B: class-routed like lowerStore — the stored
        // VALUE's class picks the mnemonic; a class without a
        // declared store fails loud.
        LirRegClass const cls = value->regClass();
        auto const storeOp = classOp(cls, RegClassOp::Store);
        if (!storeOp.has_value()) {
            reportMissingClassOp(cls, RegClassOp::Store, "MIR InsertValue");
            return;
        }
        std::array<LirOperand, 4> ops{
            LirOperand::makeReg(*value),
            LirOperand::makeReg(*base),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0),
        };
        emitInst(*storeOp, InvalidLirReg, ops);
        // The result of InsertValue is conceptually the modified
        // aggregate. In the memory-flattening model the aggregate IS
        // the base pointer; reuse it as the result. ML6's promote-to-
        // registers will turn this into a proper SSA update.
        defineValue(id, *base);
    }

    // Shared N-operand value-producing lowering. Consolidates the
    // identical scaffolding of `lowerCast`/`lowerBinaryOp`/`lowerUnaryOp`
    // (opcode-resolve → operand-arity-check → regForValue each operand
    // → newVReg(regClassFor(id)) → addInst → defineValue). Per the
    // cycle-3d simplifier review.
    template <std::size_t Arity>
    void lowerNAryOp(MirInstId id, MnemonicSlot slot) {
        auto const op = opcode(slot);
        if (!op.has_value()) {
            reportMissingOpcode(slot, mirOpcodeName(mir.instOpcode(id)));
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != Arity) {
            reportUnsupported(mir.instOpcode(id), id);
            return;
        }
        std::array<LirOperand, Arity> ops;
        for (std::size_t k = 0; k < Arity; ++k) {
            std::optional<LirReg> const r = regForValue(operands[k]);
            if (!r.has_value()) return;
            ops[k] = LirOperand::makeReg(*r);
        }
        // Result reg class follows the MIR result type — FPR for float
        // ops, VR for vector ops, GPR otherwise.
        LirReg const result = lir.newVReg(regClassFor(id));
        emitInst(*op, result, ops);
        defineValue(id, result);
    }

    void lowerBinaryOp(MirInstId id, MnemonicSlot slot) { lowerNAryOp<2>(id, slot); }
    void lowerUnaryOp (MirInstId id, MnemonicSlot slot) { lowerNAryOp<1>(id, slot); }

    // ── FC1 (V2-4.X): capability-driven MIR SDiv/UDiv/SMod/UMod lowering ──
    // (cycle 10r D-CSUBSET-DIVISION-OP-CODEGEN substrate, generalized
    //  2026-06-10 to close D-CSUBSET-MOD-OP-CODEGEN +
    //  D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-CONTRACT +
    //  D-MIR-TO-LIR-DIV-SEQUENCE-AGNOSTIC)
    //
    // ONE arm serves all four division-family MIR opcodes on every
    // target, selecting the realization from the target's DECLARED
    // OPCODE VOCABULARY (mnemonic-presence probing — the sanctioned
    // pattern `lea`/`call` already use; never an arch identity):
    //
    //   Rule 1 — NATIVE: the target declares a `result: value` opcode
    //     under the verb's own mnemonic ("sdiv"/"udiv"/"smod"/"umod")
    //     → ONE 3-address LIR op. (arm64 SDIV/UDIV; a future RISC-V
    //     DIV/DIVU/REM/REMU hits all four.) A `result: none`
    //     declaration under these mnemonics is a schema
    //     misdeclaration → fail loud, never a silent fall-through to
    //     another realization the author did not intend.
    //
    //   Rule 2 — IMPLICIT-REGISTER PAIR: the target declares the
    //     pre/core pair (x86: cqo+idiv_op signed; xor_rdx_zero+div_op
    //     unsigned) → the 4-LIR-op sequence in `emitImplicitPairDiv`;
    //     the SSA result is captured from the implicit output
    //     selected BY ROLE ("quotient" for div, "remainder" for mod)
    //     via the core op's `outputRoles` declaration — never by
    //     positional index (D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-
    //     CONTRACT: a JSON reorder of `outputs` can no longer
    //     silently flip a quotient capture into a remainder capture).
    //     The dividend pin likewise resolves via `inputRoles` role
    //     "dividend". A HALF-declared pair (pre without core or vice
    //     versa) is a misconfiguration → fail loud naming the absent
    //     half — it does NOT silently fall through to rule 3.
    //
    //   Rule 3 — EXPANSION (remainder only): no native rem and no
    //     pair, but a div realization (rule 1/2) plus the universal
    //     `mul` + `sub` verbs exist → the target-blind arithmetic
    //     identity   rem = n − (n / d) · d   (arm64 SMod/UMod:
    //     sdiv/udiv + mul + sub = 3 LIR ops). C truncated-remainder
    //     semantics hold by construction — the identity IS C23
    //     6.5.5's definition of % over truncated /. A fused MSUB
    //     realization (2 ops) is a peephole-quality refinement,
    //     pinned D-LIR-MOD-MSUB-FUSION (needs an `Ra` encoding slot
    //     kind the fixed32 vocabulary does not have).
    //
    //   Else → fail loud (no division realization declared).
    //
    // The previously sketched alternative — one result-bearing LIR op
    // whose result vreg the REGALLOC pins to the implicit output
    // register — was rejected at the FC1 plan-lock: no regalloc
    // pre-coloring mechanism exists (call results use an explicit
    // post-regalloc mov in `materializeOneFunc`), and building one
    // for a perf-only delta would speculatively erect the deferred
    // D-ML7-2.5 substrate.
    //
    // **Cycle 10r split rationale** (preserved) — cycle 10q packaged
    // CQO+IDIV into a single compound op with opcode bytes `[0x48,
    // 0x99, 0x48, 0xF7]`. The encoder auto-emits ONE REX prefix at
    // the start (e.g. `0x41` for REX.B when the divisor is in R14);
    // the embedded second `0x48` (REX.W only) then SUPERSEDED the
    // auto-REX prefix, losing REX.B. The IDIV operand decodes as
    // `modrm.rm low 3 bits` = 6 = RSI (without REX.B) instead of
    // R14 — dividing by garbage → STATUS_INTEGER_DIVIDE_BY_ZERO.
    // Splitting into two opcodes lets each instruction's REX be
    // auto-computed correctly with its operand's high bit.
    //
    // FLAG 1 (silent-miscompile guard, 2026-06-04, preserved): SDiv/
    // SMod route pre=cqo + core=idiv_op; UDiv/UMod route
    // pre=xor_rdx_zero + core=div_op. Routing the unsigned ops
    // through idiv would mis-sign-interpret any dividend ≥ INT_MAX
    // as negative — silent miscompile.
    void lowerDivLike(MirInstId id, bool isSigned, bool wantRemainder) {
        auto const operands = mir.instOperands(id);
        if (operands.size() != 2) {
            reportUnsupported(mir.instOpcode(id), id);
            return;
        }
        std::optional<LirReg> const dividend = regForValue(operands[0]);
        std::optional<LirReg> const divisor  = regForValue(operands[1]);
        if (!dividend.has_value() || !divisor.has_value()) return;
        auto const result =
            emitDivLikeValue(id, isSigned, wantRemainder, *dividend, *divisor);
        if (!result.has_value()) return;  // diagnostic already emitted
        defineValue(id, *result);
    }

    // Emits the LIR realization of one division-family value and
    // returns the vreg holding the requested quotient/remainder
    // (nullopt = no realization available / misdeclared; diagnostic
    // already emitted). Split from `lowerDivLike` so rule 3's
    // expansion can realize its inner DIVISION through the same
    // rule-1/2 probing without re-running operand validation or
    // defining the MIR value twice.
    [[nodiscard]] std::optional<LirReg> emitDivLikeValue(
            MirInstId id, bool isSigned, bool wantRemainder,
            LirReg dividend, LirReg divisor) {
        // Rule 1 — native result-bearing opcode under the verb's mnemonic.
        MnemonicSlot const nativeSlot = wantRemainder
            ? (isSigned ? MnemonicSlot::SModNative : MnemonicSlot::UModNative)
            : (isSigned ? MnemonicSlot::SDivNative : MnemonicSlot::UDivNative);
        if (auto const nativeOp = opcode(nativeSlot); nativeOp.has_value()) {
            auto const* ni = target.opcodeInfo(*nativeOp);
            if (ni == nullptr || ni->result != TargetResultRule::Value) {
                reportUnsupported(mir.instOpcode(id), id);
                return std::nullopt;
            }
            LirReg const result = lir.newVReg(regClassFor(id));
            std::array<LirOperand, 2> const ops{
                LirOperand::makeReg(dividend), LirOperand::makeReg(divisor)};
            emitInst(*nativeOp, result, ops);
            return result;
        }
        // Rule 2 — implicit-register pre/core pair.
        DivSlotPair const pair = isSigned ? kSDivPair : kUDivPair;
        auto const preOp  = opcode(pair.pre);
        auto const coreOp = opcode(pair.core);
        if (preOp.has_value() || coreOp.has_value()) {
            if (!preOp.has_value()) {
                reportMissingOpcode(pair.pre,
                                    mirOpcodeName(mir.instOpcode(id)));
                return std::nullopt;
            }
            if (!coreOp.has_value()) {
                reportMissingOpcode(pair.core,
                                    mirOpcodeName(mir.instOpcode(id)));
                return std::nullopt;
            }
            return emitImplicitPairDiv(id, *preOp, *coreOp, wantRemainder,
                                       dividend, divisor);
        }
        // Rule 3 — generic remainder expansion over div + mul + sub.
        if (wantRemainder) {
            auto const quotient = emitDivLikeValue(
                id, isSigned, /*wantRemainder=*/false, dividend, divisor);
            if (!quotient.has_value()) return std::nullopt;
            auto const mulOp = opcode(MnemonicSlot::Mul);
            if (!mulOp.has_value()) {
                reportMissingOpcode(MnemonicSlot::Mul,
                                    mirOpcodeName(mir.instOpcode(id)));
                return std::nullopt;
            }
            auto const subOp = opcode(MnemonicSlot::Sub);
            if (!subOp.has_value()) {
                reportMissingOpcode(MnemonicSlot::Sub,
                                    mirOpcodeName(mir.instOpcode(id)));
                return std::nullopt;
            }
            LirReg const product = lir.newVReg(regClassFor(id));
            std::array<LirOperand, 2> const mulOps{
                LirOperand::makeReg(*quotient), LirOperand::makeReg(divisor)};
            emitInst(*mulOp, product, mulOps);
            LirReg const remainder = lir.newVReg(regClassFor(id));
            std::array<LirOperand, 2> const subOps{
                LirOperand::makeReg(dividend), LirOperand::makeReg(product)};
            emitInst(*subOp, remainder, subOps);
            return remainder;
        }
        // No realization for a plain division: report the native verb
        // mnemonic as the canonical missing capability.
        reportMissingOpcode(nativeSlot, mirOpcodeName(mir.instOpcode(id)));
        return std::nullopt;
    }

    // Rule 2 body — the implicit-register pre/core sequence:
    //   mov  <dividend-role>_phys, dividend_vreg   ; pin dividend
    //   PRE                                        ; cqo / xor_rdx_zero
    //   CORE divisor_vreg                          ; idiv_op / div_op
    //   mov  result_vreg, <capture-role>_phys      ; quotient/remainder
    [[nodiscard]] std::optional<LirReg> emitImplicitPairDiv(
            MirInstId id, std::uint16_t preOpId, std::uint16_t coreOpId,
            bool wantRemainder, LirReg dividend, LirReg divisor) {
        auto const movOp = opcode(MnemonicSlot::Mov);
        if (!movOp.has_value()) {
            reportMissingOpcode(MnemonicSlot::Mov, "MIR Div/Mod (capture)");
            return std::nullopt;
        }
        // 7-agent review fold F2 (silent-failure 8/10, 2026-06-04):
        // validate that BOTH the pre AND core op carry implicit-
        // register declarations. If a future JSON edit drops
        // `implicitRegisters` from `cqo` or `xor_rdx_zero`, regalloc's
        // `collectImplicitClobberPositions` skips that position with
        // no diagnostic — divisor vreg could land in RDX, get zeroed
        // by xor, idiv would divide by zero. Fail loud at the lowering
        // tier captures the misconfiguration BEFORE regalloc silently
        // mis-allocates.
        auto const* preInfo = target.opcodeInfo(preOpId);
        if (preInfo == nullptr
         || !preInfo->implicitRegisters.has_value()
         || preInfo->implicitRegisters->clobberedOrdinals.empty()) {
            reportUnsupported(mir.instOpcode(id), id);
            return std::nullopt;
        }
        auto const* coreInfo = target.opcodeInfo(coreOpId);
        if (coreInfo == nullptr
         || !coreInfo->implicitRegisters.has_value()) {
            reportUnsupported(mir.instOpcode(id), id);
            return std::nullopt;
        }
        // Role-based register resolution (D-CSUBSET-MOD-OP-CODEGEN-
        // OUTPUT-INDEX-CONTRACT): the dividend pin and the captured
        // output are looked up BY ROLE from the core op's
        // inputRoles/outputRoles declarations — positional indexing
        // is gone from this projection path. An op missing the
        // required role is a misconfiguration → fail loud.
        auto const& coreIr = *coreInfo->implicitRegisters;
        auto const dividendOrdinal = coreIr.inputOrdinalForRole("dividend");
        if (!dividendOrdinal.has_value()) {
            reportMissingImplicitRole(coreOpId, "inputRoles", "dividend", id);
            return std::nullopt;
        }
        char const* const captureRole =
            wantRemainder ? "remainder" : "quotient";
        auto const captureOrdinal = coreIr.outputOrdinalForRole(captureRole);
        if (!captureOrdinal.has_value()) {
            reportMissingImplicitRole(coreOpId, "outputRoles", captureRole,
                                      id);
            return std::nullopt;
        }
        // 7-agent fold F1 (HIGH, 2026-06-04): read register CLASS
        // from the schema's register table (NOT hardcoded GPR). A
        // hypothetical future target whose dividend lives in an
        // FPR-class register would silently misallocate if this
        // hardcoded `LirRegClass::GPR`. The cast is sound — the two
        // enums are statically asserted identical-shape at
        // `lir_reg.hpp:96-100`.
        auto const* dividendRegInfo = target.registerInfo(*dividendOrdinal);
        auto const* captureRegInfo  = target.registerInfo(*captureOrdinal);
        if (dividendRegInfo == nullptr || captureRegInfo == nullptr) {
            reportUnsupported(mir.instOpcode(id), id);
            return std::nullopt;
        }
        LirRegClass const dividendCls =
            static_cast<LirRegClass>(dividendRegInfo->regClass);
        LirRegClass const captureCls =
            static_cast<LirRegClass>(captureRegInfo->regClass);
        LirReg const dividendPhys =
            makePhysicalReg(*dividendOrdinal, dividendCls);

        // 1. Pin dividend into the role-declared register.
        std::array<LirOperand, 1> const movInOps{
            LirOperand::makeReg(dividend)};
        emitInst(*movOp, dividendPhys, movInOps);

        // 2. Emit PRE op (CQO / XOR RDX,RDX). Zero operands, no
        //    SSA result. The op declares its own implicit-input/
        //    output/clobber set (RAX → RDX for CQO; RDX clobber for
        //    XOR). Regalloc reads each PRE op's implicit-register
        //    declaration independently — no extra coupling required.
        emitInst(preOpId, InvalidLirReg, std::span<LirOperand const>{});

        // 3. Emit CORE op (IDIV /7 or DIV /6). One operand (divisor
        //    in modrm.rm), no SSA result; the op declares
        //    `result: none` because outputs live in implicit phys
        //    regs. Regalloc reads the implicit-clobbered set from
        //    the schema and excludes the clobbered regs from any
        //    range that COVERS this position.
        std::array<LirOperand, 1> const divOps{
            LirOperand::makeReg(divisor)};
        emitInst(coreOpId, InvalidLirReg, divOps);

        // 4. Capture the role-selected output (quotient for div,
        //    remainder for mod) into a fresh SSA result.
        LirReg const result = lir.newVReg(regClassFor(id));
        LirReg const capturePhys =
            makePhysicalReg(*captureOrdinal, captureCls);
        std::array<LirOperand, 1> const captureOps{
            LirOperand::makeReg(capturePhys)};
        emitInst(*movOp, result, captureOps);
        return result;
    }

    void reportMissingImplicitRole(std::uint16_t opId, char const* mapName,
                                   char const* role, MirInstId at) {
        auto const* info = target.opcodeInfo(opId);
        ParseDiagnostic d;
        d.code     = DiagnosticCode::L_RequiredLirOpcodeMissing;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format(
            "target '{}': opcode '{}' declares no '{}' role in "
            "implicitRegisters.{} — required to lower MIR '{}' (inst {}); "
            "the div/mod projection resolves registers BY ROLE, never by "
            "positional index (D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-"
            "CONTRACT)",
            target.name(), info != nullptr ? info->mnemonic : "?",
            role, mapName, mirOpcodeName(mir.instOpcode(at)), at.v);
        reporter.report(std::move(d));
    }

    // MIR ICmp{Eq,Ne,Slt,...} → LIR `cmp` + `setcc(cond)` pair. Naive
    // (no peephole with subsequent CondBr); the optimizer's compare-flag
    // forwarding pass will collapse `cmp/setcc/cmp 0/jcc-ne` to
    // `cmp/jcc-cond` once a use-count analysis lands.
    void lowerICmp(MirInstId id, TargetCondCode cond) {
        if (!opcode(MnemonicSlot::Cmp).has_value()) {
            reportMissingOpcode(MnemonicSlot::Cmp, "MIR ICmp");
            return;
        }
        if (!opcode(MnemonicSlot::Setcc).has_value()) {
            reportMissingOpcode(MnemonicSlot::Setcc, "MIR ICmp");
            return;
        }
        if (!opcode(MnemonicSlot::ZExt).has_value()) {
            reportMissingOpcode(MnemonicSlot::ZExt, "MIR ICmp");
            return;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != 2) {
            reportUnsupported(mir.instOpcode(id), id);
            return;
        }
        std::optional<LirReg> const a = regForValue(operands[0]);
        std::optional<LirReg> const b = regForValue(operands[1]);
        if (!a.has_value() || !b.has_value()) return;
        // FLAGS is implicit pre-regalloc; ML6 makes it explicit. Emit the
        // cmp + setcc as paired instructions; the substrate guarantees no
        // value-producing inst sits between them (cycle 3a fallback-seal
        // + cycle 3b ICmp-immediately-followed-by-setcc).
        std::array<LirOperand, 2> cmpOps{LirOperand::makeReg(*a), LirOperand::makeReg(*b)};
        emitInst(*opcode(MnemonicSlot::Cmp), InvalidLirReg, cmpOps);

        // D-LIR-SETCC-WIDTH-CONTRACT (step 13.5 cycle 1 post-fold,
        // code-reviewer C2): emit setcc into a separate "byte" vreg,
        // then movzx the low byte into the result vreg. This makes
        // the substrate contract explicit: setcc writes the LOW
        // BYTE of its r/m8 destination; the upper 56 bits are
        // architecturally undefined per Intel SDM. The standalone
        // zext makes the zero-extend a first-class LIR operation
        // so the result is always a CLEAN 0/1 r64 — `cmp r64, 0`
        // / `cmp r64, r64` consumers downstream cannot read garbage.
        // The ICmp+CondBr fusion at `lowerCondBr` still elides the
        // cmp-against-0 (the setcc + zext become "dead" via D-LIR-
        // SETCC-DEAD-AFTER-FUSION DCE in 13.6), but non-adjacent
        // ICmp uses (bool stored, bool returned, bool ternary) now
        // get correct width semantics for free. Clean SSA: setcc
        // defines `b8`; zext consumes `b8` AND defines `result`.
        LirReg const b8 = lir.newVReg(LirRegClass::GPR);
        emitInst(*opcode(MnemonicSlot::Setcc), b8, std::span<LirOperand const>{},
                    /*payload=*/static_cast<std::uint32_t>(cond));
        LirReg const result = lir.newVReg(LirRegClass::GPR);
        std::array<LirOperand, 1> zextOps{LirOperand::makeReg(b8)};
        emitInst(*opcode(MnemonicSlot::ZExt), result, zextOps);
        defineValue(id, result);
    }

    // ── terminator + phi resolution ──────────────────────────────────

    // Emit the phi-edge moves for one (predecessor, successor) edge.
    //
    // Algorithm — parallel-copy resolution via temporaries:
    //   For each phi P in `successorMir` with incoming I_P from
    //   `currentMir`, build the pair (phiReg_P, incomingReg_P). Multiple
    //   phis in the same block may form a copy graph where phi A's
    //   incoming is phi B's pre-allocated result (the classic SSA-
    //   destruction "swap" hazard). Naive sequential `mov phi, incoming`
    //   would corrupt later reads of the overwritten incomingReg.
    //
    //   Two-step emission breaks all dependency cycles unconditionally:
    //   (1) for each (phiReg, incomingReg) pair, mint a fresh `tmpReg`
    //       and emit `mov tmpReg, incomingReg`. The incomingReg's value
    //       is now captured in a vreg that no subsequent phi-move reads.
    //   (2) for each pair, emit `mov phiReg, tmpReg`.
    //
    //   Trade-off: O(n) extra vregs vs O(n^2) cycle detection. Pre-
    //   regalloc this is cheap; ML6 coalescing will collapse the temps
    //   into direct copies where the dep-graph allows.
    //
    // The diagnostic shape pins:
    //  - phi without a pre-allocated vreg (substrate bug) → loud
    //  - incoming `value` resolves to a missing vreg → phi-specific
    //    diagnostic (NOT just regForValue's generic one), and the move
    //    is SKIPPED so a poisoned tmp does not silently propagate
    //  - phi without an incoming for this predecessor → loud
    void emitPhiMovesForEdge(MirBlockId currentMir, MirBlockId successorMir) {
        // Collect (phiReg, incomingReg) pairs for this edge.
        struct Pair { LirReg phi; LirReg incoming; };
        std::vector<Pair> pairs;
        std::uint32_t const n = mir.blockInstCount(successorMir);
        for (std::uint32_t i = 0; i < n; ++i) {
            MirInstId const inst = mir.blockInstAt(successorMir, i);
            if (mir.instOpcode(inst) != MirOpcode::Phi) continue;
            if (!valueToReg.has(inst)) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "MIR Phi %{} reached edge-move emission without a "
                    "pre-allocated LirReg — pre-pass invariant broken",
                    inst.v);
                reporter.report(std::move(d));
                continue;
            }
            LirReg const phiReg = valueToReg.get(inst);
            bool matched = false;
            for (MirPhiIncoming const& inc : mir.phiIncomings(inst)) {
                if (inc.pred != currentMir) continue;
                matched = true;
                std::optional<LirReg> const v = regForValue(inc.value);
                if (!v.has_value()) {
                    // regForValue already emitted a generic diagnostic;
                    // add the phi-edge-specific context so the failure is
                    // locally attributable.
                    ParseDiagnostic d;
                    d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
                    d.severity = DiagnosticSeverity::Error;
                    d.actual   = std::format(
                        "MIR Phi %{} edge from predecessor block %{} lost "
                        "its incoming value %{} — phi result will be "
                        "unwritten on this edge",
                        inst.v, currentMir.v, inc.value.v);
                    reporter.report(std::move(d));
                    break;
                }
                pairs.push_back(Pair{phiReg, *v});
                break;
            }
            if (!matched) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "MIR Phi %{} has no incoming for predecessor block %{}",
                    inst.v, currentMir.v);
                reporter.report(std::move(d));
            }
        }
        if (pairs.empty()) return;
        // Step 1: copy every incoming into a fresh temp. Breaks any
        // dependency cycle on the phi-reg side unconditionally.
        // Cycle 3d FPR plumbing: each temp's class follows the phi's
        // (which equals the incoming's, since SSA guarantees same-
        // typed flow on each edge). An F64 phi must use FPR-class
        // temps so the parallel-copy chain stays consistent.
        // FC2 Part B: each copy's mnemonic follows the pair's class
        // via the registerClassOps table (FPR pairs copy via movaps,
        // never the GPR mov).
        std::vector<LirReg> temps;
        temps.reserve(pairs.size());
        for (auto const& p : pairs) {
            auto const movOp = classOp(p.phi.regClass(), RegClassOp::Move);
            if (!movOp.has_value()) {
                reportMissingClassOp(p.phi.regClass(), RegClassOp::Move,
                                     "MIR Phi edge");
                return;
            }
            LirReg const tmp = lir.newVReg(p.phi.regClass());
            std::array<LirOperand, 1> ops{LirOperand::makeReg(p.incoming)};
            emitInst(*movOp, tmp, ops);
            temps.push_back(tmp);
        }
        // Step 2: write each phi-result from its captured temp.
        for (std::size_t k = 0; k < pairs.size(); ++k) {
            auto const movOp = classOp(pairs[k].phi.regClass(), RegClassOp::Move);
            if (!movOp.has_value()) return;  // diagnosed in step 1
            std::array<LirOperand, 1> ops{LirOperand::makeReg(temps[k])};
            emitInst(*movOp, pairs[k].phi, ops);
        }
    }

    // Returns true iff the terminator was emitted (block sealed). `false`
    // signals the caller to use the fallback seal.
    bool lowerTerminator(MirBlockId mb, MirInstId termId) {
        currentMir = termId;
        MirOpcode const op = mir.instOpcode(termId);
        auto succs = mir.blockSuccessors(mb);

        // Emit phi-edge moves for EVERY MIR successor of this block, in
        // declared order, BEFORE the terminator. This dominates every use
        // of the phi result in the successor regardless of which jcc arm
        // ends up taking the edge.
        for (MirBlockId s : succs) {
            emitPhiMovesForEdge(mb, s);
        }

        switch (op) {
            case MirOpcode::Return:      return lowerReturn(termId);
            case MirOpcode::Br:          return lowerBr(termId, succs);
            case MirOpcode::CondBr:      return lowerCondBr(termId, succs);
            case MirOpcode::Switch:      return lowerSwitch(termId, succs);
            case MirOpcode::Unreachable: return lowerUnreachable(termId);
            default:                     break;
        }
        reportUnsupported(op, termId);
        return false;
    }

    bool lowerReturn(MirInstId id) {
        if (!opcode(MnemonicSlot::Ret).has_value()) {
            reportMissingOpcode(MnemonicSlot::Ret, "MIR Return");
            return false;
        }
        auto const operands = mir.instOperands(id);
        if (operands.empty()) {
            emitReturn(*opcode(MnemonicSlot::Ret), std::span<LirOperand const>{});
            return true;
        }
        if (operands.size() != 1) {
            reportUnsupported(MirOpcode::Return, id);
            return false;
        }
        std::optional<LirReg> const v = regForValue(operands[0]);
        if (!v.has_value()) return false;
        std::array<LirOperand, 1> ops{LirOperand::makeReg(*v)};
        emitReturn(*opcode(MnemonicSlot::Ret), ops);
        return true;
    }

    bool lowerBr(MirInstId id, std::span<MirBlockId const> succs) {
        if (!opcode(MnemonicSlot::Jmp).has_value()) {
            reportMissingOpcode(MnemonicSlot::Jmp, "MIR Br");
            return false;
        }
        if (succs.size() != 1) {
            reportUnsupported(MirOpcode::Br, id);
            return false;
        }
        if (!mirBlockToLirBlock.has(succs[0])) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "MIR Br target block %{} has no LIR block mapping",
                succs[0].v);
            reporter.report(std::move(d));
            return false;
        }
        emitBr(*opcode(MnemonicSlot::Jmp), mirBlockToLirBlock.get(succs[0]));
        return true;
    }

    bool lowerCondBr(MirInstId id, std::span<MirBlockId const> succs) {
        if (!opcode(MnemonicSlot::Cmp).has_value()) {
            reportMissingOpcode(MnemonicSlot::Cmp, "MIR CondBr");
            return false;
        }
        if (!opcode(MnemonicSlot::Jcc).has_value()) {
            reportMissingOpcode(MnemonicSlot::Jcc, "MIR CondBr");
            return false;
        }
        if (succs.size() != 2) {
            reportUnsupported(MirOpcode::CondBr, id);
            return false;
        }
        auto const operands = mir.instOperands(id);
        if (operands.size() != 1) {
            reportUnsupported(MirOpcode::CondBr, id);
            return false;
        }
        if (!mirBlockToLirBlock.has(succs[0]) || !mirBlockToLirBlock.has(succs[1])) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = "MIR CondBr target block(s) have no LIR block mapping";
            reporter.report(std::move(d));
            return false;
        }
        LirBlockId const lirIfTrue  = mirBlockToLirBlock.get(succs[0]);
        LirBlockId const lirIfFalse = mirBlockToLirBlock.get(succs[1]);
        // D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1):
        // ICmp+CondBr FUSION. When the cond operand was produced by
        // an ICmp instruction, replace the naive
        // `cmp result_b8, 0; jcc-NE` sequence (which would read the
        // garbage upper 56 bits of setcc's r8 result and trip the
        // branch the wrong way) with a single fused `cmp lhs, rhs;
        // jcc-cond` using the ICmp's args and condition directly.
        // The setcc emitted by lowerICmp becomes dead code (its
        // result has no LIR consumer in this fused path); a future
        // dead-LIR-instruction-elimination pass anchored
        // D-LIR-SETCC-DEAD-AFTER-FUSION removes the wasted bytes.
        // This pre-empts the optimizer fusion (planned at 13.6)
        // because the substrate would otherwise be load-bearing
        // broken for every `if/while` until then.
        //
        // The non-fusable arm (cond from a non-ICmp source — bool
        // param, bool load, etc.) keeps the existing cmp-against-0
        // path. Those producers ARE responsible for zero-extending
        // their bool result; they hit the cmp-r64-against-0 path
        // correctly.
        MirInstId const condInst = operands[0];
        MirOpcode const condOp   = mir.instOpcode(condInst);
        auto const fusedCc       = condCodeForICmp(condOp);
        TargetCondCode jccCond;
        if (fusedCc.has_value()) {
            auto const icmpOperands = mir.instOperands(condInst);
            if (icmpOperands.size() != 2) {
                reportUnsupported(condOp, condInst);
                return false;
            }
            std::optional<LirReg> const a = regForValue(icmpOperands[0]);
            std::optional<LirReg> const b = regForValue(icmpOperands[1]);
            if (!a.has_value() || !b.has_value()) return false;
            std::array<LirOperand, 2> cmpOps{
                LirOperand::makeReg(*a), LirOperand::makeReg(*b)};
            emitInst(*opcode(MnemonicSlot::Cmp), InvalidLirReg, cmpOps);
            jccCond = *fusedCc;
        } else {
            std::optional<LirReg> const cond = regForValue(operands[0]);
            if (!cond.has_value()) return false;
            std::array<LirOperand, 2> cmpOps{
                LirOperand::makeReg(*cond), LirOperand::makeImmInt32(0)};
            emitInst(*opcode(MnemonicSlot::Cmp), InvalidLirReg, cmpOps);
            jccCond = TargetCondCode::Ne;
        }
        // Pass both successors as BlockRef operands. jcc remains
        // the block's terminator (schema's `cond-br` shape, 2
        // successors); the encoder reads operand[0] (taken target)
        // for the BlockRel32 displacement AND emits a trailing
        // unconditional `jmp` to operand[1] (fallthrough) so the
        // LIR block layout doesn't need to guarantee fallthrough
        // order. A future optimizer pass elides the redundant
        // trailing jmp when ifFalse IS the next-laid-out block
        // (anchored D-OPT-JCC-FALLTHROUGH).
        std::array<LirOperand, 2> jccOps{
            LirOperand::makeBlockRef(lirIfTrue.v),
            LirOperand::makeBlockRef(lirIfFalse.v)};
        emitCondBr(*opcode(MnemonicSlot::Jcc), jccOps,
                      lirIfTrue, lirIfFalse,
                      static_cast<std::uint32_t>(jccCond));
        return true;
    }

    // Cascading-compare lowering of MIR Switch. Operands: [discriminant,
    // case_const_0, case_const_1, ...]. Successors: [case_target_0, ...,
    // case_target_{N-1}, default_target].
    //
    // For each case i in [0, N): emit a fresh "compare" LIR block that does
    // `cmp discrim, case_const[i]; jcc(eq) case_target[i], next_compare`.
    // The chain terminates at a block that `jmp default_target`.
    //
    // We've already emitted phi-edge moves for ALL successors at the top of
    // `lowerTerminator`. The cascading-compare path here flows through
    // freshly-created compare blocks; jumping into the case_target preserves
    // the dominance of the phi moves (which were emitted at the END of the
    // ORIGINAL switch-bearing block, before this first jmp).
    bool lowerSwitch(MirInstId id, std::span<MirBlockId const> succs) {
        if (!opcode(MnemonicSlot::Cmp).has_value()) {
            reportMissingOpcode(MnemonicSlot::Cmp, "MIR Switch");
            return false;
        }
        if (!opcode(MnemonicSlot::Jmp).has_value()) {
            reportMissingOpcode(MnemonicSlot::Jmp, "MIR Switch");
            return false;
        }
        if (!opcode(MnemonicSlot::Jcc).has_value()) {
            reportMissingOpcode(MnemonicSlot::Jcc, "MIR Switch");
            return false;
        }
        auto const operands = mir.instOperands(id);
        if (operands.empty() || succs.size() < 1
            || operands.size() != succs.size()) {
            // Convention: 1 discriminant + N case-constants in operands;
            // N case-targets + 1 default = N+1 successors. So operands.size
            // = N+1 and succs.size = N+1 too.
            reportUnsupported(MirOpcode::Switch, id);
            return false;
        }
        std::optional<LirReg> const discrim = regForValue(operands[0]);
        if (!discrim.has_value()) return false;

        std::size_t const caseCount = operands.size() - 1;
        MirBlockId const defaultMir = succs[caseCount];
        if (!mirBlockToLirBlock.has(defaultMir)) {
            reportUnsupported(MirOpcode::Switch, id);
            return false;
        }

        // Pin the implicit invariant that the FIRST compare AND its
        // paired jcc both land in the switch-bearing block that was
        // open when `lowerSwitch` was called. A future refactor that
        // creates a new block between entry and the first compare —
        // or between the first compare and its jcc — would shift
        // emission into a different block and silently break
        // dominance for the phi moves the cycle-3b prepass emitted on
        // the ORIGINAL switch-bearing block. Two pin sites cover the
        // pre-cmp and the pre-jcc emission boundaries.
        LirBlockId const switchHeader = lir.openBlock();

        // Emit the first compare in-place; subsequent compares go in
        // freshly-allocated "next-compare" blocks that we register on the
        // open function.
        for (std::size_t i = 0; i < caseCount; ++i) {
            std::optional<LirReg> const caseConst = regForValue(operands[1 + i]);
            if (!caseConst.has_value()) return false;
            if (!mirBlockToLirBlock.has(succs[i])) {
                reportUnsupported(MirOpcode::Switch, id);
                return false;
            }
            // First-iteration pre-cmp pin: the open block must still
            // be `switchHeader` when the first compare emits.
            if (i == 0 && lir.openBlock() != switchHeader) {
                reportUnsupported(MirOpcode::Switch, id);
                return false;
            }
            std::array<LirOperand, 2> cmpOps{
                LirOperand::makeReg(*discrim), LirOperand::makeReg(*caseConst)};
            emitInst(*opcode(MnemonicSlot::Cmp), InvalidLirReg, cmpOps);

            // First-iteration pre-jcc pin: the open block must still
            // be `switchHeader` after the compare emits and before
            // the jcc emits (the jcc is the terminator that seals the
            // header block — anything between cmp and jcc must stay
            // on the header for phi-move dominance).
            if (i == 0 && lir.openBlock() != switchHeader) {
                reportUnsupported(MirOpcode::Switch, id);
                return false;
            }

            // Allocate a "next compare" block unless this is the last
            // case (in which case we fall through to a `jmp default`).
            // The jcc condition is `Eq` (case matches the constant).
            std::uint32_t const eqCond =
                static_cast<std::uint32_t>(TargetCondCode::Eq);
            LirBlockId nextBlock;
            bool const isLastCase = (i + 1 == caseCount);
            LirBlockId const caseTarget = mirBlockToLirBlock.get(succs[i]);
            // D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1
            // post-fold, code-reviewer C1): jcc now requires the
            // 2 BlockRef operands as its operand list (matches the
            // schema's `["blockref", "blockref"]` guard). Pre-fold
            // the switch lowering passed empty operands — the
            // encoder would have failed `A_NoMatchingEncodingVariant`
            // on EVERY switch statement; the gap slipped because
            // no runnable corpus example exercises switch yet.
            if (isLastCase) {
                // Final compare: jcc-eq to case target, else jcc fallthrough
                // to a tiny "jmp default" block.
                LirBlockId const defaultJump = lir.createBlock();
                std::array<LirOperand, 2> jccOps{
                    LirOperand::makeBlockRef(caseTarget.v),
                    LirOperand::makeBlockRef(defaultJump.v)};
                emitCondBr(*opcode(MnemonicSlot::Jcc), jccOps,
                              caseTarget, defaultJump, eqCond);
                lir.beginBlock(defaultJump);
                emitBr(*opcode(MnemonicSlot::Jmp), mirBlockToLirBlock.get(defaultMir));
                return true;
            }
            nextBlock = lir.createBlock();
            std::array<LirOperand, 2> jccOps{
                LirOperand::makeBlockRef(caseTarget.v),
                LirOperand::makeBlockRef(nextBlock.v)};
            emitCondBr(*opcode(MnemonicSlot::Jcc), jccOps,
                          caseTarget, nextBlock, eqCond);
            lir.beginBlock(nextBlock);
        }
        // No cases (only default): jmp default.
        emitBr(*opcode(MnemonicSlot::Jmp), mirBlockToLirBlock.get(defaultMir));
        return true;
    }

    bool lowerUnreachable(MirInstId /*id*/) {
        if (!opcode(MnemonicSlot::Unreachable).has_value()) {
            reportMissingOpcode(MnemonicSlot::Unreachable, "MIR Unreachable");
            return false;
        }
        emitUnreachable(*opcode(MnemonicSlot::Unreachable));
        return true;
    }

    // ── per-block / per-function lowering ────────────────────────────

    // Pre-pass over a function's Phi insts: allocate a fresh LirReg for
    // each phi result so predecessor blocks can emit `mov phi_reg, ...`
    // moves at the back-edge BEFORE the phi block is itself lowered.
    void prepassAllocatePhis(MirFuncId mf) {
        std::uint32_t const blockCount = mir.funcBlockCount(mf);
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            MirBlockId const mb = mir.funcBlockAt(mf, bi);
            std::uint32_t const n = mir.blockInstCount(mb);
            for (std::uint32_t i = 0; i < n; ++i) {
                MirInstId const inst = mir.blockInstAt(mb, i);
                if (mir.instOpcode(inst) != MirOpcode::Phi) continue;
                // Cycle 3d FPR plumbing: phi result class follows the
                // phi's MIR result type. An F64-typed phi (ternary
                // join on doubles, loop-carried float) must use an
                // FPR-class vreg so downstream FPR consumers see the
                // right class.
                LirReg const r = lir.newVReg(regClassFor(inst));
                defineValue(inst, r);
            }
        }
    }

    void lowerBlock(MirBlockId mb) {
        LirBlockId const lb = mirBlockToLirBlock.get(mb);
        lir.beginBlock(lb);
        std::uint32_t const n = mir.blockInstCount(mb);
        if (n == 0) {
            // Empty MIR block — defensive seal so the LIR module finishes.
            if (opcode(MnemonicSlot::Ret).has_value()) {
                emitReturn(*opcode(MnemonicSlot::Ret), std::span<LirOperand const>{});
            }
            return;
        }
        // Lower non-terminator instructions first.
        //
        // Two structural-violation guards run as we iterate:
        //   - terminator in non-last position
        //   - Phi at a non-leading position (Phi must precede every non-
        //     Phi inst by MIR's structural rule; cycle 3b's `lowerInst`
        //     no-ops Phi by design so we must catch mis-positioned Phis
        //     loud here)
        MirInstId const term = mir.blockInstAt(mb, n - 1);
        bool sawNonPhi = false;
        for (std::uint32_t i = 0; i + 1 < n; ++i) {
            MirInstId const inst = mir.blockInstAt(mb, i);
            MirOpcode const op = mir.instOpcode(inst);
            if (isMirTerminator(op)) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "MIR terminator '{}' (inst {}) in non-last position — "
                    "structural violation",
                    mirOpcodeName(op), inst.v);
                reporter.report(std::move(d));
            }
            if (op == MirOpcode::Phi && sawNonPhi) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = std::format(
                    "MIR Phi (inst {}) follows a non-Phi instruction — "
                    "structural violation (Phis must lead the block)",
                    inst.v);
                reporter.report(std::move(d));
            }
            if (op != MirOpcode::Phi) sawNonPhi = true;
            lowerInst(inst);
        }
        // Then the terminator (which is also responsible for emitting
        // phi-edge moves to all its MIR successors BEFORE the LIR
        // terminator itself).
        bool sealed = false;
        if (isMirTerminator(mir.instOpcode(term))) {
            sealed = lowerTerminator(mb, term);
        } else {
            // Last inst is not a terminator — also malformed MIR.
            ParseDiagnostic d;
            d.code     = DiagnosticCode::L_UnsupportedLoweringForOpcode;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "MIR block ended with non-terminator opcode '{}' (inst {}) "
                "— structural violation",
                mirOpcodeName(mir.instOpcode(term)), term.v);
            reporter.report(std::move(d));
            // Still lower the non-terminator (so its diagnostics surface)
            lowerInst(term);
        }
        // Fallback seal for blocks whose MIR terminator was deferred to a
        // later cycle. The diagnostic was already issued.
        if (!sealed && opcode(MnemonicSlot::Ret).has_value()) {
            emitReturn(*opcode(MnemonicSlot::Ret), std::span<LirOperand const>{});
        }
    }

    void lowerFunction(MirFuncId mf) {
        valueToReg.clear();
        mirBlockToLirBlock.clear();
        lir.addFunction(mir.funcSymbol(mf));

        // Pre-pass 1: pre-allocate LIR blocks (1:1 with MIR blocks).
        std::uint32_t const blockCount = mir.funcBlockCount(mf);
        for (std::uint32_t i = 0; i < blockCount; ++i) {
            MirBlockId const mb = mir.funcBlockAt(mf, i);
            LirBlockId const lb = lir.createBlock();
            mirBlockToLirBlock.set(mb, lb);
        }
        // Pre-pass 2: allocate vregs for all Phi results so back-edge
        // predecessor moves resolve cleanly.
        prepassAllocatePhis(mf);

        // Pass 3: lower bodies block-by-block in MIR's declared order.
        for (std::uint32_t i = 0; i < blockCount; ++i) {
            MirBlockId const mb = mir.funcBlockAt(mf, i);
            lowerBlock(mb);
        }
    }

    // Record the LIR inst → source MIR inst mapping. Resizes the
    // `lirToMir` vector so `lirToMir[li.v]` is always the producer.
    void recordSource(LirInstId li) {
        if (!li.valid()) return;
        if (li.v >= lirToMir.size()) lirToMir.resize(li.v + 1);
        lirToMir[li.v] = currentMir;
    }

    // Forwarding wrappers around `LirBuilder` emitters that AUTO-RECORD
    // the source MIR inst (via `currentMir`). Cycle 3e fix-up: every
    // emission site was previously calling `lir.addInst`/`addBr`/etc.
    // directly, leaving `lirToMir` empty so the verifier was forced to
    // walk by position (the architect-flagged Switch hazard). These
    // wrappers + the `currentMir` discipline keep the mapping
    // continuous.
    LirInstId emitInst(std::uint16_t op, LirReg result,
                       std::span<LirOperand const> ops,
                       std::uint32_t payload = 0,
                       std::uint8_t  flags   = 0) {
        LirInstId const li = lir.addInst(op, result, ops, payload, flags);
        recordSource(li);
        return li;
    }
    LirInstId emitBr(std::uint16_t op, LirBlockId target) {
        LirInstId const li = lir.addBr(op, target);
        recordSource(li);
        return li;
    }
    LirInstId emitCondBr(std::uint16_t op, std::span<LirOperand const> ops,
                         LirBlockId ifT, LirBlockId ifF,
                         std::uint32_t payload = 0) {
        LirInstId const li = lir.addCondBr(op, ops, ifT, ifF, payload);
        recordSource(li);
        return li;
    }
    LirInstId emitReturn(std::uint16_t op, std::span<LirOperand const> ops) {
        LirInstId const li = lir.addReturn(op, ops);
        recordSource(li);
        return li;
    }
    LirInstId emitUnreachable(std::uint16_t op) {
        LirInstId const li = lir.addUnreachable(op);
        recordSource(li);
        return li;
    }

    [[nodiscard]] MirToLirResult run() {
        std::size_t const fnCount = mir.moduleFuncCount();
        for (std::uint32_t i = 0; i < fnCount; ++i) {
            lowerFunction(mir.funcAt(i));
        }
        Lir frozen = std::move(lir).finish();
        // Ensure lirToMir spans every LIR inst slot (any trailing
        // slots without recorded sources default to InvalidMirInst).
        if (lirToMir.size() < frozen.nodeCount()) {
            lirToMir.resize(frozen.nodeCount());
        }
        // Designated initializers prevent a future field reorder
        // from silently rebinding the positional `{}` for
        // `externImports`. (code-simplifier + code-reviewer fold,
        // LK6 cycle 2d post-fold review.)
        return MirToLirResult{
            .lir           = std::move(frozen),
            .lirToMir      = std::move(lirToMir),
            .externImports = {},
            .ok            = !hadError()
        };
    }
};

} // namespace

MirToLirResult lowerToLir(Mir const&          mir,
                          TargetSchema const& target,
                          TypeInterner const& interner,
                          DiagnosticReporter& reporter,
                          std::vector<ExternImport> externImports,
                          std::optional<ExternCallDispatch> externCallDispatch) {
    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY post-fold (2026-06-02): pass
    // the externImports vector to the Lowerer so it can distinguish
    // extern-targeting calls from module-internal direct calls.
    // D-FFI-EXTERN-CALL-DISPATCH: also pass the active format's
    // extern-call shape — `lowerCall` selects `call_indirect_via_extern`
    // (indirect-slot: PE/Mach-O IAT/__got) vs the plain `call`
    // (direct-plt: ELF PLT stub) from it. nullopt + extern imports =
    // fail-loud (no silent default to either shape).
    Lowerer L{mir, target, interner, reporter, externImports,
              externCallDispatch};
    MirToLirResult result = std::move(L).run();
    // Append (not overwrite) so any future LIR-tier extern synthesis
    // — e.g. runtime-helper imports like `__chkstk` / `__divti3` /
    // decimal-runtime hooks the lowerer may need to materialize —
    // is not silently clobbered. The inner `run()` initializes
    // `result.externImports` to `{}` today (no LIR-side synthesis
    // yet); if/when a future cycle starts emitting externs from
    // the LIR lowerer, those rows will compose with the caller-
    // supplied vector instead of being dropped. (silent-failure
    // MEDIUM fold, LK6 cycle 2d post-fold review.)
    result.externImports.insert(result.externImports.end(),
                                std::make_move_iterator(externImports.begin()),
                                std::make_move_iterator(externImports.end()));
    return result;
}

} // namespace dss
