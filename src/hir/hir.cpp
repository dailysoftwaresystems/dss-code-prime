#include "hir/hir.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

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
                     HirOpRegistry& opRegistry) noexcept {
    arena = Arena{};
    childPool.clear();
    parentOf.clear();
    sourceLanguage.clear();
    registry = HirKindRegistry{};
    opRegistry = HirOpRegistry{};
}

} // namespace

// ── Hir ─────────────────────────────────────────────────────────────────────

Hir::Hir(Arena arena, std::vector<HirNodeId> childPool, std::vector<HirNodeId> parentOf,
         HirNodeId root, std::string sourceLanguage, HirKindRegistry registry,
         HirOpRegistry opRegistry) noexcept
    : arena_(std::move(arena)),
      childPool_(std::move(childPool)),
      parentOf_(std::move(parentOf)),
      root_(root),
      sourceLanguage_(std::move(sourceLanguage)),
      registry_(std::move(registry)),
      opRegistry_(std::move(opRegistry)) {
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
      opRegistry_(std::move(other.opRegistry_)) {
    resetMovedFrom_(other.arena_, other.childPool_, other.parentOf_,
                    other.sourceLanguage_, other.registry_, other.opRegistry_);
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
    resetMovedFrom_(other.arena_, other.childPool_, other.parentOf_,
                    other.sourceLanguage_, other.registry_, other.opRegistry_);
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
    static std::atomic<std::uint32_t> sCounter{0};
    // Overflow guard: a wrapped counter would mint HirModuleId{0} (==
    // InvalidHirModule) and stamp `arenaTag = 0` on every emitted HirNodeId,
    // which the cross-arena guard treats as "untagged" and lets pass —
    // SILENTLY defeating cross-module isolation. Practically unreachable, but a
    // standing fail-loud invariant.
    std::uint32_t const prev = sCounter.load(std::memory_order_relaxed);
    if (prev == std::numeric_limits<std::uint32_t>::max()) {
        std::fputs("dss::HirBuilder fatal: nextModuleId counter exhausted "
                   "(uint32 overflow)\n", stderr);
        std::abort();
    }
    return HirModuleId{++sCounter};
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
               std::move(opRegistry_)};
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

} // namespace dss
