#pragma once

#include "core/export.hpp"
#include "core/substrate/arena_attribute.hpp"
#include "core/substrate/arena_container.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_id.hpp"
#include "hir/hir_kind_registry.hpp"
#include "hir/hir_node.hpp"
#include "hir/hir_op.hpp"
#include "hir/hir_op_registry.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// `Hir` (HR1) — the frozen, immutable HIR module: a dense arena of `HirNode`s
// (dogfooding the SP1 substrate, like the type interner) plus a child-id pool
// for variable-arity children (mirroring Tree's childIndex_ and the interner's
// operandPool_). Built bottom-up by `HirBuilder`, then `finish(root) &&` freezes
// it. One module per `HirModuleId`; the id is provenance-stamped onto every
// `HirNodeId` so cross-module misuse aborts (the SH3 / SP1 cross-arena guard).
//
// `Hir` satisfies the substrate `Arena` concept, so `HirAttribute<T>` side-tables
// (HR5: SourceSpan, FfiMetadata, …) bind to it exactly as `NodeAttribute<T>`
// binds to `Tree` — annotation without mutating the frozen module.

namespace dss {

class DSS_EXPORT Hir {
public:
    using Arena = substrate::ArenaContainer<detail::HirNode, HirNodeId, HirModuleId>;

    // Substrate `Arena` concept surface (so ArenaAttribute<Hir, T> binds).
    using IdType  = HirNodeId;
    using TagType = HirModuleId;

    // The empty module — the transient state before a builder hands over.
    Hir() noexcept = default;

    // The lockstep invariant `parentOf.size() == arena.nodeCount()` is checked
    // here at the construction boundary — any direct (non-builder) ctor misuse
    // aborts loud rather than silently OOB-reading `parentOf_[id.v]` later.
    Hir(Arena arena, std::vector<HirNodeId> childPool, std::vector<HirNodeId> parentOf,
        HirNodeId root, std::string sourceLanguage, HirKindRegistry registry,
        HirOpRegistry opRegistry) noexcept;

    // Move-only: the arena is move-only (its cross-arena tag must stay unique),
    // so copying a module is meaningless — downstream consumers move it along.
    // Custom moves reset the source to a default-constructed observable state
    // (empty arena, invalid root, empty pools/registry) rather than the
    // std-lib's "valid but unspecified" — same fail-loud-on-misuse-after-move
    // discipline as `substrate::ArenaAttribute`.
    Hir(Hir const&)            = delete;
    Hir& operator=(Hir const&) = delete;
    Hir(Hir&&) noexcept;
    Hir& operator=(Hir&&) noexcept;

    // ── identity / introspection ──
    [[nodiscard]] HirModuleId      id()        const noexcept { return arena_.id(); }
    [[nodiscard]] std::size_t      nodeCount() const noexcept { return arena_.nodeCount(); }
    [[nodiscard]] bool             empty()     const noexcept { return arena_.empty(); }
    [[nodiscard]] HirNodeId        root()      const noexcept { return root_; }
    [[nodiscard]] std::string_view sourceLanguage() const noexcept { return sourceLanguage_; }
    [[nodiscard]] HirKindRegistry const& registry()   const noexcept { return registry_; }
    [[nodiscard]] HirOpRegistry const&   opRegistry() const noexcept { return opRegistry_; }

    // ── node accessors (bounds- + cross-module-checked via arena_.at) ──
    [[nodiscard]] HirKind       kind(HirNodeId id)    const { return arena_.at(id).kind; }
    [[nodiscard]] HirFlags      flags(HirNodeId id)   const { return arena_.at(id).flags; }
    [[nodiscard]] TypeId        typeId(HirNodeId id)  const { return arena_.at(id).typeId; }
    [[nodiscard]] std::uint32_t payload(HirNodeId id) const { return arena_.at(id).payload; }
    // Parent link from the parallel array. `arena_.at` validates bounds +
    // module provenance first (so cross-module misuse aborts here too); the
    // root's parent is InvalidHirNode.
    [[nodiscard]] HirNodeId parent(HirNodeId id) const;

    // The node's children, in source/build order. Empty span for a leaf. Aborts
    // if the POD's `childStart`/`childCount` address past the pool — mirrors
    // `Tree::children`'s fail-loud guard against corrupt POD data.
    [[nodiscard]] std::span<HirNodeId const> children(HirNodeId id) const;

private:
    Arena                  arena_;
    std::vector<HirNodeId> childPool_;
    // Parent-link parallel array (indexed by node.v; slot 0 is the sentinel —
    // invalid). Kept out of `HirNode` to preserve the small POD: kind/type
    // sweeps stay dense in cache. The constructor enforces
    // `parentOf_.size() == arena_.nodeCount()` so this index is always safe.
    std::vector<HirNodeId> parentOf_;
    HirNodeId              root_;
    std::string            sourceLanguage_;
    HirKindRegistry        registry_;
    HirOpRegistry          opRegistry_;
};

// `HirAttribute<T>` is the `ArenaT = Hir` alias of the generalized side-table —
// the HIR analog of `NodeAttribute<T>`. The static_assert documents (and pins)
// that `Hir` actually models the substrate `Arena` concept.
static_assert(substrate::Arena<Hir>,
              "Hir must satisfy substrate::Arena so HirAttribute<T> can bind to it");

template <class T>
using HirAttribute = substrate::ArenaAttribute<Hir, T>;

// ── HirBuilder ───────────────────────────────────────────────────────────────
//
// Single-use, move-only. Builds bottom-up: create children first, then an
// `addParent` over their ids (mirroring how an expression tree — operands before
// operator — and the type interner naturally build). `finish(root) &&` freezes.
class DSS_EXPORT HirBuilder {
public:
    // Default ctor mints a fresh HirModuleId from the monotonic counter (the
    // nextModuleId / nextTreeId discipline). The explicit-tag ctor lets tests
    // pin a known module id (e.g. to construct the cross-module guard scenario).
    explicit HirBuilder(std::string sourceLanguage = {});
    HirBuilder(HirModuleId tag, std::string sourceLanguage);

    // Move-only. The default moves are sufficient here: `HirBuilder` is
    // single-use by contract (`finish() &&` consumes it), so a moved-from
    // builder is never legitimately observed — only destroyed. `Hir` (above)
    // has explicit-reset moves because it IS a long-lived value whose
    // moved-from state may be observed in containers / handoffs.
    HirBuilder(HirBuilder const&)            = delete;
    HirBuilder& operator=(HirBuilder const&) = delete;
    HirBuilder(HirBuilder&&) noexcept        = default;
    HirBuilder& operator=(HirBuilder&&) noexcept = default;

    [[nodiscard]] HirModuleId id()   const noexcept { return arena_.id(); }
    [[nodiscard]] std::size_t size() const noexcept { return arena_.size(); }
    // The build's extension-kind registry — register the kinds a lowering emits
    // beyond the core enum here; ownership transfers into the frozen Hir. NOT
    // `[[nodiscard]]`: this is a mutator getter used for chaining (e.g.
    // `b.registry().registerExtension(...)`) — the reference itself isn't the
    // value of interest, the call on it is.
    HirKindRegistry& registry() noexcept { return registry_; }

    // The build's extension-operator registry — register the extension operators
    // a lowering emits beyond the core `HirOpKind` here; ownership transfers into
    // the frozen Hir. Not `[[nodiscard]]` for the same reason as `registry()`.
    HirOpRegistry& opRegistry() noexcept { return opRegistry_; }

    // A leaf node (no children).
    HirNodeId addLeaf(HirKind kind, TypeId typeId = InvalidType,
                      std::uint32_t payload = 0, HirFlags flags = HirFlags::None);

    // A node over already-built children. Appends the child ids contiguously
    // into the pool and back-patches each child's parent. A child that already
    // has a parent (double-attach / shared-subtree) is a structural error and
    // aborts loud — HIR is tree-shaped, every node has exactly one parent.
    HirNodeId addParent(HirKind kind, std::span<HirNodeId const> children,
                        TypeId typeId = InvalidType, std::uint32_t payload = 0,
                        HirFlags flags = HirFlags::None);

    // ── typed expression helpers (HR2) ──────────────────────────────────────
    //
    // Thin, self-documenting wrappers over addLeaf/addParent for the expression
    // vocabulary. Each fixes its kind's child arity + payload convention in one
    // place, so a malformed expression node can't be spelled. Every helper takes
    // the node's resolved result `type`; the HR2 verifier (HirVerifier) asserts
    // it is `valid()`. `makeBinaryOp`/`makeUnaryOp` additionally assert the
    // operator's arity matches at construction (fail-loud abort on mismatch).

    // Literal value. `literalIndex` is the per-CU literal-pool index (payload).
    HirNodeId makeLiteral(TypeId type, std::uint32_t literalIndex = 0,
                          HirFlags flags = HirFlags::None);
    // Read of a resolved symbol. `symbol` is the SymbolId.v (payload).
    HirNodeId makeRef(TypeId type, std::uint32_t symbol = 0,
                      HirFlags flags = HirFlags::None);

    // Binary operator over [lhs, rhs]. Aborts unless the operator is binary.
    HirNodeId makeBinaryOp(HirOpKind op, HirNodeId lhs, HirNodeId rhs, TypeId type,
                           HirFlags flags = HirFlags::None);
    HirNodeId makeBinaryOp(HirOpId op, HirNodeId lhs, HirNodeId rhs, TypeId type,
                           HirFlags flags = HirFlags::None);
    // Unary operator over [operand]. Aborts unless the operator is unary.
    HirNodeId makeUnaryOp(HirOpKind op, HirNodeId operand, TypeId type,
                          HirFlags flags = HirFlags::None);
    HirNodeId makeUnaryOp(HirOpId op, HirNodeId operand, TypeId type,
                          HirFlags flags = HirFlags::None);

    // Call: children are [callee, args...]; `type` is the call's result type.
    HirNodeId makeCall(HirNodeId callee, std::span<HirNodeId const> args, TypeId type,
                       HirFlags flags = HirFlags::None);
    // Intrinsic call: children are [args...]; `intrinsicId` is the payload.
    HirNodeId makeIntrinsicCall(std::uint32_t intrinsicId, std::span<HirNodeId const> args,
                                TypeId type, HirFlags flags = HirFlags::None);

    // Explicit conversion of [operand] to `type` (the target type).
    HirNodeId makeCast(HirNodeId operand, TypeId type, HirFlags flags = HirFlags::None);
    // Field read off [base]; `fieldIndex` is the payload, `type` the field type.
    HirNodeId makeMemberAccess(HirNodeId base, std::uint32_t fieldIndex, TypeId type,
                               HirFlags flags = HirFlags::None);
    // Subscript: children are [base, index]; `type` is the element type.
    HirNodeId makeIndex(HirNodeId base, HirNodeId index, TypeId type,
                        HirFlags flags = HirFlags::None);
    // Vector swizzle off [base]; `componentMask` is the payload.
    HirNodeId makeSwizzle(HirNodeId base, std::uint32_t componentMask, TypeId type,
                          HirFlags flags = HirFlags::None);
    // Aggregate construction over [fields...]; `type` is the aggregate type.
    HirNodeId makeConstructAggregate(std::span<HirNodeId const> fields, TypeId type,
                                     HirFlags flags = HirFlags::None);
    // Conditional: children are [cond, thenExpr, elseExpr].
    HirNodeId makeTernary(HirNodeId cond, HirNodeId thenExpr, HirNodeId elseExpr,
                          TypeId type, HirFlags flags = HirFlags::None);
    // Short-circuit logical-and / -or over [lhs, rhs] (a distinct kind from
    // BinaryOp because evaluation is short-circuit, not eager).
    HirNodeId makeLogicalAnd(HirNodeId lhs, HirNodeId rhs, TypeId type,
                             HirFlags flags = HirFlags::None);
    HirNodeId makeLogicalOr(HirNodeId lhs, HirNodeId rhs, TypeId type,
                            HirFlags flags = HirFlags::None);
    // sizeof a type, given as a [TypeRef] child; `type` is the size's type.
    HirNodeId makeSizeOf(HirNodeId typeRef, TypeId type, HirFlags flags = HirFlags::None);
    // Address-of [operand]; `type` is the resulting pointer type.
    HirNodeId makeAddressOf(HirNodeId operand, TypeId type, HirFlags flags = HirFlags::None);
    // Pointer dereference of [operand]; `type` is the pointee type.
    HirNodeId makeDeref(HirNodeId operand, TypeId type, HirFlags flags = HirFlags::None);
    // A type used as a value; `type` is the referenced lattice type. Leaf.
    HirNodeId makeTypeRef(TypeId type, HirFlags flags = HirFlags::None);

    // Freeze. `root` becomes the module's entry node and must be a node this
    // builder produced. Single-use; the builder is consumed.
    [[nodiscard]] Hir finish(HirNodeId root) &&;

    // Monotonic HirModuleId allocator (mirrors TreeBuilder::nextTreeId). Ids
    // start at 1; 0 is the InvalidHirModule sentinel. Thread-safe: the counter
    // is a process-global `std::atomic<std::uint32_t>` shared with any other
    // builder/hand-fabrication path that mints a module id. Aborts on
    // exhaustion (uint32 wrap) rather than recycling id 0 and defeating the
    // cross-module guard silently.
    [[nodiscard]] static HirModuleId nextModuleId() noexcept;

private:
    substrate::ArenaBuilder<detail::HirNode, HirNodeId, HirModuleId> arena_;
    std::vector<HirNodeId> childPool_;
    // Parallel parent array — grows in lockstep with the arena. Slot 0 is the
    // sentinel (invalid); each addLeaf/addParent appends one slot.
    std::vector<HirNodeId> parentOf_;
    std::string            sourceLanguage_;
    HirKindRegistry        registry_;
    HirOpRegistry          opRegistry_;
};

} // namespace dss
