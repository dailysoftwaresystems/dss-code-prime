#include "lir/lir.hpp"

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

Lir::Lir(TargetId target, InstArena instArena, BlockArena blockArena,
         FuncArena funcArena, std::vector<LirOperand> operandPool,
         std::vector<LirBlockId> succPool) noexcept
    : target_(target),
      instArena_(std::move(instArena)),
      blockArena_(std::move(blockArena)),
      funcArena_(std::move(funcArena)),
      operandPool_(std::move(operandPool)),
      succPool_(std::move(succPool)) {
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

namespace {
// Monotonic LirModuleId counter (one tag per builder lifetime).
std::uint32_t mintLirModuleId() noexcept {
    static std::uint32_t counter = 0;
    return ++counter;
}
} // namespace

LirBuilder::LirBuilder(TargetId target)
    : moduleId_(mintLirModuleId()),
      target_(target),
      instArena_(moduleId_),
      blockArena_(moduleId_),
      funcArena_(moduleId_) {
    if (target == TargetId::Invalid) {
        lirFatal("LirBuilder: target cannot be Invalid");
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
    blk.func = openFunc_.v;
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
    // Mutate the block's instStart to the current instArena position.
    auto& blk = blockArena_.at(block);
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

LirInstId LirBuilder::addBr(std::uint16_t opcode, LirBlockId target) {
    LirOperand ref;
    ref.kind = LirOperandKind::BlockRef;
    ref.blockSlot = target.v;
    std::array<LirOperand, 1> ops{ref};
    LirInstId const id = addInst(opcode, InvalidLirReg, ops);
    std::array<LirBlockId, 1> succs{target};
    recordSuccessors_(succs);
    openBlockHasTerminator_ = true;
    return id;
}

LirInstId LirBuilder::addCondBr(std::uint16_t opcode,
                                std::span<LirOperand const> operands,
                                LirBlockId ifTrue, LirBlockId ifFalse) {
    LirInstId const id = addInst(opcode, InvalidLirReg, operands);
    std::array<LirBlockId, 2> succs{ifTrue, ifFalse};
    recordSuccessors_(succs);
    openBlockHasTerminator_ = true;
    return id;
}

LirInstId LirBuilder::addReturn(std::uint16_t opcode,
                                std::span<LirOperand const> operands) {
    LirInstId const id = addInst(opcode, InvalidLirReg, operands);
    // No successors for return.
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
        // A block is "filled" iff its instCount > 0 AND its last
        // instruction is its terminator (the open-block-has-terminator
        // flag tracked at instruction time). Cycle 1 has no
        // post-build verifier — the builder's invariant IS that
        // closeFunction never sees an unterminated block.
        if (blk.instCount == 0) {
            lirFatal("LirBuilder::closeFunction: block created but never filled");
        }
    }
    // Set function's blockCount from the created list.
    auto& fn = funcArena_.at(openFunc_);
    fn.blockCount = static_cast<std::uint32_t>(openFuncBlocks_.size());
    openFunc_  = {};
    openBlock_ = {};
}

Lir LirBuilder::finish() && {
    if (openFunc_.valid()) closeFunction_();
    return Lir{
        target_,
        std::move(instArena_).finish(),
        std::move(blockArena_).finish(),
        std::move(funcArena_).finish(),
        std::move(operandPool_),
        std::move(succPool_),
    };
}

} // namespace dss
