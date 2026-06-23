#pragma once

#include "core/export.hpp"
#include "core/substrate/arena_attribute.hpp"
#include "core/substrate/arena_container.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_id.hpp"
#include "hir/hir_intrinsic_registry.hpp"
#include "hir/hir_kind_registry.hpp"
#include "hir/hir_node.hpp"
#include "hir/hir_op.hpp"
#include "hir/hir_op_registry.hpp"

#include <cstdint>
#include <optional>
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
        HirOpRegistry opRegistry, HirIntrinsicRegistry intrinsicRegistry) noexcept;

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
    [[nodiscard]] HirIntrinsicRegistry const& intrinsicRegistry() const noexcept {
        return intrinsicRegistry_;
    }

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

    // ── structured-CF typed accessors (HR3) ──────────────────────────────────
    //
    // The read side of the `make*` helpers: they hide each kind's child layout
    // and payload encoding (optional positions, the ForStmt clause mask, the
    // CaseArm default flag) so consumers — the verifier, HR8 lowering, HR7 text,
    // HR12 MIR — index by role, never by raw offset. Each asserts the node's
    // kind in debug builds; behaviour on a wrong-kind or malformed node is
    // undefined (well-formedness is the verifier's job). Optional children come
    // back as `std::optional<HirNodeId>` (nullopt = absent).

    // if: condition, then-branch, optional else-branch.
    [[nodiscard]] HirNodeId                ifCondition(HirNodeId id) const;
    [[nodiscard]] HirNodeId                ifThen(HirNodeId id)      const;
    [[nodiscard]] std::optional<HirNodeId> ifElse(HirNodeId id)     const;

    // Loop-kind-agnostic over While/DoWhile/For: the body (always present) and
    // the condition (optional — a For may omit it).
    [[nodiscard]] HirNodeId                loopBody(HirNodeId id)      const;
    [[nodiscard]] std::optional<HirNodeId> loopCondition(HirNodeId id) const;
    // For-specific clauses (decode the ForClause mask).
    [[nodiscard]] std::optional<HirNodeId> forInit(HirNodeId id)   const;
    [[nodiscard]] std::optional<HirNodeId> forUpdate(HirNodeId id) const;
    [[nodiscard]] ForClause                forClauses(HirNodeId id) const;

    // switch: discriminant + the case arms.
    [[nodiscard]] HirNodeId                  switchDiscriminant(HirNodeId id) const;
    [[nodiscard]] std::span<HirNodeId const> switchArms(HirNodeId id)         const;
    // case arm: is-default flag, optional match value, the arm's statements.
    [[nodiscard]] bool                       caseArmIsDefault(HirNodeId id) const;
    [[nodiscard]] std::optional<HirNodeId>   caseArmValue(HirNodeId id)     const;
    [[nodiscard]] std::span<HirNodeId const> caseArmBody(HirNodeId id)      const;

    // break/continue nesting index; return value; expr-stmt expression.
    [[nodiscard]] std::uint32_t            branchDepth(HirNodeId id)  const;
    [[nodiscard]] std::optional<HirNodeId> returnValue(HirNodeId id)  const;
    [[nodiscard]] HirNodeId                exprStmtExpr(HirNodeId id) const;

    // goto/label per-function ordinal (payload); a LabelStmt's labeled statement
    // (its sole child). A GotoStmt and its target LabelStmt share the ordinal.
    [[nodiscard]] std::uint32_t            labelOrdinal(HirNodeId id) const;
    [[nodiscard]] HirNodeId                labelBody(HirNodeId id)    const;
    // D-CSUBSET-COMPUTED-GOTO: IndirectGotoStmt's address expression (child 0);
    // LabelAddressOf's target label ordinal (payload, same namespace as labels).
    [[nodiscard]] HirNodeId                indirectGotoTarget(HirNodeId id) const;
    [[nodiscard]] std::uint32_t            labelAddressOrdinal(HirNodeId id) const;

    // SeqExpr: the statements evaluated for effect (all but the last child) and
    // the result expression (the last child, which supplies the SeqExpr's value
    // + type).
    [[nodiscard]] std::span<HirNodeId const> seqExprStmts(HirNodeId id)  const;
    [[nodiscard]] HirNodeId                  seqExprResult(HirNodeId id) const;

    // var decl: declared type (== typeId), declared SymbolId, optional initializer.
    [[nodiscard]] TypeId                   varDeclType(HirNodeId id)   const;
    [[nodiscard]] SymbolId                 varDeclSymbol(HirNodeId id) const;
    [[nodiscard]] std::optional<HirNodeId> varDeclInit(HirNodeId id)   const;

    // assignment: lvalue target and assigned value.
    [[nodiscard]] HirNodeId assignTarget(HirNodeId id) const;
    [[nodiscard]] HirNodeId assignValue(HirNodeId id)  const;

    // ── declaration accessors (HR4) ──────────────────────────────────────────
    // module: its top-level declarations.
    [[nodiscard]] std::span<HirNodeId const> moduleDecls(HirNodeId id) const;
    // function: FnSig, declared SymbolId, the param decls (all but last child),
    // and the body Block (last child).
    [[nodiscard]] TypeId                     functionSignature(HirNodeId id) const;
    [[nodiscard]] SymbolId                   functionSymbol(HirNodeId id)    const;
    [[nodiscard]] std::span<HirNodeId const> functionParams(HirNodeId id)    const;
    [[nodiscard]] HirNodeId                  functionBody(HirNodeId id)      const;
    // global: type, SymbolId, optional initializer.
    [[nodiscard]] TypeId                     globalType(HirNodeId id)   const;
    [[nodiscard]] SymbolId                   globalSymbol(HirNodeId id) const;
    [[nodiscard]] std::optional<HirNodeId>   globalInit(HirNodeId id)   const;
    // type declaration: the type it introduces + its SymbolId.
    [[nodiscard]] TypeId                     typeDeclType(HirNodeId id)   const;
    [[nodiscard]] SymbolId                   typeDeclSymbol(HirNodeId id) const;
    // extern function: FnSig (may be invalid), SymbolId, param decls (no body).
    [[nodiscard]] TypeId                     externFunctionSignature(HirNodeId id) const;
    [[nodiscard]] SymbolId                   externFunctionSymbol(HirNodeId id)    const;
    [[nodiscard]] std::span<HirNodeId const> externFunctionParams(HirNodeId id)    const;
    // extern global: type (may be invalid) + SymbolId.
    [[nodiscard]] TypeId                     externGlobalType(HirNodeId id)   const;
    [[nodiscard]] SymbolId                   externGlobalSymbol(HirNodeId id) const;
    // import group: the grouped members.
    [[nodiscard]] std::span<HirNodeId const> importGroupMembers(HirNodeId id) const;

private:
    // Checked positional child access for the mandatory-child accessors above:
    // aborts loud (like `children`'s pool-range guard) if `i` is past the node's
    // child count, rather than letting a compiled-out `assert` + raw span `[]`
    // become release-build UB on a structurally-malformed node. Optional-child
    // accessors don't use this — absence is legitimate there, so they return
    // `std::nullopt` via a size check.
    [[nodiscard]] HirNodeId childAt(HirNodeId id, std::uint32_t i) const;

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
    HirIntrinsicRegistry   intrinsicRegistry_;
};

// `HirAttribute<T>` is the `ArenaT = Hir` alias of the generalized side-table —
// the HIR analog of `NodeAttribute<T>`. The static_assert documents (and pins)
// that `Hir` actually models the substrate `Arena` concept.
static_assert(substrate::Arena<Hir>,
              "Hir must satisfy substrate::Arena so HirAttribute<T> can bind to it");

template <class T>
using HirAttribute = substrate::ArenaAttribute<Hir, T>;

// The enclosing loop/switch nodes of `id`, innermost-first — i.e. the structural
// branch-target stack a BreakStmt/ContinueStmt at `id` indexes into (`depth 0` =
// `result[0]`). Walks `parent()` links and collects `isBranchTargetKind` nodes.
// Shared by the verifier's break/continue rule (validates the index) and CST→HIR
// lowering (HR8, which COMPUTES the index) so the two agree by construction.
[[nodiscard]] DSS_EXPORT std::vector<HirNodeId> enclosingBranchTargets(Hir const& hir,
                                                                       HirNodeId id);

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

    // Mid-build node inspection. The frozen `Hir` exposes the same
    // shape via its own const accessors after `finish()`; pre-freeze
    // lowering passes that need to inspect a just-built subtree
    // (D-LK10-ENTRY-MAIN-IMPLICIT-RETURN reads the body's structural
    // termination state via `pathTerminates` to decide whether to
    // append a synthetic return) read through the builder directly.
    // Read-only — node mutation remains exclusively via the `make*`
    // builders so the shape invariants stay intact.
    [[nodiscard]] HirKind kind(HirNodeId id) const {
        return arena_.at(id).kind;
    }
    [[nodiscard]] HirFlags flags(HirNodeId id) const {
        return arena_.at(id).flags;
    }
    // Mid-build payload read. Frozen `Hir` exposes the same via its
    // own `payload()`. Added at step 13.3's
    // D-LANG-NULL-POINTER-CONSTANT closure — the coerce() arm needs
    // the literal-pool index of a just-built `Literal` child to look
    // up its decoded integer value (admit when value is 0). Read-only
    // by contract; payload mutation continues through `make*` only.
    [[nodiscard]] std::uint32_t payload(HirNodeId id) const {
        return arena_.at(id).payload;
    }
    [[nodiscard]] std::span<HirNodeId const>
    children(HirNodeId id) const;
    // Sub-structure accessors for `pathTerminates`-style passes that
    // walk IfStmt / SwitchStmt arms during lowering. Mirror the
    // frozen-Hir accessors of the same names.
    [[nodiscard]] HirNodeId                ifThen(HirNodeId id) const;
    [[nodiscard]] std::optional<HirNodeId> ifElse(HirNodeId id) const;
    [[nodiscard]] std::span<HirNodeId const> switchArms(HirNodeId id) const;
    [[nodiscard]] std::span<HirNodeId const> caseArmBody(HirNodeId id) const;
    [[nodiscard]] bool caseArmIsDefault(HirNodeId id) const;

    // Set the module's source-language tag (the value frozen into `Hir` at
    // `finish`). The default ctor leaves it empty; a consumer that learns the
    // language only mid-build — e.g. the `.dsshir` parser, which reads the
    // `module "lang"` header after it has already started a builder — sets it
    // here before finishing. Not `[[nodiscard]]`: a plain mutator.
    void setSourceLanguage(std::string lang) { sourceLanguage_ = std::move(lang); }
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

    // The build's intrinsic registry — register the intrinsics a lowering's
    // `IntrinsicCall`s reference here; ownership transfers into the frozen Hir.
    // Not `[[nodiscard]]` for the same reason as `registry()`.
    HirIntrinsicRegistry& intrinsicRegistry() noexcept { return intrinsicRegistry_; }

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
    // place, so a malformed expression node can't be spelled. The helpers do NOT
    // inspect `type`: an untyped node builds fine and is caught later by
    // `HirVerifier`, which *reports* `H_TypeUnresolved` (a recoverable
    // diagnostic, NOT an abort) for any expression/TypeRef node left untyped. By
    // contrast `makeBinaryOp`/`makeUnaryOp` DO check the operator's arity at
    // construction and fail-loud `abort()` on mismatch — a wrong-arity node is a
    // structurally impossible caller bug, not a diagnosable analysis outcome.

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
    // Intrinsic call: children are [args...]; the intrinsic id is the payload.
    // The typed overload takes a `HirIntrinsicId` minted by `intrinsicRegistry()`
    // (the verifier's `checkIntrinsicCalls` rejects an id this module never
    // registered); the raw-`uint32_t` overload is the low-level escape hatch.
    HirNodeId makeIntrinsicCall(HirIntrinsicId intrinsic, std::span<HirNodeId const> args,
                                TypeId type, HirFlags flags = HirFlags::None);
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
    // Sequenced expression: evaluate `stmts` in order for their effects, then
    // yield `result` (whose type is the SeqExpr's `type`). Children are
    // [stmts..., result]. The substrate for desugaring value-yielding `++`,
    // assignment-used-as-a-value, and complex-lvalue compound-assign — each
    // needs to run statements (a temp save, a store) and yield a value, which no
    // plain expression node can express. Transparently flattenable by MIR and
    // transpilable (→ comma operator / statement-expression).
    HirNodeId makeSeqExpr(std::span<HirNodeId const> stmts, HirNodeId result,
                          TypeId type, HirFlags flags = HirFlags::None);
    // Short-circuit logical-and / -or over [lhs, rhs] (a distinct kind from
    // BinaryOp because evaluation is short-circuit, not eager).
    HirNodeId makeLogicalAnd(HirNodeId lhs, HirNodeId rhs, TypeId type,
                             HirFlags flags = HirFlags::None);
    HirNodeId makeLogicalOr(HirNodeId lhs, HirNodeId rhs, TypeId type,
                            HirFlags flags = HirFlags::None);
    // sizeof a type, given as a [TypeRef] child; `type` is the size's type.
    HirNodeId makeSizeOf(HirNodeId typeRef, TypeId type, HirFlags flags = HirFlags::None);
    // FC12a-core variadic intrinsics. `makeVaArg` children are [apExpr, TypeRef]:
    // the `va_list` lvalue (value-lowered to its address) PLUS the read TYPE on a
    // SizeOf-style TypeRef child that is NEVER value-lowered; `type` is that read
    // type T (the node's result). `makeVaStart`/`makeVaEnd` take just the `va_list`
    // lvalue [apExpr] child; `type` is `void` (the void-returning-call convention).
    HirNodeId makeVaStart(HirNodeId apExpr, TypeId type, HirFlags flags = HirFlags::None);
    HirNodeId makeVaArg(HirNodeId apExpr, HirNodeId typeRef, TypeId type,
                        HirFlags flags = HirFlags::None);
    HirNodeId makeVaEnd(HirNodeId apExpr, TypeId type, HirFlags flags = HirFlags::None);
    // Address-of [operand]; `type` is the resulting pointer type.
    HirNodeId makeAddressOf(HirNodeId operand, TypeId type, HirFlags flags = HirFlags::None);
    // Pointer dereference of [operand]; `type` is the pointee type.
    HirNodeId makeDeref(HirNodeId operand, TypeId type, HirFlags flags = HirFlags::None);
    // D-CSUBSET-COMPUTED-GOTO: `&&label` — a LEAF expression yielding a code
    // address (`type` is the result pointer type, conventionally `void*`); the
    // target label's per-function ordinal (the SAME namespace GotoStmt uses) is
    // carried in `payload`, so MIR maps it to the label's block via BlockAddress.
    HirNodeId makeLabelAddressOf(std::uint32_t labelOrdinal, TypeId type,
                                 HirFlags flags = HirFlags::None);
    // A type used as a value; `type` is the referenced lattice type. Leaf.
    HirNodeId makeTypeRef(TypeId type, HirFlags flags = HirFlags::None);

    // ── typed statement helpers (HR3) ────────────────────────────────────────
    //
    // Same discipline as the expression helpers: each fixes its kind's child
    // layout in one place. Statements are not `requiresValidType` (no `TypeId`
    // parameter) EXCEPT `VarDecl`, which carries its declared type. Optional
    // positional children are passed as `std::optional<HirNodeId>` and simply
    // omitted from the child list when absent (never an invalid-sentinel child,
    // which the builder rejects); presence is recovered through the matching
    // read accessor on `Hir` (`ifElse`, `loopCondition`, …), so no consumer
    // decodes the layout by hand.

    // A block / scope: zero or more statements in order.
    HirNodeId makeBlock(std::span<HirNodeId const> stmts, HirFlags flags = HirFlags::None);

    // if (cond) then [else elseStmt]. children: [cond, then] or [cond, then, else].
    HirNodeId makeIfStmt(HirNodeId cond, HirNodeId thenStmt,
                         std::optional<HirNodeId> elseStmt = std::nullopt,
                         HirFlags flags = HirFlags::None);

    // while (cond) body — children [cond, body].
    HirNodeId makeWhileStmt(HirNodeId cond, HirNodeId body, HirFlags flags = HirFlags::None);
    // do body while (cond) — children [body, cond]: child 0 is what executes
    // first, so the order is deliberately the reverse of while's [cond, body].
    // The `loopBody`/`loopCondition` accessors hide the difference from callers.
    HirNodeId makeDoWhileStmt(HirNodeId body, HirNodeId cond, HirFlags flags = HirFlags::None);

    // C-style for. Each clause is independently optional; `body` is mandatory.
    // children = [present clauses in init,cond,update order..., body]; the
    // node payload records which clauses are present (a `ForClause` mask).
    HirNodeId makeForStmt(std::optional<HirNodeId> init, std::optional<HirNodeId> cond,
                          std::optional<HirNodeId> update, HirNodeId body,
                          HirFlags flags = HirFlags::None);

    // switch (discriminant) { arms } — children [discriminant, caseArm...].
    HirNodeId makeSwitchStmt(HirNodeId discriminant, std::span<HirNodeId const> arms,
                             HirFlags flags = HirFlags::None);
    // One switch arm. `value` absent ⇒ the `default:` arm (payload flags it);
    // otherwise child 0 is the match value. `body` are the arm's statements.
    // children: [value, body...] for a valued arm, [body...] for default.
    HirNodeId makeCaseArm(std::optional<HirNodeId> value, std::span<HirNodeId const> body,
                          HirFlags flags = HirFlags::None);

    // break / continue, by structural nesting index (0 = innermost enclosing
    // loop/switch). Leaves; the index lives in `payload`.
    HirNodeId makeBreak(std::uint32_t depth = 0, HirFlags flags = HirFlags::None);
    HirNodeId makeContinue(std::uint32_t depth = 0, HirFlags flags = HirFlags::None);

    // goto label; — leaf, the target label's per-function ordinal in `payload`
    // (FC5). label: — child [labeledStmt], the same per-function ordinal in
    // `payload`; a GotoStmt and its target LabelStmt share the ordinal. Labels
    // are function-scoped, forward-referenceable, in their own namespace.
    HirNodeId makeGotoStmt(std::uint32_t labelOrdinal, HirFlags flags = HirFlags::None);
    HirNodeId makeLabelStmt(std::uint32_t labelOrdinal, HirNodeId labeledStmt,
                            HirFlags flags = HirFlags::None);
    // D-CSUBSET-COMPUTED-GOTO: `goto *expr;` — an unconditional transfer to the
    // COMPUTED code address `addressExpr` (child 0). No payload (the target set is
    // every address-taken label, materialized as the IndirectBr's MIR successors).
    HirNodeId makeIndirectGotoStmt(HirNodeId addressExpr, HirFlags flags = HirFlags::None);

    // return [value]; — children [value] or [].
    HirNodeId makeReturn(std::optional<HirNodeId> value = std::nullopt,
                         HirFlags flags = HirFlags::None);

    // An expression evaluated for effect — child [expr].
    HirNodeId makeExprStmt(HirNodeId expr, HirFlags flags = HirFlags::None);

    // A variable declaration. `declaredType` is the variable's type (carried in
    // the node's `typeId`; the verifier requires it valid). `symbol` is the
    // declared SymbolId (payload). `init` is the optional initializer (child).
    HirNodeId makeVarDecl(TypeId declaredType, std::uint32_t symbol,
                          std::optional<HirNodeId> init = std::nullopt,
                          HirFlags flags = HirFlags::None);

    // target = value; — children [target, value]. `target` is an lvalue
    // expression (Ref / Index / MemberAccess / Deref).
    HirNodeId makeAssignStmt(HirNodeId target, HirNodeId value, HirFlags flags = HirFlags::None);

    // ── typed declaration helpers (HR4) ──────────────────────────────────────
    //
    // A function PARAMETER is just a `makeVarDecl(type, symbol)` with no
    // initializer — there is no separate Param kind; the body's `Ref`s resolve
    // to a param via its SymbolId. The verifier (`checkDeclarationShape`) enforces
    // that a Function's params are VarDecls and its last child is the body Block.

    // The module — its children are the top-level declarations, in order.
    HirNodeId makeModule(std::span<HirNodeId const> decls, HirFlags flags = HirFlags::None);

    // A function definition. `signature` is its FnSig, carried in `typeId` (not
    // as a child: the signature is lattice-interned + shared, so it rides the
    // existing `typeId` slot — cheap, dedup'd, zero node-size cost — rather than
    // spawning a per-function TypeRef child). The verifier requires it `valid()`
    // (it does NOT separately check it is a FnSig — that is the caller's
    // contract). `symbol` is the declared SymbolId (payload). children =
    // [params…, body]; `body` (a Block) is always LAST — params are variadic and
    // the body is singular, so body-last gives an O(1) `functionBody` (back of
    // the child list) and a clean `functionParams` slice with no count payload.
    HirNodeId makeFunction(TypeId signature, std::uint32_t symbol,
                           std::span<HirNodeId const> params, HirNodeId body,
                           HirFlags flags = HirFlags::None);

    // A global variable. `type` is its type (typeId, verifier-required), `symbol`
    // the SymbolId (payload), `init` the optional initializer (child).
    HirNodeId makeGlobal(TypeId type, std::uint32_t symbol,
                         std::optional<HirNodeId> init = std::nullopt,
                         HirFlags flags = HirFlags::None);

    // A named type declaration (e.g. typedef). `type` is the type it introduces
    // (typeId, verifier-required), `symbol` the SymbolId. Leaf — a struct's field
    // types live in the lattice type itself, not as HIR children.
    HirNodeId makeTypeDecl(TypeId type, std::uint32_t symbol, HirFlags flags = HirFlags::None);

    // An external function declaration (no body). `signature` is its FnSig (in
    // `typeId`), which MAY be `InvalidType` — binary-only FFI ingestion can lack
    // a resolved type, so externs are not type-required. `symbol` the SymbolId.
    // children = [params…]. FFI linkage/library metadata attaches via
    // `HirAttribute<FfiMetadata>`.
    HirNodeId makeExternFunction(TypeId signature, std::uint32_t symbol,
                                 std::span<HirNodeId const> params,
                                 HirFlags flags = HirFlags::None);

    // An external global declaration. `type` MAY be `InvalidType` (see above).
    // Leaf. FFI metadata attaches via `HirAttribute<FfiMetadata>`.
    HirNodeId makeExternGlobal(TypeId type, std::uint32_t symbol, HirFlags flags = HirFlags::None);

    // A group of imports the module brings in. The builder accepts members now;
    // the real CST→HIR lowering that fills them (e.g. the extern decls a
    // `#include` introduces) arrives at HR9. Cross-module references live in a
    // side-table, not as children (a child is always a same-module HirNodeId).
    HirNodeId makeImportGroup(std::span<HirNodeId const> members = {},
                              HirFlags flags = HirFlags::None);

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
    HirIntrinsicRegistry   intrinsicRegistry_;
};

} // namespace dss
