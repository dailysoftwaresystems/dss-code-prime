#pragma once

#include "core/export.hpp"
#include "core/substrate/transparent_string_hash.hpp"
#include "core/types/grammar_schema.hpp"   // ConfigDiagnostic + LoadResult
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

[[nodiscard]] constexpr std::string_view targetAbiModelName(TargetAbiModel m) noexcept {
    switch (m) {
        case TargetAbiModel::RegisterMachine: return "register-machine";
        case TargetAbiModel::OperandStack:    return "operand-stack";
        case TargetAbiModel::ResultId:        return "result-id";
    }
    return "register-machine";
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

[[nodiscard]] constexpr std::string_view targetCondCodeName(TargetCondCode c) noexcept {
    switch (c) {
        case TargetCondCode::Eq:  return "eq";
        case TargetCondCode::Ne:  return "ne";
        case TargetCondCode::Slt: return "slt";
        case TargetCondCode::Sle: return "sle";
        case TargetCondCode::Sgt: return "sgt";
        case TargetCondCode::Sge: return "sge";
        case TargetCondCode::Ult: return "ult";
        case TargetCondCode::Ule: return "ule";
        case TargetCondCode::Ugt: return "ugt";
        case TargetCondCode::Uge: return "uge";
    }
    return "<invalid>";  // fail-loud on enum drift (post-switch fallback)
}

// Result-type discipline mirrors MIR's `MirResultRule`.
enum class TargetResultRule : std::uint8_t {
    None,      // never defines a value
    Value,     // always defines a value
    Optional,  // may define a value (e.g. a call to a non-void fn)
};

[[nodiscard]] constexpr std::string_view targetResultRuleName(TargetResultRule r) noexcept {
    switch (r) {
        case TargetResultRule::None:     return "none";
        case TargetResultRule::Value:    return "value";
        case TargetResultRule::Optional: return "optional";
    }
    return "none";
}

// Register-class envelope (universal — every target maps its concrete
// register classes to this set; the LIR substrate sees only the envelope).
// Mirrors `LirRegClass` in `src/lir/lir_reg.hpp` — kept as a separate
// definition here so `core/types/target_schema.hpp` does not need to
// pull in the LIR substrate (header-include direction is core ← LIR).
enum class TargetRegClass : std::uint8_t {
    None  = 0,
    GPR   = 1,
    FPR   = 2,
    VR    = 3,
    Flags = 4,
};

[[nodiscard]] constexpr std::string_view targetRegClassName(TargetRegClass c) noexcept {
    switch (c) {
        case TargetRegClass::None:  return "none";
        case TargetRegClass::GPR:   return "gpr";
        case TargetRegClass::FPR:   return "fpr";
        case TargetRegClass::VR:    return "vr";
        case TargetRegClass::Flags: return "flags";
    }
    return "none";
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

    // ARM64 AAPCS64 carries the return address in a dedicated link
    // register (LR / x30) rather than on the stack; ML7 callconv
    // lowering checks `linkRegister.has_value()` to decide whether
    // to spill LR in the prologue. Empty for x86_64.
    //
    // The struct shape co-locates the JSON-side `name` (kept for
    // diagnostics) with the loader-resolved `ordinal` (cached at JSON
    // load time so ML7 does not re-resolve per function). Both fields
    // are populated atomically by the loader: the type cannot represent
    // a "name set, ordinal unset" state. `validate()` guarantees the
    // resolution succeeded when the optional is engaged.
    struct LinkRegisterRef {
        std::string   name;
        std::uint16_t ordinal = 0;
    };
    std::optional<LinkRegisterRef> linkRegister;
};

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
    std::string      mnemonic;
    TargetResultRule result         = TargetResultRule::None;
    bool             isTerminator   = false;
    bool             hasSideEffects = false;
    std::uint8_t     minOperands    = 0;
    std::uint8_t     maxOperands    = 0;
    std::uint8_t     minSuccessors  = 0;
    std::uint8_t     maxSuccessors  = 0;
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

    // Physical register file (cycle 2b). Empty when the target JSON
    // omits the `registers` array — keeps the cycle 2a-shape targets
    // valid until ML6 regalloc requires the section.
    std::vector<TargetRegisterInfo> registers;
    substrate::TransparentStringMap<std::uint16_t> registerIndex;

    // Calling conventions (cycle 2b). Same optional-for-now discipline
    // as `registers` — ML7 callconv lowering will require ≥1 entry.
    std::vector<TargetCallingConvention> callingConventions;
    substrate::TransparentStringMap<std::uint16_t> callingConventionIndex;

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
    [[nodiscard]] std::string_view  version()  const noexcept { return d_.version; }
    [[nodiscard]] TargetAbiModel    abiModel() const noexcept { return d_.abiModel; }

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
        return info != nullptr && info->isTerminator;
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
