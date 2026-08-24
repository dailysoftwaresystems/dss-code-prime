#pragma once

#include "core/export.hpp"
#include "core/substrate/transparent_string_hash.hpp"
#include "core/types/aggregate_layout.hpp"  // FC6: AggregateLayoutParams
#include "core/types/entry_shape.hpp"     // program-entry vocabulary (extracted; see its docblock)
#include "core/types/enum_name_table.hpp"  // EnumNameTable (extracted; breaks the leaf-enum cycle)
#include "core/types/grammar_schema.hpp"   // ConfigDiagnostic + LoadResult
#include "core/types/object_format_kind.hpp"  // ObjectFormatKind (charIsUnsigned's per-format axis)
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
// `EnumNameTable` now lives in `core/types/enum_name_table.hpp` (extracted so leaf
// enum headers can use it without the target_schema→grammar_schema include cycle);
// it is included above and re-exported here for the eight tables below.

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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kTargetAbiModelTable);

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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kCallConvTable);

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
// enum). Pattern mirrors `kMangleErrorTableRowsAligned` in
// `c_mangle.cpp`. Anchored
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
    // FC3.5 sweep-c2 (FCmp LIR lowering — D-COND-FLOAT-NAN-TRUTHINESS-FCMP
    // adjudication): FLOAT condition codes over the flags an FP
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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kTargetCondCodeTable);

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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kTargetResultRuleTable);

[[nodiscard]] constexpr std::string_view targetResultRuleName(TargetResultRule r) noexcept {
    return kTargetResultRuleTable.name(r);
}
// ⚠ THE `…FromName` HALF WAS MISSING, AND THE LOADER FILLED THE GAP ITSELF.
// `target_schema_json.cpp` carried a private `parseResultRule` if-chain
// spelling all three names a second time, beside the table that already owned
// them — the only opcode vocabulary in this header with a name side and no
// parse side, and the only one whose loader had to invent one. Every sibling
// enum here exposes BOTH halves off its table; this one now does too, so the
// loader has nothing left to duplicate.
[[nodiscard]] constexpr std::optional<TargetResultRule>
targetResultRuleFromName(std::string_view s) noexcept {
    return kTargetResultRuleTable.fromName(s);
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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kTargetRegClassTable);

[[nodiscard]] constexpr std::string_view targetRegClassName(TargetRegClass c) noexcept {
    return kTargetRegClassTable.name(c);
}
[[nodiscard]] constexpr std::optional<TargetRegClass>
targetRegClassFromName(std::string_view s) noexcept {
    return kTargetRegClassTable.fromName(s);
}

// ── the class set a `registerClassOps[]` ROW may name ─────────────────────
//
// `None` is a legitimate value of this enum — a register (or an operand) can
// genuinely have no class — so it is a table row and `targetRegClassFromName`
// resolves it. It is NOT a legitimate SUBJECT for a per-class operation row:
// `registerClassOps` declares the move/load/store mnemonics for a class, and
// the no-class sentinel has no registers to move.
//
// ⚠ THE LOADER'S DIAGNOSTIC ALREADY SAID SO AND THE CHECK DID NOT.
// `/registerClassOps/{}/class` refused nothing beyond a name lookup, so
// `"class": "none"` LOADED and took row 0, while the sentence beside it read
// "expected 'gpr' / 'fpr' / 'vr' / 'flags'". A message narrower than its check
// is the same defect class as a message wider than it — the sentence and the
// gate disagreed, and the sentence was the one telling the truth about intent.
// The gate now matches, and both come off this predicate.
[[nodiscard]] constexpr bool
isOperableTargetRegClass(TargetRegClass c) noexcept {
    return c != TargetRegClass::None;
}
// ⚠ The `4` is a LITERAL and the companion assert is the other half of the
// guard. D-CORE-NAMESWHERE-COUNT-DERIVED-FROM-THE-TABLE-IS-A-TAUTOLOGY: written
// as `rows.size() - 1` this would be `x == x`, because `namesWhere<M>` compares
// `M` against the rows this same table's predicate accepts. The literal reds on
// a new OPERABLE class; the assert reds on a SECOND inoperable one, which the
// literal alone cannot see. Both arms ✔MEASURED — the write-up is at
// `kSelectableExitMechanismNames` below.
inline constexpr auto kOperableTargetRegClassNames =
    namesWhere<4>(kTargetRegClassTable, isOperableTargetRegClass);
static_assert(kTargetRegClassTable.rows.size()
                  == kOperableTargetRegClassNames.size() + 1,
              "kTargetRegClassTable must have exactly ONE inoperable row (the "
              "'none' no-class value) — a second one leaves `namesWhere`'s "
              "literal count matching while `/registerClassOps/{}/class` "
              "silently stops naming the set its gate accepts");

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
        // F80 (D-CSUBSET-LONG-DOUBLE): FPR like every float — LOAD-BEARING for
        // the encoded-width wall (a GPR default would bypass the whole FPR
        // fail-loud tier and silently integer-plumb an x87 value).
        case TypeKind::F80:
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
    // ★ The psABI-assigned DWARF register number — a DIFFERENT PERMUTATION
    //   of the same register file than `hwEncoding`, and the reason this is
    //   a declared table rather than a derivation. On x86_64 SysV (psABI
    //   Fig 3.36) `rdx` is DWARF 1 but hardware 2, `rcx` is DWARF 2 but
    //   hardware 1, `rsi`/`rdi` are DWARF 4/5 but hardware 6/7, and
    //   `rbx`/`rsp`/`rbp` all move too. An unwinder handed the hardware
    //   number reads a table naming a DIFFERENT register, follows it into
    //   the wrong frame, and prints a plausible-looking backtrace — which
    //   is strictly worse than no table at all, because no table fails
    //   VISIBLY. (`D-UNWIND-NO-EH-FRAME-ANY-LANGUAGE-ON-ELF-OR-MACHO`.)
    //
    // EMPTY means "this register has no DWARF number", which is a real
    // state and not a config omission: DWARF numbers ARCHITECTURAL
    // registers, so the narrow views (`eax`, `w0`) inherit their parent's
    // identity rather than getting one of their own, and AArch64's `xzr`
    // has no DWARF number at all. A CFI rule naming such a register is
    // REFUSED by the `.eh_frame` writer, naming the register — never
    // silently given the parent's number, which would describe a save of
    // the full-width register that never happened.
    //
    // What IS required is checked in `validate()`: every register a
    // calling convention names as its stack pointer / frame pointer /
    // link register / callee-save must have one, because those are
    // exactly the registers a frame rule can be about.
    std::optional<std::uint16_t> dwarfNumber;
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

// D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH (LD-2): the six abstract wide-float
// (IEEE binary128 `long double`) operations a softfloat-libcall target
// realizes by CALLING a runtime helper (`__addtf3`/`__fixtfsi`/…) instead of
// an inline instruction sequence. The engine keys the softcall verb on
// `TypeKind::F128` + the PRESENCE of a config row for the op (never a
// target/format identity branch): a target that declares F128 but no softcall
// rows falls straight through to the `requireEncodedFloatWidth` fail-loud gate.
enum class WideFloatOp : std::uint8_t {
    Add = 0, Sub = 1, Mul = 2, Div = 3, ToInt32 = 4, FromFloat64 = 5,
};
inline constexpr std::size_t kWideFloatOpCount = 6;
inline constexpr EnumNameTable<WideFloatOp, 6> kWideFloatOpTable{{{
    { WideFloatOp::Add, "add" }, { WideFloatOp::Sub, "sub" },
    { WideFloatOp::Mul, "mul" }, { WideFloatOp::Div, "div" },
    { WideFloatOp::ToInt32, "to_i32" }, { WideFloatOp::FromFloat64, "from_f64" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kWideFloatOpTable);
[[nodiscard]] constexpr std::string_view wideFloatOpName(WideFloatOp op) noexcept {
    return kWideFloatOpTable.name(op);
}
[[nodiscard]] constexpr std::optional<WideFloatOp>
wideFloatOpFromName(std::string_view s) noexcept {
    return kWideFloatOpTable.fromName(s);
}

// One softcall row (the JSON `wideFloatSoftcalls[]` entry for a `WideFloatOp`).
// `helperSymbol` is the runtime library symbol the op lowers to a CALL of
// (`__addtf3` …); `argRegisterNames`/`resultRegisterName` are the physical
// register NAMES the operands/result are marshalled through (v0/v1 for the
// 128-bit args, x0 for a `to_i32` result, d0 for a `from_f64` source). The
// `*Ordinals` are validator-populated (each name resolved against the target's
// register table) so the lowering never re-resolves a name. `declared` is false
// on any op with no JSON row — the softcall accessor returns nullptr and the
// engine falls through to the encoded-width wall.
struct DSS_EXPORT WideFloatSoftcall {
    bool                       declared = false;
    std::string                helperSymbol;
    std::vector<std::string>   argRegisterNames;
    std::string                resultRegisterName;
    std::vector<std::uint16_t> argRegisterOrdinals;   // validator-populated
    std::uint16_t              resultRegisterOrdinal = 0;
};

// FC12a-core variadic CALLEE ABI (D-FC12A-VARIADIC-CALLEE): the layout of this
// CC's `__va_list_tag` PLUS the register-save-area geometry a `va_start`/`va_arg`
// walk needs. A target WITHOUT this block fails loud at the variadic-callee site
// (mirrors `variadicVectorCountReg.has_value()` / `indirectResultRegister.has_value()`
// — an un-built CC is never silently mis-walked). All facts are CONFIG; the per-CC
// algorithm (semantic `__va_list_tag` injection, HIR→MIR `va_arg` diamond, LIR
// prologue spill) reads them, never branching on cc.name / arch / format.
//
// SysV AMD64 (§3.5.7): `__va_list_tag` = `{u32 gp_offset; u32 fp_offset;
// void* overflow_arg_area; void* reg_save_area;}` (24B). The callee prologue of a
// variadic function spills the 6 integer arg regs (rdi..r9) + the al-gated 8 SSE
// arg regs (xmm0..xmm7) into a register-save-area; `va_arg` reads the next slot of
// the right class from there until the per-class offset hits its limit, then walks
// the overflow (incoming stack-arg) area.
//
// FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): the `VaListStrategy` closed enum that
// keys every va seam lives in `aggregate_layout.hpp` (included above) — the same
// link/target-substrate-free home as `AggregateClassKind`/`BitFieldStrategy`, so
// the SEMANTIC `va_list`-type injection can read the strategy without pulling this
// target/link substrate (the layering precedent `AggregateLayoutParams` set).
struct VaListLayout {
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): the lowering strategy — the FIRST field
    // so every consumer reads it before any strategy-specific field. Defaults to
    // SysVRegisterSave for back-compat (a `vaListLayout` block that omits "strategy"
    // is the pre-FC12b SysV shape).
    VaListStrategy strategy = VaListStrategy::SysVRegisterSave;

    // The stride one named/variadic arg slot occupies when walking the contiguous
    // home+overflow area (HomogeneousPointer) OR — on SysVRegisterSave — the GPR
    // slot stride (== gpSlotBytes; the overflow walk's per-slot quantum). Win64 = 8.
    // Read on BOTH arms; the SysV arm uses it as the stack-arg stride, the Win64 arm
    // as the uniform va_arg bump.
    std::uint32_t namedArgSlotBytes = 0;
    // One field of the `__va_list_tag` struct: its byte offset within the tag and
    // its width (the four SysV fields are gp_offset@0/w4, fp_offset@4/w4,
    // overflow_arg_area@8/w8, reg_save_area@16/w8). Offset/width are CONFIG so a CC
    // with a different tag shape (or field order) needs no substrate change.
    struct Field {
        std::uint32_t byteOffset = 0;
        std::uint32_t widthBytes = 0;
    };

    // NOTE: the `__va_list_tag` TOTAL size is NOT carried here — the sema-injected
    // builtin struct {u32,u32,void*,void*} sizes the `ap` local; these field offsets
    // + the save-area shape below are what codegen reads.
    Field gpOffsetField{};            // current GPR byte-cursor into the save area
    Field fpOffsetField{};            // current SSE byte-cursor into the save area
    Field overflowArgAreaField{};     // pointer to the next incoming STACK arg
    Field regSaveAreaField{};         // pointer to the spilled register-save-area

    // FC12c (D-FC12C-AAPCS64-VARIADIC-CALLEE): the AAPCS64 `__va_list` 5-field struct
    // (AAPCS64 §B.4). Read ONLY when strategy == Aapcs64DualCursor (the SysV fields
    // above are read only when SysVRegisterSave). The struct is
    // `{void* __stack; void* __gr_top; void* __vr_top; int __gr_offs; int __vr_offs;}`
    // (32B). `va_arg` runs a per-class dual-cursor walk: a NEGATIVE byte cursor
    // (`__gr_offs`/`__vr_offs`) counts UP toward 0 from the head of that class's save
    // block; while it is < 0 a register slot remains (read `<gr|vr>_top + cursor`,
    // bump the cursor by the slot stride); once it reaches 0 the arg is on the stack
    // (read `__stack`, bump by the GPR slot quantum). The save-area geometry reuses
    // the gpSaveCount/gpSlotBytes (GR: 8×8) + fpSaveCount/fpSlotBytes (VR: 8×16)
    // fields above; these five locate the cursor/top fields within the `ap` struct.
    Field stackField{};               // __stack: next incoming STACK arg (AAPCS64 §B.4)
    Field grTopField{};               // __gr_top: one past the GR save block
    Field vrTopField{};               // __vr_top: one past the VR save block
    Field grOffsField{};              // __gr_offs: NEGATIVE i32 GR cursor (→0)
    Field vrOffsField{};              // __vr_offs: NEGATIVE i32 VR cursor (→0)

    // Register-save-area geometry. The prologue spills `gpSaveCount` integer arg
    // regs at `gpSlotBytes` stride, then `fpSaveCount` SSE arg regs at `fpSlotBytes`
    // stride (the SSE block follows the GPR block). For SysV: 6×8 then 8×16 = 176B.
    // For AAPCS64 (Aapcs64DualCursor): GR 8×8 then VR 8×16 = 64 + 128 = 192B.
    std::uint32_t gpSaveCount = 0;    // integer arg regs spilled (SysV: 6; AAPCS64: 8)
    std::uint32_t gpSlotBytes = 0;    // bytes per integer save slot (SysV/AAPCS64: 8)
    std::uint32_t fpSaveCount = 0;    // SSE/VR arg regs spilled (SysV/AAPCS64: 8)
    std::uint32_t fpSlotBytes = 0;    // bytes per SSE/VR save slot (SysV/AAPCS64: 16)

    // `va_arg` reg-vs-overflow thresholds. gp_offset < gpOffsetLimit ⇒ read from
    // the save area (SysV: 48 = 6×8); fp_offset < fpOffsetLimit ⇒ likewise
    // (SysV: 176 = 48 + 8×16). These are CONFIG because they are a function of the
    // save-area geometry the ABI fixes, not anything the substrate may infer.
    // (Aapcs64DualCursor does NOT use these — its threshold is the NEGATIVE cursor
    // reaching 0, not a positive limit; left 0 there.)
    std::uint32_t gpOffsetLimit = 0;  // SysV: 48
    std::uint32_t fpOffsetLimit = 0;  // SysV: 176

    // FC12c (D-FC12C-APPLE-ARM64-VARIADIC-CALLEE): when true, a HomogeneousPointer
    // `va_start` anchors `ap` at the OVERFLOW (incoming-stack-arg) base
    // (`VaOverflowArgAreaAddr`) rather than the named-arg HOME base
    // (`VaHomeArgAreaAddr`). Apple arm64 has NO home area — its variadic args are
    // ALWAYS stacked (see `variadicArgsAlwaysStack` on the CC), so the first vararg
    // sits at the overflow base. Win64 (false, default) keeps the home base.
    bool variadicUsesOverflowBase = false;

    // Total register-save-area size the prologue reserves = gpSaveCount*gpSlotBytes
    // + fpSaveCount*fpSlotBytes (derived; the layout pass uses it to size the zone).
    [[nodiscard]] constexpr std::uint32_t regSaveAreaBytes() const noexcept {
        return gpSaveCount * gpSlotBytes + fpSaveCount * fpSlotBytes;
    }
};

// One calling convention. A target may declare multiple (SysV AMD64,
// Microsoft x64, fastcall, ...); the front-end picks one via attribute /
// driver flag. The `argGprs` / `argFprs` ordering is significant — the
// caller must place int args in those registers in that order, spilling
// to the stack when the register set is exhausted.
// (FC7 `AggregateClassKind` — the by-value classification strategy enum — lives
// in `aggregate_layout.hpp`, included above, so the lattice/lowering speak it
// without the link/target substrate.)
struct DSS_EXPORT TargetCallingConvention {
    std::string name;                     // "sysv_amd64" / "ms_x64" / ...
    std::vector<std::string> argGprs;     // arg-passing integer registers, in order
    std::vector<std::string> argFprs;     // arg-passing floating-point registers, in order
    std::vector<std::string> returnGprs;  // integer-return registers (rax/rdx on SysV; rax on MS)
    std::vector<std::string> returnFprs;  // float-return registers
    // D-CSUBSET-LONG-DOUBLE-AGGREGATE-ABI (LD-4): the 128-bit VR (Q-view)
    // arg-passing / return registers for an IEEE binary128 `long double`
    // (AAPCS64 v0..v7 args, v0..v3 return). Parallel to argFprs/returnFprs and
    // indexed by the SAME per-class (NSRN) ordinal — an F128 arg at NSRN k is
    // passed in `argVrs[k]` (v{k}), which ALIASES `argFprs[k]` (d{k}), so an
    // F64 and an F128 sharing a signature never collide by ordinal. EMPTY on
    // every f64/x87-80 target (x87 long double uses the implicit st0 stack; the
    // f64 axis never forms an F128), so this stays inert there. Validated
    // VR-class in `validate()` exactly as argFprs is validated FPR-class; the
    // MIR→LIR F128 boundary verb resolves each name→ordinal like argFprs.
    std::vector<std::string> argVrs;      // binary128 arg-passing VR registers, in order
    std::vector<std::string> returnVrs;   // binary128 VR-return registers
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

    // D-WIN64-LARGE-FRAME-STACK-PROBE: the OS stack guard-page size, in
    // bytes, AND the step granularity for the inline stack-probe loop.
    // A function whose total frame size EXCEEDS this value must touch
    // every guard-page-sized step on the way down (committing each page)
    // instead of doing a single bare `sub SP, F` that skips the guard
    // page (Windows reserves the stack lazily behind a single PAGE_GUARD
    // page; a `sub` that jumps over it access-violates on the first deep
    // write). The prologue reads this generically — NO arch/format/cc
    // identity branch:
    //   * `ms_x64` (Windows PE):  4096 — emit the probe loop for any
    //                             frame > 4096; the encoder lowers the
    //                             new `stack_probe` op to a page-walking
    //                             loop with THIS value as the step.
    //   * `sysv_amd64` (Linux ELF / Mach-O): 0 — Linux/macOS auto-grow
    //                             the stack (the kernel faults in deeper
    //                             pages on demand), so no probe is needed.
    //   * the arm64 CCs: 0 — large arm64 frames are handled by the
    //                             shifted-imm12 `sub sp` encoding, and
    //                             those OSes auto-grow the stack too.
    // 0 (the default) ⇒ NO probing: the prologue keeps the plain
    // `sub SP, F` for every frame (byte-identical to before this field).
    //
    // Validators (target_schema.cpp::validate): when nonzero it MUST be a
    // power of two (mirrors the stackAlignment check) — a typo'd 4000
    // would silently skip a guard page and reintroduce the crash.
    std::uint16_t stackProbePageBytes = 0;

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

    // FC12c (D-FC12C-APPLE-ARM64-VARIADIC-CALLEE): when true, EVERY variadic argument
    // (an operand past `call_payload::fixedOperandCount`) of a variadic call is forced
    // onto the stack overflow area regardless of available arg registers — Apple
    // arm64's documented divergence from AAPCS64 (variadic args are ALWAYS stacked).
    // NAMED args (operand index < fixedOperandCount) stay register-placed. Gated in
    // the caller arg-placement loop on this flag + the call's isVariadic bit, so
    // AAPCS64 (false) and the x86 CCs (false, default) are unaffected — they keep the
    // register-then-stack placement. This realizes the variadic half of
    // D-FF3-APPLE-ARM64-ABI-DIVERGENCE.
    bool variadicArgsAlwaysStack = false;

    // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: what happens to the
    // ARG REGISTERS of a class once a by-value aggregate is placed WHOLLY on the
    // stack because its pieces did not all fit (the all-or-nothing/straddle case).
    // The two shipped ABIs DIVERGE — a documented, config-distinguishable fact,
    // verified against gcc's own arg-advance logic (NOT an identity branch):
    //   * false (SysV `sysv_amd64`): BACKFILL. The leftover registers stay
    //     available for a LATER (smaller) arg — gcc `function_arg_advance_64`
    //     leaves `cum->nregs`/`regno` untouched on the stack branch; the SysV ABI
    //     "if registers were assigned for some eightbytes … the assignments get
    //     reverted". The per-class cursor is NOT advanced on route-to-stack.
    //   * true  (AAPCS64 `aapcs64`):  EXHAUST. The OVERFLOWED class is marked
    //     full (NGRN/NSRN ← 8) so every subsequent arg of that class also goes to
    //     memory — gcc `aarch64_layout_arg` sets `aapcs_nextncrn = NUM_ARG_REGS`
    //     (or `nextnvrn` for an HFA). The cursor is CLAMPED to the pool size.
    // Win64 (`ms_x64`, slotAligned) never straddles (1 struct = 1 positional
    // slot) so the flag is inert (default false). Consumed by HIR→MIR's caller
    // (Phase A) + callee (Phase B) cursor handling — kept in lockstep so the two
    // sides agree AND va_start's `__gr_offs`/`__vr_offs` clamp reflects it.
    bool aggregateStackExhaustsRegisters = false;

    // FC7 by-value aggregate ABI (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): the
    // classification STRATEGY for a struct/union passed/returned by value.
    // A closed enum (the `aggregate_abi` classifier switches on it, never on
    // identity). `None` (default) ⇒ this CC fails loud on a by-value
    // aggregate (un-built). x86_64 SysV = SysVEightbyte (C1); Win64 / AAPCS64
    // flip on in C2 / C3.
    AggregateClassKind aggregateClassification = AggregateClassKind::None;

    // The max aggregate SIZE (bytes) this CC passes/returns in registers;
    // larger ⇒ MEMORY (by-reference args / sret returns). SysV = 16 (two
    // eightbytes); Win64 = 8; AAPCS64 = 16. 0 ⇒ unused (strategy = None).
    // Consumed only by the `aggregate_abi` classifier.
    std::uint16_t aggregateMaxRegBytes = 0;

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

    // D-CSUBSET-VLA (C1b): frame-pointer register (x86_64 rbp / arm64 x29).
    // ENGAGED only for a target that supports variable-length-array (dynamic-
    // stack) codegen. When a function contains a VLA, the callconv pass reserves
    // this register (excluded from the regalloc pool + force-saved in the prologue)
    // as a stable frame BASE captured at the fixed-frame bottom, so every fixed-
    // frame reference (spills, fixed locals, incoming stack args) stays addressable
    // after the runtime `sub sp,<vlaSize>` moves SP. EMPTY ⇒ the target declares no
    // frame-pointer role; a VLA then stays fail-loud (L_VlaDynamicAllocaUnsupported)
    // rather than silently miscompiling. A NON-VLA function never consults it — its
    // frame stays SP-relative + byte-identical (the zero-blast-radius invariant).
    std::optional<NamedRegisterRef> framePointer;

    // D-LANG-VARIADIC (step 13.4, 2026-06-02): the register the caller
    // MUST load with the count of vector (FPR) arguments passed in
    // vector registers BEFORE the call instruction of any C-style
    // variadic function. SysV AMD64 (§3.2.3): `al = number of XMM
    // arguments used by varargs (0..8)`. Win64 ms_x64 has no
    // equivalent (the loader-side ABI uses GPR-shadow + double-spill
    // — anchored D-ML7-VARIADIC-WIN64-DOUBLE-SPILL — and this field
    // is left empty). AAPCS64 (ARM64-ELF): no equivalent — variadic FP
    // args pass in v0..v7 and ARE spilled to the VR save area (the dual-
    // cursor walk reads them via __vr_offs); there is simply no SysV-style
    // al count register. (APPLE arm64 is the ABI where variadic args —
    // int AND fp — are ALWAYS stacked; that is `variadicArgsAlwaysStack`,
    // NOT this field.) Both arm64 CCs leave this empty for the same
    // reason: no caller-side vector-COUNT register exists on AArch64.
    // Empty optional ⇒ this CC requires no caller-side vector-count
    // register for variadic calls. When engaged, ML7 materialize for a Call with
    // payload `isVariadic=true` counts FPR-class arg operands in
    // [fixedOperandCount..N) and emits a `mov <reg>, <count>` before
    // the call instruction.
    std::optional<NamedRegisterRef> variadicVectorCountReg;

    // FC7 by-value aggregate ABI (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): the
    // register that carries a struct-return HIDDEN POINTER (sret) when
    // ENGAGED — AAPCS64/Apple's x8 indirect-result-location register, which
    // does NOT consume a normal arg slot (x0..x7 stay free for real args).
    // ABSENT (SysV / Win64) ⇒ the sret pointer is a HIDDEN FIRST INTEGER ARG
    // (consuming argGprs[0]) returned in returnGprs[0]. This one field selects
    // between the two sret mechanisms agnostically — no per-CC branch.
    std::optional<NamedRegisterRef> indirectResultRegister;

    // FC12a-core (D-FC12A-VARIADIC-CALLEE): the `__va_list_tag` layout + register-
    // save-area geometry for `va_start`/`va_arg`/`va_end` in a variadic CALLEE.
    // ENGAGED today for EVERY shipped calling convention, each via its own
    // `VaListLayout::strategy`: sysv_amd64 = SysVRegisterSave (the __va_list_tag
    // register-save-area this field was originally named for); ms_x64 + apple_arm64
    // = HomogeneousPointer (FC12b / FC12c — `va_list` is a plain pointer into a
    // contiguous arg area); aapcs64 = Aapcs64DualCursor (FC12c — the dual gr/vr-
    // offset `__va_list` struct). ABSENT ⇒ the target/CC declares NO variadic-
    // callee model at all; a variadic function body using va_start then fails
    // LOUD ("variadic callee unsupported for this CC"), never silently mis-walked.
    // One field selects support — and which of the three lowering strategies —
    // fully agnostically, never a per-CC-name branch.
    std::optional<VaListLayout> vaListLayout;
};

// ── THE ALLOCATABLE-POOL LISTS — ONE OWNER FOR "WHICH OF A CALLING
//    CONVENTION'S REGISTER LISTS MAKE A REGISTER ALLOCATABLE" ─────────────
//
// D-TARGET-ALLOCATABLE-POOL-LIST-SET-HAS-NO-OWNER.
//
// The register-allocator's free lists and the rewriter's spill-reload scratch
// pool are BOTH "the register table INTERSECTED with some of this calling
// convention's name lists". Which lists is one fact, and until this table it
// had TWO hand-kept owners — `lir_regalloc::buildFreeLists` and
// `lir_rewrite::collectAllocatable` — each spelling out the same six
// `absorb(...)` calls. ⚠ Two copies of a set is not a redundancy that shows
// up as a build break if they drift: an allocator that thinks a register is
// reserved while the rewriter thinks it is free scratch (or the reverse) is a
// SILENT wrong-register answer, and nothing compares them.
//
// ★ WHY IT LIVES IN `core/` RATHER THAN BESIDE ITS TWO LIR CONSUMERS. It is
// read by `TargetSchemaData::validate()` — the load-time judge — which is one
// tier BELOW LIR and cannot include it. That placement is not a compromise: it
// is what makes the aliased-view rule in `validate()` see the same set the
// engine will absorb, so ADDING A LIST HERE is immediately judged against
// every shipped target rather than silently changing what the allocator hands
// out. Removing `argVrs` from this table is not an option a future cycle has
// to remember — it is a list that was never in it, and putting it in is the
// edit `validate()` refuses by name (see the aliased-view block there).
//
// ⚠ THE ARG/RETURN POOLS ARE HERE FOR A DIFFERENT REASON THAN `callerSaved`.
// `callerSaved`/`calleeSaved` DECLARE allocability; the arg/return pools are
// ABI PLACEMENT and appear here only because a register the ABI can place a
// value in must also be one the allocator may hand out. On both shipped
// targets they are subsets of `callerSaved`, so this table currently adds
// nothing beyond it — which is exactly why an inconsistency here would be
// invisible without a rule that judges the UNION.
using TargetCcRegisterList = std::vector<std::string> TargetCallingConvention::*;
inline constexpr std::array<TargetCcRegisterList, 6> kAllocatablePoolLists{{
    &TargetCallingConvention::callerSaved,
    &TargetCallingConvention::calleeSaved,
    &TargetCallingConvention::argGprs,
    &TargetCallingConvention::argFprs,
    &TargetCallingConvention::returnGprs,
    &TargetCallingConvention::returnFprs,
}};
// The JSON key each entry above is spelled with, in the SAME order — used by
// the load-time diagnostic to name the list a register was found in. Kept
// adjacent so a new entry that forgets its name fails the `static_assert`
// below rather than reporting a register as belonging to the wrong list.
inline constexpr std::array<std::string_view, 6> kAllocatablePoolListNames{{
    "callerSaved", "calleeSaved", "argGprs", "argFprs", "returnGprs",
    "returnFprs",
}};
static_assert(kAllocatablePoolLists.size() == kAllocatablePoolListNames.size(),
              "every allocatable-pool list must carry the JSON key it is "
              "spelled with — a nameless entry would be reported as another "
              "list's");

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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kTargetEncodingShapeTable);

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
    BlockRef  = 5,  // `LirOperand{kind == BlockRef}` —
                    // D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1): refers
                    // to an INTRA-FUNCTION basic block. Wired to the
                    // `BlockRel32` slot on x86 (4-byte trailing PC-
                    // relative displacement, resolved at assemble time
                    // via `walker_util::BlockRelPatch`). ARM64 will
                    // use Imm19/Imm26 with different patch arithmetic
                    // (anchored D-AS3-BLOCK-REL-IMM19/26).
    LiteralIndex = 6, // `LirOperand{kind == LiteralIndex}` — the wide-
                    // literal pool index (D-CSUBSET-BITFIELD-WIDE-UNIT).
                    // The pre-FC8 walkers never matched a `LiteralIndex`
                    // operand (the inline `ImmInt` arm carried every
                    // immediate that fit imm32, and float literals are
                    // promoted to rodata at HIR→MIR, never reaching
                    // MIR→LIR as a Const). A 64-bit INTEGER constant
                    // wider than imm32 cannot ride the 8-byte `LirOperand`
                    // POD inline, so it flows through `LirLiteralPool`;
                    // this filter lets the `mov r64, imm64` variant guard
                    // on "the operand is a wide pool literal", and the
                    // x86 walker fetches the full 64-bit value from the
                    // pool to emit the 8-byte immediate. JSON-side name
                    // `"imm64"` (the width label, mirroring `"imm32"`'s
                    // historical naming for `ImmInt`).
};

inline constexpr EnumNameTable<OperandKindFilter, 7> kOperandKindFilterTable{{{
    { OperandKindFilter::Reg,       "reg"      },
    { OperandKindFilter::ImmInt,    "imm32"    },  // JSON-side width label
    { OperandKindFilter::SymbolRef, "symbol"   },
    { OperandKindFilter::MemBase,   "membase"  },
    { OperandKindFilter::MemOffset, "memoffset"},
    { OperandKindFilter::BlockRef,  "blockref" },
    { OperandKindFilter::LiteralIndex, "imm64" },  // JSON-side width label
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kOperandKindFilterTable);

[[nodiscard]] constexpr std::string_view
operandKindFilterName(OperandKindFilter f) noexcept {
    return kOperandKindFilterTable.name(f);
}
[[nodiscard]] constexpr std::optional<OperandKindFilter>
operandKindFilterFromName(std::string_view s) noexcept {
    return kOperandKindFilterTable.fromName(s);
}

// ── GNU inline-asm constraint letters (the `asmConstraints` facet) ────
//
// A GNU `asm` statement binds every operand with a CONSTRAINT — `"=r"(o)`,
// `"a"(x)`, `"m"(*p)`, `"i"(7)`. A constraint splits cleanly in two, and the
// split is the entire reason this facet exists:
//
//   * the MODIFIERS (`=` output, `+` in-out, `&` earlyclobber, `%`
//     commutative) are GNU-asm GRAMMAR. They mean the same thing on every
//     processor, the C front end parses them, and they NEVER appear here.
//   * the LETTER is a MACHINE FACT. ✔MEASURED (gcc 13.3.0, `-O2 -S`, with
//     three competing `"r"` operands live so a lucky allocation cannot be
//     mistaken for a pin): `"=a"` lands in `%rax` on x86_64 and is rejected
//     outright on AArch64 ("impossible constraint in 'asm'"). No reading of
//     the C standard yields that; only the target can say it.
//
// ★★★ WHICH LETTER MEANS WHICH REGISTER IS TARGET VOCABULARY AND LIVES IN
// `.target.json`. A C++ table keyed on architecture would be an agnosticism
// break that NO GREP CATCHES until the second architecture's inline asm
// arrives — by which time the table is load-bearing and the break is
// expensive to undo.
//
// ⚠ THIS FACET IS DELIBERATELY NARROWER THAN THE ONE THAT WAS REVERTED.
// [[D-CONFIG-ASM-DIALECT-DECLARED-AS-TARGET-VOCABULARY]] killed an
// `asmSyntax` block on these same two files that carried `registerPrefix`,
// `immediatePrefix`, comment characters and operand ORDER. ✔MEASURED with
// gcc on ONE target: AT&T `movq %rsi, (%rdi)` vs Intel `mov QWORD PTR
// [rdi], rsi` — every one of those differs between two dialects of the SAME
// CPU, so each is a (target, DIALECT) fact and storing it per-target stored
// a per-(X,Y) fact per-X. Constraint letters face the identical test and
// SURVIVE it: `"=a"` means RAX under AT&T and under Intel, in ELF and in PE.
// The letter names a REGISTER, and a register is a property of a processor.
// ⇒ NOTHING THAT VARIES WITH DIALECT MAY ENTER THIS FACET — no sigils, no
// operand order, no mnemonic spellings. The loader enforces the one case a
// human would actually get wrong (a modifier smuggled into a letter).
//
// ★★ THE LETTERS BIND TO VOCABULARY THAT ALREADY EXISTS — there is no fourth
// axis and no `AsmConstraint*` verb set:
//   * `r`, `x`, `w` → a register CLASS    (`TargetRegClass`)
//   * `a`, `d`, `S` → a SPECIFIC register (`registers[].name` → ordinal, the
//                     same name resolution the `implicitRegisters` roles use)
//   * `i`, `m`      → an operand FORM     (`OperandKindFilter`)
enum class AsmConstraintBinding : std::uint8_t {
    RegisterClass = 0,
    Register      = 1,
    OperandKind   = 2,
};

// ★ THE DISCRIMINATOR SPELLING IS ALSO THE PAYLOAD KEY NAME, and that is
// load-bearing rather than cute: `"binds": "register"` requires the key
// `"register"`. ONE table therefore drives the discriminator vocabulary, the
// key the loader demands, AND the list every diagnostic renders — so they
// cannot drift apart the way a hand-typed valid-value list does (the failure
// `kKnownImplicitRegisterRoles` records in its own comment, where the prose
// told readers a valid role was invalid).
inline constexpr EnumNameTable<AsmConstraintBinding, 3>
    kAsmConstraintBindingTable{{{
        { AsmConstraintBinding::RegisterClass, "registerClass" },
        { AsmConstraintBinding::Register,      "register"      },
        { AsmConstraintBinding::OperandKind,   "operandKind"   },
    }}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kAsmConstraintBindingTable);

[[nodiscard]] constexpr std::string_view
asmConstraintBindingName(AsmConstraintBinding b) noexcept {
    return kAsmConstraintBindingTable.name(b);
}
[[nodiscard]] constexpr std::optional<AsmConstraintBinding>
asmConstraintBindingFromName(std::string_view s) noexcept {
    return kAsmConstraintBindingTable.fromName(s);
}

// One declared constraint letter. EXACTLY ONE payload is engaged and it is
// the one `binds` names — the loader enforces both halves, so a consumer may
// `switch (binds)` and dereference the matching arm without re-checking.
//
// ★ THE PAYLOADS ARE `optional` RATHER THAN PLAIN VALUES ON PURPOSE. Ordinal
// 0 is a perfectly valid register (`rax` on x86_64, `x0` on arm64) and
// `OperandKindFilter::Reg` is 0 as well, so a zero-initialized arm would read
// back as a PLAUSIBLE answer to a consumer that forgot to switch on `binds` —
// the silent-wrong-answer shape, not the loud one. `nullopt` is the only
// value that cannot be mistaken for a measurement.
struct DSS_EXPORT TargetAsmConstraint {
    // The letter exactly as it appears in the constraint string, WITHOUT
    // modifiers. A string rather than a `char` because gcc machine
    // constraints are not all one character (aarch64 `Ush`, x86 `Yz`), and
    // because the natural growth direction is longer spellings, not wider
    // chars. CASE-SENSITIVE and that is load-bearing: ✔MEASURED, x86_64 `d`
    // is `%rdx` and `D` is `%rdi` — two different registers.
    std::string letter;
    AsmConstraintBinding binds = AsmConstraintBinding::RegisterClass;
    std::optional<TargetRegClass>    registerClass;    // binds == RegisterClass
    std::optional<std::uint16_t>     registerOrdinal;  // binds == Register
    std::optional<OperandKindFilter> operandKind;      // binds == OperandKind
};

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
    // D-AS4-ARM64-BASE-INDEX-LEA: a MemOffset MARKER — a width-0 {0,0}
    // slot, the displacement twin of `MemBaseNoScale`. It asserts the
    // wired MemOffset operand's displacement is ZERO and writes NO bits.
    // Reason: the AArch64 base+index `lea` is `ADD Xd, Xn, Xm` (shifted-
    // register, NO displacement field), so the GEP's vestigial
    // MemOffset(0) (a SIB artifact x86's 4-op lea needs for disp32) must
    // be CONSUMED + validated zero on arm64 — wiring it to Imm12 would
    // corrupt Rm (bits 10..21 overlap Rm at 16..20). A nonzero disp with
    // an index never arises (the 3-op frame `lea` owns base+disp), so
    // this is a pure fail-loud guard. Universal (any ISA whose indexed
    // address-add has no disp field reuses it).
    MemOffsetZero = 23,
    // D-CSUBSET-BITFIELD-WIDE-UNIT (v0.0.2 FC8): the x86 "opcode + rd"
    // destination form — the result register's low 3 bits are OR'd into
    // the LAST opcode byte (`B8+rd`), and its high bit drives REX.B. NO
    // ModR/M byte. The ONLY x86-64 instruction that materializes a full
    // 64-bit immediate into a register is `mov r64, imm64` (REX.W B8+rd
    // io); its destination lives in the opcode byte, not in ModR/M, so a
    // new destination-bearing slot is required. The walker treats this as
    // the variant's `resultSlot` — it is destination-bearing (rule G).
    // Generic over any ISA whose register field rides the opcode byte
    // (x86 `push r64` 50+rd, `xchg eax, r32` 90+rd reuse it). Writes no
    // bytes itself; it modifies the opcode byte + REX.B during emission.
    OpcodePlusReg = 24,
    // D-CSUBSET-BITFIELD-WIDE-UNIT (v0.0.2 FC8): 8 immediate bytes
    // appended after the opcode (and ModR/M/SIB, when present), little-
    // endian — the `io` field of `mov r64, imm64`. The FIRST 64-bit
    // immediate slot. The wired operand is a `LiteralIndex` (the wide
    // value lives in `LirLiteralPool`, since it does not fit the inline
    // `ImmInt` 8-byte POD); the walker reads the pool value and emits 8
    // LE bytes. Range-unchecked by construction — every 64-bit pattern
    // is representable. Distinct from `Imm32` (4 bytes, sign-extended
    // consumers) and `Imm8` (1 byte, shift counts).
    Imm64         = 25,
    // D-ASM-AARCH64-LARGE-FRAME-IMM12 (v0.0.2 cycle ⑤): the AArch64
    // SCALED unsigned 12-bit displacement of the unsigned-offset LDR/STR
    // form (`LDR/STR Xt, [Xn, #pimm]`), bits 10..21 — the SAME bit-window
    // as `Imm12`, but with DISTINCT encode semantics, hence a distinct
    // slot (NOT a reuse of Imm12 + a flag — §B.2 FORCED). Whereas Imm12
    // (ADD/SUB-immediate) writes the RAW byte value, this slot writes the
    // SCALED field `imm12 = byteOffset / accessSizeBytes` (accessSizeBytes
    // = the access width in bytes: 8 for a 64-bit LDR, 4 for a 32-bit, 2
    // for 16-bit, 1 for 8-bit). The walker (fixed32.cpp MemOffset arm)
    // derives accessSizeBytes from the inst's operation width, validates
    // the byte offset is NON-NEGATIVE and ACCESS-SIZE-ALIGNED and the
    // scaled field fits 12 bits (0..4095), then writes the scaled value
    // (fail-loud A_ImmediateOperandOutOfRange on any of the three). This
    // gives a frame reach of 4095*8 = 32760 bytes for 64-bit loads — the
    // form a ≥9-fixed-param AAPCS64 callee needs to load its 9th
    // (incoming-stack) param at `[sp + frameSize]` when frameSize exceeds
    // the unscaled imm9 ±256. DECODE (disasm) extracts the RAW 12-bit
    // field — the round-trip oracle pins the scaled value (e.g. 24 for a
    // 64-bit `[sp,#192]`), NOT the byte offset. A frame offset that is
    // negative-and-out-of-imm9, OR aligned-but >32760, OR
    // non-aligned-and-out-of-imm9 stays fail-loud (anchored
    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12; the shifted imm12<<12 LDR
    // form / scratch-register address materialization is the future
    // generalization). The LOWERING (lir_callconv.cpp) picks the mnemonic
    // (load/store vs load_u/store_u) from the offset value — the variant
    // selector matches operand KINDS only and cannot inspect the value.
    Imm12Scaled   = 26,
    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12 (v0.0.2 FC12 deferral-2):
    // the AArch64 ADD/SUB-immediate `imm12 LSL #12` shifted-immediate
    // form, the WORD-PAIR encoding of a value V in (4095, 0xFFFFFF]
    // (4096 .. 16 MiB-1). This is NOT a single bit-window — it is the
    // SAME bits 10..21 window as `Imm12`, but the encoder, on matching
    // this slot, writes BOTH words of a 2-word `add`/`sub`/`lea` macro:
    //   word0 = `op Xd,Xn,#(V & 0xFFF)`          (sh=0, base word)
    //   word1 = `op Xd,Xd,#((V>>12) & 0xFFF)`    (sh=1, base|0x400000)
    // (the wire's slot lives in word0; the macro's word1 carries the
    // high 12 bits — its fixedWords[1] sets sh=1, and its extraResult
    // Slots thread Xd through word1's Rd+Rn so the second ADD reads its
    // OWN dest as the source base — SCRATCH-FREE). A function with a
    // frame > 4095 bytes (e.g. `int big[9000]` = 36000B) needs this for
    // the prologue/epilogue `sub/add sp,#frame` AND the GEP `lea
    // [base,#disp]`. Reaches 16 MiB (every realistic frame); a value
    // > 0xFFFFFF stays fail-loud (A_ImmediateOperandOutOfRange — the
    // residual D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB; a third word /
    // MOVZ+MOVK scratch materialization is its future generalization).
    // The encoder derives the split arithmetically (lo = V & 0xFFF, hi
    // = (V>>12) & 0xFFF) and writes lo into word0's window + hi into
    // word1's window — both via the same `imm12` bit-window (the slot
    // is its OWN window twin of Imm12, bits 10..21). x86_64 has no
    // imm12 slot (it uses Imm32/Disp32), so this slot is never reached
    // on an x86 variant — the gating is slot-kind `==` + value-
    // magnitude arithmetic, zero arch identity.
    Imm12HiLo24   = 27,
    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB (v0.0.2 cycle 12): the
    // AArch64 MOVZ/MOVK + EXTENDED-register `add`/`sub`/`lea` THREE-word
    // materialization of a value V in (0xFFFFFF, 0x7FFFFFFF] — a frame
    // LARGER than the 24-bit shifted-imm12 reach (16 MiB) but representable
    // in a non-negative int32 (a frame size flows through `int32_t` →
    // `> 0x7FFFFFFF` goes NEGATIVE and never matches this slot's
    // `immMin:16777216` variant guard → fail-loud, the residual
    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-2GIB). Like `Imm12HiLo24` this is
    // NOT a single bit-window — the encoder, on matching this slot, writes
    // a 3-word macro whose FIRST TWO words are `MOVZ Xs,#(V & 0xFFFF)` +
    // `MOVK Xs,#((V>>16) & 0xFFFF),LSL #16` (materialize V into a scratch
    // register Xs) and whose THIRD word is the operation:
    //   * sp adjust  — `sub sp,sp,x16` / `add sp,sp,x16` (the EXTENDED-
    //     register form 0xCB30_63FF / 0x8B30_63FF, where Rn=Rd=sp(31) is
    //     SP not XZR — the shifted-register form would write XZR). The
    //     scratch is x16 = AAPCS64 IP0, the architecturally-blessed intra-
    //     procedure scratch, BAKED into the MOVZ/MOVK base words (Rd=16)
    //     and the extended op's Rm=16. Free at the prologue (pre-arg-home)
    //     + epilogue (post-return-value).
    //   * lea        — `add Xd,sp,Xd` (extended, Rn=sp), SCRATCH-FREE: V
    //     materializes into the lea's DEST reg Xd (the MOVZ/MOVK Rd + the
    //     extended op's Rd AND Rm all thread the result register). The
    //     value writes into both MOVZ/MOVK words' imm16 windows (bits 5..20)
    //     — the SAME window as `Imm16`, the encoder splits lo16→word0 /
    //     hi16→word1 (mirroring `materializeViaMovkLadder`'s chunk split).
    // The slot's `windowFor` returns the imm16 window (bits 5..20); the
    // encoder calls `orInto` twice (lo→word0, hi→word1). x86_64 has no
    // imm16/movk-ladder slot → never reached on an x86 variant (gating is
    // slot-kind `==`, zero arch identity). The x16 scratch identity is
    // justified the same way XZR=31/sp=31 already are (a config-baked
    // architectural register).
    Imm32MovzMovk = 28,
    // TLS C1 (D-CSUBSET-THREAD-LOCAL): the x86 ABSOLUTE-SIB memory form —
    // ModR/M(mod=00, rm=100 "SIB follows") + SIB 0x25 (index=100 no-index,
    // base=101 "disp32-only, no base register") + a 4-byte LITERAL disp32
    // sourced from the wired MemOffset operand. This is the 64-bit-mode
    // encoding of `[seg:disp32]` absolute addressing — the ONLY way to
    // address a flat 32-bit displacement without a base register (mod=00
    // rm=101 alone means [rip+disp32] in 64-bit mode; the SIB base=101
    // no-index form restores the absolute meaning). First consumer: the
    // `tlsbase` opcode's `mov r64, fs:[0]` thread-pointer read (the
    // segment override rides the template's `payloadBytePrefix`; the
    // displacement value comes from the format config's
    // `tlsAccess.baseDisplacement` via the lowering's MemOffset operand).
    // Not symbol-bearing — the disp32 is a literal config value, never
    // relocated. Generic: any x86 absolute-addressed slot (a future
    // gs-based TEB read) reuses it.
    AbsoluteDisp32Mem = 29,
    // TLS C1 (D-CSUBSET-THREAD-LOCAL): the SYMBOL-BEARING memory
    // displacement — the disp32 of a `[base + disp32]` (mod=10) memory
    // operand emitted as a 4-byte RELOCATION PLACEHOLDER at the memory-
    // displacement position (right after ModR/M + SIB), NOT the trailing
    // step-8 position `Disp32` uses. Wired from a SymbolRef operand and
    // REQUIRES `relocationKind` (isSymbolBearingSlot). Pairs with a
    // `ModRmRmMem` base-register wire (the base supplies mod=10 + rm).
    // First consumer: the TLS local-exec `lea r, [tp + tpoff32(sym)]` —
    // the linker patches the 4 bytes with the symbol's link-time
    // thread-pointer offset (the `tls-tpoff32` row's Linear formula over
    // the walker's tpoff-poisoned symbolVa). Distinct from `Disp32Mem`
    // (a literal immediate displacement) and from `RipRelDisp32` (which
    // forces the RIP-relative ModR/M state); this slot leaves the
    // ModR/M state to the base wire and only owns the displacement
    // bytes + relocation. Generic: PE's C3 final
    // `lea rax, [slot + tlsOffset(sym)]` reuses it verbatim.
    MemRelocDisp32 = 30,
    // D-ASM-ARM64-NEGATIVE-IMMEDIATE-UNENCODABLE: an INVERTED 16-bit
    // immediate — the SAME bit-window as `Imm16` (bits 5..20), but the
    // encoder writes the operand's BITWISE COMPLEMENT `~V` (= `−V − 1`)
    // instead of `V`. Distinct slot, identical placement, different encode
    // arithmetic — exactly the `Imm12` / `Imm12Scaled` relationship one
    // level up.
    //
    // GENERIC BY CONSTRUCTION, and the name says the operation rather than
    // the instruction: "an ISA that materializes a negative constant by
    // storing the complement of its magnitude in an unsigned field". The
    // first consumer is AArch64 `MOVN Xd,#imm16` (which `gas` itself
    // ALIASES from `mov Xd,#-N` — ✔MEASURED with `aarch64-linux-gnu-as`
    // 2.42: `mov x1,#-8` and `movn x1,#7` both assemble to the IDENTICAL
    // word 0x928000E1), but nothing here is AArch64-shaped: any fixed-width
    // ISA with a complement-immediate form declares this slot.
    //
    // ENCODE CONTRACT (fixed32.cpp): the operand value MUST be strictly
    // NEGATIVE. `~V` for a non-negative V is itself negative and would be
    // MASKED into the window as a garbage constant — a silent miscompile —
    // so the encoder rejects a non-negative value LOUDLY rather than
    // relying on the variant guard's `negValue` sign-routing alone. The
    // representable range follows from the window WIDTH, never a baked
    // literal: V in [−2^width, −1].
    //
    // DECODE CONTRACT (fixed32_disasm.cpp): the mirror re-inverts, so the
    // round-trip oracle recovers the ORIGINAL negative LIR operand value
    // rather than the raw field.
    Imm16Inverted = 31,
    // D-ASM-X86-NO-16BIT-IMMEDIATE-SLOT: TWO immediate bytes appended after
    // the opcode (and ModR/M + SIB, when present), little-endian — the `iw`
    // field of the 16-bit immediate forms (`mov r/m16, imm16` = 66 C7 /0 iw,
    // `add/sub/and/or/xor/cmp r/m16, imm16` = 66 81 /N iw).
    //
    // ★★★ WHY IT IS NOT `Imm16`, WHICH IS THE OBVIOUS NAME AND IS ALREADY
    // TAKEN BY A DIFFERENT MACHINE FACT. `Imm16` is a 16-bit BIT-WINDOW at
    // bits 5..20 of a 32-bit fixed word (AArch64 MOVZ), so `slotShapeFor`
    // binds it to the `fixed32` shape and `validate()` rejects it under an
    // `x86-variable` opcode. This slot is the APPENDED-BYTES family — the
    // one `Imm8` (1 byte), `Imm32` (4 bytes) and `Imm64` (8 bytes) belong to
    // — and the width alone does not name it. Same relationship as
    // `Imm12` / `Imm12Scaled` / `Imm12HiLo24` one level up: identical
    // nominal width, different encode semantics, therefore distinct slots
    // with qualified names.
    //
    // ⚠ DECLARING THE VARIANT WITH THE EXISTING `imm32` SLOT INSTEAD WOULD
    // EMIT FOUR BYTES WHERE THE INSTRUCTION TAKES TWO — a corrupt
    // instruction stream with no diagnostic anywhere. That is the failure
    // this slot exists to make impossible, not merely a missing convenience.
    //
    // ENCODE CONTRACT (x86_variable.cpp `wireImm16`): the wired value must
    // fit the 2-byte field read either as SIGNED or as UNSIGNED —
    // [-32768, 65535] — because AT&T writes both `$-1` and `$65535` for the
    // same halfword. Anything outside fails loud
    // (`A_ImmediateOperandOutOfRange`); it never silently truncates.
    //
    // GENERIC BY CONSTRUCTION: any variable-length ISA with a 2-byte
    // trailing immediate wires this slot. The variant GUARD vocabulary is
    // unchanged — the operand KIND filter stays `"imm32"` (the
    // `LirOperandKind::ImmInt` discriminator, whose name is historical); the
    // SLOT decides the emitted width, exactly as `Imm8` already does.
    Imm16Bytes = 32,
    // Future fixed32 slots (paired with their consumer cycle):
    //   Sf-flag / etc.
};

inline constexpr EnumNameTable<EncodingSlotKind, 33> kEncodingSlotKindTable{{{
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
    { EncodingSlotKind::MemOffsetZero, "memoffset.zero" },
    { EncodingSlotKind::OpcodePlusReg, "opcode.reg"     },
    { EncodingSlotKind::Imm64,         "imm64"          },
    { EncodingSlotKind::Imm12Scaled,   "imm12.scaled"   },
    { EncodingSlotKind::Imm12HiLo24,   "imm12.hilo24"   },
    { EncodingSlotKind::Imm32MovzMovk, "imm32.movzmovk" },
    { EncodingSlotKind::AbsoluteDisp32Mem, "absdisp32.mem" },
    { EncodingSlotKind::MemRelocDisp32,    "memreloc.disp32" },
    { EncodingSlotKind::Imm16Inverted, "imm16.inverted" },
    { EncodingSlotKind::Imm16Bytes,   "imm16.bytes"    },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kEncodingSlotKindTable);

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
static_assert(kEncodingSlotKindCount == 33,
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
        // D-ASM-X86-NO-16BIT-IMMEDIATE-SLOT: the 2-byte APPENDED immediate is
        // an x86-variable construct (trailing bytes after ModR/M), unlike the
        // fixed32 `Imm16` bit-window it shares a nominal width with.
        case EncodingSlotKind::Imm16Bytes:
        case EncodingSlotKind::Disp32:
        case EncodingSlotKind::ModRmRmMem:
        case EncodingSlotKind::MemBaseScale:
        case EncodingSlotKind::Disp32Mem:
        case EncodingSlotKind::SibIndex:
        case EncodingSlotKind::RipRelDisp32:
        case EncodingSlotKind::CondCodeNibble:
        case EncodingSlotKind::BlockRel32:
        // D-CSUBSET-BITFIELD-WIDE-UNIT: `mov r64, imm64` (B8+rd io) is an
        // x86-variable form — opcode-byte register + 8-byte immediate.
        case EncodingSlotKind::OpcodePlusReg:
        case EncodingSlotKind::Imm64:
        // TLS C1 (D-CSUBSET-THREAD-LOCAL): the absolute-SIB literal
        // disp32 (`mov r64, seg:[disp32]`) and the relocated memory
        // displacement (`lea r, [base + tpoff32(sym)]`) are both
        // ModR/M-byte constructs — x86-variable only.
        case EncodingSlotKind::AbsoluteDisp32Mem:
        case EncodingSlotKind::MemRelocDisp32:
            return TargetEncodingShape::X86Variable;
        case EncodingSlotKind::Rd:
        case EncodingSlotKind::Rn:
        case EncodingSlotKind::Rm:
        case EncodingSlotKind::Ra:
        case EncodingSlotKind::Imm26:
        case EncodingSlotKind::Imm16:
        case EncodingSlotKind::Imm9:
        case EncodingSlotKind::MemBaseNoScale:
        case EncodingSlotKind::MemOffsetZero:
        case EncodingSlotKind::Imm12:
        // D-ASM-AARCH64-LARGE-FRAME-IMM12: the scaled imm12 LDR/STR
        // displacement is a fixed32 slot (HAZARD #6 — MUST be Fixed32 or
        // validate() rejects every `load_u`/`store_u` variant that wires
        // it as a cross-shape declaration on a fixed32 opcode).
        case EncodingSlotKind::Imm12Scaled:
        // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12: the shifted-imm12
        // word-pair form is a fixed32 (AArch64) slot — same reason as
        // Imm12 / Imm12Scaled (a cross-shape declaration on an x86
        // opcode would be rejected by validate()).
        case EncodingSlotKind::Imm12HiLo24:
        // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB: the MOVZ/MOVK +
        // extended-register 3-word form is a fixed32 (AArch64) slot —
        // same reason as Imm12 / Imm12HiLo24.
        case EncodingSlotKind::Imm32MovzMovk:
        case EncodingSlotKind::SymbolPatchMarker:
        case EncodingSlotKind::Imm19:
        // D-ASM-ARM64-NEGATIVE-IMMEDIATE-UNENCODABLE: the inverted-imm16
        // slot shares `Imm16`'s bit-window, so it is a fixed32 slot for the
        // same reason Imm16 is (an x86-variable opcode wiring it would be a
        // cross-shape declaration and validate() rejects it).
        case EncodingSlotKind::Imm16Inverted:
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

    // FC2 Part B: mandatory legacy-prefix bytes emitted BEFORE the REX
    // prefix (the x86 decode contract: a legacy prefix that
    // participates in opcode selection must precede REX, or it is not
    // part of the opcode selection). First consumers were the SSE
    // opcode-form selectors (F2/F3/66); the field is GENERIC over any
    // fixed template-declared legacy prefix — TLS C1's `tlsbase` pairs
    // it with `payloadBytePrefix` below (a per-INSTRUCTION prefix from
    // the LIR payload, emitted before even these bytes — x86 prefix
    // group 2 segment overrides precede everything). Empty = no prefix
    // (every pre-FC2 opcode). Only meaningful for the `x86-variable`
    // shape — validate() rejects it on a fixed32 variant (mirrors the
    // opcodeBytes / modrmRegExt fixed32 rejection).
    std::vector<std::uint8_t> mandatoryPrefix;

    // TLS C1 (D-CSUBSET-THREAD-LOCAL): when true, the instruction's
    // LIR `payload` LOW BYTE is emitted as the FIRST byte of the
    // instruction — before `mandatoryPrefix` and before REX (x86
    // prefix group 2, the segment-override group, precedes all other
    // prefixes per the SDM decode order). First consumer: `tlsbase`'s
    // segment-override byte (0x64 fs on ELF / 0x65 gs on PE), which
    // is PER-FORMAT config (`tlsAccess.segmentPrefixByte`) threaded
    // through the LOWERING's payload — keeping the shared x86_64
    // TARGET JSON free of any format-specific value. The encoder
    // FAILS LOUD when this is set but the instruction carries payload
    // low-byte 0 (a zero prefix byte is never a valid segment
    // override — catches a lowering that forgot to set the payload).
    // Only meaningful for the `x86-variable` shape — validate()
    // rejects it on a fixed32 variant (a fixed-word ISA has no prefix
    // bytes, like mandatoryPrefix).
    bool payloadBytePrefix = false;

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
        // TLS C1 (D-CSUBSET-THREAD-LOCAL): the relocated memory
        // displacement — a 4-byte placeholder at the memory-disp
        // position, patched by the linker per the wire's
        // relocationKind (tls-tpoff32 first). REQUIRES relocationKind.
        case EncodingSlotKind::MemRelocDisp32:
            return true;
        case EncodingSlotKind::ModRmReg:
        case EncodingSlotKind::ModRmRm:
        case EncodingSlotKind::Imm32:
        case EncodingSlotKind::Imm8:
        // D-ASM-X86-NO-16BIT-IMMEDIATE-SLOT: a literal 2-byte immediate,
        // never a linker-patched field — a 16-bit displacement reaches no
        // symbol on this ISA.
        case EncodingSlotKind::Imm16Bytes:
        // D-CSUBSET-BITFIELD-WIDE-UNIT: the `mov r64, imm64` slots write
        // the wide value / opcode-byte register directly — no relocation.
        case EncodingSlotKind::Imm64:
        case EncodingSlotKind::OpcodePlusReg:
        case EncodingSlotKind::Rd:
        case EncodingSlotKind::Rn:
        case EncodingSlotKind::Rm:
        case EncodingSlotKind::Ra:
        case EncodingSlotKind::Imm16:
        case EncodingSlotKind::Imm9:
        case EncodingSlotKind::MemBaseNoScale:
        case EncodingSlotKind::MemOffsetZero:
        case EncodingSlotKind::Imm12:
        // D-ASM-AARCH64-LARGE-FRAME-IMM12: the scaled imm12 displacement
        // writes its immediate field directly — no linker relocation.
        case EncodingSlotKind::Imm12Scaled:
        // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12: the shifted-imm12
        // word-pair writes its (split) immediate bits directly into both
        // words — no linker relocation.
        case EncodingSlotKind::Imm12HiLo24:
        // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB: the MOVZ/MOVK 3-word
        // form writes its (split) immediate halfwords directly into the
        // MOVZ/MOVK words — no linker relocation.
        case EncodingSlotKind::Imm32MovzMovk:
        case EncodingSlotKind::ModRmRmMem:
        case EncodingSlotKind::MemBaseScale:
        case EncodingSlotKind::Disp32Mem:
        // TLS C1 (D-CSUBSET-THREAD-LOCAL): the absolute-SIB disp32 is a
        // LITERAL config value (the tp slot's displacement), never a
        // relocated symbol reference — no relocationKind.
        case EncodingSlotKind::AbsoluteDisp32Mem:
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
        // D-ASM-ARM64-NEGATIVE-IMMEDIATE-UNENCODABLE: the inverted-imm16
        // slot writes the operand's COMPLEMENT into the field directly —
        // a literal value, never a relocated symbol reference (a symbol's
        // link-time address is not knowable at encode, so its complement
        // is not either; a symbol-bearing complement form would need the
        // LINKER to invert and is not a shape any target declares).
        case EncodingSlotKind::Imm16Inverted:
            // D-AS4-1 / D-AS4-5 memory-addressing slots write immediate
            // displacements / register encodings (not symbol-relative).
            // The companion symbol-bearing slot for RIP-relative `lea`
            // is `RipRelDisp32` above; it's distinct because it forces
            // the ModR/M state (mod=00 rm=101) in addition to the
            // disp32 patch site, where Disp32 alone (e.g. `call rel32`)
            // has no associated ModR/M byte. CondCodeNibble
            // (D-CSUBSET-WHILE-LOOP-SUBSTRATE) writes into the opcode byte from
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
    // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12: OPTIONAL immediate-MAGNITUDE
    // discriminators on the guard — the JSON keys `guard.immMin` /
    // `guard.immMax`. ABSENT (nullopt) ⇒ the variant matches an
    // instruction of ANY immediate magnitude (every pre-existing variant —
    // full back-compat). PRESENT ⇒ the variant matches ONLY when the
    // instruction's immediate/memOffset operand magnitude falls in
    // [immMin, immMax] (inclusive). This is what lets ONE opcode declare
    // a single-word imm12 variant (immMax:4095) AND a 2-word shifted-imm12
    // variant (immMin:4096, immMax:16777215) with the SAME operandKinds —
    // the selector routes a small frame to the 1-word form and a large
    // frame to the 2-word form by VALUE, agnostically (any ISA can declare
    // magnitude-keyed variants; the matcher reads the LIR operand's value,
    // not the arch). The magnitude is the operand's unsigned value for an
    // ImmInt, or its (signed) displacement viewed as a magnitude for a
    // MemOffset — the matcher inspects whichever immediate-bearing operand
    // the variant's operandKinds declares. A variant that declares NO
    // immediate/memOffset operand but sets immMin/immMax is a config bug
    // (validate() rejects it — there is no value to key on).
    std::optional<std::uint32_t>       immMin;
    std::optional<std::uint32_t>       immMax;
    // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB (introduced) /
    // D-ASM-ARM64-NEGATIVE-IMMEDIATE-UNENCODABLE (generalized): OPTIONAL
    // NEGATIVE-VALUE routing axis — the JSON key `guard.negValue` (bool).
    // FALSE (the default, every pre-existing variant) ⇒ the variant's
    // magnitude axis reads a NON-NEGATIVE value-bearing operand; a NEGATIVE
    // one reports nullopt magnitude and matches NO bounded variant
    // (unchanged). TRUE ⇒ the variant matches ONLY a STRICTLY NEGATIVE
    // value-bearing operand, keyed by its ABSOLUTE VALUE against
    // [immMin, immMax].
    //
    // ★ "VALUE-BEARING OPERAND", NOT "MEMOFFSET". This axis was born as
    // `negMemoffset` and was memoffset-ONLY by its validate() rule alone —
    // the matcher underneath (`variantNegMagnitude`) has ALWAYS read
    // "the first ImmInt-or-MemOffset operand", exactly like its non-negative
    // twin `variantImmMagnitude` and exactly like the immMin/immMax
    // coherence rule. Sign-routing an IMMEDIATE is the SAME QUESTION about a
    // DIFFERENT OPERAND KIND, so the axis was renamed and its validate()
    // rule widened to the immMin/immMax predicate rather than growing a
    // parallel `negImmediate` axis that would have duplicated the matcher.
    //
    // This is the third routing axis (alongside width + imm-range) that lets
    // ONE opcode carry both a positive and a negative form with the SAME
    // operandKinds:
    //   * memoffset — `lea`'s positive `ADD Xd,Xn,#disp` vs its negative
    //     `SUB Xd,Xn,#|disp|` (the encoder writes |disp| into the unsigned
    //     imm12 / shifted-imm12 / MOVZ-MOVK slot; the subtract semantics
    //     live in the fixedWord's SUB base).
    //   * immediate — `mov`'s positive `MOVZ Xd,#imm16` vs its negative
    //     `MOVN Xd,#~imm16` (the encoder writes the COMPLEMENT into the
    //     `imm16.inverted` slot; the negate semantics live in the MOVN base).
    // Agnostic by construction: the matcher reads the LIR operand's SIGN,
    // never the arch. A target whose field is SIGNED (x86 disp32 / imm32)
    // needs no negValue variant at all — its match-any (no immMin/immMax,
    // negValue=false) slot swallows both signs. validate() rejects negValue
    // on a variant with NEITHER an `imm32` NOR a `memoffset` operand (no
    // value to sign-route on).
    bool                               negValue = false;
    // ── MEMORY-DIRECTION routing axis — the JSON key
    // `guard.memoryDestination` (bool). Anchor:
    // D-ASM-X86-CMP-AGAINST-MEMORY-DIRECTION-IS-UNELECTABLE.
    //
    // ABSENT (nullopt) ⇒ this variant does not discriminate on the memory
    // reference's ROLE — every pre-existing variant, including `store`,
    // whose one operand shape is reached from a destination-LAST dialect's
    // memory-destination path AND from a destination-FIRST dialect's
    // register-destination path and must stay electable from both.
    //
    // PRESENT ⇒ the variant matches only an instruction whose
    // `kLirInstFlagMemoryIsDestination` agrees with it. `true` is the
    // direction that reads memory as the operation's LEFT operand
    // (x86 `39 /r` — `cmp mem, reg`); `false` is the direction that reads
    // it as the right one (`3B /r` — `cmp reg, mem`). The two build the
    // BYTE-IDENTICAL LIR operand list, so this axis is the only thing that
    // can separate them — and without it, declaring either would silently
    // encode the other for the opposite spelling.
    //
    // ⚠ A `true`/`false` PAIR IS THE POINT: declaring only one leaves the
    // other direction matching nothing, which is a loud refusal rather than
    // a wrong encoding, but is rarely what an author means. validate()
    // rejects the axis on a guard with no memory operand at all (nothing to
    // route) — the same coherence family as `negValue` and immMin/immMax.
    std::optional<bool>                memoryDestination;
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
    // ARM64 R_AARCH64_TLSLE_ADD_TPREL_HI12 (TLS C2,
    // D-CSUBSET-THREAD-LOCAL): the local-exec thread-pointer-offset
    // HIGH half —
    //   value = (S + A) >> 12
    //   where S is the SIGNED tpoff the walker bit-cast into
    //   symbolVa[sym] (arm64 Variant I ⇒ POSITIVE). OR (value << 10)
    //   into ADD imm12 [21:10] (the `ADD Xd, Xn, #hi12, LSL #12`
    //   word; its sh bit lives in the assembler-emitted base
    //   pattern, not here). RANGE-CHECKED, unlike the magnitude-free
    //   lo12 arm: the hi12 field is an UNSIGNED 12-bit value, so the
    //   addressable tpoff span is [0, 0xFFFFFF] (16 MiB − 1). A
    //   negative S+A (a Variant-II tpoff mis-fed to this formula) or
    //   one above the cap FAILS LOUD — a silent `& 0xFFF` wrap would
    //   address the WRONG per-thread slot (audit LOW-d). The paired
    //   lo12 word reuses Aarch64AddAbsLo12 verbatim (same formula,
    //   distinct tls-flagged KIND row on the target).
    Aarch64TprelAddHi12   = 4,
    // ARM64 R_AARCH64_ADR_GOT_PAGE (D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT,
    // TF-C52): the ADRP word of the `adrp x,:got:sym` + `ldr x,
    // [x,:got_lo12:sym]` GOT-address macro — materializes an undefined-
    // extern's address as a live code-form VALUE so a foreign default-PIE
    // link accepts it (an absolute ADR_PREL_PG_HI21 against a preemptible
    // symbol is rejected "when making a shared object"). Emitted ONLY into
    // an ELF relocatable `.o` / static-archive member, which is linked by
    // a FOREIGN toolchain (gcc/clang) — DSS itself NEVER applies this
    // reloc (no DSS-apply consumer: every DSS-linked image reaches an
    // imported object through the c117 DSS-local got-indirect slot). So the `applyExecRelocations`
    // kernel arm is an EXPLICIT FAIL-LOUD REFUSAL, not an S/A/P formula.
    // Declaring it a real (non-Linear) kind is what keeps the ET_DYN
    // slide-safe classifier from mis-treating it as a Linear-absolute-in-
    // `.text` fixup (D-LK-DYN-TEXT-ABS-RELOC keys `formulaKind == Linear`).
    Aarch64AdrGotPage     = 5,
    // ARM64 R_AARCH64_LD64_GOT_LO12_NC (D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT,
    // TF-C52): the LDR word of the same GOT-address macro (the
    // scaled 12-bit GOT-slot offset). Same foreign-linked-only /
    // fail-loud-in-kernel discipline as Aarch64AdrGotPage above.
    Aarch64Ld64GotLo12    = 6,
};

// Single source of truth — `relocFormulaName` + `parseRelocFormulaKind`
// + `acceptedRelocFormulaList` all iterate this table. Adding a new
// variant = add a row here + add the enum entry. The `static_assert`
// on size catches forgetting one half. (architect + type-design
// 4-agent convergence at post-fold #2 — was previously 3 independent
// hand-rolled enumerations, DRY hazard waiting for the 5th variant.)
inline constexpr EnumNameTable<RelocFormulaKind, 7> kRelocFormulaTable{{{
    { RelocFormulaKind::Linear,               "linear" },
    { RelocFormulaKind::Aarch64Call26,        "aarch64_call26" },
    { RelocFormulaKind::Aarch64AdrPrelPgHi21, "aarch64_adr_prel_pg_hi21" },
    { RelocFormulaKind::Aarch64AddAbsLo12,    "aarch64_add_abs_lo12" },
    { RelocFormulaKind::Aarch64TprelAddHi12,  "aarch64_tprel_add_hi12" },
    { RelocFormulaKind::Aarch64AdrGotPage,    "aarch64_adr_got_page" },
    { RelocFormulaKind::Aarch64Ld64GotLo12,   "aarch64_ld64_got_lo12" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kRelocFormulaTable);

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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kExitMechanismTable);

[[nodiscard]] constexpr std::string_view exitMechanismName(ExitMechanism m) noexcept {
    return kExitMechanismTable.name(m);
}
[[nodiscard]] constexpr std::optional<ExitMechanism>
exitMechanismFromName(std::string_view s) noexcept {
    return kExitMechanismTable.fromName(s);
}

// ── THE SELECTABLE SPELLINGS — the table MINUS the `none` sentinel ────────
//
// D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET: the set a
// `.format.json` author may actually write, which every refusal that names
// this vocabulary has to render. `processExit.mechanism` resolves the spelling
// and then rejects `ExitMechanism::None` explicitly, so the accepted set is the
// table minus that one row.
//
// ★ IT LIVES HERE, BESIDE THE ENUM, BECAUSE THE PROJECTION IS A PROPERTY OF THE
// VOCABULARY AND NOT OF ANY READER. Before this definition the SAME projection
// was computed in FOUR places — `link/linker.cpp` (`kDeclarableExitMechanismNames`),
// `link/format/exec_reloc_apply.hpp`, `link/object_format_schema_json.cpp` and
// `tests/link/test_object_format_vocabulary_projection.cpp` — each with its own
// predicate and its own count, and two of the four counts were unable to fail.
// See `kSelectableObjectFormatKindNames` in `core/types/object_format_kind.hpp`
// for the same shape one vocabulary over.
[[nodiscard]] constexpr bool isSelectableExitMechanism(ExitMechanism m) noexcept {
    return m != ExitMechanism::None;
}

// ⚠⚠ THE COUNT IS A LITERAL AND THE COMPANION `static_assert` IS NOT DECORATION
// — TOGETHER THEY ARE THE ONLY SPELLING THAT REDS IN BOTH DIRECTIONS.
// D-CORE-NAMESWHERE-COUNT-DERIVED-FROM-THE-TABLE-IS-A-TAUTOLOGY.
// `namesWhere<M>` compares the rows the predicate ACCEPTS against `M`, so
// writing `M` as `rows.size() - 1` makes both sides move together and the check
// can never fire. ✔MEASURED with `g++ -std=c++23 -fsyntax-only` over a nine-arm
// probe (a 3-row / 4-row-with-a-new-selectable-row / 4-row-with-a-second-
// sentinel copy of this exact table × the derived, literal, and literal-plus-
// assert spellings of `M`):
//   * a NEW SELECTABLE enumerator  — derived COMPILES, literal ERRORS;
//   * a SECOND UNSELECTABLE row    — derived ERRORS,   literal COMPILES.
// So the literal alone does not dominate the derived form; it MOVES the blind
// spot. The `static_assert` below closes the second direction by relating the
// table's own row count to the projection's literal count — two numbers with
// different owners — so a second sentinel reds here even though `namesWhere`
// would not see it.
inline constexpr auto kSelectableExitMechanismNames =
    namesWhere<2>(kExitMechanismTable, isSelectableExitMechanism);
static_assert(kExitMechanismTable.rows.size()
                  == kSelectableExitMechanismNames.size() + 1,
              "kExitMechanismTable must have exactly ONE unselectable row (the "
              "'none' sentinel) — a second one leaves `namesWhere`'s literal "
              "count matching while the projection silently stops being 'the "
              "table minus its sentinel'");

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
//   * `role`               — UCRT-P4: WHICH RUNTIME ROLE owns the exit
//                            primitive, named against the format's
//                            `runtimeLibraries` table. The JSON declares
//                            this and NOT a path.
//   * `importLibraryPath`  — DERIVED at load from `role` (ucrtbase.dll on
//                            Windows; "/usr/lib/libSystem.B.dylib" on
//                            macOS). Kept as a resolved copy so the entry
//                            trampoline reads exactly what it read before;
//                            `validate()` re-checks it against the table.
//   * `importMangledName`  — on-binary symbol name ("exit" on Windows and
//                            Linux; "_exit" with leading underscore via
//                            D-FF4 on macOS).
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
    RuntimeLibraryRole role = RuntimeLibraryRole::None;  // declared in JSON
    std::string importLibraryPath;   // DERIVED: resolved from `role` at load
    std::string importMangledName;
};

// ── D-RUNTIME-MAIN-ARGC-ARGV (c88): program-entry argument setup ──
//
// `ArgsMechanism` (closed-enum, no `if (os == ...)` branches) — HOW
// the OS/loader hands the C `argc`/`argv` pair to a fresh process,
// so the entry trampoline can materialize them into the entry cc's
// first two integer argument registers BEFORE calling the user
// entry. Without this, `int main(int argc, char** argv)` reads
// whatever garbage the arg registers hold at process entry (the
// c76/c87-witnessed argc=846361312 class).
//
//   * `StackVector` — the kernel/loader places the argument vector
//                     ON THE INITIAL STACK at the entry point (SysV
//                     AMD64 psABI §3.4.1; AAPCS64 Linux mirrors it):
//                     argc is a machine word at
//                     [SP + argcStackOffset]; the NULL-terminated
//                     in-place argv pointer vector STARTS at
//                     [SP + argvStackOffset] — `argv` the VALUE is
//                     that stack address itself (no copy exists
//                     anywhere else). envp follows argv's NULL
//                     terminator; it is NOT materialized (the
//                     c entry signature is
//                     `(int, char**)` — envp is reachable via
//                     libc `environ` for programs that need it).
//   * `None`        — default-constructed sentinel. "No mechanism"
//                     is encoded by `optional<ProcessArgs>` empty
//                     (exactly the ProcessExit discipline); the
//                     JSON loader rejects `mechanism="none"`.
//
// A Windows PE arm is DELIBERATELY absent (not a half-declared enum
// slot): the OS entry point there receives NO C argument vector —
// the CRT route is an out-parameter call
// (`msvcrt!__getmainargs(&argc,&argv,&env,0,&startinfo)`) that needs
// trampoline STACK LOCALS + a 5-argument import call, a genuinely
// different mechanism anchored at D-RUNTIME-PE-MAIN-ARGS. Mach-O
// needs NO mechanism at all: LC_MAIN entry is CALLED by dyld with
// argc/argv/envp/apple already in the argument registers, which the
// trampoline passes through untouched.
enum class ArgsMechanism : std::uint8_t {
    None        = 0,  // default-constructed zero; loader rejects "none"
    StackVector = 1,  // argc + in-place argv vector on the entry stack
    // ⚠ SLOT 2 WAS `CrtOutParam` — c111's msvcrt `__getmainargs` /
    // `__wgetmainargs` out-parameter route — and UCRT-P4 REMOVED IT rather than
    // leaving it as an unused vocabulary member. It was the ONLY thing in the
    // tree that could still point the pe program-entry spine at msvcrt, and a
    // declarable-but-undeclared mechanism is a second owner of "how pe gets
    // argv" waiting to be re-selected. The value is left UNUSED rather than
    // renumbered so a stale on-disk `crt-out-param` spelling fails loud at the
    // name table instead of silently resolving to the accessor arm.
    //
    // UCRT-P4 (D-FFI-PE-CRT-UCRT-MIGRATION): the UCRT ACCESSOR route, which is
    // the mechanism the Universal CRT actually offers — `__getmainargs` /
    // `__wgetmainargs` are msvcrt-ONLY exports (MEASURED 2026-08-10,
    // `objdump -p C:/Windows/System32/ucrtbase.dll`: ucrtbase exports NEITHER,
    // while msvcrt exports them at ordinals 138 / 167). UCRT instead publishes
    //   * `_configure_narrow_argv(int mode)` (ord 190) /
    //     `_configure_wide_argv(int mode)` (ord 191) — populate the CRT's
    //     internal argv state;
    //   * `__p___argc()` -> `int*`   (ord 81),
    //     `__p___argv()` -> `char***` (ord 82),
    //     `__p___wargv()` -> `wchar_t***` (ord 83) — accessors returning the
    //     ADDRESS of the state, so each needs EXACTLY ONE dereference
    //     (their `ucrt/stdlib.h` declarations and the wrapper macros beside them).
    // Like CrtOutParam this needs a real call sequence, so it rides the SAME
    // MIR-tier synth-init seam (the trampoline's own arg-setup stays a no-op)
    // — the difference is entirely in the emitted body, which is why it is a
    // sibling MECHANISM rather than a re-spelling of the msvcrt one.
    //
    // ★ MEASURED, and it is why this mechanism is viable at all (PROBE-0,
    // 2026-08-10): the accessor triple returns the REAL command line in a
    // STARTUP-LESS DSS pe64 binary, byte-identical to the `__getmainargs`
    // control, at debug AND release. No `_initterm` prologue and no
    // `__acrt_initialize` are needed. `_configure_narrow_argv` IS load-bearing
    // — without it `*__p___argc() == 0` and `*__p___argv() == NULL`.
    CrtArgvAccessors = 3,
};

inline constexpr EnumNameTable<ArgsMechanism, 3> kArgsMechanismTable{{{
    { ArgsMechanism::None,             "none"                },
    { ArgsMechanism::StackVector,      "stack-vector"        },
    { ArgsMechanism::CrtArgvAccessors, "crt-argv-accessors"  },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kArgsMechanismTable);

[[nodiscard]] constexpr std::string_view argsMechanismName(ArgsMechanism m) noexcept {
    return kArgsMechanismTable.name(m);
}
[[nodiscard]] constexpr std::optional<ArgsMechanism>
argsMechanismFromName(std::string_view s) noexcept {
    return kArgsMechanismTable.fromName(s);
}

// The selectable spellings — the table MINUS the `none` sentinel, which
// `processArgs.mechanism` resolves and then rejects explicitly. Beside the enum
// for the reason `kSelectableExitMechanismNames` states in full: the projection
// belongs to the vocabulary, not to whichever loader renders it.
[[nodiscard]] constexpr bool isSelectableArgsMechanism(ArgsMechanism m) noexcept {
    return m != ArgsMechanism::None;
}

// ⚠ Literal `M` plus the sentinel-count assert, both halves required — see the
// nine-arm measurement written out at `kSelectableExitMechanismNames`
// (D-CORE-NAMESWHERE-COUNT-DERIVED-FROM-THE-TABLE-IS-A-TAUTOLOGY). A count
// spelled `rows.size() - 1` here would be `x == x`.
inline constexpr auto kSelectableArgsMechanismNames =
    namesWhere<2>(kArgsMechanismTable, isSelectableArgsMechanism);
static_assert(kArgsMechanismTable.rows.size()
                  == kSelectableArgsMechanismNames.size() + 1,
              "kArgsMechanismTable must have exactly ONE unselectable row (the "
              "'none' sentinel) — a second one leaves `namesWhere`'s literal "
              "count matching while the projection silently stops being 'the "
              "table minus its sentinel'");

// Per-OS program-entry argument descriptor. Lives on
// `ObjectFormatData` (loaded from the format JSON's `processArgs`
// block). The trampoline emitter reads the active arm based on
// `mechanism`:
//
// For StackVector (Linux ELF x86_64 + aarch64):
//   * `argcStackOffset` — byte offset of the argc machine word from
//                         the PROCESS-ENTRY stack pointer (0 on both
//                         Linux ABIs). The offsets are defined
//                         relative to the UNTOUCHED entry SP — the
//                         trampoline materializes args BEFORE any
//                         ABI-prologue SP adjustment.
//   * `argvStackOffset` — byte offset of the FIRST argv slot from
//                         the process-entry stack pointer (8 on both
//                         LP64 Linux ABIs — one machine word past
//                         argc). The trampoline LEAs this address
//                         into the second argument register; it
//                         never dereferences it.
//
// The destination registers are intentionally NOT fields — they are
// read from the format's `entryCallingConvention.argGprs[0..1]`
// (single source of truth for the cc register vocabulary, exactly
// the ProcessExit `statusArgGpr` precedent).
struct DSS_EXPORT ProcessArgs {
    ArgsMechanism mechanism = ArgsMechanism::None;

    // StackVector arm
    std::uint32_t argcStackOffset = 0;
    std::uint32_t argvStackOffset = 0;

    // ── CrtArgvAccessors arm (UCRT-P4) ────────────────────────────────────
    //
    // The UCRT spelling of the same job. Five export NAMES + two integers, all
    // per-format config, none of them derivable from anything the engine knows:
    //   * `configureNarrowArgvFn` / `configureWideArgvFn` — the one-shot
    //     populate call. WIDE vs NARROW is selected by the verb of the SOURCE
    //     LANGUAGE's entry row that the resolved entry matched (`argc-argv`
    //     ⇒ narrow, `argc-wargv` ⇒ wide), i.e. still by the RESOLVED ENTRY'S
    //     SIGNATURE and never by a format-level flag — the c111 rule, generalized
    //     from an ad-hoc TypeKind inspection to a declared table lookup. The
    //     signature→verb mapping lives in `DeclarationRule::entryFunctions`; this
    //     format's `entryVerbs` says only WHICH verbs it can realize, and
    //     `argc-wargv` appearing there is what makes a wide entry possible at all.
    //   * `argcAccessorFn` / `narrowArgvAccessorFn` / `wideArgvAccessorFn` —
    //     the address-returning accessors. EXACTLY ONE dereference each.
    //     ★ argc is SHARED between the narrow and wide worlds (MEASURED,
    //     PROBE-0): one accessor serves both, so there is no
    //     `wideArgcAccessorFn`.
    //   * `argvMode` — the `_crt_argv_mode` value handed to the configure call.
    //     That enum is declared in the MSVC toolset's
    //     `…/VC/Tools/MSVC/<ver>/include/vcruntime_startup.h`
    //     (the MSVC TOOLSET header — NOT in the Windows SDK; a grep of the SDK
    //     include tree for the enumerator names returns zero hits), so the
    //     value is declared here rather than derived. MEASURED 2026-08-10 with a
    //     literal `*.c` argument and two `.c` files present: mode 0 ⇒ argc 0 /
    //     argv NULL; mode 1 ⇒ argc 4 with `argv[3] == "*.c"` LITERAL; mode 2 ⇒
    //     argc 5, glob EXPANDED. c111's `_dowildcard` was the literal 0
    //     (unexpanded), so **mode 1 is the behaviour-preserving choice**. The
    //     loader constrains the value to 0..2 — mode 7 does not return an error
    //     at all, the process dies at `0xC0000409` printing nothing.
    //   * `argvUnavailableExitStatus` — the status the synthesized init RETURNS
    //     when the populate call produced nothing.
    //     ★★ THIS FIELD EXISTS BECAUSE THE RETURN VALUE CANNOT BE TRUSTED
    //     (MEASURED): all three valid modes return `errno_t` **0**, and mode 0
    //     returns 0 *while yielding `argv == NULL`*. So the emitted gate tests
    //     `*__p___argv() != NULL` — never the `errno_t`, which would be a guard
    //     that asserts nothing. On failure the init RETURNS this status rather
    //     than calling the user entry, so the value flows out through the
    //     format's already-wired `processExit` path (no second exit import, no
    //     `Unreachable` in a reachable position) and the program terminates with
    //     a distinctive code instead of running `main` on a NULL argv.
    std::string   configureNarrowArgvFn;   // "_configure_narrow_argv"
    std::string   configureWideArgvFn;     // "_configure_wide_argv"
    std::string   argcAccessorFn;          // "__p___argc"
    std::string   narrowArgvAccessorFn;    // "__p___argv"
    std::string   wideArgvAccessorFn;      // "__p___wargv"
    std::uint32_t argvMode = 0;            // `_crt_argv_mode`, 0..2
    std::int32_t  argvUnavailableExitStatus = 0;

    // The import library the CRT entry-point names resolve from. The JSON
    // declares `role`; `crtLibraryPath` is the DERIVED copy the loader resolved
    // against the format's `runtimeLibraries` table (see `RuntimeLibraryRole`).
    RuntimeLibraryRole role = RuntimeLibraryRole::None;
    std::string crtLibraryPath;   // "ucrtbase.dll"
};

// ── UCRT-P4 (D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE): the program-entry
//    vocabulary MOVED OUT of this header ───────────────────────────
//
// `EntryParamShape` / `EntryReturnShape` / `EntryMaterialization` /
// `EntryFunctionShape` now live in `core/types/entry_shape.hpp`, included
// above. They had to be EXTRACTED, not merely shared: the SOURCE-LANGUAGE
// half of the entry declaration (`DeclarationRule::entryFunctions`) reads
// them, `grammar_schema.hpp` includes `semantic_config.hpp`, and THIS header
// includes `grammar_schema.hpp` — so `semantic_config.hpp` including this
// header would close a cycle. Same resolution `enum_name_table.hpp` already
// used. Read that file's docblock for WHY the signature and the realized verb
// set have two DIFFERENT single owners rather than one shared table.

// ── TLS identity (D-CSUBSET-THREAD-LOCAL, TLS C1) ──────────────────
//
// The CPU's static-TLS layout convention — which side of the thread
// pointer the TLS block sits on, and how the link-time thread-pointer
// offset (tpoff) of a symbol is computed from its template offset.
// This is TARGET (psABI-per-CPU) data, not format data: x86_64 is
// Variant II under EVERY OS; arm64 is Variant I likewise. Consumed
// ONLY by the walker's tpoff helper (slice C's `addTlsSymbolOffsets`)
// when the format's `tlsAccess.model == local-exec` — a config-keyed
// FORMULA selector, never a machine-identity branch:
//
//   * Variant I  (arm64, riscv): tp points AT the TCB head; the TLS
//     block follows it. tpoff = alignUp(tcbHeaderBytes, p_align)
//     + templateOffset — always POSITIVE.
//   * Variant II (x86_64, sparc): tp points ONE PAST the block end
//     (fs:[0] holds tp itself). tpoff = templateOffset
//     − alignUp(blockSize, p_align) — always NEGATIVE.
//
// A target that declares NO `tls` block cannot compute a tpoff; the
// walker fails loud on a TLS symbol under such a target (belt +
// braces past the `tlsbase`-opcode-missing gate at MIR→LIR).
enum class TlsVariant : std::uint8_t {
    Variant1 = 1,  // tp at TCB head; positive tpoff (arm64 — TLS C2)
    Variant2 = 2,  // tp past block end; negative tpoff (x86_64 — TLS C1)
};

inline constexpr EnumNameTable<TlsVariant, 2> kTlsVariantTable{{{
    { TlsVariant::Variant1, "variant1" },
    { TlsVariant::Variant2, "variant2" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kTlsVariantTable);

[[nodiscard]] constexpr std::string_view tlsVariantName(TlsVariant v) noexcept {
    return kTlsVariantTable.name(v);
}
[[nodiscard]] constexpr std::optional<TlsVariant>
tlsVariantFromName(std::string_view s) noexcept {
    return kTlsVariantTable.fromName(s);
}

// The target's `"tls"` identity block. `tcbHeaderBytes` is the
// Variant-I TCB header size the block is placed after (arm64: 16 —
// two pointers); 0 on Variant-II targets (the formula never reads it
// there, but the field is validated-load config, not a guess).
struct DSS_EXPORT TlsIdentity {
    TlsVariant    variant        = TlsVariant::Variant2;
    std::uint32_t tcbHeaderBytes = 0;
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
    // TLS C1 (D-CSUBSET-THREAD-LOCAL): true iff this relocation kind
    // carries a THREAD-LOCAL-OFFSET value (`"tls": true` in the JSON
    // row — x86_64 `tls-tpoff32`, arm64's future tprel pair). The
    // walker's TLS cross-check consumes it BOTH directions: a
    // relocation of a tls-flagged kind must target a TLS symbol
    // (its patched value is a tpoff, not a VA), and a TLS symbol must
    // only be reached through tls-flagged kinds (a non-TLS reloc
    // against a TLS symbol would write the bit-cast tpoff as if it
    // were an address — the CRIT-1 silent-garbage-pointer class).
    bool         tls         = false;
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
    // 2 successors. ★★ ITS BlockRef OPERANDS ARE ALSO THE ENCODER’S
    // BRANCH TARGETS — every `mir_to_lir` / `asm_text_to_lir` jcc carries
    // TWO, and `x86_variable.cpp` takes its displacement from operand[0]
    // and emits the trailing unconditional jump to operand[1].
    // ⚠ THIS LINE USED TO READ “NO BlockRef operands” AND A CONSUMER
    // IMPLEMENTED IT: the `.dsslir` reader filtered every BlockRef out and
    // called `addCondBr` with what was left, so a real lowered jcc read
    // back from text had ZERO operands and could not assemble
    // ([[D-LIR-TEXT-CONDBR-BLOCKREF-OPERANDS-DROPPED]]). `Br` survived only
    // because `addBr` re-synthesizes its operand. `LirVerifier` Rule 1b now
    // cross-checks the two writable channels, and asserts PRESENCE rather
    // than mere agreement — an agreement-only rule is vacuously satisfied
    // by the zero-operand state that WAS the defect.
    CondBr      = 2,    //                                          (LirBuilder::addCondBr)
    Switch      = 3,    // >=2 successors                           (LirBuilder::addSwitch — reserved)
    Return      = 4,    // 0 successors, may carry return-value ops (LirBuilder::addReturn)
    Unreachable = 5,    // 0 successors, 0 operands                 (LirBuilder::addUnreachable)
    IndirectBr  = 6,    // >=1 successors, 1 reg operand (the addr) (LirBuilder::addIndirectBr) — D-CSUBSET-COMPUTED-GOTO
};

// Canonical string form used by `.target.json` and `.dsslir` text.
// Single source of truth for the loader (string → enum) and any future
// emit-side serializer (enum → string).
inline constexpr EnumNameTable<TargetTerminatorKind, 7> kTargetTerminatorKindTable{{{
    { TargetTerminatorKind::None,        "none"        },
    { TargetTerminatorKind::Br,          "br"          },
    { TargetTerminatorKind::CondBr,      "cond-br"     },
    { TargetTerminatorKind::Switch,      "switch"      },
    { TargetTerminatorKind::Return,      "return"      },
    { TargetTerminatorKind::Unreachable, "unreachable" },
    { TargetTerminatorKind::IndirectBr,  "indirect-br" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kTargetTerminatorKindTable);

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

inline constexpr std::array<TargetTerminatorShape, 6> kTargetTerminatorShapes{{
    { TargetTerminatorKind::Br,          1, 1   },
    { TargetTerminatorKind::CondBr,      2, 2   },
    { TargetTerminatorKind::Switch,      2, 255 },  // 255 = unbounded sentinel
    { TargetTerminatorKind::Return,      0, 0   },
    { TargetTerminatorKind::Unreachable, 0, 0   },
    // D-CSUBSET-COMPUTED-GOTO: >=1 address-taken successors (255 = unbounded).
    { TargetTerminatorKind::IndirectBr,  1, 255 },
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

    // Role-tagged projection contract (D-CSUBSET-MOD-OP-CODEGEN-OUTPUT-INDEX-CONTRACT
    // closure, 2026-06-10). Optional JSON
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

    // ★★★ THE `outputs ⊆ clobbered` INVARIANT, AS A QUERY ON THE TYPE
    // ITSELF (D-LIR-PER-INSTRUCTION-OUTPUTS-NOT-ENFORCED-SUBSET-OF-CLOBBERED,
    // 2026-08-15). Returns the index of the first output
    // ordinal absent from `clobberedOrdinals`, or nullopt when the
    // invariant holds.
    //
    // ⚠ IT LIVES HERE BECAUSE THIS TYPE HAS **TWO** PRODUCERS AND ONLY
    // ONE OF THEM MEETS A LOADER. The `.target.json` loader enforces
    // the rule for the per-OPCODE carrier; `LirBuilder::
    // regConstraintPoolAdd` accepts a per-INSTRUCTION one built by a
    // lowering, with no loader in the path. And the register
    // allocator's forbidden set is `inputs ∪ clobbered` — outputs are
    // deliberately omitted, on the strength of this invariant holding.
    // So a violating entry does not fail: it silently leaves a value
    // allocated to a register the instruction overwrites. Two carriers,
    // one validator, and a THIRD component's safety argument resting on
    // the validation only one of them got.
    //
    // Reads ORDINALS, not names: a target may spell one register
    // several ways (sub-register aliases), and two different spellings
    // of the same physical register must satisfy the rule. Callers that
    // hold only names must resolve first — which every producer already
    // does, because the ordinals are what the allocator reads.
    [[nodiscard]] std::optional<std::size_t>
    firstOutputNotClobbered() const noexcept {
        for (std::size_t k = 0; k < outputOrdinals.size(); ++k) {
            bool found = false;
            for (auto const cl : clobberedOrdinals) {
                if (cl == outputOrdinals[k]) { found = true; break; }
            }
            if (!found) return k;
        }
        return std::nullopt;
    }
};

// ── D-TARGET-ENCODING-TABLE-EXPRESSES-ONLY-THE-DEGENERATE-SEQUENCE ────────
//
// ★★★ THE FACT AN OPCODE ROW STATES IS *"on this target, this operation is
// realized by these machine instructions"* — PLURAL. Everything above this
// point (`TargetEncodingInfo` and its variants) can only say it for ONE
// instruction, and 1:1 is the DEGENERATE CASE of the real relationship, not
// the relationship. The first consumer that needed more than one found the
// hole: x86-64 SSE2 has NO unsigned integer↔double instruction at all, so
// `ui_to_fp` / `fp_to_ui` were declared with the SIGNED cvt's bytes and
// SILENTLY MISCOMPILED every value at or above 2^63 (and, on the u32 source
// arm, every value at or above 2^31) — measured against gcc-13 and clang-19,
// which agree on all of them.
//
// A two-valued "strategy" enum (native / fixup) was REJECTED as an arch
// branch with a different spelling: no `if (arch == x86)` appears, but the
// switch arm is x86-shaped and lives in shared substrate, and x86 has more of
// these coming (popcount without POPCNT, 128-bit multiply-high, float min/max
// NaN semantics). ⇒ **each target STATES A FACT in ONE uniform form and the
// shared substrate understands ONE thing: emit what you were told.** A target
// whose hardware does the job in one instruction declares a ONE-STEP sequence
// (arm64's UCVTF / FCVTZU) — not a special case and not a `native` flag, the
// same kind of row, one entry long.
//
// ★ WHY THIS IS A SIBLING OF `encoding`, NOT AN EXTENSION OF IT. The two
// blocks answer questions at DIFFERENT TIERS and folding them would break the
// assembler. `encoding` is LIR-instruction → BYTES (one instruction in, one
// byte string out — the assembler's whole contract). `lowering` is MIR
// operation → LIR INSTRUCTIONS, consumed at MIR→LIR while virtual registers
// still exist, which is the only tier where a sequence's TEMPORARIES can be
// register-allocated instead of stealing fixed scratch registers.
//
// ★ ONE LEVEL, NEVER RECURSIVE. A step names a REAL MACHINE INSTRUCTION: the
// loader rejects a step whose opcode declares no `encoding`, and the expander
// emits each step directly without re-consulting the step opcode's own
// `lowering`. That is what makes arm64's SELF-NAMING one-step sequence
// (`ui_to_fp` → [`ui_to_fp`]) well-founded rather than an infinite regress.

// One operand of one lowering step. The four kinds are the complete
// vocabulary a straight-line expansion needs:
//   Source    — the k-th operand of the MIR instruction being lowered.
//   Temp      — a register defined by an EARLIER step of this sequence.
//   Immediate — an inline 32-bit immediate (a shift count, a mask).
//   Constant  — a 64-bit BIT PATTERN materialized into a fresh GPR by the
//               target's own declared wide-constant capability (x86's
//               `mov r64, imm64`; arm64's MOVZ/MOVK ladder) before the step
//               runs. This is the ONLY kind that costs an extra instruction,
//               and it exists because the encoding table CANNOT name a
//               constant-pool entry: `imm64` names a VALUE (integer only) and
//               `symbol` needs a SymbolId minted upstream at HIR→MIR. A
//               float constant therefore rides a GPR bit pattern and a
//               declared cross-class move, not a rodata item.
enum class TargetLoweringOperandKind : std::uint8_t {
    Source = 0, Temp = 1, Immediate = 2, Constant = 3,
};
inline constexpr EnumNameTable<TargetLoweringOperandKind, 4>
kTargetLoweringOperandKindTable{{{
    { TargetLoweringOperandKind::Source,    "source"   },
    { TargetLoweringOperandKind::Temp,      "temp"     },
    { TargetLoweringOperandKind::Immediate, "imm"      },
    { TargetLoweringOperandKind::Constant,  "const"    },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kTargetLoweringOperandKindTable);
[[nodiscard]] constexpr std::string_view
targetLoweringOperandKindName(TargetLoweringOperandKind k) noexcept {
    return kTargetLoweringOperandKindTable.name(k);
}
[[nodiscard]] constexpr std::optional<TargetLoweringOperandKind>
targetLoweringOperandKindFromName(std::string_view s) noexcept {
    return kTargetLoweringOperandKindTable.fromName(s);
}

struct DSS_EXPORT TargetLoweringOperand {
    TargetLoweringOperandKind kind = TargetLoweringOperandKind::Source;
    std::uint8_t  sourceIndex = 0;   // Source: MIR operand index
    std::uint16_t tempSlot    = 0;   // Temp: index into the sequence's temp table
    std::int32_t  immediate   = 0;   // Immediate: the inline imm32 value
    std::uint64_t constant    = 0;   // Constant: the 64-bit bit pattern
    std::string   tempName;          // Temp: the declared spelling (diagnostics)
};

// One step = one machine instruction the operation expands into.
//
// `widthBits` is the LIR instruction's operation width, i.e. the axis the
// encoding-variant guards key on. 0 means "the LIR default", which
// `lirInstWidthBits` reads as 64 — the same absent-flag state every
// pre-existing single-instruction lowering emits. It is per-STEP and not
// inherited from the sequence guard because a real sequence mixes them: the
// x86 F32→u64 expansion truncates at width 32 (CVTTSS2SI) and masks at
// width 64 (SAR/AND/OR) in the same seven steps.
struct DSS_EXPORT TargetLoweringStep {
    std::string    opcodeMnemonic;               // the machine opcode to emit
    std::uint16_t  opcodeIndex   = 0;            // loader-resolved (post-pass)
    bool           hasResult     = false;        // the step defines a register
    bool           definesResult = false;        // ...and it is the SEQUENCE's result
    std::uint16_t  resultTempSlot = 0;           // when hasResult && !definesResult
    std::string    resultName;                   // declared spelling (diagnostics)
    TargetRegClass resultClass   = TargetRegClass::None;
    std::uint8_t   widthBits     = 0;            // 0 / 8 / 16 / 32 / 64
    std::vector<TargetLoweringOperand> operands;
};

// One sequence + the width it applies to. `guardWidthBits` mirrors
// `TargetEncodingVariant::guardWidthBits` EXACTLY, including which width it
// names: whatever axis the opcode's encoding variants key on (for the
// int↔float conversions that is the SOURCE width, threaded as the
// `widthOverride` at MIR→LIR). 0 = matches any width. The loader rejects two
// sequences with the same width and the ambiguous keyed/absent mix, for the
// same first-match-shadowing reason the encoding variants do.
struct DSS_EXPORT TargetLoweringSequence {
    std::uint8_t                    guardWidthBits = 0;
    std::vector<std::string>        tempNames;   // slot → declared spelling
    std::vector<TargetLoweringStep> steps;
};

struct DSS_EXPORT TargetLoweringInfo {
    std::vector<TargetLoweringSequence> sequences;
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

    // Instruction-SEQUENCE facet
    // (D-TARGET-ENCODING-TABLE-EXPRESSES-ONLY-THE-DEGENERATE-SEQUENCE).
    // EMPTY `sequences` (the default) means this opcode declares no expansion
    // and MIR→LIR emits ONE instruction of it, exactly as before — every
    // pre-existing opcode row keeps its behaviour byte-identically. A NON-empty
    // block means MIR→LIR emits the matching sequence's steps INSTEAD, and the
    // opcode itself may then legitimately carry NO `encoding` at all (x86-64's
    // `ui_to_fp`/`fp_to_ui`: the machine has no such instruction, so declaring
    // one was the miscompile). See the docblock above `TargetLoweringOperandKind`.
    TargetLoweringInfo   lowering;

    // 2-address legalization constraint (plan 13 AS3 — `lir_2addr_
    // legalize.cpp`). ENGAGED means the LIR pre-assembly legalize pass
    // ensures the instruction's `result` register equals the operand
    // this names, before the assembler sees it — by inserting an
    // implicit `mov result, operands[j]` whenever they differ. x86's
    // reg-reg arithmetic (add/sub/mul) needs this (REX.W 0x03 /r
    // writes into r/m, so the dest IS one of the sources); ARM64's
    // reg-reg arithmetic is 3-address natively and leaves this
    // DISENGAGED.
    //
    // ★★★ THE VALUE IS THE TIED SOURCE OPERAND'S INDEX, NOT A FLAG
    // (D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE, 2026-08-15). It was a
    // `bool` with `0` written as a LITERAL at four consumer sites, so
    // the only tie the whole pipeline could express was
    // *result == operand[0]* — no `(result, operand j)` pair existed
    // anywhere, which is what made a read-write asm operand (`"+r"`)
    // unrepresentable. `.target.json` spells the default shape as
    // `requires2Address: true` (⇒ index 0, every shipped opcode's
    // meaning, unchanged) and a non-zero tie as that key PLUS
    // `twoAddressSourceOperand: <j>`; the loader rejects the index
    // without the flag, and rejects an index outside `maxOperands`.
    //
    // ⚠ NEVER COMPARE THIS TO `true` OR `false`. `opt == true` is a
    // VALUE comparison (`*opt == 1`), so it silently means "tied to
    // operand 1" — the trap that comes with folding a flag and an
    // index into one field. Ask `.has_value()` (or use it in a
    // boolean context, which every pre-existing reader already did
    // and which keeps meaning "is this opcode two-address"), and read
    // the index with `*`.
    std::optional<std::uint8_t> requires2Address;

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

    // ★★★ THE INSTRUCTION-SET ARCHITECTURE THIS TARGET EXECUTES — the
    // TARGET half of the (language emits · target executes) ISA axis
    // (D-ISA-LANGUAGE-BOUND-TO-ARCHITECTURE). The LANGUAGE half is
    // `GrammarSchema::isa()`; the engine compares the two DECLARED values
    // for equality and never reads a name, a format kind, or a machine code.
    //
    // ⚠ IT IS NOT `name`, AND THE DIFFERENCE IS THE WHOLE POINT. `name` is
    // this document's IDENTITY — the filename stem, the string a user types
    // in `--target <name>:<format>` — and keying a build decision on it is
    // the identity branch the agnosticism veto forbids. `isa` is a FACT
    // ABOUT THE HARDWARE that several distinct target documents may
    // legitimately share: the day an `x86_64-v3` or an `arm64e` target
    // ships, it declares the SAME `isa` as its sibling and every ISA-bound
    // language accepts it with NO edit to any language document. That
    // property is the design's acceptance criterion, not a side effect.
    // ✔MEASURED: the shipped values are `x86_64` and `aarch64`, and the
    // arm64 target's `isa` DIFFERS from its own `name` — so an
    // `if (name == …)` impostor cannot reproduce the shipped verdicts
    // without inventing a second mapping table.
    //
    // ⚠ IT IS NOT THE `machine` CODE EITHER. Those are per-FORMAT encodings
    // of the arch (`elf.machine` 183 / `pe.machine` 0xAA64 /
    // `macho.cputype` 0x0100000C all name the same ISA), they live on the
    // format side, and `cross_validate_target_format.cpp` reaches them only
    // through a table keyed on the target NAME — so they answer
    // "do this target and this format agree?", never "what does this target
    // execute?".
    //
    // EMPTY ⇒ the target declares no ISA. Optional at load (a new REQUIRED
    // key breaks the load of every existing target document and fixture),
    // and the gate is FAIL-CLOSED on the absence: a language that DOES
    // declare a binding cannot be shown to match an undeclared target, so
    // the pair is refused with the message saying the target declares none.
    // A language that declares nothing is portable and is unaffected — the
    // common case pays nothing for this axis existing.
    std::string             isa;              // "x86_64" / "aarch64" / ""

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

    // GNU inline-asm constraint letters declared by this target (the
    // `asmConstraints` root key — see `TargetAsmConstraint` for the
    // vocabulary-vs-grammar line this facet is drawn on). OPTIONAL and
    // EMPTY BY DEFAULT: a target that declares none simply refuses every
    // constraint letter by name, which is the correct answer for a
    // processor whose inline-asm binding has not been described yet.
    //
    // ★ Populated AFTER `registers`, because a `binds: "register"` row
    // resolves its name to an ORDINAL at load — the same precedent as the
    // `implicitRegisters` roles. A dangling name is a load error, never a
    // lookup that fails later at a site with no target in hand.
    //
    // ★ No side index. The table is a handful of rows (✔MEASURED: 10 on
    // x86_64, 4 on arm64) keyed by a one- or two-character string, so a
    // `TransparentStringMap` would cost more to build than the linear scan
    // it replaces; the loader rejects duplicate letters, so the scan is
    // unambiguous by construction rather than by convention.
    std::vector<TargetAsmConstraint> asmConstraints;

    // The CIE's `return_address_register` — the DWARF column an unwinder
    // reads to find where this frame's return address went.
    //
    // ★ DECLARED, NOT DERIVED, and this is the whole reason it is a
    //   separate field rather than "the DWARF number of `cc.linkRegister`":
    //   on x86_64 SysV it is column **16**, which is NOT A REGISTER — it is
    //   a synthetic column with no hardware counterpart, so no register row
    //   can carry it. On AArch64 it is 30, which happens also to be x30's
    //   ordinary number. A derivation that worked on AArch64 would have
    //   nothing to read on x86_64 and would have to invent a value.
    //
    // Empty ⇒ the target declares no DWARF numbering at all, and the
    // `.eh_frame` writer refuses (naming the target and this key) rather
    // than emitting a table it cannot number. The loader keeps the two
    // halves of the psABI table together: declaring this without any
    // register number, or any register number without this, is REJECTED —
    // half a numbering is a table that loads clean and cannot be used.
    std::optional<std::uint16_t> dwarfReturnAddressColumn;

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

    // D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH (LD-2): the per-`WideFloatOp`
    // softfloat-libcall table (the JSON `wideFloatSoftcalls[]` section),
    // indexed by the `WideFloatOp` ordinal. An op with no row keeps
    // `declared == false` → `wideFloatSoftcall()` returns nullptr → the
    // F128 engine verb falls through to the fail-loud encoded-width gate
    // (this ABSENCE, not a target/format check, is what gates the softcall
    // path). `wideFloatSoftcallLibraryByFormat` maps an object FORMAT KIND
    // (Elf) → the DT_NEEDED library the minted extern imports bind to
    // ("libgcc_s.so.1"); the LIR lowerer resolves the ACTIVE format's entry
    // and threads it in (nullopt = the format declares none → the softcall
    // fails loud rather than mint an unbound extern).
    //
    // ★ KEYED ON THE ENUM, NOT ON A STRING — the `charIsUnsignedByFormat`
    // reshape, applied here. This map USED to be an
    // `unordered_map<std::string,std::string>` filled from arbitrary JSON keys,
    // so a misspelled `"elff"` (or a mis-cased `"ELF"`) STORED cleanly, the
    // accessor's raw-string lookup missed, and the F128 softcall path reported
    // "this format declares no softcall library" — a config typo degrading
    // long-double arithmetic with no diagnostic pointing at the config. An
    // `ObjectFormatKind`-indexed array makes that state UNREPRESENTABLE: there
    // is no slot for a name that is not in `kObjectFormatKindTable`, so the
    // loader MUST resolve every key through `objectFormatKindFromName` (and
    // reject the `unknown` sentinel) before it can store anything at all.
    //
    // An EMPTY slot means "this format declares no softcall library". The
    // loader rejects an empty library STRING for exactly that reason — an
    // empty value would be indistinguishable from an absent key, which is the
    // same silent-fallback shape one layer down.
    std::array<WideFloatSoftcall, kWideFloatOpCount> wideFloatSoftcalls{};
    std::array<std::string, kObjectFormatKindCount>
        wideFloatSoftcallLibraryByFormat{};

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

    // ── Bare-`char` signedness — THE SINGLE SOURCE OF TRUTH ──────────────
    // (TF-C56 D-CSUBSET-BARE-CHAR-SIGNEDNESS-PER-TARGET,
    //  TF-C75 D-TARGET-CHAR-SIGNEDNESS-PER-PLATFORM)
    //
    // Whether bare `char` (the distinct `TypeKind::Char`, NOT `signed char` /
    // `unsigned char`) is an UNSIGNED type. C 6.2.5p15 leaves this
    // IMPLEMENTATION-DEFINED, and the implementation that decides is the
    // (processor × PLATFORM) pair, not either alone: the SAME AArch64 CPU is
    // UNSIGNED under GNU/Linux AAPCS64 and SIGNED under Apple's Darwin ABI.
    //
    // ★ THE WHOLE FACT LIVES HERE, IN ONE KEY. `charIsUnsignedDefault` is the
    // processor's answer; `charIsUnsignedByFormat[kind]` overrides it for the
    // object formats whose PLATFORM fixes the answer for every CPU it serves
    // (Darwin/macho and Windows/pe both chose SIGNED). It is NOT split across
    // the target and the format schemas: `elf` serves both aarch64 (unsigned)
    // and x86_64 (signed), so a flat value on the FORMAT would be a lie on one
    // of them, and making it honest would force all 24 `.format.json` files to
    // enumerate CPU architectures — a layering inversion. The processor half
    // and the platform half are both per-processor knowledge, so they belong
    // in the per-processor file.
    //
    // `charIsUnsignedByFormatDeclared[kind]` is the presence bit — the
    // `condCodeDeclared` discipline. It exists because `false` (signed) is a
    // MEANINGFUL override value: without it, "this format declares signed"
    // would be indistinguishable from "this format says nothing", and the
    // macho/pe rows (which declare exactly `false`) would silently vanish.
    //
    // Indexed by `static_cast<std::size_t>(ObjectFormatKind)`; the array size
    // is derived from the enum's own name table (`kObjectFormatKindCount`), so
    // a new format kind cannot leave the table one slot short.
    //
    // Consumed through ONE accessor, `charIsUnsigned(ObjectFormatKind)`, whose
    // result is threaded into `MirLoweringConfig.charIsUnsigned` — sole reader
    // `isSignedIntKind(Char)`, the single SExt-vs-ZExt decision on the char→int
    // promotion. No arch, format, or platform NAME is ever compared.
    bool charIsUnsignedDefault = false;
    std::array<bool, kObjectFormatKindCount> charIsUnsignedByFormat{};
    std::array<bool, kObjectFormatKindCount> charIsUnsignedByFormatDeclared{};

    // TF-C74 (D-CONFIG-PER-ARCH-PREDEFINED-MACROS): the target's
    // PER-ARCHITECTURE IDENTITY predefined macros
    // (`"predefinedMacros"` — the same entry grammar as the language's
    // `preprocess.predefinedMacros`, parsed by the SHARED
    // `parsePredefinedMacroArray`). This lives on the TARGET, not the
    // language, because it is per-CPU-architecture semantics — exactly like
    // `charIsUnsigned` / `aggregateLayout` / `tls` above. The alternative
    // (a language-side arch filter) would force `c.lang.json` to
    // enumerate CPU architectures: a layering inversion.
    //
    // Merged with the language list at preprocess time
    // (`mergePredefinedMacros`), which is where a name declared by BOTH
    // sides fails LOUD — there is no last-writer-wins in either direction.
    // OPTIONAL; absent ⇒ empty ⇒ the preprocessor's effective list is
    // byte-identical to the language-only list (the no-regression
    // invariant). Per-entry `availableObjectFormats` still applies, which is
    // how the Apple-only `__arm64__`/`__arm64` spellings stay macho-gated
    // while `__aarch64__` is universal.
    std::vector<PredefinedMacroDef> predefinedMacros;

    // D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET: the NAME of the shipped source
    // language document that spells THIS processor's assembly
    // (`"defaultAssemblyLanguage": "asm-x86_64-att"`). A `<stem>` for
    // `GrammarSchema::loadShipped`, i.e. exactly what `--language` takes.
    // OPTIONAL; empty ⇒ this target declares none, and a build that would have
    // needed it fails loud NAMING THIS TARGET rather than guessing a dialect.
    //
    // ★★ A NAME IS VOCABULARY; A PREFIX IS GRAMMAR, AND THE DIFFERENCE IS THE
    // WHOLE REASON THIS KEY IS ONE STRING. An earlier cycle put an `asmSyntax`
    // BLOCK here — `registerPrefix`, `immediatePrefix`, `commentPrefixes`, a
    // per-instruction `destinationOperand` — and it was reverted the same day
    // ([[D-CONFIG-ASM-DIALECT-DECLARED-AS-TARGET-VOCABULARY]]). ✔MEASURED with
    // gcc on ONE target: AT&T `movq %rsi, (%rdi)` vs Intel (`-masm=intel`)
    // `mov QWORD PTR [rdi], rsi` — same CPU, same compiler, and yet the
    // register sigil, the immediate sigil, the comment character, the operand
    // ORDER and the memory-operand form all differ. Every one of those is a
    // function of (target, DIALECT), so storing it per-TARGET stores a
    // per-(X,Y) fact per-X. What genuinely IS per-target is WHICH dialect
    // document this processor defaults to — a name, resolved by the loader
    // that owns the grammar. Do not re-propose the block.
    //
    // ★ Deliberately NOT a list. A second dialect of one CPU
    // (`asm-x86_64-intel`) would need a way to SELECT between them, and the
    // only honest selector is the one that already exists: naming the language
    // explicitly. Building a `-masm=`-style flag before a second dialect ships
    // would be a knob with nothing to switch to.
    std::string defaultAssemblyLanguage;

    // TLS C1 (D-CSUBSET-THREAD-LOCAL): the target's static-TLS layout
    // convention (`"tls"` block — variant + tcbHeaderBytes). OPTIONAL:
    // a target without it (arm64 until TLS C2) cannot lay out a TLS
    // block — the walker's tpoff helper fails loud on a TLS symbol
    // (the lowering-tier `tlsbase`-opcode gate fires first anyway).
    // nullopt-vs-engaged mirrors `vaListLayout` / `processExit`:
    // absence IS the capability signal, never a zero-filled default.
    std::optional<TlsIdentity> tls;

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
    //     - NO referenced register may declare `subOf`
    //       (D-TARGET-CC-NAMES-SUB-REGISTER). A cc names full registers
    //       only. Applies to every list AND every singleton role
    //       (stackPointer / framePointer / linkRegister /
    //       indirectResultRegister), because all of them resolve to a
    //       table ordinal that the allocator pools — built by
    //       intersecting the register table with these very names —
    //       would otherwise skip in silence. This rule is what makes
    //       that name filter load-bearing on its own.
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

    // Lowercase 64-hex SHA-256 of the EXACT document bytes this schema was
    // loaded from — the cache key for the runtime-object cache, which keys on
    // the config documents a build actually loaded.
    //
    // WHY RETAINED RATHER THAN RECOMPUTED. Re-walking `src/dss-config/` from
    // disk to hash it costs ~165 ms per invocation (MEASURED 2026-08-17: 86
    // files, 2,078,133 bytes; I/O-dominated — walk+read 152–160 ms, hash only
    // 9–13 ms), and would be paid on EVERY build. The loaders already hold the
    // bytes; they read them, parse them, and discard them. Digesting them
    // where they already are costs zero extra I/O and happens once per load
    // (loads are memoized in-process), and retaining 32 bytes of digest
    // instead of up to 440 KB of document is what makes retention free.
    //
    // ⚠ EMPTY MEANS UNKNOWN, NEVER "no content". A schema built through a path
    // that does NOT go via `loadFromText` — the public `TargetSchemaData`
    // constructor above, which tests and in-memory mutators use — has no
    // document bytes to digest and leaves this EMPTY. That is deliberate: an
    // empty digest is a DETECTABLE unknown a cache can refuse to key on,
    // whereas a fabricated or stale one is a silent wrong key. Every file
    // route (`loadShipped` → `loadFromFile` → `loadFromText`) is digested.
    [[nodiscard]] std::string_view  contentDigest() const noexcept {
        return contentDigest_;
    }

    [[nodiscard]] TargetSchemaId    id()       const noexcept { return d_.id; }
    [[nodiscard]] std::string_view  name()     const noexcept { return d_.name; }
    // Semantic version string declared by the target JSON. Round-trip
    // contracts (e.g. `.dsslir` preamble) emit this so a cross-version
    // load is loudly rejected at parse time rather than silently
    // mis-interpreting opcode numbers / register table layouts that a
    // version bump might have permuted.
    [[nodiscard]] std::string_view  version()  const noexcept { return d_.version; }
    [[nodiscard]] TargetAbiModel    abiModel() const noexcept { return d_.abiModel; }
    // The instruction-set architecture this target EXECUTES (the `target.isa`
    // key). EMPTY ⇒ undeclared; see `TargetSchemaData::isa` for why that is a
    // fail-CLOSED input to the language↔target gate rather than a wildcard,
    // and for why this is not `name()` and not a `machine` code.
    [[nodiscard]] std::string_view  isa()      const noexcept { return d_.isa; }
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

    // ── GNU inline-asm constraint letters ───────────────────────
    // Every letter this target declares, in declaration order. EMPTY is a
    // legitimate state: it means this processor has not described its
    // inline-asm binding, and every constraint is then refused BY NAME.
    [[nodiscard]] std::span<TargetAsmConstraint const>
    asmConstraints() const noexcept {
        return d_.asmConstraints;
    }
    [[nodiscard]] std::size_t asmConstraintCount() const noexcept {
        return d_.asmConstraints.size();
    }

    // Resolve one constraint LETTER. Returns nullptr when this target does
    // not declare it — which the caller must turn into a diagnostic naming
    // the letter AND the target, never into a fallback guess. ✔MEASURED,
    // this is exactly gcc's own behaviour: `"=a"` is `%rax` on x86_64 and
    // "impossible constraint in 'asm'" on AArch64.
    //
    // ⚠ Takes the LETTER ONLY. A caller holding `"=&a"` must have stripped
    // the modifiers first: `=`/`+`/`&`/`%` are GNU-asm grammar owned by the
    // front end, and the loader refuses to store them, so passing a raw
    // constraint string here always misses. Case-sensitive (`d` ≠ `D`).
    [[nodiscard]] TargetAsmConstraint const*
    asmConstraint(std::string_view letter) const noexcept {
        for (auto const& c : d_.asmConstraints) {
            if (c.letter == letter) return &c;
        }
        return nullptr;
    }

    // ★ THE VALID-LIST RENDERER FOR THE CONSUMER'S DIAGNOSTIC, so that the
    // one message this facet cannot emit itself — "this target does not
    // declare letter X" — still cannot be written with a hand-typed list.
    // The list is per-TARGET, so there is no correct constant to type: the
    // letters differ between processors, which is the whole point of the
    // facet. ✔MEASURED, x86_64 declares 'a' and arm64 does not.
    [[nodiscard]] std::string declaredAsmConstraintLetters() const {
        std::string out;
        for (auto const& c : d_.asmConstraints) {
            if (!out.empty()) out += ", ";
            out += '\'';
            out += c.letter;
            out += '\'';
        }
        return out;
    }

    // The CIE's `return_address_register` (see the field's docblock in
    // `TargetSchemaData`). Nullopt ⇒ this target declares no DWARF
    // register numbering, which every unwind-table writer treats as a
    // REFUSAL rather than a licence to guess.
    [[nodiscard]] std::optional<std::uint16_t>
    dwarfReturnAddressColumn() const noexcept {
        return d_.dwarfReturnAddressColumn;
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

    // ── Wide-float softcalls (LD-2, D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH) ──
    // The softfloat-libcall row for `op`, or nullptr when this target
    // declares none (`!declared`). The F128 engine verb keys its softcall
    // path on a NON-NULL return here — a target with F128 but zero rows
    // returns nullptr for every op and falls through to the encoded-width
    // wall (the load-bearing agnosticism condition: no target/format branch).
    [[nodiscard]] WideFloatSoftcall const*
    wideFloatSoftcall(WideFloatOp op) const noexcept {
        auto const idx = static_cast<std::size_t>(op);
        if (idx >= d_.wideFloatSoftcalls.size()) return nullptr;
        auto const& row = d_.wideFloatSoftcalls[idx];
        return row.declared ? &row : nullptr;
    }
    // The DT_NEEDED library the minted F128-softcall externs bind to, for the
    // object format `format`, or empty when the format declares none.
    //
    // Takes the KIND, never a name string: both call sites (program.cpp's merge
    // path and compile_pipeline.cpp's single-CU path) already hold an
    // `ObjectFormatKind` and used to stringify it just to feed a string lookup
    // that could silently miss. The round trip is gone — an unresolvable name
    // can no longer reach this function, because the loader could not have
    // stored one.
    [[nodiscard]] std::string_view
    wideFloatSoftcallLibrary(ObjectFormatKind format) const noexcept {
        auto const idx = static_cast<std::size_t>(format);
        if (idx >= d_.wideFloatSoftcallLibraryByFormat.size()) return {};
        return d_.wideFloatSoftcallLibraryByFormat[idx];
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

    // ── Bare-char signedness (TF-C56 + TF-C75) ────────────────────────────
    // THE one resolution point for "is bare `char` unsigned here". True ⇒ the
    // char→int promotion zero-extends; false ⇒ it sign-extends.
    //
    // ★ The OBJECT FORMAT is a REQUIRED argument, and there is deliberately NO
    // zero-argument overload. Bare-`char` signedness is a (processor ×
    // PLATFORM) fact, so a no-arg accessor could only return the processor
    // half — and a caller that forgot the format would silently get arm64's
    // `true` on Darwin, which is precisely the zero-vs-sign-extend miscompile
    // this shape exists to make unrepresentable. Requiring the argument moves
    // that error from "silently wrong output" to "does not compile".
    //
    // Resolution: the format's declared override if this target declared one
    // for that kind, else the target's own default. Both halves come from the
    // ONE `charIsUnsigned` key in the `.target.json`; no format schema
    // contributes. Selects on the enum ordinal only — never a format, arch, or
    // platform NAME.
    [[nodiscard]] bool charIsUnsigned(ObjectFormatKind format) const noexcept {
        auto const idx = static_cast<std::size_t>(format);
        // Bounds-check a corrupt/out-of-enum cast rather than index out of
        // range; an unrepresentable kind reads as "no override declared".
        if (idx < d_.charIsUnsignedByFormatDeclared.size()
            && d_.charIsUnsignedByFormatDeclared[idx]) {
            return d_.charIsUnsignedByFormat[idx];
        }
        return d_.charIsUnsignedDefault;
    }

    // ── Per-architecture identity predefined macros (TF-C74) ──────
    // The target's `predefinedMacros` rows, in declaration order and
    // UNFILTERED (the per-entry `availableObjectFormats` filter is
    // applied ONCE downstream, in `mergePredefinedMacros`, alongside
    // the language's — one filter, so the four preprocessor seed
    // sites can never disagree). EMPTY for a target that declares
    // none ⇒ the preprocessor's effective list is byte-identical to
    // the language-only list.
    [[nodiscard]] std::span<PredefinedMacroDef const>
    predefinedMacros() const noexcept {
        return d_.predefinedMacros;
    }

    // ── Default assembly language (D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET) ──
    // The NAME of the shipped source-language document that spells this
    // processor's assembly, or EMPTY when the target declares none. A name
    // only — never a grammar fact; see the data member for the measurement
    // that settled that line.
    //
    // The driver consumes this as the per-target source language when the
    // CALLER NAMED NONE (`--language` omitted). Explicit `--language` wins:
    // that is the correct interface when a file is written for one CPU, and it
    // is what `examples/asm/*/expected.json` uses.
    [[nodiscard]] std::string_view
    defaultAssemblyLanguage() const noexcept {
        return d_.defaultAssemblyLanguage;
    }

    // ── TLS identity (TLS C1, D-CSUBSET-THREAD-LOCAL) ─────────────
    // The target's static-TLS layout convention (Variant I/II +
    // tcbHeaderBytes), or nullopt if the target declared no `tls`
    // block (the walker's tpoff helper then fails loud on any TLS
    // symbol — absence is the capability signal).
    [[nodiscard]] std::optional<TlsIdentity> const& tlsIdentity() const noexcept {
        return d_.tls;
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
    // Lowercase 64-hex SHA-256 of the document bytes — see `contentDigest()`.
    // Written ONLY by `loadFromText` (a static member, so no friend is
    // needed); every other construction path leaves it empty on purpose.
    std::string contentDigest_;

    detail::TargetSchemaData d_;
};

} // namespace dss
