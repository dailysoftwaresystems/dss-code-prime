#pragma once

#include "core/export.hpp"
#include "core/substrate/transparent_string_hash.hpp"
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
};

inline constexpr EnumNameTable<TargetCondCode, 10> kTargetCondCodeTable{{{
    { TargetCondCode::Eq,  "eq"  },
    { TargetCondCode::Ne,  "ne"  },
    { TargetCondCode::Slt, "slt" },
    { TargetCondCode::Sle, "sle" },
    { TargetCondCode::Sgt, "sgt" },
    { TargetCondCode::Sge, "sge" },
    { TargetCondCode::Ult, "ult" },
    { TargetCondCode::Ule, "ule" },
    { TargetCondCode::Ugt, "ugt" },
    { TargetCondCode::Uge, "uge" },
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
};

inline constexpr EnumNameTable<OperandKindFilter, 3> kOperandKindFilterTable{{{
    { OperandKindFilter::Reg,       "reg"    },
    { OperandKindFilter::ImmInt,    "imm32"  },  // JSON-side width label
    { OperandKindFilter::SymbolRef, "symbol" },
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
    // Future fixed32 slots (paired with their consumer cycle):
    //   Imm12 bits 10..21  (12-bit immediate for ADD/SUB-imm forms)
    //   ImmShift / Sf-flag / etc.
};

inline constexpr EnumNameTable<EncodingSlotKind, 8> kEncodingSlotKindTable{{{
    { EncodingSlotKind::ModRmReg, "modrm.reg" },
    { EncodingSlotKind::ModRmRm,  "modrm.rm"  },
    { EncodingSlotKind::Imm32,    "imm32"     },
    { EncodingSlotKind::Rd,       "rd"        },
    { EncodingSlotKind::Rn,       "rn"        },
    { EncodingSlotKind::Rm,       "rm"        },
    { EncodingSlotKind::Disp32,   "disp32"    },
    { EncodingSlotKind::Imm26,    "imm26"     },
}}};

// Centralised count — promoted from per-translation-unit local
// constexpr per simplifier review. Used as the size of
// `std::array<bool, N>` slot-tracking buffers in both validate()
// and the fixed32 walker; keeps both sites in lockstep with the
// shared enum table.
inline constexpr std::size_t kEncodingSlotKindCount =
    kEncodingSlotKindTable.rows.size();

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
        case EncodingSlotKind::Disp32:
            return TargetEncodingShape::X86Variable;
        case EncodingSlotKind::Rd:
        case EncodingSlotKind::Rn:
        case EncodingSlotKind::Rm:
        case EncodingSlotKind::Imm26:
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
            return true;
        case EncodingSlotKind::ModRmReg:
        case EncodingSlotKind::ModRmRm:
        case EncodingSlotKind::Imm32:
        case EncodingSlotKind::Rd:
        case EncodingSlotKind::Rn:
        case EncodingSlotKind::Rm:
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
struct DSS_EXPORT TargetEncodingVariant {
    std::vector<OperandKindFilter>     operandKinds;
    TargetEncodingTemplate             tmpl;
    // Where the instruction's RESULT register goes (when the inst
    // has a result). Nullopt for value-less instructions (e.g.
    // `ret`). Most binary/unary register opcodes use ModRmReg here;
    // immediate-destination forms use ModRmRm with `modrmRegExt`
    // filling the reg field.
    std::optional<EncodingSlotKind>    resultSlot;
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
// it via `schema.relocationInfo(kind)` to resolve the formula. The
// `formula` is documentation for humans (and future debug tooling) —
// the substrate treats it as opaque text.
//
// `kind` slot-0 is reserved as an invalid sentinel: every declared
// row MUST carry a `kind != 0` (loader-enforced). Two rows with the
// same `kind` are also rejected.
struct DSS_EXPORT TargetRelocationInfo {
    std::string    name;            // canonical text key (e.g. "rel32", "abs64")
    RelocationKind kind{};          // opaque tag — written into Relocation::kind;
                                    // values flow ONLY from this field + the
                                    // schema's `relocationInfo`/`relocationByName`
                                    // accessors, never assembler-fabricated.
    std::string    formula;         // human-readable formula (e.g. "S + A - P - 4")
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

    // Calling conventions (cycle 2b). Same optional-for-now discipline
    // as `registers` — ML7 callconv lowering will require ≥1 entry.
    std::vector<TargetCallingConvention> callingConventions;
    substrate::TransparentStringMap<std::uint16_t> callingConventionIndex;

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
