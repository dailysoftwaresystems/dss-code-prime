#pragma once

#include "core/export.hpp"
#include "core/substrate/transparent_string_hash.hpp"
#include "core/types/aggregate_layout.hpp"  // FC6: AggregateLayoutParams
#include "core/types/grammar_schema.hpp"   // ConfigDiagnostic + LoadResult
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"  // TypeKind for regClassForCoreType

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

// `TargetSchema` (plan 12 §2.6 ML5 cycle 2 pivot) — JSON-configured
// compile-target descriptor. Parallel to `GrammarSchema` for the
// frontend: a compile target (x86_64 / arm64 / wasm / spirv / ...)
// is a JSON file, NOT C++ code. Adding a new target = drop a new
// `*.target.json` in `src/dss-config/targets/`; nothing in the
// substrate or LIR builder changes.
//
// Cycle 2a (commit 2609b70): opcode table + slot-0 invalid sentinel.
// Cycle 2b (this revision):  physical register file + calling
// conventions. ML6 regalloc consumes the register file; ML7 callconv
// lowering consumes the calling-convention sections.
//
// Lifecycle: `loadShipped` / `loadFromFile` / `loadFromText` mirror
// `GrammarSchema`'s loaders verbatim. Each call returns a freshly-
// allocated `shared_ptr<TargetSchema>` (no caching here — that's a
// separate concern; the value is move-only and non-mutating after the
// loader returns it). Discovery: cwd-walk for
// `src/dss-config/targets/<name>.target.json` (up to 8 levels).

namespace dss {

// ── Closed-enum name table (substrate) ────────────────────────────
//
// Eight closed enums in this header all carry `XxxName(e)` /
// `XxxFromName(s)` constexpr helpers (TargetAbiModel,
// TargetCondCode, TargetResultRule, TargetRegClass,
// TargetTerminatorKind, TargetEncodingShape, OperandKindFilter,
// EncodingSlotKind). Each helper pair is mechanical — a switch +
// an `if/else if` chain — but the two halves are independent
// sources of truth that must agree (simplifier review's "two ways
// to parse an enum string").
//
// This template gives ONE source of truth (the `kXxxTable` array)
// and derives both helpers from it. Adding a new enumerator: one
// row in the table, no helper edits. The `entry == e` lookup is
// linear but the enums are tiny (<= ~12 entries) and the helpers
// are constexpr — the cost is one branch per entry, eliminated by
// the compiler on constant evaluation.
template <typename E, std::size_t N>
struct EnumNameTable {
    std::array<std::pair<E, std::string_view>, N> rows;

    [[nodiscard]] constexpr std::string_view name(E e) const noexcept {
        for (auto const& r : rows) {
            if (r.first == e) return r.second;
        }
        // Fall-back returns the FIRST row's string — semantically a
        // sentinel "unknown/invalid". Each enum's `name()` historically
        // had its own fall-back string; using row 0 means each enum
        // controls its fall-back by ordering the table.
        return rows[0].second;
    }

    [[nodiscard]] constexpr std::optional<E> fromName(std::string_view s) const noexcept {
        for (auto const& r : rows) {
            if (r.second == s) return r.first;
        }
        return std::nullopt;
    }
};

// ABI model — selects the lowering shape downstream consumers (ML6
// regalloc, ML7 calling-convention lowering, AS1 assembler) expect.
// `register-machine` is the x86/ARM/RISC-V shape: physical register
// file, stack frame, calling conventions with arg-passing registers.
// `operand-stack` is the WASM/JVM-bytecode shape: no physical regs,
// values flow through a stack. `result-id` is the SPIR-V shape: typed
// SSA result IDs, no physical regs, no stack.
//
// Cycle 2b registers + callingConventions sections are MEANINGFUL only
// when `abiModel == register-machine`; for the other models they may
// be empty without `validate()` flagging it.
enum class TargetAbiModel : std::uint8_t {
    RegisterMachine = 0,  // default — x86_64, ARM64, RISC-V
    OperandStack    = 1,  // WASM, JVM bytecode
    ResultId        = 2,  // SPIR-V
};

inline constexpr EnumNameTable<TargetAbiModel, 3> kTargetAbiModelTable{{{
    { TargetAbiModel::RegisterMachine, "register-machine" },
    { TargetAbiModel::OperandStack,    "operand-stack"    },
    { TargetAbiModel::ResultId,        "result-id"        },
}}};

[[nodiscard]] constexpr std::string_view targetAbiModelName(TargetAbiModel m) noexcept {
    return kTargetAbiModelTable.name(m);
}
[[nodiscard]] constexpr std::optional<TargetAbiModel>
targetAbiModelFromName(std::string_view s) noexcept {
    return kTargetAbiModelTable.fromName(s);
}

// Calling-convention name table. The `CallConv` enum itself lives in
// `core/types/type_lattice/core_type.hpp` (TypeRecord's scalar pool
// stores it as the underlying integer for FnSig). The name↔enum
// mapping lives here alongside the 5 other `EnumNameTable` instances
// so the cross-tier text emit/parse (HIR `.dsshir`, MIR `.dssir`)
// reads a single source of truth. Adding a row here is the only edit
// needed when a `CallConv` lands; the round-trip parsers + emitters
// pick it up automatically. Audit-promoted from per-TU hand-rolled
// if-chains in `hir_text.cpp` + `mir_text.cpp` (2026-06-02 cycle).
inline constexpr EnumNameTable<CallConv, 9> kCallConvTable{{{
    { CallConv::CcSysV,       "sysv"       },
    { CallConv::CcMS64,       "ms64"       },
    { CallConv::CcAAPCS64,    "aapcs64"    },
    { CallConv::CcApple,      "apple"      },
    { CallConv::CcFastcall,   "fastcall"   },
    { CallConv::CcThiscall,   "thiscall"   },
    { CallConv::CcVectorcall, "vectorcall" },
    { CallConv::CcWasm,       "wasm"       },
    { CallConv::CcSpirv,      "spirv"      },
}}};

[[nodiscard]] constexpr std::string_view callConvName(CallConv cc) noexcept {
    return kCallConvTable.name(cc);
}
[[nodiscard]] constexpr std::optional<CallConv>
callConvFromName(std::string_view s) noexcept {
    return kCallConvTable.fromName(s);
}

// Compile-time silent-failure closure (silent-failure 2nd-order
// audit, 2026-06-02): `EnumNameTable::name(e)` returns `rows[0].second`
// on miss — `"sysv"` here. A future `CallConv` value added without
// a matching `kCallConvTable` row would silently mint `"sysv"`-
// labeled functions into `.dsshir` / `.dssir` text, corrupting
// round-trip with no diagnostic. Pin: table size MUST cover every
// enum value, AND each row MUST sit at the index matching its
// enum's underlying value (also makes the lookup O(1) on a dense
// enum). Pattern mirrors `c_mangle.cpp:42`. Anchored
// D-ENUM-NAME-TABLE-STATIC-ASSERTS for retrofit to the 5 sibling
// tables (TargetAbiModel / TargetCondCode / TargetResultRule /
// TargetRegClass / TargetEncodingShape) — same silent-fallback
// shape applies to each.
static_assert(kCallConvTable.rows.size()
              == static_cast<std::size_t>(CallConv::CcSpirv) + 1u,
    "kCallConvTable must cover every CallConv — add the new row "
    "when extending the enum or HIR/MIR text will silently emit "
    "row-0 ('sysv') for the missing value.");
static_assert([]{
    for (std::size_t i = 0; i < kCallConvTable.rows.size(); ++i) {
        if (static_cast<std::size_t>(kCallConvTable.rows[i].first) != i) {
            return false;
        }
    }
    return true;
}(), "kCallConvTable rows must be ordered by CallConv underlying "
     "value (enables O(1) name lookup AND surfaces a row-vs-enum "
     "misorder at constexpr time).");

// Universal integer-comparison condition codes (target-blind). Used by
// LIR `jcc` (conditional branch) and `setcc` (materialize 0/1 from
// FLAGS) opcodes via the LIR instruction's `payload` field. Every
// register-machine target either has these natively (x86_64 jcc, ARM64
// b.cond) or trivially synthesizes them by operand swap (RISC-V's
// branch instructions). Float ordered/unordered variants (Oeq/Une/...)
// will join this enum when MIR FCmp lowering lands in cycle 3c+.
enum class TargetCondCode : std::uint8_t {
    Eq  = 0,  // ==
    Ne  = 1,  // !=
    Slt = 2,  // signed <
    Sle = 3,  // signed <=
    Sgt = 4,  // signed >
    Sge = 5,  // signed >=
    Ult = 6,  // unsigned <
    Ule = 7,  // unsigned <=
    Ugt = 8,  // unsigned >
    Uge = 9,  // unsigned >=
    // FC3.5 sweep-c2 (FCmp LIR lowering — D-COND-FLOAT-NAN-TRUTHINESS-
    // FCMP adjudication): FLOAT condition codes over the flags an FP
    // compare instruction sets (x86 UCOMISD/UCOMISS → ZF/PF/CF; arm64
    // FCMP → NZCV). These are SEPARATE entries from the integer codes
    // because the (predicate → ISA condition) mapping diverges per
    // target: arm64 float `>` is GT (the SIGNED nibble — FCMP's NZCV
    // makes N=V mean ordered-ge), while x86 float `>` is `a` (the
    // UNSIGNED nibble — UCOMI sets CF like an unsigned compare).
    // Reusing the integer entries would silently encode HI on arm64
    // (TRUE on unordered — a NaN miscompile). Declared per-target in
    // `condCodeEncoding`; the float arms are OPTIONAL — an undeclared
    // float code means the target realizes that predicate by the
    // universal two-setcc COMPOSITION (see mir_to_lir's
    // `floatCmpPlan`), and the encoder fails loud if a single-cc
    // setcc/jcc reaches it anyway.
    Fogt = 10,  // float ordered >   (false on unordered)
    Foge = 11,  // float ordered >=  (false on unordered)
    Foeq = 12,  // float ordered ==  (false on unordered)
    Fone = 13,  // float ordered !=  (false on unordered)
    Fune = 14,  // float unordered-or-unequal != (TRUE on unordered — C 6.5.9)
    Fuo  = 15,  // unordered (NaN operand): x86 PF=1 / arm64 VS
    Ford = 16,  // ordered (no NaN):        x86 PF=0 / arm64 VC
};

inline constexpr std::size_t kTargetCondCodeCount = 17;

inline constexpr EnumNameTable<TargetCondCode, kTargetCondCodeCount>
kTargetCondCodeTable{{{
    { TargetCondCode::Eq,   "eq"   },
    { TargetCondCode::Ne,   "ne"   },
    { TargetCondCode::Slt,  "slt"  },
    { TargetCondCode::Sle,  "sle"  },
    { TargetCondCode::Sgt,  "sgt"  },
    { TargetCondCode::Sge,  "sge"  },
    { TargetCondCode::Ult,  "ult"  },
    { TargetCondCode::Ule,  "ule"  },
    { TargetCondCode::Ugt,  "ugt"  },
    { TargetCondCode::Uge,  "uge"  },
    { TargetCondCode::Fogt, "fogt" },
    { TargetCondCode::Foge, "foge" },
    { TargetCondCode::Foeq, "foeq" },
    { TargetCondCode::Fone, "fone" },
    { TargetCondCode::Fune, "fune" },
    { TargetCondCode::Fuo,  "fuo"  },
    { TargetCondCode::Ford, "ford" },
}}};

[[nodiscard]] constexpr std::string_view targetCondCodeName(TargetCondCode c) noexcept {
    return kTargetCondCodeTable.name(c);
}

// (`regClassForCoreType` defined below `TargetRegClass`.)

// Result-type discipline mirrors MIR's `MirResultRule`.
enum class TargetResultRule : std::uint8_t {
    None,      // never defines a value
    Value,     // always defines a value
    Optional,  // may define a value (e.g. a call to a non-void fn)
};

inline constexpr EnumNameTable<TargetResultRule, 3> kTargetResultRuleTable{{{
    { TargetResultRule::None,     "none"     },
    { TargetResultRule::Value,    "value"    },
    { TargetResultRule::Optional, "optional" },
}}};

[[nodiscard]] constexpr std::string_view targetResultRuleName(TargetResultRule r) noexcept {
    return kTargetResultRuleTable.name(r);
}

// Register-class envelope (universal — every target maps its concrete
// register classes to this set; the LIR substrate sees only the envelope).
// Mirrors `LirRegClass` in `src/lir/lir_reg.hpp` — kept as a separate
// definition here so `core/types/target_schema.hpp` does not need to
// pull in the LIR substrate (header-include direction is core ← LIR).
//
// The numeric values MUST stay in lockstep with `LirRegClass` (a
// static_assert in `lir_reg.hpp` pins the alignment); both enums
// declare the same set so callers can `static_cast` between them
// when bridging from substrate-tier (this header) to LIR-tier types.
enum class TargetRegClass : std::uint8_t {
    None  = 0,
    GPR   = 1,
    FPR   = 2,
    VR    = 3,
    Flags = 4,
};

inline constexpr EnumNameTable<TargetRegClass, 5> kTargetRegClassTable{{{
    { TargetRegClass::None,  "none"  },
    { TargetRegClass::GPR,   "gpr"   },
    { TargetRegClass::FPR,   "fpr"   },
    { TargetRegClass::VR,    "vr"    },
    { TargetRegClass::Flags, "flags" },
}}};

[[nodiscard]] constexpr std::string_view targetRegClassName(TargetRegClass c) noexcept {
    return kTargetRegClassTable.name(c);
}
[[nodiscard]] constexpr std::optional<TargetRegClass>
targetRegClassFromName(std::string_view s) noexcept {
    return kTargetRegClassTable.fromName(s);
}

// Map a substrate-tier `TypeKind` to its `TargetRegClass`. Universal
// across all register-machine targets — floats use the FPR envelope,
// vectors use VR, integers/pointers/bool use GPR, and aggregates
// (Struct/Union/Array/Enum/Tuple/Slice) default to GPR with the
// caller responsible for further flattening (ML5 cycle 3e via memory
// ops + multiple Loads/Stores). Promoted from the LIR lowerer
// (cycle 3d) to substrate (cycle 3e) per the architect agent's
// recommendation — ML6 regalloc, ML7 callconv lowering, and the
// LirVerifier all consume the same mapping.
[[nodiscard]] constexpr TargetRegClass regClassForCoreType(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::F16:
        case TypeKind::F32:
        case TypeKind::F64:
        case TypeKind::F128:
            return TargetRegClass::FPR;
        case TypeKind::Vector:
            return TargetRegClass::VR;
        default:
            return TargetRegClass::GPR;
    }
}

// Per-physical-register descriptor. Position in the schema's `registers`
// vector is the register's numeric ordinal (consumed by `LirReg::id` once
// regalloc lands in ML6). A register's mnemonic name (`"rdi"` / `"xmm0"`)
// is the JSON-side identifier — calling-convention sections reference
// registers by name.
struct DSS_EXPORT TargetRegisterInfo {
    std::string    name;          // canonical mnemonic ("rax" / "xmm0" / ...)
    TargetRegClass regClass = TargetRegClass::None;
    // `subOf` lets a target declare aliasing relationships (e.g. "eax"
    // is the low 32 bits of "rax") so ML6 regalloc can track full
    // clobber sets correctly. Empty when this register is independent.
    std::string    subOf;
    // 16/8/4/1 etc. — width in bytes. Required so ML6 knows spill-slot
    // sizing without re-deriving it from the regClass.
    std::uint16_t  widthBytes = 0;
    // Hardware encoding (e.g. ModR/M ordinal on x86) — opaque to the
    // substrate; AS1 assembler reads this directly to emit machine code.
    std::uint16_t  hwEncoding = 0;
};

// FC2 Part B (per-register-class operation table): the three universal
// register-data-movement ROLES every lowering pass emits on a value of
// some register class. Distinct from a mnemonic: a single x86 mnemonic
// vocabulary covers GPRs ("mov"/"load"/"store") but the FPR class needs
// DIFFERENT instructions (movaps / movsd) — a GPR mov against an XMM
// hwEncoding assembles to valid-looking-but-wrong bytes (the silent
// class-blind miscompile this table kills).
enum class RegClassOp : std::uint8_t {
    Move  = 0,  // register→register copy within the class
    Load  = 1,  // register ← [memory]
    Store = 2,  // [memory] ← register
};
inline constexpr std::size_t kRegClassOpCount = 3;

[[nodiscard]] constexpr std::string_view regClassOpName(RegClassOp op) noexcept {
    switch (op) {
        case RegClassOp::Move:  return "move";
        case RegClassOp::Load:  return "load";
        case RegClassOp::Store: return "store";
    }
    return "?";
}

// One register class's declared operation mnemonics (the JSON
// `registerClassOps[]` row). An EMPTY string means "not declared" —
// an op consulted on a declared row with an empty slot fails loud at
// the consumer (e.g. x86_64's fpr declares move+load but NO store
// until a real FPR-store consumer exists — trigger discipline; a
// silent fallback to the GPR "store" would 8-byte-GPR-write an XMM
// ordinal).
struct DSS_EXPORT TargetRegisterClassOps {
    bool        declared = false;  // a JSON row exists for this class
    std::string move;
    std::string load;
    std::string store;

    [[nodiscard]] std::string_view nameFor(RegClassOp op) const noexcept {
        switch (op) {
            case RegClassOp::Move:  return move;
            case RegClassOp::Load:  return load;
            case RegClassOp::Store: return store;
        }
        return {};
    }
};

// One calling convention. A target may declare multiple (SysV AMD64,
// Microsoft x64, fastcall, ...); the front-end picks one via attribute /
// driver flag. The `argGprs` / `argFprs` ordering is significant — the
// caller must place int args in those registers in that order, spilling
// to the stack when the register set is exhausted.
struct DSS_EXPORT TargetCallingConvention {
    std::string name;                     // "sysv_amd64" / "ms_x64" / ...
    std::vector<std::string> argGprs;     // arg-passing integer registers, in order
    std::vector<std::string> argFprs;     // arg-passing floating-point registers, in order
    std::vector<std::string> returnGprs;  // integer-return registers (rax/rdx on SysV; rax on MS)
    std::vector<std::string> returnFprs;  // float-return registers
    std::vector<std::string> callerSaved; // volatile across calls (caller must spill if reused)
    std::vector<std::string> calleeSaved; // non-volatile (callee must restore on return)
    std::uint16_t stackAlignment   = 0;   // alignment of RSP at call site (16 on SysV/MS x64)
    std::uint16_t shadowSpaceBytes = 0;   // MS x64: 32 bytes of home space; SysV: 0
    std::uint16_t redZoneBytes     = 0;   // SysV leaf-fn red zone (128); MS x64: 0

    // RSP-bias mod `stackAlignment` at the START of a function that
    // serves as the PROCESS ENTRY POINT (D-LK10-ENTRY-TRAMP-PROLOGUE).
    // This is the single new piece of vocabulary that closes the
    // trampoline ABI-prologue without storing a derived constant:
    // the bias, together with `stackAlignment` and `shadowSpaceBytes`
    // already on this struct, determines the smallest `sub sp, N`
    // the trampoline must emit. Algorithm lives in `lir_callconv.hpp`'s
    // `alignedSizeWithBias()` so ML7 and the trampoline call ONE
    // formula.
    //
    // Concrete values (encode the OS-loader convention for the entry
    // cc):
    //   * `ms_x64`     (Windows PE):   8  — `RtlUserThreadStart` does
    //                                       a CALL into the entry
    //                                       point, so the first
    //                                       instruction sees RSP ≡ 8
    //                                       mod 16.
    //   * `sysv_amd64` (Linux ELF /    0  — kernel maps the image and
    //                  macOS Mach-O):       JUMPS to `_start`/`main`
    //                                       with RSP 16-byte-aligned
    //                                       and NO return address
    //                                       pushed.
    //   * `aapcs64`    (Linux/Win/Mac  0  — ARM64 BL doesn't push,
    //                  ARM64):              and the kernel sets SP
    //                                       aligned at process entry.
    //
    // This field is consumed ONLY by the trampoline emitter (the
    // entry-cc-of-the-program scenario). Normal-function frames
    // computed by ML7 use the function-entry bias (= the cc's
    // post-CALL RSP offset, typically equal to `callInstructionPush
    // Bytes mod stackAlignment` = 8 for x86_64 / 0 for ARM64) — that
    // bias is NOT this field. Wiring ML7 onto this field is anchored
    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY for when normal-function call-
    // site shadow-space lands (separately tracked; not the
    // trampoline's concern).
    //
    // Validators (target_schema.cpp::validate): MUST be 0 if the cc
    // has all other stack fields at 0; otherwise MUST be <
    // `stackAlignment`.
    std::uint16_t entryStackPointerBias = 0;

    // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY: byte count the architecture's
    // `call` instruction PUSHES onto the stack (the return-address
    // word). RSP delta at function entry FROM A CALLER, BEFORE the
    // callee's prologue runs. Distinct from `entryStackPointerBias`
    // above:
    //
    //   * `entryStackPointerBias`: RSP delta at PROCESS-ENTRY (the
    //     kernel/loader's transition). OS-dependent (Win64 = 8 because
    //     RtlUserThreadStart issues a CALL; SysV ELF/Mach-O = 0 because
    //     the kernel JUMPs to `_start`).
    //   * `callPushBytes`: RSP delta at NORMAL-CALL-ENTRY (the
    //     in-program CALL instruction's push). ISA-dependent only
    //     (x86_64 = 8 — `call` pushes a 64-bit return address;
    //     ARM64 = 0 — `bl` writes LR, no stack push).
    //
    // The two fields COINCIDE on Win64 (both = 8) because Windows uses
    // a CALL-style entry transition; they DIVERGE on Linux x86_64
    // (entry = 0 via JMP, normal call = 8 via CALL push). Putting the
    // facts in two distinct fields named for their distinct triggers
    // prevents a future maintainer from "deduping" them on Win64 and
    // silently breaking Linux x86_64.
    //
    // Consumed by ML7 `computeFrameLayout` for non-leaf functions: the
    // function's prologue must (a) reserve `shadowSpaceBytes` for any
    // call it makes AND (b) end at an RSP value that satisfies the
    // callee's alignment expectation, which means the prologue's
    // `sub sp, N` must satisfy `N ≡ callPushBytes (mod stackAlignment)`
    // so that after our entry's `callPushBytes` and our `sub`, RSP is
    // `(0 - callPushBytes - N) mod alignment = 0` at the next call
    // site. The formula is the same `alignedSizeWithBias` used by the
    // trampoline emitter — one helper, two distinct bias inputs.
    //
    // Validators: MUST be strictly < `stackAlignment` (the bias is an
    // OFFSET into the alignment quantum — parallel to
    // entryStackPointerBias's contract); MUST be 0 when no ABI info
    // is declared (consistent with entryStackPointerBias's
    // "zero-when-cc-is-empty" rule). In practice the call instruction
    // pushes a multiple of pointer-width bytes, but the validator
    // expresses the alignment-quantum invariant rather than the
    // implementation detail.
    std::uint16_t callPushBytes = 0;

    // D-ML7-2.6 (closed co-with-D-ML7-2.2, 2026-06-02): when true,
    // the cc uses SLOT-ALIGNED arg passing — each arg consumes ONE
    // shared slot index regardless of its register class, AND both
    // argGprs[N] AND argFprs[N] are reserved by slot N (matters for
    // mixed int/float arg sequences). When false, the cc uses
    // INDEPENDENT counters — gprIdx and fprIdx advance separately.
    //
    // Concrete shipped values:
    //   * `ms_x64`     (Windows PE):           true  — `f(int, double,
    //                                                   int, double,
    //                                                   int)` consumes
    //                                                   slots 0..4; slot 4
    //                                                   overflows to stack.
    //   * `sysv_amd64` (Linux ELF / Mach-O):   false — independent
    //                                                   counters; ints fill
    //                                                   rdi..r9, floats fill
    //                                                   xmm0..xmm7 separately.
    //   * `aapcs64`    (ARM64):                false — independent counters.
    //
    // Consumed by ML7 `materializeOneFunc`'s `arg` + `call` arms +
    // `computeMaxOutgoingStackArgs` pre-scan. Under slot-aligned the
    // outgoing-arg-area overflow count is `max(0, total_args -
    // max(argGprs.size(), argFprs.size()))`; under independent it's
    // `max(0, gprArgs - argGprs.size()) + max(0, fprArgs - argFprs.size())`.
    // Pure-GPR calls (WriteFile, GetStdHandle, etc.) coincide between
    // the two shapes; only mixed-class 5+ arg calls diverge.
    bool slotAligned = false;

    // Named register reference. Used for distinguished-role registers
    // (link register, stack pointer, frame pointer in future cycles).
    // The struct shape co-locates the JSON-side `name` (kept for
    // diagnostics) with the loader-resolved `ordinal` (cached at JSON
    // load time so consumers don't re-resolve per use). Both fields
    // are populated atomically by the loader: the type cannot represent
    // a "name set, ordinal unset" state. `validate()` guarantees the
    // resolution succeeded when the optional is engaged.
    struct NamedRegisterRef {
        std::string   name;
        std::uint16_t ordinal = 0;
    };

    // ARM64 AAPCS64 carries the return address in a dedicated link
    // register (LR / x30) rather than on the stack; ML7 callconv
    // lowering checks `linkRegister.has_value()` to decide whether
    // to spill LR in the prologue. Empty for x86_64.
    std::optional<NamedRegisterRef> linkRegister;

    // Stack-pointer register. Required for any register-machine ABI —
    // ML7 callconv lowering uses this register's ordinal as the base
    // for prologue/epilogue stack adjustments and frame_load/store
    // memory addressing. Empty optional means a stack-pointer-less
    // target (operand-stack VMs).
    std::optional<NamedRegisterRef> stackPointer;

    // D-LANG-VARIADIC (step 13.4, 2026-06-02): the register the caller
    // MUST load with the count of vector (FPR) arguments passed in
    // vector registers BEFORE the call instruction of any C-style
    // variadic function. SysV AMD64 (§3.2.3): `al = number of XMM
    // arguments used by varargs (0..8)`. Win64 ms_x64 has no
    // equivalent (the loader-side ABI uses GPR-shadow + double-spill
    // — anchored D-ML7-VARIADIC-WIN64-DOUBLE-SPILL — and this field
    // is left empty). AAPCS64 (ARM64): no equivalent (variadic floats
    // pass on the stack, no count register). Empty optional ⇒ this
    // CC requires no caller-side vector-count register for variadic
    // calls. When engaged, ML7 materialize for a Call with
    // payload `isVariadic=true` counts FPR args in
    // [fixedArgCount..N) and emits a `mov <reg>, <count>` before
    // the call instruction.
    std::optional<NamedRegisterRef> variadicVectorCountReg;
};

// Discriminates the byte-encoding shape an opcode commits to (plan 13
// AS1). `None` is the default; opcodes without an `encoding` block in
// the target JSON stay at `None` and the assembler emits
// `A_NoEncodingDeclared` for them. Adding a new ISA family that fits
// an existing shape (e.g. RV32 fixed-word + bit-field) = drop a new
// `*.target.json` declaring `format: "fixed32"`, no substrate change.
// Adding a genuinely novel encoding shape = one new enum entry here
// and one new format walker in the assembler.
//
// Cross-plan: this enum is the **shape-keyed dispatch vocabulary** for
// plan 13 §2.4. The assembler's format-walker registry keys on this
// enum (NOT on target name / arch identity).
enum class TargetEncodingShape : std::uint8_t {
    None        = 0,    // no encoding declared (substrate refuses to guess)
    X86Variable = 1,    // x86 variable-length: REX/VEX/EVEX prefix + opcode + ModR/M + SIB + imm
    Fixed32     = 2,    // 32-bit fixed word + bit-field slots (ARM64, RV32, MIPS-fixed)
};

inline constexpr EnumNameTable<TargetEncodingShape, 3> kTargetEncodingShapeTable{{{
    { TargetEncodingShape::None,        "none"         },
    { TargetEncodingShape::X86Variable, "x86-variable" },
    { TargetEncodingShape::Fixed32,     "fixed32"      },
}}};

[[nodiscard]] constexpr std::string_view
targetEncodingShapeName(TargetEncodingShape s) noexcept {
    return kTargetEncodingShapeTable.name(s);
}
[[nodiscard]] constexpr std::optional<TargetEncodingShape>
targetEncodingShapeFromName(std::string_view s) noexcept {
    return kTargetEncodingShapeTable.fromName(s);
}

// Operand-kind filter (plan 13 AS2 — variant-guard vocabulary). A
// `encoding.variants[k].guard.operandKinds[i]` entry declares what
// kind of LIR operand the i-th source operand of the instruction
// must be for this variant to match. Closed vocabulary, shape-keyed:
// the walker dispatches on this enum, NEVER on per-target identity.
//
// Enum names mirror `LirOperandKind` (the substrate boundary) — a
// filter is "the LIR operand pool slot's kind discriminator." JSON
// names preserve historical width labels (e.g. `"imm32"` for the
// `ImmInt` filter — current scope holds 32-bit immediates; a future
// Imm8/Imm16/Imm64 widening WILL gain its own filter when its
// consumer lands).
enum class OperandKindFilter : std::uint8_t {
    Reg       = 0,  // `LirOperand{kind == Reg}`
    ImmInt    = 1,  // `LirOperand{kind == ImmInt}` — current cycle's
                    // immInt32 arm; future Imm8/Imm16/Imm64 join as
                    // distinct filters when their walkers land.
    SymbolRef = 2,  // `LirOperand{kind == SymbolRef}` — used by call /
                    // branch instructions in cycle-4. The walker
                    // emits a Relocation entry when this operand
                    // reaches a symbol-bearing slot (Disp32 / Imm26).
                    // Global-address load/store forms (x86 RIP-relative
                    // mov, ARM64 ADRP+ADD pair) join with their
                    // consumer cycle (plan 13 §3.1 D-AS4-1 / D-AS4-2).
    MemBase   = 3,  // `LirOperand{kind == MemBase}` — carries the scale
                    // factor for base+index*scale addressing. Cycle 2's
                    // load/store/lea walkers consume this for shape
                    // validation only (scale==1 in v1; D-AS4-1 anchors
                    // index+scale support).
    MemOffset = 4,  // `LirOperand{kind == MemOffset}` — carries the
                    // signed 32-bit displacement for [base+disp]
                    // addressing. Wired to `Disp32Mem` to emit 4 LE
                    // bytes after the ModR/M (and SIB when present).
    BlockRef  = 5,  // `LirOperand{kind == BlockRef}` — D-CSUBSET-
                    // WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1): refers
                    // to an INTRA-FUNCTION basic block. Wired to the
                    // `BlockRel32` slot on x86 (4-byte trailing PC-
                    // relative displacement, resolved at assemble time
                    // via `walker_util::BlockRelPatch`). ARM64 will
                    // use Imm19/Imm26 with different patch arithmetic
                    // (anchored D-AS3-BLOCK-REL-IMM19/26).
};

inline constexpr EnumNameTable<OperandKindFilter, 6> kOperandKindFilterTable{{{
    { OperandKindFilter::Reg,       "reg"      },
    { OperandKindFilter::ImmInt,    "imm32"    },  // JSON-side width label
    { OperandKindFilter::SymbolRef, "symbol"   },
    { OperandKindFilter::MemBase,   "membase"  },
    { OperandKindFilter::MemOffset, "memoffset"},
    { OperandKindFilter::BlockRef,  "blockref" },
}}};

[[nodiscard]] constexpr std::string_view
operandKindFilterName(OperandKindFilter f) noexcept {
    return kOperandKindFilterTable.name(f);
}
[[nodiscard]] constexpr std::optional<OperandKindFilter>
operandKindFilterFromName(std::string_view s) noexcept {
    return kOperandKindFilterTable.fromName(s);
}

// Encoding slot — names WHERE a register/immediate value goes inside
// the emitted byte sequence. Closed vocabulary. The x86-variable
// walker reads this enum to project an operand (or the instruction's
// `result` register) into the right slot of the encoded bytes:
//   * `ModRmReg` → low 3 bits of the operand's `hwEncoding` fill
//     the ModR/M byte's `reg` field (bits 3..5); the high bit
//     drives REX.R.
//   * `ModRmRm` → low 3 bits fill the ModR/M byte's `rm` field
//     (bits 0..2); the high bit drives REX.B. For register-direct
//     operands (mod=3) — current cycle's only shape; memory
//     addressing modes land alongside their consumers.
//   * `Imm32` → 4 immediate bytes appended after the ModR/M (and
//     SIB, when present), little-endian.
// Future: `Imm8` / `Imm64` / `Disp8` / `Disp32` / `OpcodePlusReg` /
// `SibBase` / `SibIndex` ... — each gains a row when first walker
// consumer lands.
enum class EncodingSlotKind : std::uint8_t {
    // ── x86-variable shape ────────────────────────────────────────
    ModRmReg = 0,  // bits 3..5 of ModR/M byte; REX.R = hwEncoding bit 3
    ModRmRm  = 1,  // bits 0..2 of ModR/M byte; REX.B = hwEncoding bit 3
    Imm32    = 2,  // 4 immediate bytes appended after ModR/M, LE
    // ── fixed32 shape (plan 13 AS3) ────────────────────────────────
    // Names mirror AArch64 / RV32 register-field nomenclature: each
    // entry pins a 5-bit-wide window inside the 32-bit fixed word
    // where the operand's `hwEncoding` is OR'd. AArch64 GPR ordinals
    // fit in 5 bits (X0..X30 + XZR=31); FPR likewise. The fixed-word
    // template carries the base bit pattern; the walker just OR's
    // the slot-positioned operand bits.
    Rd       = 3,  // destination register, bits 0..4
    Rn       = 4,  // first source register, bits 5..9
    Rm       = 5,  // second source register, bits 16..20
    // Plan 13 AS4: symbol-bearing slots — values written by the
    // walker are RELOCATABLE. A wire targeting Disp32 / Imm26
    // declares its `relocationKind` (the schema row's name from
    // `relocations[]`); the walker emits a Relocation entry into
    // the AssembledFunction at the slot's byte offset AND writes
    // ZEROS at that position. The linker (plan 14) reads the
    // Relocation and patches in the final displacement at link
    // time. (Cycle-4 hardcodes the on-bytes value to 0 + addend
    // 0; a future wire-declared addend bias is anchored at plan
    // 13 §3.1 D-AS4-4.)
    Disp32   = 6,  // x86 PC-relative 32-bit displacement (e.g. `call rel32`)
    Imm26    = 7,  // ARM64 26-bit branch offset / 4 (e.g. `bl imm26`)
    // ── x86 memory-addressing slots (plan 13 §3.1 D-AS4-1) ──────────
    //
    // Closes the load/store/lea byte-encoding gap for `[base + disp32]`
    // addressing. The trio MemBaseScale + ModRmRmMem + Disp32Mem
    // models the LIR shape `<base_reg> <MemBase(scale)> <MemOffset(disp)>`:
    //
    //   ModRmRmMem    — operand is a base Reg; writes ModR/M.rm with
    //                   mod = 10 (memory + 32-bit disp) and forces a
    //                   SIB byte when base.lo3 == 4 (the x86-64 rule
    //                   for rsp/r12). REX.B from base hwEncoding bit 3
    //                   as usual.
    //   MemBaseScale  — operand is a MemBase; defense-in-depth shape
    //                   check (cycle scope: scale == 1 only). Future
    //                   `[base + index*scale]` would re-use this slot
    //                   with a paired SibIndex.
    //   Disp32Mem     — operand is a MemOffset; emits 4 LE bytes of
    //                   the offset field after ModR/M (and SIB when
    //                   present). Distinct from `Disp32` which is
    //                   symbol-relative; `Disp32Mem` is an immediate
    //                   memory displacement.
    ModRmRmMem    = 8,
    MemBaseScale  = 9,
    Disp32Mem     = 10,
    // D-AS4-5 closure (2026-06-01): SIB.index field (bits 3..5 of
    // the SIB byte). Wires an index register's hwEncoding low 3
    // bits into SIB.index and the high bit into REX.X (the AS2
    // pre-declared `rexX` field is finally consumed here). Paired
    // with `MemBaseScale` (which now also supplies the 2-bit scale
    // exponent for SIB.scale bits 6..7).
    //
    // Schema variant guard adds `Reg` between the base `Reg` and
    // `MemBase` for the with-index shape:
    //   3-op no-index: [base, MemBase(scale=1), MemOffset(disp)]
    //   4-op indexed:  [base, index, MemBase(scale∈{1,2,4,8}), MemOffset(disp)]
    // The walker dispatches on the presence of `SibIndex` wiring to
    // emit the SIB byte unconditionally (separate from the rsp/r12
    // force-presence rule that fires today on no-index addressing).
    SibIndex      = 11,
    // RIP-relative 32-bit displacement (D-LK4-RODATA-PRODUCER
    // 2026-06-02). Symbol-bearing slot like `Disp32`, but the
    // encoder additionally forces ModR/M to the RIP-relative
    // form: mod=00 reg=destination rm=101 (no SIB byte, no base
    // register operand). Used by the new `lea r64, [rip + sym]`
    // variant that materializes a module-level global's address
    // into a register. Pairs with `relocationKind: "rel32"`.
    //
    // The encoder emits the 4-byte placeholder + Relocation at
    // the trailing byte position (same byte-emit pattern as
    // `Disp32`); the only difference is the forced ModR/M state.
    RipRelDisp32  = 12,
    // D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1, 2026-06-03):
    // OR the target's condition-code numeric encoding (looked up from
    // the inst's payload, which carries a `TargetCondCode` value) into
    // the LAST opcode byte's low 4 bits. Used by x86 setcc (`0F 90+cc`)
    // and jcc (`0F 80+cc`). Wire has `index: 0` by convention (the
    // payload is implicit; the wire just declares "the cond goes in
    // the opcode byte"). The encoder fail-loud (A_NoCondCodeEncoding)
    // if the target hasn't loaded `condCodeEncoding[]` — a missing
    // table would silently OR zero (= TargetCondCode::Eq's nibble) and
    // every conditional branch would resolve as `je`.
    CondCodeNibble = 13,
    // D-CSUBSET-WHILE-LOOP-SUBSTRATE: 32-bit PC-relative displacement
    // to an INTRA-FUNCTION basic block (resolved at assemble time, no
    // linker relocation emitted). The walker emits 4 LE bytes of zero
    // + records a (patch-byte-offset, target-LirBlockId) entry in the
    // per-function patch list. After all blocks/insts of the function
    // are encoded, `asm.cpp` resolves each patch by computing
    // `block_offsets[target] - (patch_offset + 4)` and writing back
    // the 4 LE bytes. Wire reads the BlockRef from operands[index]
    // (jmp's operand[0]) — for opcodes whose block target lives in
    // the block's successor pool rather than operands, the lowering
    // pass MUST duplicate the BlockRef as an operand (jcc lowering
    // passes both successors as BlockRef operands AS WELL AS via the
    // `recordSuccessors_` API). Symmetric with the `addBr` precedent
    // which already encodes the target both as operand[0] and via
    // successors[0].
    BlockRel32     = 14,
    // D-LK10-ENTRY-ARM64 (v0.0.2 V2-1): 16-bit UNSIGNED immediate for
    // the AArch64 MOVZ wide-immediate form (`MOVZ Xd, #imm16`). Bits
    // 5..20 of the fixed word (Rd occupies bits 0..4). This is the
    // FIRST fixed32 immediate slot: the entry trampoline loads the
    // exit-syscall number into x8 via `mov x8, #94` → MOVZ. Unlike
    // the symbol-bearing Imm26, the walker writes the operand's
    // immediate value DIRECTLY into the bit window (range-checked —
    // a value wider than the slot fails loud, never silently
    // truncates), no relocation. Reusable by RV32 `addi`/`lui`-style
    // immediate forms when a RISC-V target lands (its own slot when
    // the bit-window differs).
    Imm16          = 15,
    // D-LK10-ENTRY-ARM64 (v0.0.2 V2-1): SIGNED 9-bit offset for the
    // AArch64 unscaled load/store form (`LDUR/STUR Xt, [Xn, #simm9]`),
    // bits 12..20. The frame load/store materialized by the callconv
    // (spill reload / store, callee-saved save/restore) encodes its
    // byte offset here — a RAW byte displacement (unscaled), range
    // -256..255; a wider frame offset fails loud (a scaled LDR/STR
    // imm12 form is the future generalization). Two's-complement: the
    // walker writes the low 9 bits; negative offsets carry bit 8 set.
    Imm9           = 16,
    // D-LK10-ENTRY-ARM64 (v0.0.2 V2-1): the memory-base operand
    // position in a fixed32 memory instruction whose ISA encoding has
    // NO scale field (AArch64 unscaled LDUR/STUR). The shared LIR
    // load/store form carries a MemBase(scale) operand (an x86-SIB-ism);
    // on AArch64 it is a structural marker that must be wired (the
    // "every guard position is wired" validate rule) yet contributes
    // ZERO bits — a width-0 slot. The walker validates scale==1 here
    // and writes nothing. (A scaled ISA would use a real bit-field slot
    // instead, like x86's MemBaseScale.)
    MemBaseNoScale = 17,
    // D-LK10-ENTRY-ARM64 (v0.0.2 V2-1): UNSIGNED 12-bit immediate for
    // the AArch64 ADD/SUB-immediate form (`ADD/SUB Xd, Xn, #imm12`),
    // bits 10..21. The callconv's prologue/epilogue stack adjust
    // (`sub sp, sp, #frame` / `add sp, sp, #frame`) encodes the frame
    // size here. Range 0..4095; a larger frame needs the shifted
    // imm12<<12 form (future). Unsigned (frame sizes are non-negative).
    Imm12          = 18,
    // D-AS4-3 (multi-instruction-macro / multi-relocation encoder):
    // a SYMBOL-PATCH MARKER — a write-no-bits (width-0) slot that
    // marks an operand position as a linker-patched symbol reference.
    // The walker writes NO immediate bits (the bit-window is {0,0},
    // exactly like `MemBaseNoScale`) and emits a Relocation at the
    // START of the slot's word (the wire's `wordIndex`); the linker
    // owns the patched field ENTIRELY, computing the bits from the
    // wire's `relocationKind` formula. Generic over the patch shape:
    // the SAME marker serves AArch64 `ADRP Xd, sym@PG` (word 0,
    // `adr_prel_pg_hi21` — a split immlo[30:29]+immhi[23:5] field no
    // single bit-window could express) AND `ADD Xd, Xd, #:lo12:sym`
    // (word 1, `add_abs_lo12_nc`), and is reusable by any ISA's
    // linker-patched symbol field (RISC-V `auipc`+`addi`, etc.). The
    // distinguishing facts — which word, which patch formula — live
    // on the WIRE (`wordIndex` + `relocationKind`), NOT on the slot,
    // so one marker covers every such position. isSymbolBearingSlot
    // returns true (a `relocationKind` is required + emitted).
    SymbolPatchMarker = 19,
    // D-AS3-BLOCK-REL-IMM19/26 (ARM64 conditional control-flow): the
    // SIGNED 19-bit PC-relative branch offset of the AArch64 `B.cond`
    // instruction (`B.cond <label>`), bits 5..23 of the 32-bit word
    // (the cond nibble occupies bits 0..3). BLOCK-RELATIVE, NOT
    // symbol-bearing: like the INTRA-FUNCTION use of Imm26 (the
    // `B <label>` form), the value is the displacement to an intra-
    // function basic block, resolved at ASSEMBLE time by the asm.cpp
    // resolver (NOT a linker relocation). The walker writes ZERO bits
    // + pushes a `walker_util::BlockRelPatch{ kind = Arm64Imm19 }`;
    // the resolver computes `(target - patchOffset) >> 2` (no +4 bias
    // — ARM64 branches are PC-relative to the instruction itself) and
    // read-modify-writes the 19-bit field. The displacement is SCALED
    // by 4 (word-aligned), a ±1 MiB reach; a larger intra-function
    // span needs inverted-cond + long `B` (future, anchored
    // D-CSUBSET-LONG-BRANCH). `isSymbolBearingSlot` returns FALSE (no
    // relocationKind — resolved intra-function, not at link time).
    Imm19 = 20,
    // FC3.5 sweep-c1 (shifts end-to-end): the x86 8-bit immediate
    // slot — ONE byte appended after ModR/M (and SIB when present),
    // before any imm32 bytes. First consumer: the constant-count
    // shift forms `SHL/SHR/SAR r/m, imm8` (C1 /4 /5 /7 ib per the
    // Intel SDM). The walker range-checks the wired value to [0,255]
    // fail-loud (never silently truncates a wider immediate to one
    // byte). The variant GUARD vocabulary is unchanged — the operand
    // KIND filter stays `"imm32"` (= the LirOperandKind::ImmInt
    // discriminator; the historical width-labeled name); the SLOT
    // decides the emitted width.
    Imm8 = 21,
    // FC3.5 sweep-c3 (D-LIR-MOD-MSUB-FUSION): the fixed32 THIRD source-
    // register field at bits 10..14 — AArch64's `Ra` (the addend /
    // minuend register of the multiply-accumulate family: MADD/MSUB/
    // SMADDL/UMSUBL all carry Rm[20:16] | o0[15] | Ra[14:10] | Rn[9:5]
    // | Rd[4:0]). First consumer: the arm64 `msub` opcode (MSUB Xd,
    // Xn, Xm, Xa = Xa − Xn·Xm), the fused realization of rule 3's
    // remainder expansion rem = n − (n/d)·d. NOT symbol-bearing; a
    // plain 5-bit register window exactly like Rd/Rn/Rm.
    Ra = 22,
    // Future fixed32 slots (paired with their consumer cycle):
    //   ImmShift / Sf-flag / scaled LDR imm12 / etc.
};

inline constexpr EnumNameTable<EncodingSlotKind, 23> kEncodingSlotKindTable{{{
    { EncodingSlotKind::ModRmReg,     "modrm.reg"     },
    { EncodingSlotKind::ModRmRm,      "modrm.rm"      },
    { EncodingSlotKind::Imm32,        "imm32"         },
    { EncodingSlotKind::Rd,           "rd"            },
    { EncodingSlotKind::Rn,           "rn"            },
    { EncodingSlotKind::Rm,           "rm"            },
    { EncodingSlotKind::Disp32,       "disp32"        },
    { EncodingSlotKind::Imm26,        "imm26"         },
    { EncodingSlotKind::ModRmRmMem,   "modrm.rm.mem"  },
    { EncodingSlotKind::MemBaseScale, "membase.scale" },
    { EncodingSlotKind::Disp32Mem,    "disp32.mem"    },
    { EncodingSlotKind::SibIndex,     "sib.index"     },
    { EncodingSlotKind::RipRelDisp32, "riprel.disp32" },
    { EncodingSlotKind::CondCodeNibble, "condcode.nibble" },
    { EncodingSlotKind::BlockRel32,    "block.rel32"    },
    { EncodingSlotKind::Imm16,         "imm16"          },
    { EncodingSlotKind::Imm9,          "imm9"           },
    { EncodingSlotKind::MemBaseNoScale, "membase.noscale" },
    { EncodingSlotKind::Imm12,         "imm12"          },
    { EncodingSlotKind::SymbolPatchMarker, "sym.patch"   },
    { EncodingSlotKind::Imm19,         "imm19"          },
    { EncodingSlotKind::Imm8,          "imm8"           },
    { EncodingSlotKind::Ra,            "ra"             },
}}};

// Centralised count — promoted from per-translation-unit local
// constexpr per simplifier review. Used as the size of
// `std::array<bool, N>` slot-tracking buffers in both validate()
// and the fixed32 walker; keeps both sites in lockstep with the
// shared enum table.
inline constexpr std::size_t kEncodingSlotKindCount =
    kEncodingSlotKindTable.rows.size();

// Belt-and-suspenders: if a new EncodingSlotKind enumerator is
// added without extending the table (or vice versa), the
// `EnumNameTable<E, N>` template would let an ordinal escape
// without a row. Pin the equation here so the build breaks
// loudly at the next compile, not silently at first lookup.
// (Each enumerator gets exactly one row; ordinals are
// contiguous 0..N-1; both invariants are validated by the
// table's `name()`/`fromName()` semantics.)
static_assert(kEncodingSlotKindCount == 23,
              "EncodingSlotKind enum / kEncodingSlotKindTable drift — "
              "add a row to the table or remove the enumerator");

// Architect AS3 followup: each `EncodingSlotKind` is tied to ONE
// encoding shape — ModRm* and Imm32 are x86-variable; Rd/Rn are
// fixed32. Returns the shape the slot belongs to, so `validate()`
// can reject cross-shape variants (a fixed32 variant declaring
// `modrm.rm`, or an x86-variable variant declaring `rd`).
//
// Future slots add a new row here when they join their walker's
// vocabulary.
[[nodiscard]] constexpr TargetEncodingShape
slotShapeFor(EncodingSlotKind s) noexcept {
    switch (s) {
        case EncodingSlotKind::ModRmReg:
        case EncodingSlotKind::ModRmRm:
        case EncodingSlotKind::Imm32:
        case EncodingSlotKind::Imm8:
        case EncodingSlotKind::Disp32:
        case EncodingSlotKind::ModRmRmMem:
        case EncodingSlotKind::MemBaseScale:
        case EncodingSlotKind::Disp32Mem:
        case EncodingSlotKind::SibIndex:
        case EncodingSlotKind::RipRelDisp32:
        case EncodingSlotKind::CondCodeNibble:
        case EncodingSlotKind::BlockRel32:
            return TargetEncodingShape::X86Variable;
        case EncodingSlotKind::Rd:
        case EncodingSlotKind::Rn:
        case EncodingSlotKind::Rm:
        case EncodingSlotKind::Ra:
        case EncodingSlotKind::Imm26:
        case EncodingSlotKind::Imm16:
        case EncodingSlotKind::Imm9:
        case EncodingSlotKind::MemBaseNoScale:
        case EncodingSlotKind::Imm12:
        case EncodingSlotKind::SymbolPatchMarker:
        case EncodingSlotKind::Imm19:
            return TargetEncodingShape::Fixed32;
    }
    return TargetEncodingShape::None;  // unreachable; satisfies non-exhaustive switches
}

[[nodiscard]] constexpr std::string_view
encodingSlotKindName(EncodingSlotKind s) noexcept {
    return kEncodingSlotKindTable.name(s);
}
[[nodiscard]] constexpr std::optional<EncodingSlotKind>
encodingSlotKindFromName(std::string_view s) noexcept {
    return kEncodingSlotKindTable.fromName(s);
}

// One per-variant byte-emission template (plan 13 §2.5). The walker
// reads this and emits: optional REX prefix (with W/R/B bits derived
// from the wired registers' `hwEncoding`), opcode bytes, optional
// ModR/M byte (with `modrmRegExt` filling the `reg` field when an
// instruction uses the `/digit` ModR/M extension instead of a real
// register), then SIB+disp+imm per the slot wiring.
struct DSS_EXPORT TargetEncodingTemplate {
    // REX.W bit (operand-size override: 1 for 64-bit operations on
    // GPR opcodes; 0 for 32-bit). When ANY REX bit (W/R/B/X) is set,
    // the walker emits a REX prefix byte (0x40 base + bits). Only
    // meaningful for the `x86-variable` shape.
    bool rexW = false;

    // FC2 Part B (SSE float backend): mandatory legacy-prefix bytes
    // emitted BEFORE the REX prefix (the x86 decode contract: a
    // mandatory prefix like F2/F3/66 that selects an SSE opcode
    // form must precede REX, or the prefix is not part of the
    // opcode selection). Empty = no prefix (every pre-FC2 opcode).
    // Only meaningful for the `x86-variable` shape — validate()
    // rejects it on a fixed32 variant (mirrors the opcodeBytes /
    // modrmRegExt fixed32 rejection).
    std::vector<std::uint8_t> mandatoryPrefix;

    // Fixed opcode bytes (e.g. `[0x03]` for `add r64, r/m64`; `[0x0F,
    // 0xAF]` for `imul r64, r/m64`). Non-empty for any non-`None`
    // variant of the `x86-variable` shape.
    std::vector<std::uint8_t> opcodeBytes;

    // When the instruction uses a `/digit` ModR/M-reg extension (e.g.
    // `/0` for the immediate form of `add` — opcode 0x81 reg=0 means
    // ADD; reg=1 means OR; reg=5 means SUB; etc.), this field carries
    // the 3-bit digit. When set, the variant has NO `ModRmReg` slot
    // (the digit IS the reg field); the variant's `ModRmRm` slot
    // wires the destination register. Only meaningful for the
    // `x86-variable` shape.
    std::optional<std::uint8_t> modrmRegExt;

    // D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1): when true,
    // the encoder reads the inst's `payload` field as a
    // `TargetCondCode`, looks up the schema's `condCodeEncoding[]`
    // nibble for that condition, and OR's it into the LAST opcode
    // byte of `opcodeBytes`. Used by x86 setcc (`0F 90+cc`) and jcc
    // (`0F 80+cc`). Fail-loud when the target hasn't loaded
    // `condCodeEncoding[]` (A_NoCondCodeEncoding) — silently OR'ing
    // zero would map every condition to `eq`.
    bool condCodeFromPayload = false;

    // D-AS3-COND-CODE-ARM64 (ARM64 control-flow): cond-nibble PLACEMENT
    // + INVERSION knobs for the `fixed32` walker's `condCodeFromPayload`
    // arm. Both default to the x86 / B.cond shape (LSB 0, no invert) so
    // every existing cond-bearing opcode is byte-identical after these
    // fields land.
    //   * `condBitPos` — the LSB inside word 0 where the 4-bit cond
    //     nibble is OR'd. 0 for AArch64 `B.cond` (bits 0..3); 12 for
    //     AArch64 `CSET` (= `CSINC Xd,XZR,XZR,invcond`, cond at bits
    //     12..15). (The x86-variable walker has its own opcode-byte
    //     placement and ignores this field.)
    //   * `condInvert` — when true, XOR the cond nibble with 1 before
    //     placing it (the AArch64 inverse-condition trick). `CSET cond`
    //     materializes 1-when-cond by encoding `CSINC` with the INVERTED
    //     condition (the false-arm increments XZR→1); so `cset x,gt`
    //     (GT=0xC) encodes condition 0xC^1 = 0xD. `B.cond` does NOT
    //     invert (false here).
    // Only meaningful when `condCodeFromPayload` is true.
    std::uint8_t condBitPos = 0;
    bool         condInvert = false;

    // D-LIR-SETCC-WIDTH-CONTRACT (step 13.5 cycle 1 post-fold,
    // code-reviewer C2): force a REX prefix even when no REX bit
    // (W/R/X/B) is set. Required by x86 byte-register-bearing
    // opcodes like setcc that target rsp/rbp/rsi/rdi (hwEncoding
    // 4..7) — without a REX prefix, ModR/M.rm=4..7 references the
    // legacy {ah, ch, dh, bh} high-byte aliases instead of the
    // {spl, bpl, sil, dil} low-byte registers; setcc would silently
    // write to the high byte of a different physical register.
    // With ANY REX bit set (or this flag forcing one), the encoder
    // uses the spl/bpl/sil/dil aliasing — correct low-byte access
    // across all 16 GPRs.
    bool forceRexPrefix = false;

    // Fixed-word template (plan 13 AS3 — `fixed32` shape). The 32-bit
    // base bit pattern of an AArch64 / RV32-style instruction; the
    // walker emits this word with each declared slot's `hwEncoding`
    // OR'd into the slot's bit window, then writes the resulting
    // word LE-encoded as 4 bytes. Only meaningful for the `fixed32`
    // shape — non-`fixed32` variants leave this at 0 (the loader
    // accepts but does not require the field; validate() flags
    // `opcodeBytes` / `modrmRegExt` declared on a fixed32 variant
    // since those ARE x86-only fields with no fixed32 meaning).
    //
    // Sentinel note: `fixedWord = 0` is the default. A legitimate
    // fixed32 base of all-zeros (currently undefined on every
    // shipped ISA — AArch64 reserves it as UDF, RV32 as illegal)
    // is therefore indistinguishable from "default". When the
    // first ISA needs the zero-base, promote this to
    // `std::optional<std::uint32_t>`.
    std::uint32_t fixedWord = 0;

    // D-AS4-3 (multi-instruction-macro encoder): the base bit pattern
    // of a MULTI-WORD `fixed32` instruction (an N-word macro-op such
    // as AArch64 `lea` = ADRP+ADD, or a future RISC-V `auipc`+`addi`).
    // EMPTY by default — every existing single-word opcode keeps
    // `fixedWord` and emits byte-identically. When non-empty, the
    // walker emits one 32-bit word per element (LE) in order, each
    // word's slots OR'd per the wires' `wordIndex`; per-word
    // relocations stamp at the START of their word. `fixedWord` and
    // `fixedWords` are MUTUALLY EXCLUSIVE — validate() rejects a
    // template that sets both (the single-word default would be
    // silently shadowed). Only meaningful for the `fixed32` shape.
    std::vector<std::uint32_t> fixedWords;

    // Number of 32-bit words this template emits: the multi-word
    // count when `fixedWords` is set, else 1 (the single-word
    // `fixedWord` path). The walker + validate() size their per-word
    // structures (the `words` vector, the per-word slot-tracking) from
    // this — a single source of truth for the word count.
    [[nodiscard]] std::size_t wordCount() const noexcept {
        return fixedWords.empty() ? 1u : fixedWords.size();
    }

    // The base bit pattern of word `i` (0-based). For the single-word
    // path (`fixedWords` empty) word 0 is `fixedWord`; any other index
    // is out of range. For the multi-word path it is `fixedWords[i]`.
    // Caller guarantees `i < wordCount()` (the walker loops to
    // wordCount(); validate() bounds every `wordIndex`).
    [[nodiscard]] std::uint32_t wordAt(std::size_t i) const noexcept {
        if (fixedWords.empty()) return fixedWord;  // i==0 by precondition
        return fixedWords[i];
    }
};

// True iff the slot kind carries a SYMBOL-RELATIVE value that the
// assembler emits as a RELOCATION entry (rather than the operand's
// hwEncoding or immediate value). The walker writes zeros (or the
// addend) at the slot's byte position and pushes a Relocation into
// the AssembledFunction; the linker (plan 14) patches the slot at
// link time. validate() rule: a wire targeting a symbol-bearing
// slot MUST declare `relocationKind`; a wire to a non-symbol slot
// MUST NOT.
[[nodiscard]] constexpr bool
isSymbolBearingSlot(EncodingSlotKind s) noexcept {
    switch (s) {
        case EncodingSlotKind::Disp32:
        case EncodingSlotKind::Imm26:
        case EncodingSlotKind::RipRelDisp32:
        // D-AS4-3: the generic symbol-patch marker is symbol-bearing —
        // the walker emits a Relocation (per the wire's relocationKind)
        // and writes no immediate bits; the linker patches the field.
        case EncodingSlotKind::SymbolPatchMarker:
            return true;
        case EncodingSlotKind::ModRmReg:
        case EncodingSlotKind::ModRmRm:
        case EncodingSlotKind::Imm32:
        case EncodingSlotKind::Imm8:
        case EncodingSlotKind::Rd:
        case EncodingSlotKind::Rn:
        case EncodingSlotKind::Rm:
        case EncodingSlotKind::Ra:
        case EncodingSlotKind::Imm16:
        case EncodingSlotKind::Imm9:
        case EncodingSlotKind::MemBaseNoScale:
        case EncodingSlotKind::Imm12:
        case EncodingSlotKind::ModRmRmMem:
        case EncodingSlotKind::MemBaseScale:
        case EncodingSlotKind::Disp32Mem:
        case EncodingSlotKind::SibIndex:
        case EncodingSlotKind::CondCodeNibble:
        case EncodingSlotKind::BlockRel32:
        // D-AS3-BLOCK-REL-IMM19/26: Imm19 (ARM64 B.cond displacement) is
        // block-relative like BlockRel32 / the intra-function Imm26 use —
        // resolved at assemble time, no linker relocation. (Imm26 itself
        // stays symbol-bearing above for the BL/`call` form; the encoder
        // distinguishes Imm26's dual use by operand kind — a BlockRef
        // operand is block-relative, a SymbolRef operand emits the reloc.)
        case EncodingSlotKind::Imm19:
            // D-AS4-1 / D-AS4-5 memory-addressing slots write immediate
            // displacements / register encodings (not symbol-relative).
            // The companion symbol-bearing slot for RIP-relative `lea`
            // is `RipRelDisp32` above; it's distinct because it forces
            // the ModR/M state (mod=00 rm=101) in addition to the
            // disp32 patch site, where Disp32 alone (e.g. `call rel32`)
            // has no associated ModR/M byte. CondCodeNibble (D-CSUBSET-
            // WHILE-LOOP-SUBSTRATE) writes into the opcode byte from
            // the inst payload — no symbol. BlockRel32 patches a 4-byte
            // intra-function displacement at assemble time — also no
            // symbol-tier relocation.
            return false;
    }
    return false;
}

// One operand-wire: "source operand at LIR-index `index` goes into
// `slotKind` of the emitted bytes." The struct is intentionally named
// `Wire` — the LIR-side `operands[]` are the things being wired (the
// containing variant has both an `operandKinds` guard AND a `wires`
// list; reusing `operands` for both made the role read ambiguously).
//
// `relocationKind` (plan 13 AS4) names which row of
// `TargetSchemaData::relocations[]` the walker emits when this wire
// references a `SymbolRef` LIR operand. The loader resolves the
// name to its opaque `RelocationKind` tag at load time and stashes
// it here. Required when `slotKind` is symbol-bearing (Disp32 /
// Imm26); forbidden otherwise.
struct DSS_EXPORT TargetEncodingWire {
    std::uint8_t     index           = 0;
    EncodingSlotKind slotKind        = EncodingSlotKind::ModRmReg;
    std::optional<RelocationKind> relocationKind;
    // D-AS4-3 (multi-instruction-macro encoder): which 32-bit word
    // (0-based) of a multi-word `fixed32` template this wire's slot
    // lives in. DEFAULT 0 — every existing single-word wire is
    // unchanged (its slot is interpreted within word 0). The slot's
    // bit-window (`windowFor`) is applied INSIDE word[wordIndex]; a
    // symbol-bearing wire's relocation stamps at word[wordIndex]'s
    // byte offset. validate() requires `wordIndex < template.wordCount()`.
    std::uint8_t     wordIndex       = 0;
    // D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1): bytes
    // emitted IMMEDIATELY BEFORE this wire's slot bytes (between
    // the previous wire's emission and this one). Used by jcc's
    // compound encoding: the second BlockRel32 wire (fallthrough
    // target) declares `prefixOpcodeBytes: [0xE9]` so the encoder
    // emits `E9 rel32` (the unconditional jmp to fallthrough)
    // after the cond branch's `0F 8x rel32`. Empty for every
    // other wire (no extra bytes between slots).
    std::vector<std::uint8_t> prefixOpcodeBytes;
};

// One encoding variant — guard + template + slot-wiring. The walker
// picks the FIRST variant whose `operandKinds` guard matches the LIR
// instruction's actual operand-kind sequence (operand 0 against
// operandKinds[0], etc.). No matching variant ⇒
// `A_NoMatchingEncodingVariant`.
//
// `validate()` (in target_schema.cpp) enforces the load-time invariants:
//   * Two variants with identical `operandKinds` are rejected
//     (overlapping guards would silently first-match-win).
//   * `result != None ⇒ either `resultSlot` is set OR the template
//     declares `modrmRegExt`` (otherwise the destination register
//     would be silently dropped from the encoding).
//   * `modrmRegExt` is incompatible with ANY wire targeting ModRmReg
//     (the `/digit` extension IS the reg field; co-declaring it with
//     a wire would silently overwrite one or the other).
//   * No two slots in `{resultSlot} ∪ wires[*].slotKind` may target
//     the same ModR/M-byte slot (ModRmReg, ModRmRm) — would silently
//     overwrite at encode time.
//   * Every guard position must have a matching wire (or be unused
//     by-design; the validator's positional check pins this).
// D-AS4-3 (multi-instruction-macro encoder): an ADDITIONAL placement
// of the instruction's RESULT register, beyond the primary `resultSlot`
// (which is implicitly word 0). The same result register's hwEncoding
// is OR'd into `slotKind` of `word[wordIndex]`. Needed when a multi-word
// macro repeats the destination across words — AArch64 `lea` is
// `ADRP Xd, sym; ADD Xd, Xd, #:lo12:sym`, so Xd lands in word0.Rd
// (resultSlot), word1.Rd, AND word1.Rn (the ADD reads its own dest as
// the source base). Generic: any ISA whose multi-word materialization
// threads the destination register through later words uses this — no
// per-opcode special case. Empty for every single-result opcode.
struct DSS_EXPORT ResultSlotExtra {
    EncodingSlotKind slotKind  = EncodingSlotKind::Rd;
    std::uint8_t     wordIndex = 0;
};

struct DSS_EXPORT TargetEncodingVariant {
    std::vector<OperandKindFilter>     operandKinds;
    // FC3 c2 (D-CSUBSET-32BIT-ALU-FORMS): optional WIDTH discriminator
    // on the guard — the JSON key `guard.width`. 0 = absent = the
    // variant matches an instruction of ANY width (every pre-FC3
    // variant; width-invariant ops like loads/stores/branches keep
    // this). 32/64 = the variant matches ONLY an instruction whose
    // `lirInstWidthBits(flags)` equals it (the 32-bit no-REX.W x86
    // forms / arm64 W-forms vs their 64-bit siblings — same mnemonic,
    // same operand shape, different encoded width). The loader
    // rejects any other value; validate() rejects two same-kind
    // variants with the same width AND the ambiguous mix of a
    // width-keyed variant with a width-absent same-kind sibling
    // (first-match dispatch would silently shadow one of them).
    std::uint8_t                       guardWidthBits = 0;
    TargetEncodingTemplate             tmpl;
    // Where the instruction's RESULT register goes (when the inst
    // has a result). Nullopt for value-less instructions (e.g.
    // `ret`). Most binary/unary register opcodes use ModRmReg here;
    // immediate-destination forms use ModRmRm with `modrmRegExt`
    // filling the reg field. Implicitly word 0 for multi-word
    // templates; additional placements go in `extraResultSlots`.
    std::optional<EncodingSlotKind>    resultSlot;
    // D-AS4-3: additional placements of the SAME result register in a
    // multi-word template (see `ResultSlotExtra`). Empty for every
    // single-word / single-placement opcode. validate() requires a
    // `resultSlot` when this is non-empty (an extra placement of a
    // result that has no primary slot is malformed) and bounds each
    // `wordIndex < template.wordCount()`.
    std::vector<ResultSlotExtra>       extraResultSlots;
    // Where each LIR source operand (`inst.operands[wire.index]`)
    // goes in the emitted bytes.
    std::vector<TargetEncodingWire>    wires;
};

// The full encoding facet on a `TargetOpcodeInfo`. Carries the shape
// discriminator (closed enum, plan 13 §2.4 shape-keyed dispatch) and
// the per-variant rows the walker consumes. `shape == None` means
// "no encoding declared"; `variants.empty()` is only legal when
// `shape == None`.
struct DSS_EXPORT TargetEncodingInfo {
    TargetEncodingShape                shape = TargetEncodingShape::None;
    std::vector<TargetEncodingVariant> variants;
};

// One relocation kind declared by the target schema (plan 13 §2.6, the
// bucket-1 reloc taxonomy facet). Each row defines an opaque
// `uint32_t kind` tag whose meaning is the row itself — the assembler
// writes the tag onto `Relocation::kind`; the linker (plan 14) reads
// it via `schema.relocationInfo(kind)` to resolve the formula.
//
// `kind` slot-0 is reserved as an invalid sentinel: every declared
// row MUST carry a `kind != 0` (loader-enforced). Two rows with the
// same `kind` are also rejected.
//
// **Formula dispatch (D-LK6-1 closure — LK10 cycle 3 post-fold #2
// sibling cycle, 2026-06-01):** every relocation row carries a
// `formulaKind: RelocFormulaKind` closed-enum discriminator. The
// linker (`applyExecRelocations` in `link/format/exec_reloc_apply.hpp`)
// dispatches ONCE on this enum to compute the patch and write it. The
// JSON-side `formula` key is **load-bearing** — it accepts exactly the
// string set declared by `parseRelocFormulaKind`.
//
//   * `Linear` covers x86_64 rel32 / abs32 / abs64 + ARM64 abs64:
//       value = S + A + (pcRel ? -P : 0) + addendBias
//       written `widthBytes` LE at the patch site. The structured
//       triple (`pcRelative`, `addendBias`, `widthBytes`) parameterises
//       the formula.
//   * `Aarch64Call26` / `Aarch64AdrPrelPgHi21` / `Aarch64AddAbsLo12`
//       encode bit-shift / bitfield-insert ARM64 formulas — see the
//       per-variant comments on `RelocFormulaKind` below.
//
// Coherence rules (enforced at JSON load + `validate()`):
//   (a) `widthBytes != 0` ⇒ `widthBytes ∈ {4, 8}`.
//   (b) Linear, `pcRelative || addendBias != 0` ⇒ `widthBytes != 0`.
//   (c) Linear, `addendBias != 0` ⇒ `pcRelative` (no absolute-with-bias).
//   (d) Linear, `widthBytes != 0` ⇒ `|addendBias|` fits signed in widthBytes.
//   (e) `formulaKind != Linear` ⇒ `widthBytes == 4` (auto-defaulted by
//       the JSON loader), `pcRelative == false`, `addendBias == 0`
//       (the variant fully encodes the formula).
// Rule (e) is what makes the wide-product struct safe: every non-Linear
// row leaves the Linear sub-triple at default; the kernel ignores them.
// A `std::variant<LinearReloc, Aarch64Call26, ...>` would make this
// type-encoded but the three ARM64 variants are stateless tag types
// (the bit layout lives in code, not on the row), so the variant
// reshape gives little. Anchored D-LK6-17 — fold when RISC-V's first
// reloc kind lands (next ISA likely to add a 5th formula class).

// Closed-enum tagged variant — D-LK6-1 closure (plan 14 §3.1, 2026-06-01).
// Each variant names a concrete relocation-formula class with a fixed
// bit-layout + shift policy. The kernel dispatches once on this
// discriminator in `applyExecRelocations` and applies the named formula.
//
// Adding a new target's reloc kind (RISC-V, MIPS, etc.) = add a new
// variant + one switch arm in the kernel. JSON-side: declare the new
// name string. NO target-name branching in the kernel — the formula
// class is a property of the target's `*.target.json`, not the kernel.
//
// **Source / target / linker agnostic**: the discriminator is on
// `TargetSchema`; ELF / PE / Mach-O walkers reuse the kernel verbatim.
// HIR / MIR / LIR see opaque `Relocation::kind` values — they never
// inspect `formulaKind`.
enum class RelocFormulaKind : std::uint8_t {
    // value = S + A + (pcRel ? -P : 0) + addendBias  — write widthBytes LE
    // bytes. Covers x86_64 rel32 / abs32 / abs64 + ARM64 abs64.
    Linear                = 0,
    // ARM64 R_AARCH64_CALL26 / R_AARCH64_JUMP26:
    //   value = (S + A - P) >> 2
    //   range-check signed 26-bit; OR (value & 0x03FFFFFF) into the
    //   ARM64 instruction word's bits[25:0] (BL / B target).
    Aarch64Call26         = 1,
    // ARM64 R_AARCH64_ADR_PREL_PG_HI21:
    //   value = ((S + A) >> 12) - (P >> 12)
    //   range-check signed 21-bit; ADRP-style split: bits[1:0] of value
    //   → instruction immlo[30:29]; bits[20:2] → immhi[23:5].
    Aarch64AdrPrelPgHi21  = 2,
    // ARM64 R_AARCH64_ADD_ABS_LO12_NC (and LDST equivalents):
    //   value = (S + A) & 0xFFF
    //   range-check S+A ∈ [0, UINT32_MAX] (kernel rejects negative or
    //   out-of-32-bit values — the paired ADRP companion can only
    //   compute pages within the 32-bit space without an additional
    //   high-bit reloc); OR (value << 10) into ADD imm12 [21:10].
    Aarch64AddAbsLo12     = 3,
};

// Single source of truth — `relocFormulaName` + `parseRelocFormulaKind`
// + `acceptedRelocFormulaList` all iterate this table. Adding a new
// variant = add a row here + add the enum entry. The `static_assert`
// on size catches forgetting one half. (architect + type-design
// 4-agent convergence at post-fold #2 — was previously 3 independent
// hand-rolled enumerations, DRY hazard waiting for the 5th variant.)
inline constexpr EnumNameTable<RelocFormulaKind, 4> kRelocFormulaTable{{{
    { RelocFormulaKind::Linear,               "linear" },
    { RelocFormulaKind::Aarch64Call26,        "aarch64_call26" },
    { RelocFormulaKind::Aarch64AdrPrelPgHi21, "aarch64_adr_prel_pg_hi21" },
    { RelocFormulaKind::Aarch64AddAbsLo12,    "aarch64_add_abs_lo12" },
}}};

[[nodiscard]] DSS_EXPORT std::string_view
    relocFormulaName(RelocFormulaKind k) noexcept;

// ── D-LK10-ENTRY: ProcessExit substrate (plan 14 §2.13 Slice B) ────
//
// Vocabulary types for the runnable-binary spine's process-exit
// mechanism. The FIELD lives on `ObjectFormatData` (not
// `TargetSchemaData`) because the mechanism + syscall number +
// import library are per-OS data, and format JSONs are already
// keyed per CPU × OS. These types live here in `target_schema.hpp`
// alongside the other closed-enum vocabulary (RelocFormulaKind,
// TargetCondCode, TargetAbiModel, ...) — the vocabulary is shared
// between target + format schema layers even though the field is
// format-side. The trampoline emitter (Slice C) reads the field
// via `formatSchema.processExit()` and dispatches on
// `ExitMechanism` (closed-enum, no `if (os == ...)` branches).
//
//   * `Syscall`        — raw kernel transition (Linux `exit_group`,
//                        macOS BSD `exit`). Per-OS data: syscall
//                        number, syscall-num register name, syscall
//                        opcode bytes.
//   * `ByNameImport`   — call through an extern-import IAT slot
//                        (Windows `kernel32!ExitProcess`, future
//                        macOS libSystem). Per-OS data: library
//                        path, mangled name.
//   * `None`           — default-constructed sentinel. "No
//                        mechanism" is encoded by the field type
//                        itself (`optional<ProcessExit>` empty),
//                        NOT by `None` appearing in a validated
//                        ProcessExit. The JSON loader explicitly
//                        rejects `mechanism="none"` so the
//                        sentinel cannot leak into a validated
//                        schema.
enum class ExitMechanism : std::uint8_t {
    None         = 0,  // default-constructed zero; loader rejects "none"
    Syscall      = 1,  // raw syscall (Linux exit_group / Mach-O BSD exit)
    ByNameImport = 2,  // call qword ptr [iat] (Windows ExitProcess)
};

inline constexpr EnumNameTable<ExitMechanism, 3> kExitMechanismTable{{{
    { ExitMechanism::None,         "none"           },
    { ExitMechanism::Syscall,      "syscall"        },
    { ExitMechanism::ByNameImport, "by-name-import" },
}}};

[[nodiscard]] constexpr std::string_view exitMechanismName(ExitMechanism m) noexcept {
    return kExitMechanismTable.name(m);
}
[[nodiscard]] constexpr std::optional<ExitMechanism>
exitMechanismFromName(std::string_view s) noexcept {
    return kExitMechanismTable.fromName(s);
}

// Per-OS process-exit descriptor. Lives on `ObjectFormatData`
// (loaded from format JSON's `processExit` block). The trampoline
// emitter (Slice C) reads the active arm based on `mechanism`:
//
// For Syscall (Linux / macOS-BSD-syscall):
//   * `syscallNumber`      — syscall-table index (Linux x86_64
//                            exit_group = 231; ARM64 Linux = 94;
//                            macOS BSD exit = 0x2000001).
//   * `syscallNumGpr`      — register that holds the syscall number
//                            at the syscall transition ("rax" on
//                            x86_64; "x8" on ARM64).
//   * `syscallOpcodeBytes` — instruction bytes in STORED ORDER
//                            (memory-layout order, NOT disassembler
//                            display order). x86_64 SYSCALL byte
//                            stream = [0x0F, 0x05]. ARM64 SVC #0
//                            instruction word = 0xD4000001; in
//                            ARM64's little-endian memory layout
//                            this stores as [0x01, 0x00, 0x00, 0xD4]
//                            — that's what the JSON declares + what
//                            the emitter writes verbatim. NOT used
//                            by Slice C's LIR-driven emitter today
//                            (the Slice A `syscall` LIR opcode emits
//                            these bytes through the assembler) —
//                            retained on the substrate as an escape
//                            hatch for future kernels whose syscall
//                            instruction differs from the LIR
//                            opcode's lowering (e.g. legacy BSD
//                            `int 0x80`) without requiring a new
//                            LIR opcode.
//
// For ByNameImport (Windows / macOS-libSystem):
//   * `importLibraryPath`  — DLL/dylib path ("kernel32.dll" on
//                            Windows; future "/usr/lib/libSystem.B
//                            .dylib" on macOS).
//   * `importMangledName`  — on-binary symbol name ("ExitProcess"
//                            on Windows; "_exit" with leading
//                            underscore via D-FF4 on macOS).
//
// The `statusArgGpr` is intentionally NOT a field — it's read from
// the format's `entryCallingConvention.argGprs[0]` (preserves
// single source of truth for the calling-convention register
// vocabulary).
struct DSS_EXPORT ProcessExit {
    ExitMechanism mechanism = ExitMechanism::None;

    // Syscall arm
    std::uint32_t            syscallNumber     = 0;
    std::string              syscallNumGpr;
    std::vector<std::uint8_t> syscallOpcodeBytes;

    // ByNameImport arm
    std::string importLibraryPath;
    std::string importMangledName;
};

[[nodiscard]] DSS_EXPORT std::optional<RelocFormulaKind>
    parseRelocFormulaKind(std::string_view s) noexcept;

// Comma-separated quoted list of accepted formula-discriminator
// strings — used by the JSON loader's error messages. Driven from
// `kRelocFormulaTable` so the accepted set never lags the enum.
[[nodiscard]] DSS_EXPORT std::string acceptedRelocFormulaList();

struct DSS_EXPORT TargetRelocationInfo {
    std::string      name;            // canonical text key (e.g. "rel32", "abs64")
    RelocationKind   kind{};          // opaque tag — written into Relocation::kind;
                                      // values flow ONLY from this field + the
                                      // schema's `relocationInfo`/`relocationByName`
                                      // accessors, never assembler-fabricated.
    RelocFormulaKind formulaKind = RelocFormulaKind::Linear; // D-LK6-1 closure
    bool         pcRelative  = false;  // Linear only: include `-P` (PC-relative)
    std::int32_t addendBias  = 0;      // Linear only: implicit constant bias
                                       // (e.g. -4 for x86 rel32 to
                                       // skip past the 4-byte
                                       // displacement field)
    std::uint8_t widthBytes  = 0;      // Linear: 4 / 8 — bytes to write
                                       // at the patch site. 0 reaches
                                       // only via legacy / malformed
                                       // JSON (kernel rejects with
                                       // K_RelocationKindMismatch).
                                       // Non-Linear: always 4 (ARM64
                                       // instruction word; auto-defaulted
                                       // by the JSON loader if absent).
};

// Discriminates the FIVE concrete terminator shapes a target's opcode
// table can declare. Required because the `.dsslir` parser
// (`parseInst` in `src/lir/lir_text.cpp`) dispatches terminator
// construction (`addBr` / `addCondBr` / `addReturn` / `addUnreachable`)
// based on the opcode's role, NOT on operand-list emptiness heuristics
// — earlier draft used "0 successors + 0 operands + result=None →
// Unreachable, else Return" which silently mis-classified any future
// target whose `ret` opcode takes zero operands.
//
// `None` is the default for non-terminator opcodes. `TargetOpcodeInfo::
// isTerminator()` derives boolean terminator-ness from this single
// field — the substrate has ONE source of truth, not a redundant pair.
enum class TargetTerminatorKind : std::uint8_t {
    None        = 0,    // non-terminator opcode (default)
    Br          = 1,    // 1 successor, embeds BlockRef operand (LirBuilder::addBr)
    CondBr      = 2,    // 2 successors, NO BlockRef operands       (LirBuilder::addCondBr)
    Switch      = 3,    // >=2 successors                           (LirBuilder::addSwitch — reserved)
    Return      = 4,    // 0 successors, may carry return-value ops (LirBuilder::addReturn)
    Unreachable = 5,    // 0 successors, 0 operands                 (LirBuilder::addUnreachable)
};

// Canonical string form used by `.target.json` and `.dsslir` text.
// Single source of truth for the loader (string → enum) and any future
// emit-side serializer (enum → string).
inline constexpr EnumNameTable<TargetTerminatorKind, 6> kTargetTerminatorKindTable{{{
    { TargetTerminatorKind::None,        "none"        },
    { TargetTerminatorKind::Br,          "br"          },
    { TargetTerminatorKind::CondBr,      "cond-br"     },
    { TargetTerminatorKind::Switch,      "switch"      },
    { TargetTerminatorKind::Return,      "return"      },
    { TargetTerminatorKind::Unreachable, "unreachable" },
}}};

[[nodiscard]] constexpr std::string_view
targetTerminatorKindName(TargetTerminatorKind k) noexcept {
    return kTargetTerminatorKindTable.name(k);
}
[[nodiscard]] constexpr std::optional<TargetTerminatorKind>
targetTerminatorKindFromName(std::string_view s) noexcept {
    return kTargetTerminatorKindTable.fromName(s);
}

// Per-kind contract: successor-count window the loader's `validate()`
// enforces AND `.dsslir` parser dispatch consults. Single source of
// truth — adding e.g. `IndirectBr` is one row here, not two switches.
// `None` is omitted: non-terminators are policed by the `maxSuccessors
// == 0` rule on the opposite branch.
struct TargetTerminatorShape {
    TargetTerminatorKind kind;
    std::uint8_t         minSuccessors;
    std::uint8_t         maxSuccessors;
    // Sentinel: `Switch.maxSuccessors == 255` means "unbounded above
    // the minimum" — Switch arity is open-ended by design. Validator
    // treats `maxSuccessors == 255` as "no upper bound".
};

inline constexpr std::array<TargetTerminatorShape, 5> kTargetTerminatorShapes{{
    { TargetTerminatorKind::Br,          1, 1   },
    { TargetTerminatorKind::CondBr,      2, 2   },
    { TargetTerminatorKind::Switch,      2, 255 },  // 255 = unbounded sentinel
    { TargetTerminatorKind::Return,      0, 0   },
    { TargetTerminatorKind::Unreachable, 0, 0   },
}};

[[nodiscard]] constexpr TargetTerminatorShape const*
findTerminatorShape(TargetTerminatorKind k) noexcept {
    for (auto const& s : kTargetTerminatorShapes) {
        if (s.kind == k) return &s;
    }
    return nullptr;  // `None` — non-terminator
}

// Per-opcode descriptor — populated from the JSON `opcodes` array.
// Implicit-register constraint declaration (cycle 10p substrate,
// 2026-06-04). Carried on per-opcode `TargetOpcodeInfo`; arrays are
// register NAMES (e.g. "rax") at this struct's level, resolved to
// register ordinals at TargetSchema load time by the validator (any
// unknown name fails loud — same precedent as TargetCallingConvention's
// argGprs/callerSaved validation). Resolved ordinals live alongside
// the names so downstream consumers (regalloc, future-MIR-verifier
// reload-from-text round-trip) read O(1) without a re-resolution
// walk.
//
// All three arrays are SEMANTICALLY distinct:
//   * `inputs` — registers whose values are implicitly READ by the
//     instruction. Regalloc must keep these live across the
//     instruction's def site OR materialize the value into the named
//     register before the instruction.
//   * `outputs` — registers implicitly WRITTEN. Regalloc must
//     consume the value FROM the named register after the
//     instruction (or insert a move out if downstream wants it in a
//     different reg).
//   * `clobbered` — registers DESTROYED (values become indeterminate
//     post-instruction) but not modeled as outputs. Regalloc must
//     spill any live vreg that occupied this physical register
//     across the instruction boundary.
//
// An opcode CAN have a register appear in multiple sets (idiv's RAX
// is both input dividend AND output quotient; RDX is both input
// dividend-high AND output remainder). This is structurally legal —
// the sets describe orthogonal aspects of the contract.
struct DSS_EXPORT ImplicitRegisterConstraint {
    // Source-of-truth: register names as authored in the JSON. The
    // shape mirrors TargetCallingConvention.argGprs (vector of
    // strings). Authored at load time; validator resolves each name
    // through the target's register table.
    std::vector<std::string>   inputNames;
    std::vector<std::string>   outputNames;
    std::vector<std::string>   clobberedNames;

    // Validator-populated ordinals — parallel to the names arrays.
    // Empty iff the names array is empty. Consumers (regalloc) read
    // these directly; the names are kept for diagnostics + .target
    // round-trip + .dsslir round-trip.
    std::vector<std::uint16_t> inputOrdinals;
    std::vector<std::uint16_t> outputOrdinals;
    std::vector<std::uint16_t> clobberedOrdinals;

    // Role-tagged projection contract (D-CSUBSET-MOD-OP-CODEGEN-
    // OUTPUT-INDEX-CONTRACT closure, 2026-06-10). Optional JSON
    // objects `inputRoles` / `outputRoles` map a ROLE name (from the
    // loader's registered role vocabulary — "dividend", "quotient",
    // "remainder") to a register name that must ALSO appear in the
    // corresponding positional array. The MIR→LIR div/mod lowering
    // reads its pinned/captured registers BY ROLE, never by
    // positional index — so a JSON reorder of `outputs` can no
    // longer silently flip a quotient capture into a remainder
    // capture (the silent-miscompile class the anchor named). The
    // positional arrays REMAIN the regalloc/invariant surface
    // (outputs ⊆ clobbered; forbidden-set construction); ops whose
    // implicit registers are never projected by the lowering (cqo,
    // xor_rdx_zero) simply omit the role maps.
    std::vector<std::pair<std::string, std::string>> inputRoleNames;
    std::vector<std::pair<std::string, std::string>> outputRoleNames;

    // Validator-populated {role, ordinal} pairs — only successfully
    // resolved roles appear (a role whose register fails resolution
    // is diagnosed at load and omitted here, so consumers see the
    // failure as a missing role, fail-loud at the query site).
    std::vector<std::pair<std::string, std::uint16_t>> inputRoleOrdinals;
    std::vector<std::pair<std::string, std::uint16_t>> outputRoleOrdinals;

    [[nodiscard]] std::optional<std::uint16_t>
    inputOrdinalForRole(std::string_view role) const noexcept {
        for (auto const& [r, ord] : inputRoleOrdinals) {
            if (r == role) return ord;
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::uint16_t>
    outputOrdinalForRole(std::string_view role) const noexcept {
        for (auto const& [r, ord] : outputRoleOrdinals) {
            if (r == role) return ord;
        }
        return std::nullopt;
    }
};

// One row per opcode; index in the vector IS the opcode's numeric
// value (stored as `std::uint16_t` in the LIR instruction PODs).
//
// The min/max arity fields are advisory metadata today: the substrate
// only checks `isTerminator` (via `LirBuilder::add{Br,CondBr,Return}`
// and the closeFunction terminator-required guard). Cycle 3 isel +
// the MIR verifier will start consuming the operand/successor bounds;
// until then they document expected shape without enforcing it.
struct DSS_EXPORT TargetOpcodeInfo {
    std::string          mnemonic;
    TargetResultRule     result         = TargetResultRule::None;
    bool                 hasSideEffects = false;
    // True iff this opcode performs a function call (or an intrinsic
    // dispatch). The register allocator uses this to determine which
    // ranges cross a call boundary and therefore must avoid caller-
    // saved registers. Promotes "call-detection" out of mnemonic
    // matching (the allocator was previously matching "call" /
    // "intrinsic_call" strings, breaking target-agnosticism).
    bool                 isCall         = false;
    // Concrete terminator shape (see TargetTerminatorKind). Drives the
    // `.dsslir` parser's `parseInst` terminator-dispatch fork AND
    // `LirVerifier`'s successor-count cross-check. `None` for all
    // non-terminator opcodes. The previous design carried a separate
    // `isTerminator` bool too; that field was deleted because it was
    // derivable from `terminatorKind != None` (3-agent convergence
    // ML8 cycle 3 review: type-design + simplifier + silent-failure).
    TargetTerminatorKind terminatorKind = TargetTerminatorKind::None;
    std::uint8_t         minOperands    = 0;
    std::uint8_t         maxOperands    = 0;
    std::uint8_t         minSuccessors  = 0;
    std::uint8_t         maxSuccessors  = 0;

    // Byte-encoding facet (plan 13 AS1 substrate + AS2 variant rows).
    // `encoding.shape == None` (default) means no encoding declared —
    // the assembler emits `A_NoEncodingDeclared`. A non-`None` shape
    // requires a non-empty `encoding.variants[]` (validate()-enforced);
    // each variant carries its guard + template + slot wiring.
    TargetEncodingInfo   encoding;

    // 2-address legalization constraint (plan 13 AS3 — `lir_2addr_
    // legalize.cpp`). When `true`, the LIR pre-assembly legalize
    // pass ensures the instruction's `result` register equals
    // `operands[0]` before the assembler sees it — by inserting an
    // implicit `mov result, operands[0]` whenever they differ. x86's
    // reg-reg arithmetic (add/sub/mul) needs this (REX.W 0x03 /r
    // writes into r/m, so the dest IS one of the sources); ARM64's
    // reg-reg arithmetic is 3-address natively and leaves this
    // false.
    bool                 requires2Address = false;

    // Implicit-register constraint (cycle 10p substrate, 2026-06-04).
    // Optional per-opcode block describing fixed-register semantics
    // (e.g., x86 idiv ties RDX:RAX). See `ImplicitRegisterConstraint`
    // docblock above for the full contract + canonical examples.
    // Pre-cycle-10q invariant: shipped opcodes leave this nullopt;
    // regalloc consumer wiring lands in 10q.
    std::optional<ImplicitRegisterConstraint> implicitRegisters;

    // Terminator-ness derives from `terminatorKind` — single source of
    // truth. Callers ported from the old `isTerminator` bool field
    // gain a trailing `()` and keep working unchanged.
    [[nodiscard]] constexpr bool isTerminator() const noexcept {
        return terminatorKind != TargetTerminatorKind::None;
    }
};

namespace detail {

// Index maps reuse the project-wide `substrate::TransparentStringMap`
// (heterogeneous `string_view` lookup with no `std::string` allocation
// per call) — promoted to substrate in cycle 3a per the cycle-2b
// deferred-item closure. The three indexes (mnemonic / register-name /
// calling-convention-name) all instantiate it with `std::uint16_t`.

// In-memory schema. Mirrors `detail::GrammarSchemaData` — owned-by-
// value POD the loader builds + moves into the frozen `TargetSchema`.
// Hidden in `detail::` so it can only be constructed by the loader
// path (which enforces opcode-table invariants); arbitrary callers
// cannot hand-build a `TargetSchema` with a broken slot-0 sentinel
// or a mnemonicIndex out of sync with `opcodes`.
struct DSS_EXPORT TargetSchemaData {
    TargetSchemaId          id{};
    std::string             name;             // "x86_64" / "arm64" / ...
    std::string             version;          // semantic version string
    TargetAbiModel          abiModel = TargetAbiModel::RegisterMachine;

    // Opcode table — slot 0 carries the `"invalid"` sentinel mnemonic
    // (loader-enforced). Other slot-0 fields (terminator/result/arity)
    // are NOT pinned by the loader; the substrate treats opcode 0 as
    // unconditionally invalid via the addInst guard, not via these
    // fields.
    std::vector<TargetOpcodeInfo> opcodes;
    substrate::TransparentStringMap<std::uint16_t> mnemonicIndex;

    // Frame-op opcode role tags — the schema-side name of the
    // pseudo-ops that the post-regalloc rewrite pass emits and ML7
    // callconv lowering consumes. Defaults are "frame_load" /
    // "frame_store" but a target may override (e.g. a hypothetical
    // target with a `spill_reload` mnemonic instead of `frame_load`).
    // Empty string means the target does not declare frame pseudo-ops
    // (operand-stack ABIs).
    std::string frameLoadMnemonic  = "frame_load";
    std::string frameStoreMnemonic = "frame_store";

    // Physical register file (cycle 2b). Empty when the target JSON
    // omits the `registers` array — keeps the cycle 2a-shape targets
    // valid until ML6 regalloc requires the section.
    std::vector<TargetRegisterInfo> registers;
    substrate::TransparentStringMap<std::uint16_t> registerIndex;

    // FC2 Part B: per-register-class move/load/store mnemonic table
    // (the JSON `registerClassOps[]` section), indexed by the
    // TargetRegClass ordinal. A class WITHOUT a row resolves to the
    // universal default bindings ("mov"/"load"/"store") iff it is the
    // substrate's default class (GPR — the class every existing
    // lowering pass assumed); any OTHER row-less class resolves to
    // nothing so the consumer fails loud instead of silently emitting
    // the GPR instruction forms against a foreign register file.
    // validate() guarantees every DECLARED mnemonic resolves to an
    // opcode row. arm64 (no table, no fpr registers) is untouched.
    std::array<TargetRegisterClassOps, 5> registerClassOps{};

    // Calling conventions (cycle 2b). Same optional-for-now discipline
    // as `registers` — ML7 callconv lowering will require ≥1 entry.
    std::vector<TargetCallingConvention> callingConventions;
    substrate::TransparentStringMap<std::uint16_t> callingConventionIndex;

    // D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1, 2026-06-03):
    // per-target mapping from abstract `TargetCondCode` (substrate-tier
    // enum: 10 integer arms Eq/Ne/Slt/Sle/Sgt/Sge/Ult/Ule/Ugt/Uge + the
    // FC3.5-c2 float arms) to a numeric encoding used by the ISA's
    // conditional opcodes. x86_64 uses the low 4 bits of the setcc/jcc
    // opcode byte: Eq=4, Ne=5, Slt=12, Sle=14, Sgt=15, Sge=13, Ult=2,
    // Ule=6, Ugt=7, Uge=3. ARM64 uses the same low-4-bits position but
    // a different numeric mapping in bits 0..3 of the 32-bit B.cc
    // instruction word. Empty means the target has no cond-code-bearing
    // opcodes (declarative-only targets). When populated, MUST contain
    // all 10 INTEGER entries indexed by `(uint8_t)TargetCondCode`; the
    // FLOAT arms (fogt/foge/foeq/fone/fune/fuo/ford) are OPTIONAL —
    // `condCodeDeclared` records which entries the JSON actually
    // declared, and `condCodeEncoding()` returns nullopt for an
    // undeclared one (the MIR→LIR FCmp lowering reads that as "this
    // target realizes the predicate via the two-setcc composition";
    // the encoder fails loud if a single-cc inst reaches it anyway).
    std::array<std::uint8_t, kTargetCondCodeCount> condCodeEncoding{};
    std::array<bool, kTargetCondCodeCount>         condCodeDeclared{};
    // Companion bit: `true` once `condCodeEncoding` has been populated
    // from the JSON (any value, including all-zero, is legal — the
    // distinction is "is this table loaded vs. default-initialized").
    // Consumers gate the `EncodingSlotKind::CondCodeNibble` walker on
    // this flag — emitting a cond-code wire against an un-populated
    // table fails loud at the per-inst encoder rather than silently
    // OR'ing zero into the opcode byte.
    bool condCodeEncodingLoaded = false;

    // FC6 (D-FF3-1 layout half): the per-ABI aggregate-layout parameters
    // (`"aggregateLayout"` in .target.json) the generic `type_layout` engine reads
    // — the natural-alignment rule + the ISA max alignment. OPTIONAL at load (a
    // minimal target may omit it, like `callingConventions` / `registers`); the
    // fail-loud is CONSUMER-side, not loader-side — `aggregateLayoutLoaded` gates
    // the layout/`sizeof` path so an un-declared block fails loud (a positioned
    // diagnostic, no artifact) at use rather than silently returning a zero param.
    AggregateLayoutParams aggregateLayout{};
    bool                  aggregateLayoutLoaded = false;

    // Relocation taxonomy (plan 13 AS1 §2.6 — the bucket-1 reloc
    // facet). Each row declares one relocation kind: a canonical text
    // name (for the linker's `*.format.json` cross-reference per plan
    // 14 §2.0) + an opaque `kind` tag the assembler stamps onto
    // `Relocation::kind` + a human-readable formula. Empty is legal
    // (a target that emits no relocations); a non-empty section must
    // satisfy the `validate()` rules (unique `kind`, non-zero `kind`,
    // non-empty `name`).
    std::vector<TargetRelocationInfo> relocations;
    substrate::TransparentStringMap<std::uint16_t> relocationNameIndex;
    // Opaque-tag → row-index index for the assembler/linker hot path.
    // Plan 14's relocation-apply pass calls `relocationInfo(kind)`
    // once per relocation in every assembled object — a linear scan
    // there is an O(R·F) blowup at link time. validate() enforces
    // `kind` uniqueness across rows, so this index is safe to build
    // from the same monotonic loader path the name index uses.
    std::unordered_map<RelocationKind, std::uint16_t> relocationKindIndex;

    // Cross-field invariants the per-field JSON parse cannot express.
    // Returns the list of problems as fully-shaped `ConfigDiagnostic`s
    // (each one carries its JSON path in `.path`); the loader stamps
    // them as fatal. Empty result = well-formed. Called once at the
    // end of `loadFromText`; never called by external consumers.
    //
    // Rules enforced (cycle 2b):
    //   Opcode arity:
    //     - minOperands <= maxOperands
    //     - minSuccessors <= maxSuccessors
    //     - isTerminator && minSuccessors>0 && maxSuccessors==0 (contradiction)
    //     - !isTerminator && maxSuccessors>0 (non-terminator has no successors)
    //   Register file:
    //     - widthBytes > 0 when regClass != None (silent-zero guard)
    //     - subOf resolves to a known register
    //     - subOf chain is acyclic (mark-and-visit)
    //   Calling conventions:
    //     - every name resolves to a register (gated on
    //       `registers.empty() || callingConventions.empty()` to allow
    //       cycle-2a-shape configs but trap the silent-failure case
    //       where ONLY callingConventions is declared)
    //     - argGprs/returnGprs/callerSaved/calleeSaved must be GPR class
    //     - argFprs/returnFprs must be FPR class
    //     - stackAlignment is a power of two (and >0 when ANY field set)
    //     - shadowSpaceBytes % stackAlignment == 0
    //     - redZoneBytes    % stackAlignment == 0
    //   Relocations (AS1):
    //     - every `kind` is non-zero (slot-0 reserved as invalid sentinel)
    //     - every `kind` is unique across the section (collision rejection)
    //     - every `name` is non-empty (the linker's *.format.json lookup
    //       key cannot be the empty string)
    [[nodiscard]] std::vector<ConfigDiagnostic> validate() const;
};

} // namespace detail

class DSS_EXPORT TargetSchema {
public:
    // Frozen — moved out of the loader. Constructor is public so the
    // JSON loader (in target_schema_json.cpp) can build the data POD
    // and hand ownership over without a friend declaration.
    explicit TargetSchema(detail::TargetSchemaData data) noexcept
        : d_(std::move(data)) {}

    TargetSchema(TargetSchema const&)            = delete;
    TargetSchema& operator=(TargetSchema const&) = delete;
    TargetSchema(TargetSchema&&) noexcept        = default;
    TargetSchema& operator=(TargetSchema&&) noexcept = default;

    [[nodiscard]] TargetSchemaId    id()       const noexcept { return d_.id; }
    [[nodiscard]] std::string_view  name()     const noexcept { return d_.name; }
    // Semantic version string declared by the target JSON. Round-trip
    // contracts (e.g. `.dsslir` preamble) emit this so a cross-version
    // load is loudly rejected at parse time rather than silently
    // mis-interpreting opcode numbers / register table layouts that a
    // version bump might have permuted.
    [[nodiscard]] std::string_view  version()  const noexcept { return d_.version; }
    [[nodiscard]] TargetAbiModel    abiModel() const noexcept { return d_.abiModel; }
    [[nodiscard]] std::string_view  frameLoadMnemonic()  const noexcept {
        return d_.frameLoadMnemonic;
    }
    [[nodiscard]] std::string_view  frameStoreMnemonic() const noexcept {
        return d_.frameStoreMnemonic;
    }

    // ── Opcodes ─────────────────────────────────────────────────
    [[nodiscard]] std::span<TargetOpcodeInfo const> opcodes() const noexcept {
        return d_.opcodes;
    }
    [[nodiscard]] std::size_t opcodeCount() const noexcept { return d_.opcodes.size(); }

    // Look up opcode info by numeric index. Out-of-range returns
    // nullptr (caller decides whether that's an error).
    [[nodiscard]] TargetOpcodeInfo const* opcodeInfo(std::uint16_t op) const noexcept {
        return (op < d_.opcodes.size()) ? &d_.opcodes[op] : nullptr;
    }

    // True iff `op` is a terminator opcode. Out-of-range returns
    // false (defensive: an unknown opcode should fail loud at the
    // higher level, not silently pass as a terminator).
    [[nodiscard]] bool isTerminator(std::uint16_t op) const noexcept {
        auto const* info = opcodeInfo(op);
        return info != nullptr && info->isTerminator();
    }

    // Look up an opcode index by mnemonic. Returns nullopt for an
    // unknown mnemonic. Heterogeneous lookup — no `std::string`
    // allocation per call.
    [[nodiscard]] std::optional<std::uint16_t> opcodeByMnemonic(
            std::string_view mnemonic) const noexcept {
        auto it = d_.mnemonicIndex.find(mnemonic);
        if (it == d_.mnemonicIndex.end()) return std::nullopt;
        return it->second;
    }

    // ── Registers (cycle 2b) ────────────────────────────────────
    [[nodiscard]] std::span<TargetRegisterInfo const> registers() const noexcept {
        return d_.registers;
    }
    [[nodiscard]] std::size_t registerCount() const noexcept { return d_.registers.size(); }

    // Look up by ordinal (index in the `registers` vector). Out-of-range
    // returns nullptr.
    [[nodiscard]] TargetRegisterInfo const* registerInfo(std::uint16_t ordinal) const noexcept {
        return (ordinal < d_.registers.size()) ? &d_.registers[ordinal] : nullptr;
    }

    // Look up by name (heterogeneous; no allocation). Returns the
    // ordinal that `registerInfo(ordinal)` expects.
    [[nodiscard]] std::optional<std::uint16_t> registerByName(
            std::string_view name) const noexcept {
        auto it = d_.registerIndex.find(name);
        if (it == d_.registerIndex.end()) return std::nullopt;
        return it->second;
    }

    // FC2 Part B: resolve the opcode handle that performs `op` on a
    // value of register class `cls` (the per-register-class operation
    // table — `registerClassOps[]` in the target JSON). Resolution:
    //   * class has a declared row + the row names this op → that
    //     mnemonic's opcode (validate() guarantees it resolves; the
    //     optional still guards a hand-built schema);
    //   * class has a declared row but the row OMITS this op →
    //     nullopt — the CALLER fails loud naming class+op (e.g. an
    //     FPR store with no declared store mnemonic must never fall
    //     back to the GPR `store` encoding);
    //   * class has NO row: GPR (the substrate default class — what
    //     every pre-FC2 lowering pass emitted unconditionally) → the
    //     universal "mov"/"load"/"store" bindings; any other class →
    //     nullopt (fail loud at the caller).
    [[nodiscard]] std::optional<std::uint16_t> regClassOpOpcode(
            TargetRegClass cls, RegClassOp op) const noexcept {
        auto const idx = static_cast<std::size_t>(cls);
        if (idx >= d_.registerClassOps.size()) return std::nullopt;
        auto const& row = d_.registerClassOps[idx];
        if (row.declared) {
            auto const name = row.nameFor(op);
            if (name.empty()) return std::nullopt;
            return opcodeByMnemonic(name);
        }
        if (cls != TargetRegClass::GPR) return std::nullopt;
        switch (op) {
            case RegClassOp::Move:  return opcodeByMnemonic("mov");
            case RegClassOp::Load:  return opcodeByMnemonic("load");
            case RegClassOp::Store: return opcodeByMnemonic("store");
        }
        return std::nullopt;
    }

    // ── Calling conventions (cycle 2b) ──────────────────────────
    [[nodiscard]] std::span<TargetCallingConvention const> callingConventions() const noexcept {
        return d_.callingConventions;
    }
    [[nodiscard]] std::size_t callingConventionCount() const noexcept {
        return d_.callingConventions.size();
    }

    [[nodiscard]] TargetCallingConvention const* callingConvention(std::uint16_t i) const noexcept {
        return (i < d_.callingConventions.size()) ? &d_.callingConventions[i] : nullptr;
    }

    [[nodiscard]] TargetCallingConvention const* callingConventionByName(
            std::string_view name) const noexcept {
        auto it = d_.callingConventionIndex.find(name);
        if (it == d_.callingConventionIndex.end()) return nullptr;
        return &d_.callingConventions[it->second];
    }

    // ── Cond-code encoding (D-CSUBSET-WHILE-LOOP-SUBSTRATE) ──────
    // Returns the target's numeric encoding for `cond`, or `nullopt`
    // when this target hasn't declared a `condCodeEncoding` table OR
    // hasn't declared THIS entry (the float arms are per-entry
    // optional — FC3.5 sweep-c2; an undeclared float cond means the
    // MIR→LIR FCmp lowering must use the two-setcc composition).
    // The encoder for cond-code-bearing opcodes (setcc / jcc on x86;
    // B.cc / CSET on ARM64) gates on this — a missing table/entry
    // fails loud (A_NoCondCodeEncoding) rather than silently OR'ing
    // zero into the opcode byte (which would map every condition to
    // `eq`). The bounds check guards a corrupt payload cast: an
    // out-of-enum payload reads as undeclared, never out-of-bounds.
    [[nodiscard]] std::optional<std::uint8_t> condCodeEncoding(
            TargetCondCode cond) const noexcept {
        if (!d_.condCodeEncodingLoaded) return std::nullopt;
        auto const idx = static_cast<std::size_t>(cond);
        if (idx >= d_.condCodeEncoding.size()) return std::nullopt;
        if (!d_.condCodeDeclared[idx]) return std::nullopt;
        return d_.condCodeEncoding[idx];
    }
    [[nodiscard]] bool condCodeEncodingLoaded() const noexcept {
        return d_.condCodeEncodingLoaded;
    }

    // ── Aggregate layout (FC6, D-FF3-1) ──────────────────────────
    // The per-ABI struct/union/array layout params the `type_layout` engine
    // reads. `aggregateLayoutLoaded()` is false for a target that never declared
    // the block (OPTIONAL at load; this accessor lets a consumer assert it and
    // fail loud BEFORE computing layout — the consumer-side fail-loud, no loader
    // requirement).
    [[nodiscard]] AggregateLayoutParams aggregateLayout() const noexcept {
        return d_.aggregateLayout;
    }
    [[nodiscard]] bool aggregateLayoutLoaded() const noexcept {
        return d_.aggregateLayoutLoaded;
    }

    // ── Relocations (AS1) ────────────────────────────────────────
    [[nodiscard]] std::span<TargetRelocationInfo const> relocations() const noexcept {
        return d_.relocations;
    }
    [[nodiscard]] std::size_t relocationCount() const noexcept { return d_.relocations.size(); }

    // Look up by opaque `kind` tag (the value the assembler stamps onto
    // `Relocation::kind`). Returns nullptr for an unknown kind so the
    // linker can fail loud at relocation-resolve time rather than
    // silently apply the wrong formula. O(1) via `relocationKindIndex`.
    [[nodiscard]] TargetRelocationInfo const* relocationInfo(RelocationKind kind) const noexcept {
        auto it = d_.relocationKindIndex.find(kind);
        if (it == d_.relocationKindIndex.end()) return nullptr;
        return &d_.relocations[it->second];
    }

    // Look up by canonical text name (the linker's *.format.json
    // cross-reference key per plan 14 §2.0). Heterogeneous lookup —
    // no allocation per call.
    [[nodiscard]] TargetRelocationInfo const* relocationByName(
            std::string_view name) const noexcept {
        auto it = d_.relocationNameIndex.find(name);
        if (it == d_.relocationNameIndex.end()) return nullptr;
        return &d_.relocations[it->second];
    }

    // ── Loaders ──────────────────────────────────────────────────
    // `sourceLabel` defaults to `"<inline>"` for parity with
    // `GrammarSchema::loadFromText`; callers parsing a file should pass
    // the path so the diagnostic carries it.
    static LoadResult<std::shared_ptr<TargetSchema>> loadFromFile(
        std::filesystem::path const& path);

    static LoadResult<std::shared_ptr<TargetSchema>> loadShipped(
        std::string_view name);

    static LoadResult<std::shared_ptr<TargetSchema>> loadFromText(
        std::string_view jsonText, std::string_view sourceLabel = "<inline>");

private:
    detail::TargetSchemaData d_;
};

} // namespace dss
