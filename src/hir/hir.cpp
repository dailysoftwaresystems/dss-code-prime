#include "hir/hir.hpp"

#include "core/substrate/mint_monotonic_id.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace dss {

namespace {

// Reset a `Hir`-shaped object's fields to the default-constructed observable
// state. Used by the custom move ops on both `Hir` and `HirBuilder` so a
// moved-from instance reports `empty() == true` / `size() == 0` /
// `id().valid() == false` / `root().valid() == false`, instead of relying on
// the std-lib's "valid but unspecified" guarantee on the underlying containers.
template <class Arena>
void resetMovedFrom_(Arena& arena,
                     std::vector<HirNodeId>& childPool,
                     std::vector<HirNodeId>& parentOf,
                     std::string& sourceLanguage,
                     HirKindRegistry& registry,
                     HirOpRegistry& opRegistry,
                     HirIntrinsicRegistry& intrinsicRegistry) noexcept {
    arena = Arena{};
    childPool.clear();
    parentOf.clear();
    sourceLanguage.clear();
    registry = HirKindRegistry{};
    opRegistry = HirOpRegistry{};
    intrinsicRegistry = HirIntrinsicRegistry{};
}

} // namespace

// ── Hir ─────────────────────────────────────────────────────────────────────

Hir::Hir(Arena arena, std::vector<HirNodeId> childPool, std::vector<HirNodeId> parentOf,
         HirNodeId root, std::string sourceLanguage, HirKindRegistry registry,
         HirOpRegistry opRegistry, HirIntrinsicRegistry intrinsicRegistry) noexcept
    : arena_(std::move(arena)),
      childPool_(std::move(childPool)),
      parentOf_(std::move(parentOf)),
      root_(root),
      sourceLanguage_(std::move(sourceLanguage)),
      registry_(std::move(registry)),
      opRegistry_(std::move(opRegistry)),
      intrinsicRegistry_(std::move(intrinsicRegistry)) {
    // The parallel parent array must align 1:1 with the arena (slot 0 sentinel
    // + one slot per minted node). HirBuilder maintains this in lockstep; this
    // boundary check catches any direct ctor misuse before `parent()` could
    // silently OOB-read the parallel array.
    if (parentOf_.size() != arena_.nodeCount()) {
        std::fprintf(stderr,
                     "dss::Hir fatal: parentOf_/arena_ size mismatch "
                     "(parentOf=%zu, arena=%zu) — invariant violated\n",
                     parentOf_.size(), arena_.nodeCount());
        std::abort();
    }
}

Hir::Hir(Hir&& other) noexcept
    : arena_(std::move(other.arena_)),
      childPool_(std::move(other.childPool_)),
      parentOf_(std::move(other.parentOf_)),
      root_(std::exchange(other.root_, InvalidHirNode)),
      sourceLanguage_(std::move(other.sourceLanguage_)),
      registry_(std::move(other.registry_)),
      opRegistry_(std::move(other.opRegistry_)),
      intrinsicRegistry_(std::move(other.intrinsicRegistry_)) {
    resetMovedFrom_(other.arena_, other.childPool_, other.parentOf_,
                    other.sourceLanguage_, other.registry_, other.opRegistry_,
                    other.intrinsicRegistry_);
}

Hir& Hir::operator=(Hir&& other) noexcept {
    if (this == &other) return *this;
    arena_          = std::move(other.arena_);
    childPool_      = std::move(other.childPool_);
    parentOf_       = std::move(other.parentOf_);
    root_           = std::exchange(other.root_, InvalidHirNode);
    sourceLanguage_ = std::move(other.sourceLanguage_);
    registry_       = std::move(other.registry_);
    opRegistry_     = std::move(other.opRegistry_);
    intrinsicRegistry_ = std::move(other.intrinsicRegistry_);
    resetMovedFrom_(other.arena_, other.childPool_, other.parentOf_,
                    other.sourceLanguage_, other.registry_, other.opRegistry_,
                    other.intrinsicRegistry_);
    return *this;
}

HirNodeId Hir::parent(HirNodeId id) const {
    // Validate provenance + bounds via arena_.at first (so cross-module misuse
    // aborts here too), then the constructor-checked lockstep guarantees
    // parentOf_[id.v] is in bounds.
    (void)arena_.at(id);
    return parentOf_[id.v];
}

std::span<HirNodeId const> Hir::children(HirNodeId id) const {
    detail::HirNode const& n = arena_.at(id);
    // Mirrors `Tree::children`: a corrupted POD whose `childStart+childCount`
    // addresses past the pool aborts rather than yielding a span over garbage.
    // The builder path can never trip it; this guards the hand-fabrication
    // surface the rest of the project leaves open for tests/lowering.
    if (static_cast<std::size_t>(n.childStart) + n.childCount > childPool_.size()) {
        std::fprintf(stderr,
                     "dss::Hir fatal: children: child range [%u, %u) exceeds child pool "
                     "size %zu\n",
                     n.childStart, n.childStart + n.childCount, childPool_.size());
        std::abort();
    }
    return std::span<HirNodeId const>{childPool_.data() + n.childStart, n.childCount};
}

// ── HirBuilder ──────────────────────────────────────────────────────────────

HirModuleId HirBuilder::nextModuleId() noexcept {
    // Aborts on uint32 overflow — see `substrate::mintMonotonicId`.
    return substrate::mintMonotonicId<HirModuleId>();
}

HirBuilder::HirBuilder(std::string sourceLanguage)
    : arena_(nextModuleId()), sourceLanguage_(std::move(sourceLanguage)) {
    // Slot 0 in the parent array mirrors the arena's slot-0 sentinel — the
    // two stay index-aligned so node.v keys both.
    parentOf_.emplace_back();
}

HirBuilder::HirBuilder(HirModuleId tag, std::string sourceLanguage)
    : arena_(tag), sourceLanguage_(std::move(sourceLanguage)) {
    parentOf_.emplace_back();
}


HirNodeId HirBuilder::addLeaf(HirKind kind, TypeId typeId,
                              std::uint32_t payload, HirFlags flags) {
    detail::HirNode n;
    n.kind    = kind;
    n.flags   = flags;
    n.typeId  = typeId;
    n.payload = payload;
    // childStart/childCount default to 0 — a leaf addresses no pool slots.
    HirNodeId const id = arena_.addNode(n);
    parentOf_.emplace_back();                  // unattached: parent invalid
    return id;
}

HirNodeId HirBuilder::addParent(HirKind kind, std::span<HirNodeId const> children,
                                TypeId typeId, std::uint32_t payload, HirFlags flags) {
    detail::HirNode n;
    n.kind       = kind;
    n.flags      = flags;
    n.typeId     = typeId;
    n.payload    = payload;
    n.childStart = static_cast<std::uint32_t>(childPool_.size());
    n.childCount = static_cast<std::uint32_t>(children.size());
    HirNodeId const id = arena_.addNode(n);
    parentOf_.emplace_back();                  // the new parent itself is unattached

    for (HirNodeId child : children) {
        // arena_.at validates provenance + bounds; the parallel-array check
        // catches a double-attach (a node handed to two parents) — structural
        // corruption, not a recoverable miss.
        (void)arena_.at(child);
        if (parentOf_[child.v].valid()) {
            std::fprintf(stderr,
                         "dss::HirBuilder fatal: addParent: HirNodeId=%u already has a "
                         "parent (double-attach / shared subtree); HIR is tree-shaped\n",
                         child.v);
            std::abort();
        }
        childPool_.push_back(child);
        parentOf_[child.v] = id;
    }
    return id;
}

Hir HirBuilder::finish(HirNodeId root) && {
    // The root must be a node this builder produced (validates bounds +
    // provenance; aborts loud otherwise rather than freezing a dangling root).
    (void)arena_.at(root);
    // Beyond "exists in the arena", the root must actually be a root — i.e. it
    // has no parent. `finish` is single-shot, so freezing a still-attached
    // inner node as the module root would silently elide every ancestor that
    // was also built. Catch it loudly.
    if (parentOf_[root.v].valid()) {
        std::fprintf(stderr,
                     "dss::HirBuilder fatal: finish: HirNodeId=%u is not a root (parent "
                     "HirNodeId=%u); the module root must be parentless\n",
                     root.v, parentOf_[root.v].v);
        std::abort();
    }
    return Hir{std::move(arena_).finish(), std::move(childPool_),
               std::move(parentOf_), root,
               std::move(sourceLanguage_), std::move(registry_),
               std::move(opRegistry_), std::move(intrinsicRegistry_)};
}

// ── typed expression helpers (HR2) ──────────────────────────────────────────

namespace {

// A construction-time arity check: `makeBinaryOp` on a unary operator (or vice
// versa) is a caller bug that would build a structurally-impossible node, so it
// aborts loud rather than producing malformed HIR the verifier only catches
// later. `opLabel` names the operator for the message (the core opName() or the
// extension descriptor name).
[[noreturn]] void arityMismatch_(std::string_view opLabel, HirOpArity declared,
                                 HirOpArity expected) {
    std::fprintf(stderr,
                 "dss::HirBuilder fatal: operator '%.*s' is %s but was used to build a "
                 "%s expression node\n",
                 static_cast<int>(opLabel.size()), opLabel.data(),
                 arityLabel(declared), arityLabel(expected));
    std::abort();
}

} // namespace

HirNodeId HirBuilder::makeLiteral(TypeId type, std::uint32_t literalIndex, HirFlags flags) {
    return addLeaf(HirKind::Literal, type, literalIndex, flags);
}

HirNodeId HirBuilder::makeRef(TypeId type, std::uint32_t symbol, HirFlags flags) {
    return addLeaf(HirKind::Ref, type, symbol, flags);
}

HirNodeId HirBuilder::makeBinaryOp(HirOpKind op, HirNodeId lhs, HirNodeId rhs, TypeId type,
                                   HirFlags flags) {
    HirOpArity const declared = arityOf(op);
    if (declared != HirOpArity::Binary) {
        arityMismatch_(opName(op), declared, HirOpArity::Binary);
    }
    HirNodeId const kids[] = {lhs, rhs};
    return addParent(HirKind::BinaryOp, kids, type, encodeOp(op), flags);
}

HirNodeId HirBuilder::makeBinaryOp(HirOpId op, HirNodeId lhs, HirNodeId rhs, TypeId type,
                                   HirFlags flags) {
    HirOpDescriptor const& d = opRegistry_.descriptor(op);  // aborts if never minted
    if (d.arity() != HirOpArity::Binary) {
        arityMismatch_(d.name(), d.arity(), HirOpArity::Binary);
    }
    HirNodeId const kids[] = {lhs, rhs};
    return addParent(HirKind::BinaryOp, kids, type, encodeOp(op), flags);
}

HirNodeId HirBuilder::makeUnaryOp(HirOpKind op, HirNodeId operand, TypeId type, HirFlags flags) {
    HirOpArity const declared = arityOf(op);
    if (declared != HirOpArity::Unary) {
        arityMismatch_(opName(op), declared, HirOpArity::Unary);
    }
    HirNodeId const kids[] = {operand};
    return addParent(HirKind::UnaryOp, kids, type, encodeOp(op), flags);
}

HirNodeId HirBuilder::makeUnaryOp(HirOpId op, HirNodeId operand, TypeId type, HirFlags flags) {
    HirOpDescriptor const& d = opRegistry_.descriptor(op);  // aborts if never minted
    if (d.arity() != HirOpArity::Unary) {
        arityMismatch_(d.name(), d.arity(), HirOpArity::Unary);
    }
    HirNodeId const kids[] = {operand};
    return addParent(HirKind::UnaryOp, kids, type, encodeOp(op), flags);
}

HirNodeId HirBuilder::makeCall(HirNodeId callee, std::span<HirNodeId const> args, TypeId type,
                               HirFlags flags) {
    std::vector<HirNodeId> kids;
    kids.reserve(args.size() + 1);
    kids.push_back(callee);
    kids.insert(kids.end(), args.begin(), args.end());
    return addParent(HirKind::Call, kids, type, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeIntrinsicCall(HirIntrinsicId intrinsic,
                                        std::span<HirNodeId const> args, TypeId type,
                                        HirFlags flags) {
    return addParent(HirKind::IntrinsicCall, args, type, intrinsic.v, flags);
}

HirNodeId HirBuilder::makeIntrinsicCall(std::uint32_t intrinsicId,
                                        std::span<HirNodeId const> args, TypeId type,
                                        HirFlags flags) {
    return addParent(HirKind::IntrinsicCall, args, type, intrinsicId, flags);
}

HirNodeId HirBuilder::makeCast(HirNodeId operand, TypeId type, HirFlags flags) {
    HirNodeId const kids[] = {operand};
    return addParent(HirKind::Cast, kids, type, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeMemberAccess(HirNodeId base, std::uint32_t fieldIndex, TypeId type,
                                       HirFlags flags) {
    HirNodeId const kids[] = {base};
    return addParent(HirKind::MemberAccess, kids, type, fieldIndex, flags);
}

HirNodeId HirBuilder::makeIndex(HirNodeId base, HirNodeId index, TypeId type, HirFlags flags) {
    HirNodeId const kids[] = {base, index};
    return addParent(HirKind::Index, kids, type, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeSwizzle(HirNodeId base, std::uint32_t componentMask, TypeId type,
                                  HirFlags flags) {
    HirNodeId const kids[] = {base};
    return addParent(HirKind::Swizzle, kids, type, componentMask, flags);
}

HirNodeId HirBuilder::makeConstructAggregate(std::span<HirNodeId const> fields, TypeId type,
                                             HirFlags flags) {
    return addParent(HirKind::ConstructAggregate, fields, type, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeTernary(HirNodeId cond, HirNodeId thenExpr, HirNodeId elseExpr,
                                  TypeId type, HirFlags flags) {
    HirNodeId const kids[] = {cond, thenExpr, elseExpr};
    return addParent(HirKind::Ternary, kids, type, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeSeqExpr(std::span<HirNodeId const> stmts, HirNodeId result,
                                  TypeId type, HirFlags flags) {
    std::vector<HirNodeId> kids(stmts.begin(), stmts.end());
    kids.push_back(result);   // result is always the LAST child
    return addParent(HirKind::SeqExpr, kids, type, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeLogicalAnd(HirNodeId lhs, HirNodeId rhs, TypeId type, HirFlags flags) {
    HirNodeId const kids[] = {lhs, rhs};
    return addParent(HirKind::LogicalAnd, kids, type, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeLogicalOr(HirNodeId lhs, HirNodeId rhs, TypeId type, HirFlags flags) {
    HirNodeId const kids[] = {lhs, rhs};
    return addParent(HirKind::LogicalOr, kids, type, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeSizeOf(HirNodeId typeRef, TypeId type, HirFlags flags) {
    HirNodeId const kids[] = {typeRef};
    return addParent(HirKind::SizeOf, kids, type, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeAddressOf(HirNodeId operand, TypeId type, HirFlags flags) {
    HirNodeId const kids[] = {operand};
    return addParent(HirKind::AddressOf, kids, type, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeDeref(HirNodeId operand, TypeId type, HirFlags flags) {
    HirNodeId const kids[] = {operand};
    return addParent(HirKind::Deref, kids, type, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeTypeRef(TypeId type, HirFlags flags) {
    return addLeaf(HirKind::TypeRef, type, /*payload=*/0, flags);
}

// ── typed statement helpers (HR3) ───────────────────────────────────────────

HirNodeId HirBuilder::makeBlock(std::span<HirNodeId const> stmts, HirFlags flags) {
    return addParent(HirKind::Block, stmts, InvalidType, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeIfStmt(HirNodeId cond, HirNodeId thenStmt,
                                 std::optional<HirNodeId> elseStmt, HirFlags flags) {
    if (elseStmt) {
        HirNodeId const kids[] = {cond, thenStmt, *elseStmt};
        return addParent(HirKind::IfStmt, kids, InvalidType, /*payload=*/0, flags);
    }
    HirNodeId const kids[] = {cond, thenStmt};
    return addParent(HirKind::IfStmt, kids, InvalidType, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeWhileStmt(HirNodeId cond, HirNodeId body, HirFlags flags) {
    HirNodeId const kids[] = {cond, body};
    return addParent(HirKind::WhileStmt, kids, InvalidType, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeDoWhileStmt(HirNodeId body, HirNodeId cond, HirFlags flags) {
    HirNodeId const kids[] = {body, cond};
    return addParent(HirKind::DoWhileStmt, kids, InvalidType, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeForStmt(std::optional<HirNodeId> init, std::optional<HirNodeId> cond,
                                  std::optional<HirNodeId> update, HirNodeId body,
                                  HirFlags flags) {
    ForClause mask = ForClause::None;
    std::vector<HirNodeId> kids;
    kids.reserve(4);
    if (init)   { mask = mask | ForClause::Init;   kids.push_back(*init); }
    if (cond)   { mask = mask | ForClause::Cond;   kids.push_back(*cond); }
    if (update) { mask = mask | ForClause::Update; kids.push_back(*update); }
    kids.push_back(body);  // body is always last
    return addParent(HirKind::ForStmt, kids, InvalidType,
                     static_cast<std::uint32_t>(mask), flags);
}

HirNodeId HirBuilder::makeSwitchStmt(HirNodeId discriminant, std::span<HirNodeId const> arms,
                                     HirFlags flags) {
    std::vector<HirNodeId> kids;
    kids.reserve(arms.size() + 1);
    kids.push_back(discriminant);
    kids.insert(kids.end(), arms.begin(), arms.end());
    return addParent(HirKind::SwitchStmt, kids, InvalidType, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeCaseArm(std::optional<HirNodeId> value, std::span<HirNodeId const> body,
                                  HirFlags flags) {
    std::vector<HirNodeId> kids;
    kids.reserve(body.size() + 1);
    std::uint32_t payload = 0;
    if (value) {
        kids.push_back(*value);          // valued arm: match value is child 0
    } else {
        payload = kCaseArmIsDefault;     // default arm: no value child
    }
    kids.insert(kids.end(), body.begin(), body.end());
    return addParent(HirKind::CaseArm, kids, InvalidType, payload, flags);
}

HirNodeId HirBuilder::makeBreak(std::uint32_t depth, HirFlags flags) {
    return addLeaf(HirKind::BreakStmt, InvalidType, depth, flags);
}

HirNodeId HirBuilder::makeContinue(std::uint32_t depth, HirFlags flags) {
    return addLeaf(HirKind::ContinueStmt, InvalidType, depth, flags);
}

HirNodeId HirBuilder::makeReturn(std::optional<HirNodeId> value, HirFlags flags) {
    if (value) {
        HirNodeId const kids[] = {*value};
        return addParent(HirKind::ReturnStmt, kids, InvalidType, /*payload=*/0, flags);
    }
    return addParent(HirKind::ReturnStmt, {}, InvalidType, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeExprStmt(HirNodeId expr, HirFlags flags) {
    HirNodeId const kids[] = {expr};
    return addParent(HirKind::ExprStmt, kids, InvalidType, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeVarDecl(TypeId declaredType, std::uint32_t symbol,
                                  std::optional<HirNodeId> init, HirFlags flags) {
    if (init) {
        HirNodeId const kids[] = {*init};
        return addParent(HirKind::VarDecl, kids, declaredType, symbol, flags);
    }
    return addParent(HirKind::VarDecl, {}, declaredType, symbol, flags);
}

HirNodeId HirBuilder::makeAssignStmt(HirNodeId target, HirNodeId value, HirFlags flags) {
    HirNodeId const kids[] = {target, value};
    return addParent(HirKind::AssignStmt, kids, InvalidType, /*payload=*/0, flags);
}

// ── typed declaration helpers (HR4) ─────────────────────────────────────────

HirNodeId HirBuilder::makeModule(std::span<HirNodeId const> decls, HirFlags flags) {
    return addParent(HirKind::Module, decls, InvalidType, /*payload=*/0, flags);
}

HirNodeId HirBuilder::makeFunction(TypeId signature, std::uint32_t symbol,
                                   std::span<HirNodeId const> params, HirNodeId body,
                                   HirFlags flags) {
    std::vector<HirNodeId> kids;
    kids.reserve(params.size() + 1);
    kids.insert(kids.end(), params.begin(), params.end());
    kids.push_back(body);  // body is always the last child
    return addParent(HirKind::Function, kids, signature, symbol, flags);
}

HirNodeId HirBuilder::makeGlobal(TypeId type, std::uint32_t symbol,
                                 std::optional<HirNodeId> init, HirFlags flags) {
    if (init) {
        HirNodeId const kids[] = {*init};
        return addParent(HirKind::Global, kids, type, symbol, flags);
    }
    return addParent(HirKind::Global, {}, type, symbol, flags);
}

HirNodeId HirBuilder::makeTypeDecl(TypeId type, std::uint32_t symbol, HirFlags flags) {
    return addLeaf(HirKind::TypeDecl, type, symbol, flags);
}

HirNodeId HirBuilder::makeExternFunction(TypeId signature, std::uint32_t symbol,
                                         std::span<HirNodeId const> params, HirFlags flags) {
    return addParent(HirKind::ExternFunction, params, signature, symbol, flags);
}

HirNodeId HirBuilder::makeExternGlobal(TypeId type, std::uint32_t symbol, HirFlags flags) {
    return addLeaf(HirKind::ExternGlobal, type, symbol, flags);
}

HirNodeId HirBuilder::makeImportGroup(std::span<HirNodeId const> members, HirFlags flags) {
    return addParent(HirKind::ImportGroup, members, InvalidType, /*payload=*/0, flags);
}

// ── structured-CF typed accessors (HR3) ─────────────────────────────────────

HirNodeId Hir::childAt(HirNodeId id, std::uint32_t i) const {
    std::span<HirNodeId const> const kids = children(id);  // fail-loud on corrupt pool range
    if (i >= kids.size()) {
        std::fprintf(stderr,
                     "dss::Hir fatal: childAt: child index %u out of range (HirNodeId=%u "
                     "has %zu children)\n",
                     i, id.v, kids.size());
        std::abort();
    }
    return kids[i];
}

HirNodeId Hir::ifCondition(HirNodeId id) const {
    assert(kind(id) == HirKind::IfStmt);
    return childAt(id, 0);
}
HirNodeId Hir::ifThen(HirNodeId id) const {
    assert(kind(id) == HirKind::IfStmt);
    return childAt(id, 1);
}
std::optional<HirNodeId> Hir::ifElse(HirNodeId id) const {
    assert(kind(id) == HirKind::IfStmt);
    auto kids = children(id);
    return kids.size() >= 3 ? std::optional<HirNodeId>{kids[2]} : std::nullopt;
}

HirNodeId Hir::loopBody(HirNodeId id) const {
    switch (kind(id)) {
        case HirKind::WhileStmt:   return childAt(id, 1);
        case HirKind::DoWhileStmt: return childAt(id, 0);
        case HirKind::ForStmt: {
            // body is always the last child; childAt aborts loud if the node is
            // malformed (no children at all).
            auto const n = static_cast<std::uint32_t>(children(id).size());
            return childAt(id, n == 0 ? 0 : n - 1);
        }
        default: assert(false && "loopBody on a non-loop kind"); return InvalidHirNode;
    }
}
std::optional<HirNodeId> Hir::loopCondition(HirNodeId id) const {
    switch (kind(id)) {
        case HirKind::WhileStmt:   return childAt(id, 0);
        case HirKind::DoWhileStmt: return childAt(id, 1);
        case HirKind::ForStmt: {
            ForClause const mask = forClauses(id);
            if (!has(mask, ForClause::Cond)) return std::nullopt;
            std::uint32_t const idx = has(mask, ForClause::Init) ? 1u : 0u;
            return childAt(id, idx);
        }
        default: assert(false && "loopCondition on a non-loop kind"); return std::nullopt;
    }
}
ForClause Hir::forClauses(HirNodeId id) const {
    assert(kind(id) == HirKind::ForStmt);
    // Mask off any stray high bits so accessors stay robust on a malformed
    // payload; the verifier (checkNodeArity) is the layer that FLAGS stray bits.
    return static_cast<ForClause>(payload(id) & kForClauseMask);
}
std::optional<HirNodeId> Hir::forInit(HirNodeId id) const {
    ForClause const mask = forClauses(id);
    if (!has(mask, ForClause::Init)) return std::nullopt;
    return childAt(id, 0);  // Init is first in the clause order when present
}
std::optional<HirNodeId> Hir::forUpdate(HirNodeId id) const {
    ForClause const mask = forClauses(id);
    if (!has(mask, ForClause::Update)) return std::nullopt;
    std::uint32_t const idx = (has(mask, ForClause::Init) ? 1u : 0u)
                            + (has(mask, ForClause::Cond) ? 1u : 0u);
    return childAt(id, idx);
}

HirNodeId Hir::switchDiscriminant(HirNodeId id) const {
    assert(kind(id) == HirKind::SwitchStmt);
    return childAt(id, 0);
}
std::span<HirNodeId const> Hir::switchArms(HirNodeId id) const {
    assert(kind(id) == HirKind::SwitchStmt);
    auto kids = children(id);
    return kids.empty() ? kids : kids.subspan(1);  // empty only on a malformed switch
}
bool Hir::caseArmIsDefault(HirNodeId id) const {
    assert(kind(id) == HirKind::CaseArm);
    return (payload(id) & kCaseArmIsDefault) != 0;
}
std::optional<HirNodeId> Hir::caseArmValue(HirNodeId id) const {
    if (caseArmIsDefault(id)) return std::nullopt;
    auto kids = children(id);
    return kids.empty() ? std::nullopt : std::optional<HirNodeId>{kids[0]};
}
std::span<HirNodeId const> Hir::caseArmBody(HirNodeId id) const {
    auto kids = children(id);
    // A valued arm's child 0 is the match value; default arms are all body.
    if (!caseArmIsDefault(id) && !kids.empty()) return kids.subspan(1);
    return kids;
}

std::uint32_t Hir::branchDepth(HirNodeId id) const {
    assert(kind(id) == HirKind::BreakStmt || kind(id) == HirKind::ContinueStmt);
    return payload(id);
}
std::optional<HirNodeId> Hir::returnValue(HirNodeId id) const {
    assert(kind(id) == HirKind::ReturnStmt);
    auto kids = children(id);
    return kids.empty() ? std::nullopt : std::optional<HirNodeId>{kids[0]};
}
HirNodeId Hir::exprStmtExpr(HirNodeId id) const {
    assert(kind(id) == HirKind::ExprStmt);
    return childAt(id, 0);
}
std::span<HirNodeId const> Hir::seqExprStmts(HirNodeId id) const {
    assert(kind(id) == HirKind::SeqExpr);
    auto kids = children(id);
    // [stmts..., result] — all but the last (childArity guarantees ≥ 1 child).
    return kids.subspan(0, kids.size() - 1);
}
HirNodeId Hir::seqExprResult(HirNodeId id) const {
    assert(kind(id) == HirKind::SeqExpr);
    auto kids = children(id);
    return kids.back();
}

TypeId Hir::varDeclType(HirNodeId id) const {
    assert(kind(id) == HirKind::VarDecl);
    return typeId(id);
}
SymbolId Hir::varDeclSymbol(HirNodeId id) const {
    assert(kind(id) == HirKind::VarDecl);
    return SymbolId{payload(id)};
}
std::optional<HirNodeId> Hir::varDeclInit(HirNodeId id) const {
    assert(kind(id) == HirKind::VarDecl);
    auto kids = children(id);
    return kids.empty() ? std::nullopt : std::optional<HirNodeId>{kids[0]};
}

HirNodeId Hir::assignTarget(HirNodeId id) const {
    assert(kind(id) == HirKind::AssignStmt);
    return childAt(id, 0);
}
HirNodeId Hir::assignValue(HirNodeId id) const {
    assert(kind(id) == HirKind::AssignStmt);
    return childAt(id, 1);
}

// ── declaration accessors (HR4) ─────────────────────────────────────────────

std::span<HirNodeId const> Hir::moduleDecls(HirNodeId id) const {
    assert(kind(id) == HirKind::Module);
    return children(id);
}

TypeId Hir::functionSignature(HirNodeId id) const {
    assert(kind(id) == HirKind::Function);
    return typeId(id);
}
SymbolId Hir::functionSymbol(HirNodeId id) const {
    assert(kind(id) == HirKind::Function);
    return SymbolId{payload(id)};
}
std::span<HirNodeId const> Hir::functionParams(HirNodeId id) const {
    assert(kind(id) == HirKind::Function);
    auto kids = children(id);  // [params…, body]; body is the last child
    return kids.empty() ? kids : kids.subspan(0, kids.size() - 1);
}
HirNodeId Hir::functionBody(HirNodeId id) const {
    assert(kind(id) == HirKind::Function);
    // The body is the last child. A bodyless Function is an arity violation the
    // verifier flags first (Function arity is {1, ∞}); aborting here is the
    // defense-in-depth path for a consumer that reached an unverified node.
    auto kids = children(id);
    if (kids.empty()) {
        std::fprintf(stderr,
                     "dss::Hir fatal: functionBody: Function HirNodeId=%u has no body child\n",
                     id.v);
        std::abort();
    }
    return kids.back();
}

TypeId Hir::globalType(HirNodeId id) const {
    assert(kind(id) == HirKind::Global);
    return typeId(id);
}
SymbolId Hir::globalSymbol(HirNodeId id) const {
    assert(kind(id) == HirKind::Global);
    return SymbolId{payload(id)};
}
std::optional<HirNodeId> Hir::globalInit(HirNodeId id) const {
    assert(kind(id) == HirKind::Global);
    auto kids = children(id);
    return kids.empty() ? std::nullopt : std::optional<HirNodeId>{kids[0]};
}

TypeId Hir::typeDeclType(HirNodeId id) const {
    assert(kind(id) == HirKind::TypeDecl);
    return typeId(id);
}
SymbolId Hir::typeDeclSymbol(HirNodeId id) const {
    assert(kind(id) == HirKind::TypeDecl);
    return SymbolId{payload(id)};
}

TypeId Hir::externFunctionSignature(HirNodeId id) const {
    assert(kind(id) == HirKind::ExternFunction);
    return typeId(id);  // may be InvalidType — extern type is optional
}
SymbolId Hir::externFunctionSymbol(HirNodeId id) const {
    assert(kind(id) == HirKind::ExternFunction);
    return SymbolId{payload(id)};
}
std::span<HirNodeId const> Hir::externFunctionParams(HirNodeId id) const {
    assert(kind(id) == HirKind::ExternFunction);
    return children(id);  // all children are params — an extern has no body
}

TypeId Hir::externGlobalType(HirNodeId id) const {
    assert(kind(id) == HirKind::ExternGlobal);
    return typeId(id);  // may be InvalidType
}
SymbolId Hir::externGlobalSymbol(HirNodeId id) const {
    assert(kind(id) == HirKind::ExternGlobal);
    return SymbolId{payload(id)};
}

std::span<HirNodeId const> Hir::importGroupMembers(HirNodeId id) const {
    assert(kind(id) == HirKind::ImportGroup);
    return children(id);
}

// ── shared structural resolver (HR3) ─────────────────────────────────────────

std::vector<HirNodeId> enclosingBranchTargets(Hir const& hir, HirNodeId id) {
    std::vector<HirNodeId> targets;
    // A well-formed module's parent chain is acyclic, so it terminates at the
    // parentless root in fewer than nodeCount() hops. Exceeding that bound means
    // a corrupt parent cycle — fail loud (like Hir::children's pool-range guard)
    // rather than silently returning a truncated target stack that would feed a
    // bogus break/continue verdict downstream.
    std::size_t const cap = hir.nodeCount();
    std::size_t steps = 0;
    for (HirNodeId cur = hir.parent(id); cur.valid(); cur = hir.parent(cur), ++steps) {
        if (steps >= cap) {
            std::fprintf(stderr,
                         "dss::enclosingBranchTargets fatal: parent chain from HirNodeId=%u "
                         "exceeds nodeCount (%zu) — corrupt cycle\n",
                         id.v, cap);
            std::abort();
        }
        if (isBranchTargetKind(hir.kind(cur))) targets.push_back(cur);
    }
    return targets;
}

} // namespace dss
