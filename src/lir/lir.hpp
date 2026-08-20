#pragma once

#include "core/export.hpp"
#include "core/substrate/arena_attribute.hpp"
#include "core/substrate/arena_container.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir_literal_pool.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

// `Lir` (ML5) — the frozen, immutable low-level IR module. Per-target
// instruction set carried as a `TargetSchemaId` (runtime tag minted
// when the target's JSON config was loaded via `TargetSchema::
// loadShipped`). Cycle 2 pivot: targets are config files in
// `src/dss-config/targets/*.target.json`, NOT C++ code; the LIR
// substrate is fully target-blind and looks up opcode info via the
// schema attached to the module.
//
// Mirrors `Mir` (ML1): three arenas (instructions, blocks, functions)
// tagged by one `LirModuleId`, module-owned pools for non-contiguous
// lists (operand pool, succ pool). Build-once-freeze: `LirBuilder` is
// single-use and `finish()` returns the immutable module.
//
// Cycle 2 scope: substrate refactored to consume `TargetSchema`. No
// instruction selection (cycle 3), no register allocation (ML6), no
// calling-convention lowering (ML7), no text format (ML8). Production
// code using LIR today would be:
//   `TargetSchema::loadShipped("x86_64")` → LirBuilder(schema) →
//   MIR→LIR isel (cycle 3) → Lir → ML6's regalloc → encoded bytes.

namespace dss {

class DSS_EXPORT Lir {
public:
    using InstArena  = substrate::ArenaContainer<detail::LirInst,  LirInstId,  LirModuleId>;
    using BlockArena = substrate::ArenaContainer<detail::LirBlock, LirBlockId, LirModuleId>;
    using FuncArena  = substrate::ArenaContainer<detail::LirFunc,  LirFuncId,  LirModuleId>;

    // Substrate `Arena` concept surface: the instruction tier (so
    // `LirAttribute<T>` = `ArenaAttribute<Lir, T>` keys on LirInstId).
    using IdType  = LirInstId;
    using TagType = LirModuleId;

    Lir() noexcept = default;
    // ⚠ The two module-level SIDE STRUCTURES (`literalPool`,
    // `regConstraintPool`) are trailing ctor parameters rather than
    // optional-with-default deliberately: a third side structure added
    // with a default would be silently omitted by `LirBuilder::finish()`,
    // which is the same class of drop this pair's verifier rules exist to
    // catch. `LirBuilder::finish()` is the only caller (✔MEASURED
    // 2026-08-14: `grep -rn "Lir{" src/ tests/` finds one construction
    // site plus one `Lir{}` empty-result sentinel), so the cost of
    // keeping them mandatory is one call site.
    Lir(TargetSchemaId target, InstArena instArena, BlockArena blockArena,
        FuncArena funcArena, std::vector<LirOperand> operandPool,
        std::vector<LirBlockId> succPool,
        LirLiteralPool literalPool,
        LirRegConstraintPool regConstraintPool) noexcept;

    Lir(Lir const&)            = delete;
    Lir& operator=(Lir const&) = delete;
    Lir(Lir&&) noexcept;
    Lir& operator=(Lir&&) noexcept;

    // ── identity / introspection ──
    [[nodiscard]] LirModuleId    id()          const noexcept { return instArena_.id(); }
    [[nodiscard]] TargetSchemaId targetId()    const noexcept { return target_; }
    [[nodiscard]] std::size_t nodeCount()  const noexcept { return instArena_.nodeCount(); }
    [[nodiscard]] bool        empty()      const noexcept { return instArena_.empty(); }
    [[nodiscard]] std::size_t instCount()  const noexcept { return nodeCount(); }
    [[nodiscard]] std::size_t blockCount() const noexcept { return blockArena_.nodeCount(); }
    [[nodiscard]] std::size_t funcCount()  const noexcept { return funcArena_.nodeCount(); }

    [[nodiscard]] BlockArena const& blockArena() const noexcept { return blockArena_; }
    [[nodiscard]] FuncArena  const& funcArena()  const noexcept { return funcArena_; }

    // ── instruction accessors ──
    [[nodiscard]] std::uint16_t            instOpcode(LirInstId id) const { return instArena_.at(id).opcode; }
    [[nodiscard]] std::uint8_t             instFlags(LirInstId id)  const { return instArena_.at(id).flags; }
    [[nodiscard]] LirReg                   instResult(LirInstId id) const { return instArena_.at(id).result; }
    [[nodiscard]] std::uint32_t            instPayload(LirInstId id) const { return instArena_.at(id).payload; }
    [[nodiscard]] std::span<LirOperand const> instOperands(LirInstId id) const;

    // ── per-instruction register constraints (inline-asm carrier) ──
    //
    // The RAW handle: `kLirNoRegConstraints` (0) when the instruction
    // declares none, otherwise a 1-based index into `regConstraintPool()`.
    // Exposed unresolved for the verifier + the `.dsslir` codec, which
    // must be able to SEE a dangling handle rather than abort on it.
    [[nodiscard]] std::uint32_t instRegConstraintHandle(LirInstId id) const {
        return instArena_.at(id).regConstraints;
    }
    // Resolved: `nullptr` when the instruction declares no
    // per-instruction constraints. Mirrors `TargetSchema::opcodeInfo`'s
    // nullptr-for-absent convention. Aborts on a DANGLING handle (a
    // producer bug — `LirRegConstraintPool::at`'s contract), which is why
    // `verifyLirSideStructures` reports it as a diagnostic first.
    [[nodiscard]] ImplicitRegisterConstraint const*
    instRegConstraints(LirInstId id) const {
        std::uint32_t const h = instRegConstraintHandle(id);
        if (h == kLirNoRegConstraints) return nullptr;
        return &regConstraintPool_.at(lirRegConstraintIndexForHandle(h));
    }

    // ── block accessors ──
    [[nodiscard]] std::uint32_t                blockInstCount(LirBlockId id) const { return blockArena_.at(id).instCount; }
    [[nodiscard]] LirFuncId                    blockFunc(LirBlockId id) const;
    [[nodiscard]] LirInstId                    blockInstAt(LirBlockId id, std::uint32_t i) const;
    [[nodiscard]] LirInstId                    blockTerminator(LirBlockId id) const;
    [[nodiscard]] std::span<LirBlockId const>  blockSuccessors(LirBlockId id) const;

    // ── function accessors ──
    [[nodiscard]] SymbolId      funcSymbol(LirFuncId id)    const { return SymbolId{funcArena_.at(id).symbol}; }
    [[nodiscard]] std::uint32_t funcBlockCount(LirFuncId id) const { return funcArena_.at(id).blockCount; }
    [[nodiscard]] std::uint32_t funcNumVRegs(LirFuncId id)   const { return funcArena_.at(id).numVRegs; }
    [[nodiscard]] LirBlockId    funcBlockAt(LirFuncId id, std::uint32_t i) const;
    [[nodiscard]] LirBlockId    funcEntry(LirFuncId id) const;

    // ── module-level iteration ──
    [[nodiscard]] std::size_t moduleFuncCount() const noexcept;
    [[nodiscard]] LirFuncId   funcAt(std::uint32_t i) const;

    // ── literal pool (cycle 3c) ──
    [[nodiscard]] LirLiteralValue const& literalValue(std::uint32_t i) const {
        return literalPool_.at(i);
    }
    [[nodiscard]] LirLiteralPool const& literalPool() const noexcept { return literalPool_; }

    // ── register-constraint pool (inline-asm carrier) ──
    [[nodiscard]] LirRegConstraintPool const& regConstraintPool() const noexcept {
        return regConstraintPool_;
    }

private:
    TargetSchemaId            target_{};
    InstArena                 instArena_;
    BlockArena                blockArena_;
    FuncArena                 funcArena_;
    std::vector<LirOperand>   operandPool_;
    std::vector<LirBlockId>   succPool_;
    LirLiteralPool            literalPool_;
    LirRegConstraintPool      regConstraintPool_;
};

static_assert(substrate::Arena<Lir>,
              "Lir must satisfy substrate::Arena so LirAttribute<T> can bind to it");

template <class T>
using LirAttribute      = substrate::ArenaAttribute<Lir, T>;
template <class T>
using LirBlockAttribute = substrate::ArenaAttribute<Lir::BlockArena, T>;
template <class T>
using LirFuncAttribute  = substrate::ArenaAttribute<Lir::FuncArena, T>;

// ── LirBuilder ──────────────────────────────────────────────────────
//
// Mirrors MirBuilder's create-then-fill discipline. Block CREATION
// (`createBlock` returns an id) is separated from block FILLING
// (`beginBlock` opens for inst emission) so forward branches can
// target a block before its body is materialized.
class DSS_EXPORT LirBuilder {
public:
    // The SCHEMA must outlive the builder (the schema's opcode-info
    // table is read on every `addInst`/terminator call to validate
    // the opcode + classify terminator-ness). The schema's lifetime
    // is typically managed by a `shared_ptr` higher up; the builder
    // holds a reference, NOT ownership.
    explicit LirBuilder(TargetSchema const& schema);

    LirBuilder(LirBuilder const&)            = delete;
    LirBuilder& operator=(LirBuilder const&) = delete;
    LirBuilder(LirBuilder&&) noexcept        = default;
    // Move-assignment must be deleted explicitly: `target_` is a
    // reference member that cannot be re-bound. The default move-
    // assign would be implicitly deleted by the compiler and clang's
    // `-Wdefaulted-function-deleted` flags the `= default` as
    // misleading. The single move-ctor + deleted assignment matches
    // the "single-use builder consumed by `finish() &&`" contract:
    // a moved-from builder is destructed, never re-assigned.
    LirBuilder& operator=(LirBuilder&&) noexcept = delete;

    [[nodiscard]] LirModuleId         id()       const noexcept { return moduleId_; }
    [[nodiscard]] TargetSchemaId      targetId() const noexcept { return target_.id(); }
    [[nodiscard]] TargetSchema const& schema()   const noexcept { return target_; }

    // Currently-open block, or default-constructed (invalid) id when
    // no block is open. Mirrors `MirBuilder::currentlyOpenBlock` —
    // exposed so lowerers can pin the "first emission must land in
    // the entry-time block" invariant (ML5 cycle-3e deferral D-3e.1
    // for switch-lowering's first compare).
    [[nodiscard]] LirBlockId openBlock() const noexcept { return openBlock_; }

    // ★★★ HAS THE OPEN BLOCK ALREADY BEEN SEALED? The read half of the flag
    // every terminator builder sets, and the sibling of `instResult`/
    // `orInstFlags`: it exists for the same reason those two do — **a producer
    // that did not call the terminator itself still has to know one was
    // emitted**. `mir_to_lir`'s `asm goto` expansion is that producer. The
    // instructions of an `__asm__` template are appended by the SHARED assembly
    // engine (`lowerAsmTemplateToLirRun`), which returns only a bool, so the
    // embedding lowering cannot see whether the template's last statement was
    // an unconditional branch.
    //
    // ⚠ WHY ASKING IS NOT OPTIONAL: BOTH ANSWERS ABORT IF GUESSED WRONG.
    // Appending to a sealed block hits `appendInst_`'s *"block already
    // terminated"* fatal; `beginBlock` on an UNSEALED one hits its *"current
    // block has no terminator"* fatal. Those guards are producer-contract
    // aborts, and the producer here is fed by USER TEXT (`__asm__ goto ("jmp
    // %l[done]" …)` seals the block; `__asm__ goto ("" …)` does not) — so
    // without this reader the choice between the two would be a coin flip that
    // kills the process on a legal program. ✔MEASURED: `AsmLoweringHost`
    // exposes the same question to the ENGINE as `blockIsTerminated()`; this is
    // the CALLER's half of it, and both shipped hosts answer it from a bool
    // they maintain by hand because the builder did not publish its own.
    //
    // ⓘ Read-only, and deliberately not paired with a setter: only the
    // terminator builders may set the flag.
    [[nodiscard]] bool openBlockIsTerminated() const noexcept {
        return openBlockHasTerminator_;
    }

    // Open a function. Closes the current function first (which
    // requires its current block be terminated + the function have
    // ≥1 block). `symbol` is the declared SymbolId of the function.
    LirFuncId addFunction(SymbolId symbol);

    // Reserve a basic block in the current function WITHOUT opening
    // it. Block must later be opened with `beginBlock` and given a
    // terminator before the function closes.
    LirBlockId createBlock();

    // Open a previously-created block for instruction emission.
    void beginBlock(LirBlockId block);

    // Mint a fresh virtual register of the given class. The id is
    // monotonic per-function (resets when `addFunction` is called).
    [[nodiscard]] LirReg newVReg(LirRegClass cls);

    // Append a non-terminator instruction to the current block.
    // `result` is the value-defining virtual register (`InvalidLirReg`
    // for value-less ops). `operands` are passed verbatim into the
    // module's operand pool.
    LirInstId addInst(std::uint16_t opcode, LirReg result,
                      std::span<LirOperand const> operands,
                      std::uint32_t payload = 0,
                      std::uint8_t  flags   = 0);

    // ── terminators (each seals the current block) ──
    //
    // ALL terminator builders accept optional `payload` + `flags` so the
    // text format round-trip (ML8) is lossless across every opcode:
    // `addBr`/`addReturn`/`addUnreachable` previously hard-coded these
    // to 0, silently dropping any non-zero state on parse. Defaulted to
    // 0 so existing in-process call sites stay source-compatible.
    LirInstId addBr(std::uint16_t opcode, LirBlockId target,
                    std::uint32_t payload = 0, std::uint8_t flags = 0);
    // `payload` carries the per-opcode scalar (for conditional branches:
    // the `TargetCondCode` enum / jcc condition). `flags` carries the
    // 8-bit per-inst flag byte used by future annotation passes.
    LirInstId addCondBr(std::uint16_t opcode,
                        std::span<LirOperand const> operands,
                        LirBlockId ifTrue, LirBlockId ifFalse,
                        std::uint32_t payload = 0,
                        std::uint8_t  flags   = 0);
    LirInstId addReturn(std::uint16_t opcode,
                        std::span<LirOperand const> operands,
                        std::uint32_t payload = 0,
                        std::uint8_t  flags   = 0);
    // D-CSUBSET-COMPUTED-GOTO: indirect branch (x86 jmp r/m64 / arm64 BR Xn).
    // `operands` = [the address register]; `targets` = every address-taken
    // successor block (variadic, like a Switch). Seals the open block.
    LirInstId addIndirectBr(std::uint16_t opcode,
                            std::span<LirOperand const> operands,
                            std::span<LirBlockId const> targets,
                            std::uint32_t payload = 0,
                            std::uint8_t  flags   = 0);
    // Zero-successor terminator that is NOT a return — separated from
    // `addReturn` so the call-site spelling matches the semantics. AS1
    // maps to x86_64 ud2 / ARM64 brk / WASM unreachable.
    LirInstId addUnreachable(std::uint16_t opcode,
                             std::uint32_t payload = 0,
                             std::uint8_t  flags   = 0);

    // Append a wide literal to the module's pool; returns the index
    // suitable for `LirOperand::makeLiteralIndex(...)`. Cycle 3c uses
    // this for int64/double/string literals that don't fit in the
    // 8-byte operand POD's `immInt32` arm.
    [[nodiscard]] std::uint32_t literalPoolAdd(LirLiteralValue value);

    // ── per-instruction register constraints ──────────────────────
    //
    // Append a constraint set to the module's pool; returns its INDEX
    // (feed it straight to `setInstRegConstraints`). The builder RESOLVES
    // every register name through the active target schema and overwrites
    // the ordinal arrays from that resolution:
    //
    // ★ the caller supplies NAMES ONLY and cannot desynchronise the two
    // representations. `ImplicitRegisterConstraint` carries names AND
    // parallel ordinals precisely because the JSON loader validates one
    // into the other; a second producer that filled both by hand would be
    // free to fill them INCONSISTENTLY, and a name/ordinal disagreement
    // is invisible to every consumer (regalloc reads ordinals, diagnostics
    // and the `.dsslir` codec read names — they would simply describe
    // different registers). Resolving here makes that state unreachable.
    // An unresolvable name ABORTS: it is the same schema-misconfiguration
    // the loader fails loud on, and the builder has no reporter — the same
    // contract every other `LirBuilder` guard uses (invalid opcode,
    // cross-module block id, unterminated block).
    //
    // ⚠ THEREFORE A PRODUCER FED BY *USER* TEXT MUST PRE-VALIDATE. An
    // `asm("" ::: "nosuchreg")` carries a register name the user chose, and
    // a bad name in user code must be a DIAGNOSTIC, never a compiler
    // crash. Resolve every name through `schema.registerByName` and report
    // before you get here; `lir_text.cpp`'s `constraintNamesResolve` is the
    // reference implementation of exactly that guard, written for the same
    // reason (a malformed `.dsslir` may not kill the process).
    [[nodiscard]] std::uint32_t
    regConstraintPoolAdd(ImplicitRegisterConstraint constraint);

    // How many constraint sets the pool holds so far. Exposed for the
    // `.dsslir` reader, which must range-check a handle it read from text
    // and REPORT rather than abort — `setInstRegConstraints`'s out-of-
    // range guard is a producer contract (it aborts), and a malformed
    // input file must never kill the process.
    [[nodiscard]] std::size_t regConstraintPoolSize() const noexcept {
        return regConstraintPool_.size();
    }

    // Attach a pool entry to an already-appended instruction. Separated
    // from `addInst` on purpose: threading a 7th parameter through the
    // six `add*` entrypoints would put an optional-with-default on every
    // one of them, and a defaulted "no constraints" is exactly the silent
    // drop this carrier has to survive. `inst` must belong to this
    // builder and `poolIndex` must be in range — both abort otherwise
    // (builder-contract discipline, same as `beginBlock`'s guards).
    void setInstRegConstraints(LirInstId inst, std::uint32_t poolIndex);

    // OR bits into an already-appended instruction's flag byte
    // (D-LIR-EARLYCLOBBER-FLAG-UNSETTABLE-AFTER-EMISSION). The sibling
    // of `setInstRegConstraints`, and it exists for the same reason: a
    // producer that did not call `addInst` itself still has a fact to
    // record about the instruction. The inline-asm expansion is that
    // producer — the instruction writing an unpinned `"=&r"` output is
    // emitted by the SHARED assembly engine, so the tier that knows the
    // operand is earlyclobber never holds the `flags` argument.
    //
    // ★★ OR-IN, NEVER ASSIGN, AND THAT IS THE WHOLE POINT OF THE
    // SIGNATURE. `flags` already carries the WIDTH selector
    // (`kLirInstFlagWidth32/8/16`), which the engine sets from the
    // operand's register spelling. A plain `setInstFlags` would let a
    // caller adding one annotation bit silently clear the width and
    // re-run a 32-bit operation at 64 bits — a miscompile with no
    // diagnostic, and exactly the class of bug this carrier exists to
    // prevent. There is deliberately no clearing counterpart: no
    // producer has ever needed to UNSET a flag, and offering it would
    // make the same overwrite reachable in one call.
    //
    // `inst` must belong to this builder — aborts otherwise, the same
    // builder-contract discipline as `setInstRegConstraints`.
    void orInstFlags(LirInstId inst, std::uint8_t flags);

    // The result register of an already-appended instruction. The read
    // half `orInstFlags` needs to be usable at all: a producer that did
    // not call `addInst` cannot know WHICH of the instructions a shared
    // engine appended defines a given vreg, and the ids are contiguous,
    // so the only way to find it is to ask. `Lir` exposes the same
    // reader on the frozen module; this is the mid-build one. Aborts on
    // a cross-module id, like every other id-taking builder method.
    [[nodiscard]] LirReg instResult(LirInstId inst) const;

    // The most recently appended instruction. Exists so a pass can carry
    // side data onto a terminator: the terminator emitters return an id,
    // but the SHARED dispatch `lir_pass_util::emitTerminator` returns a
    // bool (it routes to one of five builder entrypoints), so its callers
    // otherwise have no handle on what it just emitted. Aborts if nothing
    // has been appended yet.
    [[nodiscard]] LirInstId lastInst() const;

    // Consume the builder, returning the frozen Lir module. Aborts
    // on any contract violation (open function with no terminated
    // block; created-but-never-opened block; etc.) — same discipline
    // as `MirBuilder::finish()`.
    [[nodiscard]] Lir finish() &&;

private:
    void closeFunction_();
    void appendInst_(detail::LirInst const& inst);
    void recordSuccessors_(std::span<LirBlockId const> succs);
    [[nodiscard]] std::uint32_t appendOperands_(std::span<LirOperand const> operands);

    LirModuleId             moduleId_;
    TargetSchema const&     target_;
    substrate::ArenaBuilder<detail::LirInst,  LirInstId,  LirModuleId> instArena_;
    substrate::ArenaBuilder<detail::LirBlock, LirBlockId, LirModuleId> blockArena_;
    substrate::ArenaBuilder<detail::LirFunc,  LirFuncId,  LirModuleId> funcArena_;
    std::vector<LirOperand> operandPool_;
    std::vector<LirBlockId> succPool_;
    LirLiteralPool          literalPool_;
    LirRegConstraintPool    regConstraintPool_;

    // Per-function state, reset by `addFunction`.
    LirFuncId  openFunc_{};
    LirBlockId openBlock_{};
    bool       openBlockHasTerminator_ = false;
    std::uint32_t openFuncBlockStart_  = 0;
    std::uint32_t nextVReg_            = 1;  // 0 reserved for invalid

    // Per-function block creation tracking — we need to know which
    // blocks belong to the open function so closeFunction_ can scan
    // them for the never-terminated check.
    std::vector<LirBlockId> openFuncBlocks_;
};

// Pin the move-semantics contract at compile time. The `LirBuilder`'s
// reference member (`target_`) makes move-assignment impossible to
// implement correctly — the contract is "single-use, consumed by
// `finish() &&`". A future change that re-defaults the move-assign
// would silently regress clang's `-Wdefaulted-function-deleted` to
// "implicitly deleted" again; the static_assert pins the contract
// independently of compiler warnings. Type-design Q4 fold (3rd-order
// audit on 39897eb).
static_assert(!std::is_move_assignable_v<LirBuilder>,
              "LirBuilder must not be move-assignable — `target_` "
              "is a reference and the builder is single-use "
              "(consumed by `finish() &&`).");
static_assert(std::is_move_constructible_v<LirBuilder>,
              "LirBuilder must remain move-constructible to support "
              "transfer-by-rvalue (`Lir lir = std::move(b).finish()` "
              "patterns + factory returns).");

} // namespace dss
