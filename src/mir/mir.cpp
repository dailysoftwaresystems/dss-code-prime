#include "mir/mir.hpp"

#include "core/substrate/mint_monotonic_id.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace dss {

// The builder's terminator methods hardcode CFG-successor counts (addBr → 1,
// addCondBr → 2, addSwitch → cases+1 ≥ 1, addReturn/addUnreachable → 0); these
// counts MUST agree with the opcodeInfo() descriptor that recordSuccessors_ and
// the ML3 verifier read. The compile-time checks below pin that agreement at
// the row level — a descriptor edit that breaks the contract fails the build
// instead of the runtime assertion.
static_assert(opcodeInfo(MirOpcode::Br).minSuccessors == 1
              && opcodeInfo(MirOpcode::Br).maxSuccessors == 1,
              "Br is unconditional → exactly 1 successor");
static_assert(opcodeInfo(MirOpcode::CondBr).minSuccessors == 2
              && opcodeInfo(MirOpcode::CondBr).maxSuccessors == 2,
              "CondBr → exactly 2 successors (true, false)");
static_assert(opcodeInfo(MirOpcode::Switch).minSuccessors == 1
              && opcodeInfo(MirOpcode::Switch).maxSuccessors == kMirUnboundedSuccessors,
              "Switch → ≥1 successor (default is always present)");
static_assert(opcodeInfo(MirOpcode::Return).minSuccessors == 0
              && opcodeInfo(MirOpcode::Return).maxSuccessors == 0,
              "Return → 0 successors");
static_assert(opcodeInfo(MirOpcode::Unreachable).minSuccessors == 0
              && opcodeInfo(MirOpcode::Unreachable).maxSuccessors == 0,
              "Unreachable → 0 successors");

namespace {

// Reset a moved-from `Mir` to the default-constructed observable state (empty
// arenas, empty pools), so a moved-from module reports `empty() == true` /
// `!id().valid()` instead of the std-lib's "valid but unspecified" — the same
// fail-loud-on-use-after-move discipline as `Hir`.
void resetMovedFrom_(Mir::InstArena& inst, Mir::BlockArena& block, Mir::FuncArena& func,
                     Mir::GlobalArena& global,
                     std::vector<MirBlockId>& instBlock, std::vector<MirInstId>& operandPool,
                     std::vector<MirPhiIncoming>& phiPool, std::vector<MirBlockId>& succPool,
                     MirLiteralPool& literalPool) noexcept {
    inst   = Mir::InstArena{};
    block  = Mir::BlockArena{};
    func   = Mir::FuncArena{};
    global = Mir::GlobalArena{};
    instBlock.clear();
    operandPool.clear();
    phiPool.clear();
    succPool.clear();
    literalPool = MirLiteralPool{};
}

} // namespace

// ── Mir ───────────────────────────────────────────────────────────────────────

Mir::Mir(InstArena instArena, BlockArena blockArena, FuncArena funcArena,
         GlobalArena globalArena,
         std::vector<MirBlockId> instBlock, std::vector<MirInstId> operandPool,
         std::vector<MirPhiIncoming> phiPool, std::vector<MirBlockId> succPool,
         MirLiteralPool literalPool,
         MirAliasingMode aliasingMode,
         bool charTypesAliasAll) noexcept
    : instArena_(std::move(instArena)),
      blockArena_(std::move(blockArena)),
      funcArena_(std::move(funcArena)),
      globalArena_(std::move(globalArena)),
      instBlock_(std::move(instBlock)),
      operandPool_(std::move(operandPool)),
      phiPool_(std::move(phiPool)),
      succPool_(std::move(succPool)),
      literalPool_(std::move(literalPool)),
      aliasingMode_(aliasingMode),
      charTypesAliasAll_(charTypesAliasAll) {
    // The four arenas are tagged by ONE module id (the cross-tier guard relies
    // on it: a MirBlockId and a MirInstId of the same module share the tag). A
    // direct (non-builder) ctor that mismatched them would let an id from one
    // tier validate against another tier's tag silently. Catch it at the boundary.
    if (instArena_.id() != blockArena_.id() || instArena_.id() != funcArena_.id()
        || instArena_.id() != globalArena_.id()) {
        std::fprintf(stderr,
                     "dss::Mir fatal: arena module-tag mismatch (inst=%u, block=%u, "
                     "func=%u, global=%u) — all four MIR arenas must share one "
                     "MirModuleId\n",
                     instArena_.id().v, blockArena_.id().v, funcArena_.id().v,
                     globalArena_.id().v);
        std::abort();
    }
    // `instBlock_` is the parallel inst→block array; it must align 1:1 with the
    // instruction arena (slot 0 sentinel + one slot per instruction), so a direct
    // ctor misuse aborts here before `instBlock()` could OOB-read it.
    if (instBlock_.size() != instArena_.nodeCount()) {
        std::fprintf(stderr,
                     "dss::Mir fatal: instBlock_/instArena_ size mismatch (instBlock=%zu, "
                     "inst=%zu) — invariant violated\n",
                     instBlock_.size(), instArena_.nodeCount());
        std::abort();
    }
}

Mir::Mir(Mir&& other) noexcept
    : instArena_(std::move(other.instArena_)),
      blockArena_(std::move(other.blockArena_)),
      funcArena_(std::move(other.funcArena_)),
      globalArena_(std::move(other.globalArena_)),
      instBlock_(std::move(other.instBlock_)),
      operandPool_(std::move(other.operandPool_)),
      phiPool_(std::move(other.phiPool_)),
      succPool_(std::move(other.succPool_)),
      literalPool_(std::move(other.literalPool_)),
      aliasingMode_(other.aliasingMode_),
      charTypesAliasAll_(other.charTypesAliasAll_) {
    other.aliasingMode_ = MirAliasingMode::Permissive;
    other.charTypesAliasAll_ = true;
    resetMovedFrom_(other.instArena_, other.blockArena_, other.funcArena_,
                    other.globalArena_, other.instBlock_,
                    other.operandPool_, other.phiPool_, other.succPool_, other.literalPool_);
}

Mir& Mir::operator=(Mir&& other) noexcept {
    if (this == &other) return *this;
    instArena_   = std::move(other.instArena_);
    blockArena_  = std::move(other.blockArena_);
    funcArena_   = std::move(other.funcArena_);
    globalArena_ = std::move(other.globalArena_);
    instBlock_   = std::move(other.instBlock_);
    operandPool_ = std::move(other.operandPool_);
    phiPool_     = std::move(other.phiPool_);
    succPool_    = std::move(other.succPool_);
    literalPool_ = std::move(other.literalPool_);
    aliasingMode_ = other.aliasingMode_;
    other.aliasingMode_ = MirAliasingMode::Permissive;
    charTypesAliasAll_ = other.charTypesAliasAll_;
    other.charTypesAliasAll_ = true;
    resetMovedFrom_(other.instArena_, other.blockArena_, other.funcArena_,
                    other.globalArena_, other.instBlock_,
                    other.operandPool_, other.phiPool_, other.succPool_, other.literalPool_);
    return *this;
}

MirBlockId Mir::instBlock(MirInstId id) const {
    (void)instArena_.at(id);  // validates bounds + provenance; lockstep guards instBlock_[id.v]
    return instBlock_[id.v];
}

std::span<MirInstId const> Mir::instOperands(MirInstId id) const {
    detail::MirInst const& n = instArena_.at(id);  // validates bounds + provenance
    // The descriptor's `usesPhiPool` flag is the single source of truth for which
    // pool a node's operand range addresses (generalises if a second pool-bearing
    // opcode is ever added).
    if (opcodeInfo(n.opcode).usesPhiPool) {
        std::fprintf(stderr,
                     "dss::Mir fatal: instOperands: MirInstId=%u is a Phi — use phiIncomings\n",
                     id.v);
        std::abort();
    }
    if (static_cast<std::size_t>(n.operandStart) + n.operandCount > operandPool_.size()) {
        std::fprintf(stderr,
                     "dss::Mir fatal: instOperands: operand range [%u, %u) exceeds operand "
                     "pool size %zu\n",
                     n.operandStart, n.operandStart + n.operandCount, operandPool_.size());
        std::abort();
    }
    return std::span<MirInstId const>{operandPool_.data() + n.operandStart, n.operandCount};
}

std::span<MirPhiIncoming const> Mir::phiIncomings(MirInstId id) const {
    detail::MirInst const& n = instArena_.at(id);  // validates bounds + provenance
    if (!opcodeInfo(n.opcode).usesPhiPool) {
        std::fprintf(stderr,
                     "dss::Mir fatal: phiIncomings: MirInstId=%u is not a Phi (opcode '%.*s')\n",
                     id.v, static_cast<int>(mnemonic(n.opcode).size()), mnemonic(n.opcode).data());
        std::abort();
    }
    if (static_cast<std::size_t>(n.operandStart) + n.operandCount > phiPool_.size()) {
        std::fprintf(stderr,
                     "dss::Mir fatal: phiIncomings: range [%u, %u) exceeds phi pool size %zu\n",
                     n.operandStart, n.operandStart + n.operandCount, phiPool_.size());
        std::abort();
    }
    return std::span<MirPhiIncoming const>{phiPool_.data() + n.operandStart, n.operandCount};
}

namespace {

// Read `payload` only after confirming the instruction's opcode — a typed
// payload accessor on the wrong opcode is a caller bug, not a value.
std::uint32_t payloadForOpcode_(detail::MirInst const& n, MirOpcode expected,
                                MirInstId id, char const* accessor) {
    if (n.opcode != expected) {
        std::fprintf(stderr,
                     "dss::Mir fatal: %s: MirInstId=%u is '%.*s', not '%.*s'\n",
                     accessor, id.v,
                     static_cast<int>(mnemonic(n.opcode).size()), mnemonic(n.opcode).data(),
                     static_cast<int>(mnemonic(expected).size()), mnemonic(expected).data());
        std::abort();
    }
    return n.payload;
}

} // namespace

std::uint32_t Mir::argIndex(MirInstId id) const {
    return payloadForOpcode_(instArena_.at(id), MirOpcode::Arg, id, "argIndex");
}
std::uint32_t Mir::constLiteralIndex(MirInstId id) const {
    return payloadForOpcode_(instArena_.at(id), MirOpcode::Const, id, "constLiteralIndex");
}
SymbolId Mir::globalAddrSymbol(MirInstId id) const {
    return SymbolId{payloadForOpcode_(instArena_.at(id), MirOpcode::GlobalAddr, id,
                                      "globalAddrSymbol")};
}
std::uint32_t Mir::intrinsicId(MirInstId id) const {
    return payloadForOpcode_(instArena_.at(id), MirOpcode::IntrinsicCall, id, "intrinsicId");
}
std::uint32_t Mir::returnPieceOrdinal(MirInstId id) const {
    return payloadForOpcode_(instArena_.at(id), MirOpcode::ReturnPiece, id, "returnPieceOrdinal");
}

MirFuncId Mir::blockFunc(MirBlockId id) const {
    return MirFuncId{blockArena_.at(id).func, this->id().v};
}

MirInstId Mir::blockInstAt(MirBlockId id, std::uint32_t i) const {
    detail::MirBlock const& b = blockArena_.at(id);
    if (i >= b.instCount) {
        std::fprintf(stderr,
                     "dss::Mir fatal: blockInstAt: index %u out of range (block MirBlockId=%u "
                     "has %u instructions)\n",
                     i, id.v, b.instCount);
        std::abort();
    }
    return MirInstId{b.instStart + i, this->id().v};
}

MirInstId Mir::blockTerminator(MirBlockId id) const {
    detail::MirBlock const& b = blockArena_.at(id);
    if (b.instCount == 0) {
        std::fprintf(stderr,
                     "dss::Mir fatal: blockTerminator: MirBlockId=%u is empty (no terminator)\n",
                     id.v);
        std::abort();
    }
    return MirInstId{b.instStart + b.instCount - 1, this->id().v};
}

std::span<MirBlockId const> Mir::blockSuccessors(MirBlockId id) const {
    detail::MirBlock const& b = blockArena_.at(id);
    if (static_cast<std::size_t>(b.succStart) + b.succCount > succPool_.size()) {
        std::fprintf(stderr,
                     "dss::Mir fatal: blockSuccessors: range [%u, %u) exceeds succ pool size %zu\n",
                     b.succStart, b.succStart + b.succCount, succPool_.size());
        std::abort();
    }
    return std::span<MirBlockId const>{succPool_.data() + b.succStart, b.succCount};
}

MirBlockId Mir::funcBlockAt(MirFuncId id, std::uint32_t i) const {
    detail::MirFunc const& f = funcArena_.at(id);
    if (i >= f.blockCount) {
        std::fprintf(stderr,
                     "dss::Mir fatal: funcBlockAt: index %u out of range (func MirFuncId=%u "
                     "has %u blocks)\n",
                     i, id.v, f.blockCount);
        std::abort();
    }
    return MirBlockId{f.blockStart + i, this->id().v};
}

MirBlockId Mir::funcEntry(MirFuncId id) const {
    detail::MirFunc const& f = funcArena_.at(id);
    if (f.blockCount == 0) {
        std::fprintf(stderr,
                     "dss::Mir fatal: funcEntry: MirFuncId=%u has no blocks\n", id.v);
        std::abort();
    }
    return MirBlockId{f.blockStart, this->id().v};
}

std::size_t Mir::moduleFuncCount() const noexcept {
    // Slot 0 is the sentinel; real functions are slots [1, nodeCount).
    std::size_t const n = funcArena_.nodeCount();
    return n == 0 ? 0 : n - 1;
}

MirFuncId Mir::funcAt(std::uint32_t i) const {
    if (i >= moduleFuncCount()) {
        std::fprintf(stderr,
                     "dss::Mir fatal: funcAt: index %u out of range (module has %zu functions)\n",
                     i, moduleFuncCount());
        std::abort();
    }
    return MirFuncId{i + 1, id().v};  // real funcs start at arena slot 1
}

std::size_t Mir::moduleGlobalCount() const noexcept {
    std::size_t const n = globalArena_.nodeCount();
    return n == 0 ? 0 : n - 1;
}

MirGlobalId Mir::globalAt(std::uint32_t i) const {
    if (i >= moduleGlobalCount()) {
        std::fprintf(stderr,
                     "dss::Mir fatal: globalAt: index %u out of range (module has %zu "
                     "globals)\n",
                     i, moduleGlobalCount());
        std::abort();
    }
    return MirGlobalId{i + 1, id().v};
}

// ── MirBuilder ──────────────────────────────────────────────────────────────

MirModuleId MirBuilder::nextModuleId() noexcept {
    // Aborts on uint32 overflow — see `substrate::mintMonotonicId`.
    return substrate::mintMonotonicId<MirModuleId>();
}

MirBuilder::MirBuilder() : MirBuilder(nextModuleId()) {}

MirBuilder::MirBuilder(MirModuleId tag)
    : moduleId_(tag), instArena_(tag), blockArena_(tag), funcArena_(tag),
      globalArena_(tag) {
    // Slot 0 mirrors the arenas' slot-0 sentinels so a block .v keys
    // `blockState_` and an inst .v keys `instBlock_` directly.
    blockState_.push_back(BlockState::Sealed);
    instBlock_.push_back(InvalidMirBlock);
}

void MirBuilder::checkSameModule_(std::uint32_t arenaTag, char const* what) const {
    // Untagged ids (arenaTag == 0) pass — literal-id test ergonomics, mirroring
    // the substrate cross-arena guard.
    if (arenaTag != 0 && arenaTag != moduleId_.v) {
        std::fprintf(stderr,
                     "dss::MirBuilder fatal: %s id from module=%u used in module=%u\n",
                     what, arenaTag, moduleId_.v);
        std::abort();
    }
}

void MirBuilder::closeBlock_() {
    if (!openBlock_.valid()) return;
    if (blockState_[openBlock_.v] != BlockState::Sealed) {
        std::fprintf(stderr,
                     "dss::MirBuilder fatal: block MirBlockId=%u has no terminator "
                     "(every block must end in br/condbr/switch/return/unreachable)\n",
                     openBlock_.v);
        std::abort();
    }
    openBlock_ = MirBlockId{};
}

void MirBuilder::closeFunction_() {
    closeBlock_();
    if (!openFunc_.valid()) return;
    detail::MirFunc const& f = funcArena_.at(openFunc_);
    if (f.blockCount == 0) {
        std::fprintf(stderr,
                     "dss::MirBuilder fatal: function MirFuncId=%u has no blocks\n", openFunc_.v);
        std::abort();
    }
    // Every block created for this function must have been filled + terminated.
    for (std::uint32_t slot = f.blockStart; slot < f.blockStart + f.blockCount; ++slot) {
        if (blockState_[slot] != BlockState::Sealed) {
            std::fprintf(stderr,
                         "dss::MirBuilder fatal: function MirFuncId=%u: block MirBlockId=%u was "
                         "created but never filled + terminated\n",
                         openFunc_.v, slot);
            std::abort();
        }
    }
    openFunc_ = MirFuncId{};
}

MirFuncId MirBuilder::addFunction(TypeId signature, SymbolId symbol,
                                  SymbolBinding    binding,
                                  SymbolVisibility visibility) {
    // `symbol` MAY be invalid (0): synthetic/anonymous functions (thunks the
    // backend generates) have no source symbol. The signature, however, is always
    // required — codegen sizes the ABI from it.
    if (!signature.valid()) {
        std::fputs("dss::MirBuilder fatal: addFunction: signature TypeId must be valid (FnSig)\n",
                   stderr);
        std::abort();
    }
    closeFunction_();
    detail::MirFunc f;
    f.signature  = signature;
    f.symbol     = symbol.v;
    f.blockStart = static_cast<std::uint32_t>(blockArena_.size());  // next block slot
    f.blockCount = 0;
    f.binding    = binding;
    f.visibility = visibility;
    MirFuncId const id = funcArena_.addNode(f);
    openFunc_ = id;
    return id;
}

std::uint32_t MirBuilder::literalPoolAdd(MirLiteralValue value) {
    return literalPool_.add(std::move(value));
}

MirGlobalId MirBuilder::addGlobal(TypeId type, SymbolId symbol,
                                  std::uint32_t initLiteralIndex,
                                  MirFuncId initFunc,
                                  SymbolBinding    binding,
                                  SymbolVisibility visibility) {
    if (!type.valid()) {
        std::fputs("dss::MirBuilder fatal: addGlobal: type TypeId must be valid\n",
                   stderr);
        std::abort();
    }
    if (!symbol.valid()) {
        std::fputs("dss::MirBuilder fatal: addGlobal: symbol must be valid (a "
                   "globals table without a stable name has no codegen anchor)\n",
                   stderr);
        std::abort();
    }
    if (initLiteralIndex != UINT32_MAX && initFunc.valid()) {
        std::fprintf(stderr,
                     "dss::MirBuilder fatal: addGlobal: initLiteralIndex (%u) and "
                     "initFunc (id=%u) are mutually exclusive — a global is either "
                     "constant-init, function-init, or zero-init\n",
                     initLiteralIndex, initFunc.v);
        std::abort();
    }
    if (initFunc.valid()) checkSameModule_(initFunc.arenaTag, "addGlobal initFunc");
    detail::MirGlobal g;
    g.type             = type;
    g.symbol           = symbol.v;
    g.initLiteralIndex = initLiteralIndex;
    g.initFunc         = initFunc;
    g.binding          = binding;
    g.visibility       = visibility;
    return globalArena_.addNode(g);
}

MirBlockId MirBuilder::createBlock(StructCfMarker marker) {
    if (!openFunc_.valid()) {
        std::fputs("dss::MirBuilder fatal: createBlock: no open function (call addFunction first)\n",
                   stderr);
        std::abort();
    }
    detail::MirBlock b;
    b.instStart = 0;  // fixed when beginBlock opens it
    b.instCount = 0;
    b.succStart = 0;
    b.succCount = 0;
    b.func      = openFunc_.v;
    b.marker    = marker;
    MirBlockId const id = blockArena_.addNode(b);
    funcArena_.at(openFunc_).blockCount += 1;
    blockState_.push_back(BlockState::Created);  // id.v == blockState_.size()-1
    return id;
}

void MirBuilder::beginBlock(MirBlockId block) {
    (void)blockArena_.at(block);  // validates bounds + provenance
    if (blockState_[block.v] != BlockState::Created) {
        std::fprintf(stderr,
                     "dss::MirBuilder fatal: beginBlock: MirBlockId=%u was already opened "
                     "(a block may be filled exactly once)\n",
                     block.v);
        std::abort();
    }
    if (blockArena_.at(block).func != openFunc_.v) {
        std::fprintf(stderr,
                     "dss::MirBuilder fatal: beginBlock: MirBlockId=%u belongs to function %u, "
                     "not the open function %u\n",
                     block.v, blockArena_.at(block).func, openFunc_.v);
        std::abort();
    }
    closeBlock_();  // the previously-open block must be terminated first
    blockArena_.at(block).instStart = static_cast<std::uint32_t>(instArena_.size());
    blockState_[block.v] = BlockState::Open;
    openBlock_ = block;
}

void MirBuilder::setBlockMarker(MirBlockId block, StructCfMarker marker) {
    blockArena_.at(block).marker = marker;  // validates bounds + provenance
}

MirInstId MirBuilder::appendInst_(detail::MirInst pod, std::span<MirInstId const> operands,
                                  bool terminates) {
    if (!openBlock_.valid()) {
        std::fputs("dss::MirBuilder fatal: instruction added with no open block "
                   "(call beginBlock first)\n", stderr);
        std::abort();
    }
    if (blockState_[openBlock_.v] != BlockState::Open) {
        std::fprintf(stderr,
                     "dss::MirBuilder fatal: block MirBlockId=%u is already terminated; no "
                     "instruction may follow its terminator\n",
                     openBlock_.v);
        std::abort();
    }
    pod.operandStart = static_cast<std::uint32_t>(operandPool_.size());
    pod.operandCount = static_cast<std::uint32_t>(operands.size());
    for (MirInstId op : operands) {
        checkSameModule_(op.arenaTag, "operand");
        operandPool_.push_back(op);
    }
    MirInstId const id = instArena_.addNode(pod);
    instBlock_.push_back(openBlock_);  // id.v == instBlock_.size()-1: stays in lockstep
    blockArena_.at(openBlock_).instCount += 1;
    if (terminates) blockState_[openBlock_.v] = BlockState::Sealed;
    return id;
}

void MirBuilder::recordSuccessors_(MirOpcode terminator, std::span<MirBlockId const> succs) {
    // The successor count must match the terminator's CFG arity in the single
    // opcodeInfo() descriptor (Br=1, CondBr=2, Switch≥1, …) — so a future
    // terminator builder that recorded the wrong number of edges fails loud here
    // rather than producing a malformed CFG. This assertion is unreachable from
    // the public API today (the five terminator methods all hardcode counts that
    // satisfy the table — and the static_asserts at the top of this TU lock that
    // mutual consistency at compile time); it earns its keep when (a) ML3's
    // verifier re-runs the same descriptor check on any frozen module — including
    // the direct-Mir-ctor path this builder doesn't own — and (b) a future
    // terminator method is added that miscounts.
    MirOpcodeInfo const info = opcodeInfo(terminator);
    auto const n = succs.size();
    if (n < info.minSuccessors
        || (info.maxSuccessors != kMirUnboundedSuccessors && n > info.maxSuccessors)) {
        if (info.maxSuccessors == kMirUnboundedSuccessors) {
            std::fprintf(stderr,
                         "dss::MirBuilder fatal: terminator '%.*s' takes [%u, ∞] CFG successors "
                         "but got %zu\n",
                         static_cast<int>(info.mnemonic.size()), info.mnemonic.data(),
                         info.minSuccessors, n);
        } else {
            std::fprintf(stderr,
                         "dss::MirBuilder fatal: terminator '%.*s' takes [%u, %u] CFG successors "
                         "but got %zu\n",
                         static_cast<int>(info.mnemonic.size()), info.mnemonic.data(),
                         info.minSuccessors, info.maxSuccessors, n);
        }
        std::abort();
    }
    detail::MirBlock& b = blockArena_.at(openBlock_);
    b.succStart = static_cast<std::uint32_t>(succPool_.size());
    b.succCount = static_cast<std::uint32_t>(succs.size());
    for (MirBlockId s : succs) {
        checkSameModule_(s.arenaTag, "successor");
        // A CFG edge must target a real block of the SAME function (the target
        // may be a forward-created block not yet filled — it still exists in the
        // arena with the right `func`). Catches cross-function / dangling targets
        // at the terminator, not hundreds of lines later in a backend pass.
        if (blockArena_.at(s).func != openFunc_.v) {
            std::fprintf(stderr,
                         "dss::MirBuilder fatal: branch target MirBlockId=%u belongs to function "
                         "%u, not the open function %u\n",
                         s.v, blockArena_.at(s).func, openFunc_.v);
            std::abort();
        }
        succPool_.push_back(s);
    }
}

MirInstId MirBuilder::addInst(MirOpcode opcode, std::span<MirInstId const> operands,
                              TypeId resultType, std::uint32_t payload, MirInstFlags flags) {
    MirOpcodeInfo const info = opcodeInfo(opcode);
    if (info.isTerminator) {
        std::fprintf(stderr,
                     "dss::MirBuilder fatal: addInst: opcode '%.*s' is a terminator; use the "
                     "terminator API (addBr/addCondBr/addSwitch/addReturn/addUnreachable)\n",
                     static_cast<int>(info.mnemonic.size()), info.mnemonic.data());
        std::abort();
    }
    if (opcode == MirOpcode::Phi) {
        std::fputs("dss::MirBuilder fatal: addInst: use addPhi to build a Phi\n", stderr);
        std::abort();
    }
    // The value-origin opcodes and the sentinel have dedicated builders (so a
    // Const's payload is always a real literal-pool index, etc.); reject them here
    // so the only way to spell them is the safe path.
    if (opcode == MirOpcode::Arg || opcode == MirOpcode::Const
        || opcode == MirOpcode::GlobalAddr || opcode == MirOpcode::Invalid) {
        std::fprintf(stderr,
                     "dss::MirBuilder fatal: addInst: opcode '%.*s' has a dedicated builder "
                     "(addArg/addConst/addGlobalAddr); do not build it via addInst\n",
                     static_cast<int>(info.mnemonic.size()), info.mnemonic.data());
        std::abort();
    }
    // Operand-count bounds.
    auto const count = operands.size();
    if (count < info.minOperands
        || (info.maxOperands != kMirUnboundedOperands && count > info.maxOperands)) {
        if (info.maxOperands == kMirUnboundedOperands) {
            std::fprintf(stderr,
                         "dss::MirBuilder fatal: addInst: opcode '%.*s' takes [%u, ∞] operands "
                         "but got %zu\n",
                         static_cast<int>(info.mnemonic.size()), info.mnemonic.data(),
                         info.minOperands, count);
        } else {
            std::fprintf(stderr,
                         "dss::MirBuilder fatal: addInst: opcode '%.*s' takes [%u, %u] operands "
                         "but got %zu\n",
                         static_cast<int>(info.mnemonic.size()), info.mnemonic.data(),
                         info.minOperands, info.maxOperands, count);
        }
        std::abort();
    }
    // Result-type rule.
    if (info.result == MirResultRule::Value && !resultType.valid()) {
        std::fprintf(stderr,
                     "dss::MirBuilder fatal: addInst: opcode '%.*s' produces a value but the "
                     "result type is invalid\n",
                     static_cast<int>(info.mnemonic.size()), info.mnemonic.data());
        std::abort();
    }
    if (info.result == MirResultRule::None && resultType.valid()) {
        std::fprintf(stderr,
                     "dss::MirBuilder fatal: addInst: opcode '%.*s' produces no value but a "
                     "result type was supplied\n",
                     static_cast<int>(info.mnemonic.size()), info.mnemonic.data());
        std::abort();
    }
    detail::MirInst pod;
    pod.opcode  = opcode;
    pod.flags   = flags;
    pod.typeId  = resultType;
    pod.payload = payload;
    return appendInst_(pod, operands, /*terminates=*/false);
}

namespace {

[[noreturn]] void requireValueType_(char const* what) {
    std::fprintf(stderr, "dss::MirBuilder fatal: %s requires a valid result type\n", what);
    std::abort();
}

} // namespace

MirInstId MirBuilder::addArg(std::uint32_t paramIndex, TypeId type, MirInstFlags flags) {
    if (!type.valid()) requireValueType_("addArg");
    detail::MirInst pod;
    pod.opcode  = MirOpcode::Arg;
    pod.flags   = flags;
    pod.typeId  = type;
    pod.payload = paramIndex;
    return appendInst_(pod, {}, /*terminates=*/false);
}

MirInstId MirBuilder::addReturnPiece(MirInstId call, std::uint32_t ordinal,
                                     TypeId pieceType, MirInstFlags flags) {
    if (!pieceType.valid()) requireValueType_("addReturnPiece");
    if (!call.valid()) {
        std::fputs("dss::MirBuilder fatal: addReturnPiece: call operand must be valid\n",
                   stderr);
        std::abort();
    }
    detail::MirInst pod;
    pod.opcode  = MirOpcode::ReturnPiece;
    pod.flags   = flags;
    pod.typeId  = pieceType;
    pod.payload = ordinal;
    MirInstId const operands[] = {call};
    return appendInst_(pod, operands, /*terminates=*/false);
}

MirInstId MirBuilder::addConst(MirLiteralValue value, TypeId type, MirInstFlags flags) {
    if (!type.valid()) requireValueType_("addConst");
    std::uint32_t const index = literalPool_.add(std::move(value));
    detail::MirInst pod;
    pod.opcode  = MirOpcode::Const;
    pod.flags   = flags;
    pod.typeId  = type;
    pod.payload = index;
    return appendInst_(pod, {}, /*terminates=*/false);
}

MirInstId MirBuilder::addGlobalAddr(SymbolId symbol, TypeId type, MirInstFlags flags) {
    if (!type.valid()) requireValueType_("addGlobalAddr");
    if (!symbol.valid()) {
        std::fputs("dss::MirBuilder fatal: addGlobalAddr: symbol must be valid\n", stderr);
        std::abort();
    }
    detail::MirInst pod;
    pod.opcode  = MirOpcode::GlobalAddr;
    pod.flags   = flags;
    pod.typeId  = type;
    pod.payload = symbol.v;
    return appendInst_(pod, {}, /*terminates=*/false);
}

// D5.6: first-class aggregate ops. Each path element is interned as a
// Const MirInst of type i32 carrying the index value; the resulting
// operand vector is `[aggregate, (value,) idx0, idx1, ...]` (Gep-shaped).
namespace {
[[noreturn]] void requireAggregatePath_(char const* what) {
    std::fprintf(stderr,
                 "dss::MirBuilder fatal: %s: path must have at least one index\n",
                 what);
    std::abort();
}
[[noreturn]] void requireMatchingResultType_(char const* what) {
    std::fprintf(stderr,
                 "dss::MirBuilder fatal: %s: resultType must equal the "
                 "aggregate operand's type (InsertValue produces a value of "
                 "the same aggregate shape)\n",
                 what);
    std::abort();
}
} // namespace

void MirBuilder::appendIndexPathOperands_(std::vector<MirInstId>& operands,
                                           std::span<std::uint32_t const> path,
                                           TypeId indexType) {
    // indexType MUST be valid (asserted by addConst's value-rule). Static
    // contract: path indices are i32. Callers thread the interner's i32
    // TypeId through this signature because MirBuilder intentionally
    // doesn't carry a TypeInterner (build-once / freeze separation).
    for (std::uint32_t idx : path) {
        MirLiteralValue lv;
        lv.value = static_cast<std::int64_t>(idx);
        lv.core  = TypeKind::I32;
        operands.push_back(addConst(std::move(lv), indexType));
    }
}

MirInstId MirBuilder::addExtractValue(MirInstId aggregate,
                                       std::span<std::uint32_t const> path,
                                       TypeId resultType, TypeId indexType,
                                       MirInstFlags flags) {
    if (path.empty()) requireAggregatePath_("addExtractValue");
    std::vector<MirInstId> operands;
    operands.reserve(1 + path.size());
    operands.push_back(aggregate);
    appendIndexPathOperands_(operands, path, indexType);
    return addInst(MirOpcode::ExtractValue, operands, resultType, 0, flags);
}

MirInstId MirBuilder::addInsertValue(MirInstId aggregate, MirInstId value,
                                      std::span<std::uint32_t const> path,
                                      TypeId resultType, TypeId indexType,
                                      MirInstFlags flags) {
    if (path.empty()) requireAggregatePath_("addInsertValue");
    // D5.6 invariant: InsertValue's result type IS the aggregate's type
    // (same shape, one slot replaced). Catch a caller mistake here
    // rather than letting it silently propagate as type-confused IR
    // downstream. Multi-reviewer bug-finding (D5.6 7-agent review).
    if (resultType != instArena_.at(aggregate).typeId) {
        requireMatchingResultType_("addInsertValue");
    }
    std::vector<MirInstId> operands;
    operands.reserve(2 + path.size());
    operands.push_back(aggregate);
    operands.push_back(value);
    appendIndexPathOperands_(operands, path, indexType);
    return addInst(MirOpcode::InsertValue, operands, resultType, 0, flags);
}

MirInstId MirBuilder::addPhi(TypeId resultType, std::span<MirPhiIncoming const> incomings,
                             MirInstFlags flags) {
    if (!resultType.valid()) requireValueType_("addPhi");
    detail::MirInst pod;
    pod.opcode = MirOpcode::Phi;
    pod.flags  = flags;
    pod.typeId = resultType;
    // operandStart/operandCount are patched to the phi-pool range at finish, after
    // all incomings (including loop back-edges) have been collected.
    MirInstId const id = appendInst_(pod, {}, /*terminates=*/false);
    auto& pending = pendingPhi_[id.v];
    for (MirPhiIncoming inc : incomings) {
        checkSameModule_(inc.value.arenaTag, "phi value");
        checkSameModule_(inc.pred.arenaTag, "phi predecessor");
        pending.push_back(inc);
    }
    return id;
}

void MirBuilder::addPhiIncoming(MirInstId phi, MirPhiIncoming incoming) {
    detail::MirInst const& n = instArena_.at(phi);  // validates bounds + provenance
    if (n.opcode != MirOpcode::Phi) {
        std::fprintf(stderr,
                     "dss::MirBuilder fatal: addPhiIncoming: MirInstId=%u is not a Phi\n", phi.v);
        std::abort();
    }
    checkSameModule_(incoming.value.arenaTag, "phi value");
    checkSameModule_(incoming.pred.arenaTag, "phi predecessor");
    pendingPhi_[phi.v].push_back(incoming);
}

MirInstId MirBuilder::addBr(MirBlockId target) {
    checkSameModule_(target.arenaTag, "branch target");
    detail::MirInst pod;
    pod.opcode = MirOpcode::Br;
    MirInstId const id = appendInst_(pod, {}, /*terminates=*/true);
    MirBlockId const succs[] = {target};
    recordSuccessors_(MirOpcode::Br, succs);
    return id;
}

MirInstId MirBuilder::addCondBr(MirInstId cond, MirBlockId ifTrue, MirBlockId ifFalse) {
    checkSameModule_(cond.arenaTag, "condition");
    detail::MirInst pod;
    pod.opcode = MirOpcode::CondBr;
    MirInstId const operands[] = {cond};
    MirInstId const id = appendInst_(pod, operands, /*terminates=*/true);
    MirBlockId const succs[] = {ifTrue, ifFalse};  // [0]=true target, [1]=false target
    recordSuccessors_(MirOpcode::CondBr, succs);
    return id;
}

MirInstId MirBuilder::addSwitch(MirInstId discriminant,
                                std::span<std::pair<MirInstId, MirBlockId> const> cases,
                                MirBlockId defaultTarget) {
    checkSameModule_(discriminant.arenaTag, "discriminant");
    // operands = [discriminant, case constant values…]; successors = [case
    // targets…, default].
    std::vector<MirInstId>  operands;
    std::vector<MirBlockId> succs;
    operands.reserve(cases.size() + 1);
    succs.reserve(cases.size() + 1);
    operands.push_back(discriminant);
    for (auto const& [caseValue, caseTarget] : cases) {
        operands.push_back(caseValue);
        succs.push_back(caseTarget);
    }
    succs.push_back(defaultTarget);
    detail::MirInst pod;
    pod.opcode = MirOpcode::Switch;
    MirInstId const id = appendInst_(pod, operands, /*terminates=*/true);
    recordSuccessors_(MirOpcode::Switch, succs);
    return id;
}

MirInstId MirBuilder::addReturn(std::optional<MirInstId> value) {
    detail::MirInst pod;
    pod.opcode = MirOpcode::Return;
    MirInstId id;
    if (value) {
        MirInstId const operands[] = {*value};
        id = appendInst_(pod, operands, /*terminates=*/true);
    } else {
        id = appendInst_(pod, {}, /*terminates=*/true);
    }
    // No CFG edges, but route through recordSuccessors_ for symmetry with the
    // other terminators — the descriptor row is now the single source of truth
    // for every terminator's edge count, even the zero-edge ones.
    recordSuccessors_(MirOpcode::Return, {});
    return id;
}

MirInstId MirBuilder::addReturnMulti(std::span<MirInstId const> values) {
    detail::MirInst pod;
    pod.opcode = MirOpcode::Return;
    MirInstId const id = appendInst_(pod, values, /*terminates=*/true);
    recordSuccessors_(MirOpcode::Return, {});
    return id;
}

MirInstId MirBuilder::addUnreachable() {
    detail::MirInst pod;
    pod.opcode = MirOpcode::Unreachable;
    MirInstId const id = appendInst_(pod, {}, /*terminates=*/true);
    recordSuccessors_(MirOpcode::Unreachable, {});  // same symmetry as addReturn
    return id;
}

bool MirBuilder::openBlockHasTerminator() const noexcept {
    // No open block ⇒ no terminator to ask about; conservative false. A
    // sealed block's state is `BlockState::Sealed` — the block-state vector
    // is indexed by block .v (slot 0 is the sentinel).
    if (!openBlock_.valid()) return false;
    return blockState_[openBlock_.v] == BlockState::Sealed;
}

bool MirBuilder::isBlockUnopened(MirBlockId block) const noexcept {
    // An invalid id has no state; conservatively false. A valid id outside
    // the block-state range is a structural bug — return false rather than
    // OOB-read; the next `beginBlock`/`at` call surfaces it loudly.
    if (!block.valid() || block.v >= blockState_.size()) return false;
    return blockState_[block.v] == BlockState::Created;
}

Mir MirBuilder::finish() && {
    closeFunction_();  // closes the open block + function (validates termination / ≥1 block)

    // Flush pending phi incomings into the phi pool, patching each phi's operand
    // range. Each phi gets a contiguous slice regardless of the order incomings
    // were added in.
    for (auto& [phiV, incomings] : pendingPhi_) {
        MirInstId const phiId{phiV, moduleId_.v};
        detail::MirInst& inst = instArena_.at(phiId);
        inst.operandStart = static_cast<std::uint32_t>(phiPool_.size());
        inst.operandCount = static_cast<std::uint32_t>(incomings.size());
        for (MirPhiIncoming inc : incomings) phiPool_.push_back(inc);
    }

    // Freeze-boundary sweep: every pooled reference must name a real, defined
    // instruction/block of this module. `checkSameModule_` only guards the tag
    // (and lets untagged ids through for test ergonomics); THIS is where a
    // dangling or out-of-range reference fails loud rather than surfacing later in
    // a backend pass on a corrupt frozen module.
    std::size_t const instSlots  = instArena_.size();
    std::size_t const blockSlots = blockArena_.size();
    auto requireInst = [&](MirInstId ref, char const* what) {
        if (ref.v == 0 || ref.v >= instSlots) {
            std::fprintf(stderr,
                         "dss::MirBuilder fatal: finish: %s references non-existent instruction "
                         "%u (module has %zu instruction slots)\n",
                         what, ref.v, instSlots);
            std::abort();
        }
    };
    auto requireBlock = [&](MirBlockId ref, char const* what) {
        if (ref.v == 0 || ref.v >= blockSlots) {
            std::fprintf(stderr,
                         "dss::MirBuilder fatal: finish: %s references non-existent block "
                         "%u (module has %zu block slots)\n",
                         what, ref.v, blockSlots);
            std::abort();
        }
    };
    for (MirInstId op : operandPool_)      requireInst(op, "operand");
    for (MirPhiIncoming inc : phiPool_) {  requireInst(inc.value, "phi value");
                                           requireBlock(inc.pred, "phi predecessor"); }
    for (MirBlockId s : succPool_)         requireBlock(s, "successor");

    // Globals carry two cross-arena references: `initFunc` (a MirFuncId)
    // and `initLiteralIndex` (an index into the literal pool). Sweep both
    // at the freeze boundary — same fail-loud-on-dangling discipline as
    // the operand/phi/succ pools above.
    std::size_t const funcSlots    = funcArena_.size();
    std::size_t const literalSlots = literalPool_.size();
    for (std::uint32_t slot = 1; slot < globalArena_.size(); ++slot) {
        MirGlobalId const gid{slot, moduleId_.v};
        detail::MirGlobal const& g = globalArena_.at(gid);
        if (g.initFunc.valid()
            && (g.initFunc.v == 0 || g.initFunc.v >= funcSlots)) {
            std::fprintf(stderr,
                         "dss::MirBuilder fatal: finish: global MirGlobalId=%u "
                         "initFunc references non-existent function %u (module "
                         "has %zu function slots)\n",
                         slot, g.initFunc.v, funcSlots);
            std::abort();
        }
        if (g.initLiteralIndex != UINT32_MAX
            && g.initLiteralIndex >= literalSlots) {
            std::fprintf(stderr,
                         "dss::MirBuilder fatal: finish: global MirGlobalId=%u "
                         "initLiteralIndex %u exceeds literal-pool size %zu\n",
                         slot, g.initLiteralIndex, literalSlots);
            std::abort();
        }
    }

    return Mir{std::move(instArena_).finish(), std::move(blockArena_).finish(),
               std::move(funcArena_).finish(), std::move(globalArena_).finish(),
               std::move(instBlock_),
               std::move(operandPool_), std::move(phiPool_), std::move(succPool_),
               std::move(literalPool_),
               aliasingMode_,
               charTypesAliasAll_};
}

} // namespace dss
