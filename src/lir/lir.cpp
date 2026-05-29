#include "lir/lir.hpp"

#include "core/substrate/mint_monotonic_id.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace dss {

namespace {

[[noreturn]] void lirFatal(char const* what) {
    std::fputs("dss::Lir fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace

// ── Lir ──────────────────────────────────────────────────────────

Lir::Lir(TargetSchemaId target, InstArena instArena, BlockArena blockArena,
         FuncArena funcArena, std::vector<LirOperand> operandPool,
         std::vector<LirBlockId> succPool,
         LirLiteralPool literalPool) noexcept
    : target_(target),
      instArena_(std::move(instArena)),
      blockArena_(std::move(blockArena)),
      funcArena_(std::move(funcArena)),
      operandPool_(std::move(operandPool)),
      succPool_(std::move(succPool)),
      literalPool_(std::move(literalPool)) {
    // Cross-arena module-id check — all four arenas must share one tag.
    if (instArena_.id() != blockArena_.id()
     || instArena_.id() != funcArena_.id()) {
        lirFatal("Lir ctor: arena module ids disagree");
    }
}

Lir::Lir(Lir&&) noexcept = default;
Lir& Lir::operator=(Lir&&) noexcept = default;

std::span<LirOperand const> Lir::instOperands(LirInstId id) const {
    auto const& inst = instArena_.at(id);
    if (inst.operandStart > operandPool_.size()
     || inst.operandStart + inst.operandCount > operandPool_.size()) {
        lirFatal("Lir::instOperands: operand range out of pool");
    }
    return std::span<LirOperand const>(
        operandPool_.data() + inst.operandStart, inst.operandCount);
}

LirFuncId Lir::blockFunc(LirBlockId id) const {
    return LirFuncId{blockArena_.at(id).func, id.arenaTag};
}

LirInstId Lir::blockInstAt(LirBlockId id, std::uint32_t i) const {
    auto const& blk = blockArena_.at(id);
    if (i >= blk.instCount) lirFatal("Lir::blockInstAt: index out of range");
    return LirInstId{blk.instStart + i, id.arenaTag};
}

LirInstId Lir::blockTerminator(LirBlockId id) const {
    auto const& blk = blockArena_.at(id);
    if (blk.instCount == 0) lirFatal("Lir::blockTerminator: empty block");
    return LirInstId{blk.instStart + blk.instCount - 1, id.arenaTag};
}

std::span<LirBlockId const> Lir::blockSuccessors(LirBlockId id) const {
    auto const& blk = blockArena_.at(id);
    if (blk.succStart > succPool_.size()
     || blk.succStart + blk.succCount > succPool_.size()) {
        lirFatal("Lir::blockSuccessors: succ range out of pool");
    }
    return std::span<LirBlockId const>(
        succPool_.data() + blk.succStart, blk.succCount);
}

LirBlockId Lir::funcBlockAt(LirFuncId id, std::uint32_t i) const {
    auto const& fn = funcArena_.at(id);
    if (i >= fn.blockCount) lirFatal("Lir::funcBlockAt: index out of range");
    return LirBlockId{fn.blockStart + i, id.arenaTag};
}

LirBlockId Lir::funcEntry(LirFuncId id) const {
    auto const& fn = funcArena_.at(id);
    if (fn.blockCount == 0) lirFatal("Lir::funcEntry: zero-block function");
    return LirBlockId{fn.blockStart, id.arenaTag};
}

std::size_t Lir::moduleFuncCount() const noexcept {
    auto const n = funcArena_.nodeCount();
    return (n == 0) ? 0u : (n - 1);  // slot 0 = sentinel
}

LirFuncId Lir::funcAt(std::uint32_t i) const {
    if (i >= moduleFuncCount()) lirFatal("Lir::funcAt: index out of range");
    return LirFuncId{i + 1, id().v};
}

// ── LirBuilder ──────────────────────────────────────────────────

LirBuilder::LirBuilder(TargetSchema const& schema)
    : moduleId_(substrate::mintMonotonicId<LirModuleId>()),
      target_(schema),
      instArena_(moduleId_),
      blockArena_(moduleId_),
      funcArena_(moduleId_) {
    if (!schema.id().valid()) {
        lirFatal("LirBuilder: TargetSchema has an invalid id");
    }
    if (schema.opcodeCount() == 0) {
        lirFatal("LirBuilder: TargetSchema has no opcodes (slot-0 sentinel required)");
    }
    // ArenaBuilder(Tag) already reserves slot 0 — no explicit add needed.
}

LirFuncId LirBuilder::addFunction(SymbolId symbol) {
    if (openFunc_.valid()) closeFunction_();
    detail::LirFunc fn;
    fn.symbol     = symbol.v;
    fn.blockStart = static_cast<std::uint32_t>(blockArena_.size());
    fn.blockCount = 0;
    fn.numVRegs   = 0;
    LirFuncId const id = funcArena_.addNode(fn);
    openFunc_           = id;
    openBlock_          = {};
    openBlockHasTerminator_ = false;
    openFuncBlockStart_ = fn.blockStart;
    nextVReg_           = 1;
    openFuncBlocks_.clear();
    return id;
}

LirBlockId LirBuilder::createBlock() {
    if (!openFunc_.valid()) lirFatal("LirBuilder::createBlock: no open function");
    detail::LirBlock blk;
    blk.func      = openFunc_.v;
    // Sentinel value for "block created but never `beginBlock`'d".
    // `beginBlock` sets the real `instStart` and resets `instCount` to 0;
    // a second `beginBlock` on the same block is caught because
    // `instStart` was already non-sentinel.
    blk.instStart = UINT32_MAX;
    LirBlockId const id = blockArena_.addNode(blk);
    openFuncBlocks_.push_back(id);
    return id;
}

void LirBuilder::beginBlock(LirBlockId block) {
    if (!openFunc_.valid()) lirFatal("LirBuilder::beginBlock: no open function");
    if (openBlock_.valid() && !openBlockHasTerminator_) {
        lirFatal("LirBuilder::beginBlock: current block has no terminator");
    }
    if (block.arenaTag != moduleId_.v) {
        lirFatal("LirBuilder::beginBlock: cross-module block id");
    }
    auto& blk = blockArena_.at(block);
    // Guard against a second `beginBlock` on an already-opened block:
    // `createBlock` set `instStart = UINT32_MAX`; the first
    // `beginBlock` writes the real arena position; a second call
    // would silently clobber it and orphan previously-emitted insts.
    if (blk.instStart != UINT32_MAX) {
        lirFatal("LirBuilder::beginBlock: block has already been opened");
    }
    blk.instStart = static_cast<std::uint32_t>(instArena_.size());
    blk.instCount = 0;
    openBlock_              = block;
    openBlockHasTerminator_ = false;
}

LirReg LirBuilder::newVReg(LirRegClass cls) {
    if (!openFunc_.valid()) lirFatal("LirBuilder::newVReg: no open function");
    LirReg const r = makeVirtualReg(nextVReg_++, cls);
    // Also bump the function's vreg counter (read at freeze).
    auto& fn = funcArena_.at(openFunc_);
    fn.numVRegs = nextVReg_ - 1;
    return r;
}

std::uint32_t LirBuilder::appendOperands_(std::span<LirOperand const> operands) {
    std::uint32_t const start = static_cast<std::uint32_t>(operandPool_.size());
    for (auto const& o : operands) operandPool_.push_back(o);
    return start;
}

void LirBuilder::appendInst_(detail::LirInst const& inst) {
    if (!openBlock_.valid()) lirFatal("LirBuilder: no open block");
    if (openBlockHasTerminator_) lirFatal("LirBuilder: block already terminated");
    (void)instArena_.addNode(inst);
    auto& blk = blockArena_.at(openBlock_);
    ++blk.instCount;
}

void LirBuilder::recordSuccessors_(std::span<LirBlockId const> succs) {
    std::uint32_t const start = static_cast<std::uint32_t>(succPool_.size());
    for (auto const& s : succs) {
        if (s.arenaTag != moduleId_.v) {
            lirFatal("LirBuilder: successor id is from a different module");
        }
        succPool_.push_back(s);
    }
    auto& blk = blockArena_.at(openBlock_);
    blk.succStart = start;
    blk.succCount = static_cast<std::uint32_t>(succs.size());
}

LirInstId LirBuilder::addInst(std::uint16_t opcode, LirReg result,
                              std::span<LirOperand const> operands,
                              std::uint32_t payload, std::uint8_t flags) {
    if (opcode == 0) lirFatal("LirBuilder::addInst: Invalid opcode");
    // Per-target opcode-range guard: a caller passing an opcode from
    // outside the schema's opcode table silently freezes a mismatched
    // module today; the schema's `opcodeInfo` returns nullptr for
    // out-of-range opcodes.
    if (target_.opcodeInfo(opcode) == nullptr) {
        lirFatal("LirBuilder::addInst: opcode not registered for the active target schema");
    }
    detail::LirInst inst;
    inst.opcode       = opcode;
    inst.flags        = flags;
    inst.result       = result;
    inst.operandStart = appendOperands_(operands);
    inst.operandCount = static_cast<std::uint32_t>(operands.size());
    inst.payload      = payload;
    appendInst_(inst);
    return LirInstId{static_cast<std::uint32_t>(instArena_.size() - 1),
                     moduleId_.v};
}

LirInstId LirBuilder::addBr(std::uint16_t opcode, LirBlockId target,
                            std::uint32_t payload, std::uint8_t flags) {
    if (!target_.isTerminator(opcode)) {
        lirFatal("LirBuilder::addBr: opcode is not a terminator for this target");
    }
    LirOperand ref;
    ref.kind = LirOperandKind::BlockRef;
    ref.blockSlot = target.v;
    std::array<LirOperand, 1> ops{ref};
    LirInstId const id = addInst(opcode, InvalidLirReg, ops, payload, flags);
    std::array<LirBlockId, 1> succs{target};
    recordSuccessors_(succs);
    openBlockHasTerminator_ = true;
    return id;
}

LirInstId LirBuilder::addCondBr(std::uint16_t opcode,
                                std::span<LirOperand const> operands,
                                LirBlockId ifTrue, LirBlockId ifFalse,
                                std::uint32_t payload,
                                std::uint8_t  flags) {
    if (!target_.isTerminator(opcode)) {
        lirFatal("LirBuilder::addCondBr: opcode is not a terminator for this target");
    }
    LirInstId const id = addInst(opcode, InvalidLirReg, operands, payload, flags);
    std::array<LirBlockId, 2> succs{ifTrue, ifFalse};
    recordSuccessors_(succs);
    openBlockHasTerminator_ = true;
    return id;
}

LirInstId LirBuilder::addReturn(std::uint16_t opcode,
                                std::span<LirOperand const> operands,
                                std::uint32_t payload, std::uint8_t flags) {
    if (!target_.isTerminator(opcode)) {
        lirFatal("LirBuilder::addReturn: opcode is not a terminator for this target");
    }
    LirInstId const id = addInst(opcode, InvalidLirReg, operands, payload, flags);
    // No successors for return.
    auto& blk = blockArena_.at(openBlock_);
    blk.succStart = static_cast<std::uint32_t>(succPool_.size());
    blk.succCount = 0;
    openBlockHasTerminator_ = true;
    return id;
}

LirInstId LirBuilder::addUnreachable(std::uint16_t opcode,
                                     std::uint32_t payload,
                                     std::uint8_t  flags) {
    if (!target_.isTerminator(opcode)) {
        lirFatal("LirBuilder::addUnreachable: opcode is not a terminator for this target");
    }
    LirInstId const id = addInst(opcode, InvalidLirReg,
                                 std::span<LirOperand const>{}, payload, flags);
    auto& blk = blockArena_.at(openBlock_);
    blk.succStart = static_cast<std::uint32_t>(succPool_.size());
    blk.succCount = 0;
    openBlockHasTerminator_ = true;
    return id;
}

void LirBuilder::closeFunction_() {
    if (!openFunc_.valid()) return;
    // Validate every created block is opened + terminated.
    for (LirBlockId const b : openFuncBlocks_) {
        auto const& blk = blockArena_.at(b);
        if (blk.instStart == UINT32_MAX) {
            lirFatal("LirBuilder::closeFunction: block created but never `beginBlock`'d");
        }
        if (blk.instCount == 0) {
            lirFatal("LirBuilder::closeFunction: block opened but never filled");
        }
        // Verify the block's last instruction IS a terminator. The
        // builder's per-instruction `openBlockHasTerminator_` flag is
        // only meaningful for the currently-open block; older blocks
        // might have been closed without ever calling a terminator-
        // builder. Re-check via the opcode-info table here so the
        // invariant is enforced regardless of build path.
        std::uint32_t const lastSlot = blk.instStart + blk.instCount - 1;
        auto const& lastInst = instArena_.at(LirInstId{lastSlot, moduleId_.v});
        if (!target_.isTerminator(lastInst.opcode)) {
            lirFatal("LirBuilder::closeFunction: block's last instruction is not a terminator");
        }
    }
    // Set function's blockCount from the created list.
    auto& fn = funcArena_.at(openFunc_);
    fn.blockCount = static_cast<std::uint32_t>(openFuncBlocks_.size());
    openFunc_  = {};
    openBlock_ = {};
}

std::uint32_t LirBuilder::literalPoolAdd(LirLiteralValue value) {
    return literalPool_.add(std::move(value));
}

Lir LirBuilder::finish() && {
    if (openFunc_.valid()) closeFunction_();
    return Lir{
        target_.id(),
        std::move(instArena_).finish(),
        std::move(blockArena_).finish(),
        std::move(funcArena_).finish(),
        std::move(operandPool_),
        std::move(succPool_),
        std::move(literalPool_),
    };
}

} // namespace dss
