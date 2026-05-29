#pragma once

#include "core/export.hpp"
#include "core/substrate/arena_attribute.hpp"
#include "core/substrate/arena_container.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"

#include <cstdint>
#include <optional>
#include <span>
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
    Lir(TargetSchemaId target, InstArena instArena, BlockArena blockArena,
        FuncArena funcArena, std::vector<LirOperand> operandPool,
        std::vector<LirBlockId> succPool) noexcept;

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

private:
    TargetSchemaId            target_{};
    InstArena                 instArena_;
    BlockArena                blockArena_;
    FuncArena                 funcArena_;
    std::vector<LirOperand>   operandPool_;
    std::vector<LirBlockId>   succPool_;
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
    LirBuilder& operator=(LirBuilder&&) noexcept = default;

    [[nodiscard]] LirModuleId         id()       const noexcept { return moduleId_; }
    [[nodiscard]] TargetSchemaId      targetId() const noexcept { return target_.id(); }
    [[nodiscard]] TargetSchema const& schema()   const noexcept { return target_; }

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
    LirInstId addBr(std::uint16_t opcode, LirBlockId target);
    // `payload` is the per-opcode scalar field on the emitted inst —
    // for conditional branches it carries the `TargetCondCode` enum (jcc
    // condition). Default 0 preserves the cycle-1/2 zero-payload contract
    // for callers that don't care about the condition channel.
    LirInstId addCondBr(std::uint16_t opcode,
                        std::span<LirOperand const> operands,
                        LirBlockId ifTrue, LirBlockId ifFalse,
                        std::uint32_t payload = 0);
    LirInstId addReturn(std::uint16_t opcode,
                        std::span<LirOperand const> operands);
    // Zero-successor terminator that is NOT a return — separated from
    // `addReturn` so the call-site spelling matches the semantics. AS1
    // maps to x86_64 ud2 / ARM64 brk / WASM unreachable.
    LirInstId addUnreachable(std::uint16_t opcode);

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

} // namespace dss
