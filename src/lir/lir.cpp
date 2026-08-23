#include "lir/lir.hpp"

#include "core/substrate/mint_monotonic_id.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace dss {

namespace {

[[noreturn]] void lirFatal(char const* what) {
    std::fputs("dss::Lir fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace

// ── LirRegConstraintPool ─────────────────────────────────────────
//
// Byte-for-byte the shape of `LirLiteralPool` (append-only, by-index,
// abort-on-out-of-range) — see the docblock in `lir_node.hpp` for why
// the two side structures are deliberately the same shape. It lives in
// this TU rather than a header-inline definition so the fatal path
// keeps `<cstdio>`/`<cstdlib>` out of the widely-included
// `lir_node.hpp`.

std::uint32_t LirRegConstraintPool::add(ImplicitRegisterConstraint c) {
    auto const idx = static_cast<std::uint32_t>(pool_.size());
    pool_.push_back(std::move(c));
    return idx;
}

ImplicitRegisterConstraint const&
LirRegConstraintPool::at(std::uint32_t index) const {
    if (index >= pool_.size()) {
        lirFatal("LirRegConstraintPool::at: index out of range (a "
                 "register-constraint handle outlived its pool)");
    }
    return pool_[index];
}

// ── Lir ──────────────────────────────────────────────────────────

Lir::Lir(TargetSchemaId target, InstArena instArena, BlockArena blockArena,
         FuncArena funcArena, std::vector<LirOperand> operandPool,
         std::vector<LirBlockId> succPool,
         LirLiteralPool literalPool,
         LirRegConstraintPool regConstraintPool) noexcept
    : target_(target),
      instArena_(std::move(instArena)),
      blockArena_(std::move(blockArena)),
      funcArena_(std::move(funcArena)),
      operandPool_(std::move(operandPool)),
      succPool_(std::move(succPool)),
      literalPool_(std::move(literalPool)),
      regConstraintPool_(std::move(regConstraintPool)) {
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

LirInstId LirBuilder::addIndirectBr(std::uint16_t opcode,
                                    std::span<LirOperand const> operands,
                                    std::span<LirBlockId const> targets,
                                    std::uint32_t payload, std::uint8_t flags) {
    if (!target_.isTerminator(opcode)) {
        lirFatal("LirBuilder::addIndirectBr: opcode is not a terminator for this target");
    }
    // D-CSUBSET-COMPUTED-GOTO: operand[0] = address register; successors = all
    // address-taken blocks (variadic, like a Switch). Records them into the succ
    // pool exactly as addCondBr does for its two successors.
    LirInstId const id = addInst(opcode, InvalidLirReg, operands, payload, flags);
    recordSuccessors_(targets);
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

namespace {

// Resolve one register NAME through the active schema, or abort naming
// the offender. Shared by the three positional arrays and the two role
// maps so every arm fails identically — a per-arm resolution would be
// free to diverge, which is how `unistd.json`'s alias set ended up half-
// applied.
[[nodiscard]] std::uint16_t
resolveConstraintReg(TargetSchema const& schema, std::string const& name,
                     char const* what) {
    auto const ord = schema.registerByName(name);
    if (!ord.has_value()) {
        std::fputs("dss::Lir fatal: LirBuilder::regConstraintPoolAdd: ", stderr);
        std::fputs(what, stderr);
        std::fputs(" register name '", stderr);
        std::fputs(name.c_str(), stderr);
        std::fputs("' is not in the target schema's register table\n", stderr);
        std::abort();
    }
    return *ord;
}

void resolveConstraintArray(TargetSchema const& schema,
                            std::vector<std::string> const& names,
                            std::vector<std::uint16_t>& ordinals,
                            char const* what) {
    ordinals.clear();
    ordinals.reserve(names.size());
    for (auto const& n : names) {
        ordinals.push_back(resolveConstraintReg(schema, n, what));
    }
}

void resolveConstraintRoles(
    TargetSchema const& schema,
    std::vector<std::pair<std::string, std::string>> const& roleNames,
    std::vector<std::pair<std::string, std::uint16_t>>& roleOrdinals,
    char const* what) {
    roleOrdinals.clear();
    roleOrdinals.reserve(roleNames.size());
    for (auto const& [role, reg] : roleNames) {
        roleOrdinals.emplace_back(role, resolveConstraintReg(schema, reg, what));
    }
}

} // namespace

std::uint32_t
LirBuilder::regConstraintPoolAdd(ImplicitRegisterConstraint constraint) {
    // ★ The caller's ordinal arrays (if any) are OVERWRITTEN, never
    // merged or checked-against. See the header docblock: names are the
    // authored source of truth in `ImplicitRegisterConstraint`'s own
    // contract, so re-deriving is what makes "the names and the ordinals
    // name the same registers" true by construction rather than by
    // review. The target's register table is the only vocabulary
    // consulted — the pool stores ORDINALS and never learns which CPU
    // this is.
    resolveConstraintArray(target_, constraint.inputNames,
                           constraint.inputOrdinals, "implicit-input");
    resolveConstraintArray(target_, constraint.outputNames,
                           constraint.outputOrdinals, "implicit-output");
    resolveConstraintArray(target_, constraint.clobberedNames,
                           constraint.clobberedOrdinals, "clobbered");
    resolveConstraintRoles(target_, constraint.inputRoleNames,
                           constraint.inputRoleOrdinals, "input-role");
    resolveConstraintRoles(target_, constraint.outputRoleNames,
                           constraint.outputRoleOrdinals, "output-role");
    // ★★★ `outputs ⊆ clobbered`, ENFORCED WHERE THE ENTRY IS BUILT
    // (D-LIR-PER-INSTRUCTION-OUTPUTS-NOT-ENFORCED-SUBSET-OF-CLOBBERED).
    // The `.target.json` loader enforces this for the per-OPCODE
    // carrier; until now the per-INSTRUCTION carrier — which meets no
    // loader — was governed by a COMMENT. The register allocator omits
    // outputs from its forbidden set on the strength of the rule, so a
    // violating entry does not fail loudly downstream: it leaves a live
    // value in a register the instruction overwrites, with no
    // diagnostic. Checked AFTER resolution so the ordinals it reads are
    // the builder's own, never a caller's (which are overwritten
    // above), and so an unresolvable NAME is still reported as such.
    //
    // ⛔ THE ALTERNATIVE — teaching the allocator to exclude outputs —
    // was rejected deliberately: it would diverge the per-instruction
    // path from the per-opcode one and pessimise every shipped compound
    // op (`idiv`, shift-by-CL) whose output legitimately aliases an
    // operand. The two carriers must agree by construction.
    if (auto const bad = constraint.firstOutputNotClobbered();
        bad.has_value()) {
        std::fputs("dss::Lir fatal: LirBuilder::regConstraintPoolAdd: "
                   "implicit-output register '", stderr);
        std::fputs(constraint.outputNames[*bad].c_str(), stderr);
        std::fputs("' is not in this constraint's clobbered set. Every "
                   "register the instruction WRITES is by definition "
                   "clobbered for any value live across it, and register "
                   "allocation omits outputs from its forbidden set on "
                   "exactly that basis — so accepting this entry would "
                   "let a live value keep a register the instruction "
                   "overwrites, with no diagnostic. This mirrors the "
                   "rule the .target.json loader enforces for the "
                   "per-opcode carrier; a producer fed by USER text must "
                   "pre-validate with `firstOutputNotClobbered()` and "
                   "REPORT.\n", stderr);
        std::abort();
    }
    return regConstraintPool_.add(std::move(constraint));
}

void LirBuilder::setInstRegConstraints(LirInstId inst,
                                       std::uint32_t poolIndex) {
    if (inst.arenaTag != moduleId_.v) {
        lirFatal("LirBuilder::setInstRegConstraints: cross-module inst id");
    }
    if (poolIndex >= regConstraintPool_.size()) {
        lirFatal("LirBuilder::setInstRegConstraints: pool index out of range "
                 "(call regConstraintPoolAdd first)");
    }
    // `ArenaBuilder::at` re-validates the id against the arena bounds +
    // tag, so an id from a DIFFERENT builder of the same module (there is
    // no such thing today, but the guard is free) still cannot land here.
    instArena_.at(inst).regConstraints =
        lirRegConstraintHandleForIndex(poolIndex);
}

void LirBuilder::orInstFlags(LirInstId inst, std::uint8_t flags) {
    if (inst.arenaTag != moduleId_.v) {
        lirFatal("LirBuilder::orInstFlags: cross-module inst id");
    }
    // `ArenaBuilder::at` re-validates index + tag, so an id this builder
    // never minted cannot land here.
    auto& slot = instArena_.at(inst);
    slot.flags = static_cast<std::uint8_t>(slot.flags | flags);
}

LirReg LirBuilder::instResult(LirInstId inst) const {
    if (inst.arenaTag != moduleId_.v) {
        lirFatal("LirBuilder::instResult: cross-module inst id");
    }
    return instArena_.at(inst).result;
}

LirInstId LirBuilder::lastInst() const {
    // Slot 0 is the arena's reserved sentinel, so `size() <= 1` means
    // "nothing appended".
    if (instArena_.size() <= 1) {
        lirFatal("LirBuilder::lastInst: no instruction has been appended");
    }
    return LirInstId{static_cast<std::uint32_t>(instArena_.size() - 1),
                     moduleId_.v};
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
        std::move(regConstraintPool_),
    };
}

} // namespace dss
