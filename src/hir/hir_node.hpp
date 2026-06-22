#pragma once

#include "core/export.hpp"
#include "core/substrate/arena_tag.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_id.hpp"   // TypeId

#include <cstdint>
#include <type_traits>

// HIR node model (HR1). The HIR is the language-neutral, structured, typed
// pivot layer every downstream lowering reads. This header ships the node
// vocabulary; the arena container (`Hir`/`HirBuilder`), walker (`HirCursor`),
// and extension-kind registry (`HirKindRegistry`) live in sibling headers.
//
// `HirKind` is an OPEN enum mirroring the core type lattice's `TypeKind`/
// `TypeRegistry` model (08.5 SP2): a fixed universal CORE in [0, 256) plus a
// registered-EXTENSION space >= 256 (kindIds minted by `HirKindRegistry`).
// The core is the paradigm-neutral structured-typed-imperative vocabulary;
// anything it can't express is a registered extension kind, NEVER a new core
// member. This is what keeps the core fixed while any source language onboards
// by registering kinds (thesis decision #4 — see 09-hir-plan §2.2).
//
// HR1 stores the concrete extension kindId in the node `payload` for nodes whose
// `kind == Extension`. Later HR phases may relocate it to a dedicated
// HirAttribute side-table if per-Extension metadata accumulates beyond a single
// 32-bit slot; plan §2.6 lists the side-tables intended for HR5 (SourceSpan /
// FfiMetadata / ShaderIntrinsic / TranspileHint / DiagnosticInfo). HR1 makes no
// commitment either way.

namespace dss {

// ── HirKind: the open core vocabulary [0, 256) ───────────────────────────────
//
// Grouped per 09-hir-plan §2.2. Members are stable ordinals; `Count_` must stay
// last (it counts the core members and pins the < 256 invariant). Extension
// kinds are `HirKindId`s >= kFirstHirExtensionKind, carried on a node whose
// `HirKind == Extension` (the concrete kind lives in a side-table at HR5; HR1
// stores it in the node `payload`).
enum class HirKind : std::uint16_t {
    // ── Modules / Declarations ──
    Module, Function, Global, TypeDecl, ExternFunction, ExternGlobal, ImportGroup,
    // ── Structured Statements ──
    Block, IfStmt, WhileStmt, DoWhileStmt, ForStmt, SwitchStmt, CaseArm,
    BreakStmt, ContinueStmt, ReturnStmt, ExprStmt, VarDecl, AssignStmt,
    // ── Unstructured control flow (FC5) — GotoStmt jumps to a function-scoped
    //    LabelStmt; the CFG is general at the MIR tier (struct-CF markers are a
    //    derived function of the CFG, so unstructured edges derive Linear). ──
    GotoStmt, LabelStmt,
    // ── Expressions ──
    Literal, Ref, Call, IntrinsicCall, BinaryOp, UnaryOp, Cast, MemberAccess,
    Index, Swizzle, ConstructAggregate, Ternary, LogicalAnd, LogicalOr,
    SizeOf, AddressOf, Deref, SeqExpr,
    // ── Variadic intrinsics (FC12a-core) — DEDICATED nodes mirroring SizeOf:
    //    `VaArg` carries the read TYPE on a [TypeRef] child it NEVER value-lowers
    //    (vs IntrinsicCall, which value-lowers every child). VaStart/VaEnd take the
    //    `va_list` lvalue [apExpr] child. Each is an expression node (VaArg yields
    //    the read value typed T; VaStart/VaEnd yield void, modelled as a typed
    //    expr like a void-returning call so they sit in expression position). ──
    VaStart, VaArg, VaEnd,
    // ── Types-as-values ──
    TypeRef,
    // ── Special ──
    Unreachable,
    Error,        // recovery sentinel — analog of CST's Error leaf
    // ── extension marker (concrete kind lives in HirKindRegistry; HR1 holds the
    //    minted HirKindId in the node `payload`. Later phases may promote it to
    //    a side-table if per-Extension data accumulates; HR1 makes no such
    //    promise.) ──
    Extension,

    Count_        // keep last — counts the core members
};

static_assert(static_cast<std::uint32_t>(HirKind::Count_) < 256,
              "core HirKind members must occupy [0, 256); extensions use "
              "registry-minted HirKindIds >= 256");

// First registry-minted extension kind. Core kinds (the HirKind enum) occupy
// the [0, kFirstHirExtensionKind) range of the open kind space. Mirrors the
// type lattice's kFirstExtensionKind.
inline constexpr std::uint32_t kFirstHirExtensionKind = 256;

// Does a node of this kind carry a resolved result type — i.e. must its `typeId`
// be `valid()`? True for the whole Expressions group (every expression has a
// type, plan §2.4), `TypeRef` (carries the referenced lattice TypeId), and the
// SOURCE-DEFINED declarations `VarDecl`/`Function`/`Global`/`TypeDecl` (each
// materializes its declared type onto the node — rather than re-deriving it from
// a CST or symbol-table lookup at lowering time — so MIR can size the alloca /
// build the ABI directly; a typeless one is unlowerable, so it fails loud at
// verify-time). False for `Module`, `ImportGroup`, the EXTERN declarations
// (`ExternFunction`/`ExternGlobal` — see below), statements, and the
// `Error`/`Unreachable` sentinels.
//
// `ExternFunction`/`ExternGlobal` are deliberately NOT required: 11-ffi-plan's
// binary-only ingestion can yield an extern with no resolved type (its
// resolvedType is optional), so a typeless extern is legitimate.
//
// `Extension` is intentionally false here: whether an extension kind produces a
// value (and so requires a type) lives in its `HirKindDescriptor` operand/
// attribute shape, added in a later phase (HR5/HR6) — the core predicate can't
// know it.
[[nodiscard]] constexpr bool requiresValidType(HirKind kind) noexcept {
    switch (kind) {
        // ── Expressions (plan §2.2) ──
        case HirKind::Literal: case HirKind::Ref: case HirKind::Call:
        case HirKind::IntrinsicCall: case HirKind::BinaryOp: case HirKind::UnaryOp:
        case HirKind::Cast: case HirKind::MemberAccess: case HirKind::Index:
        case HirKind::Swizzle: case HirKind::ConstructAggregate: case HirKind::Ternary:
        case HirKind::LogicalAnd: case HirKind::LogicalOr: case HirKind::SizeOf:
        case HirKind::AddressOf: case HirKind::Deref: case HirKind::SeqExpr:
        // ── Variadic intrinsics (FC12a-core): every one is an expression node.
        //    VaArg's type is the READ value's type T; VaStart/VaEnd carry a valid
        //    `void` type (the void-returning-call convention — a valid TypeId, not
        //    InvalidType), so all three require a resolved type and fail loud if
        //    typeless. ──
        case HirKind::VaStart: case HirKind::VaArg: case HirKind::VaEnd:
        // ── Types-as-values: carries the referenced lattice TypeId ──
        case HirKind::TypeRef:
        // ── Declarations carrying their own (source-defined) type ──
        //   VarDecl: declared local type. Function: its FnSig. Global: var type.
        //   TypeDecl: the type it introduces. These are source-defined, so a
        //   resolved type is always available and a typeless one is unlowerable.
        //   ExternFunction/ExternGlobal are NOT here: 11-ffi-plan binary-only
        //   ingestion can yield an extern with no resolved type (optional).
        case HirKind::VarDecl: case HirKind::Function: case HirKind::Global:
        case HirKind::TypeDecl:
            return true;
        // ── Module / extern decls / import / statements / sentinels / Extension ──
        case HirKind::Module: case HirKind::ExternFunction: case HirKind::ExternGlobal:
        case HirKind::ImportGroup: case HirKind::Block: case HirKind::IfStmt:
        case HirKind::WhileStmt: case HirKind::DoWhileStmt: case HirKind::ForStmt:
        case HirKind::SwitchStmt: case HirKind::CaseArm: case HirKind::BreakStmt:
        case HirKind::ContinueStmt: case HirKind::ReturnStmt: case HirKind::ExprStmt:
        case HirKind::GotoStmt: case HirKind::LabelStmt:
        case HirKind::AssignStmt: case HirKind::Unreachable:
        case HirKind::Error: case HirKind::Extension: case HirKind::Count_:
            return false;
    }
    return false;  // unreachable for a well-formed core kind
}

// ── Structured-CF kind predicates + payload vocabulary (HR3) ─────────────────
//
// A "loop" kind iterates (continue targets one); a "branch target" kind is
// anything break can leave (loops + switch). Used by the verifier's break/
// continue scoping rule and by CST→HIR lowering (HR8) to compute branch indices.
[[nodiscard]] constexpr bool isLoopKind(HirKind kind) noexcept {
    return kind == HirKind::WhileStmt || kind == HirKind::DoWhileStmt
        || kind == HirKind::ForStmt;
}
[[nodiscard]] constexpr bool isBranchTargetKind(HirKind kind) noexcept {
    return isLoopKind(kind) || kind == HirKind::SwitchStmt;
}

// `ForStmt` clause-presence mask (HR3). The three C-style clauses are each
// independently optional and absent parts cannot be invalid-sentinel children
// (the builder rejects invalid child ids), so a `ForStmt`'s child list holds
// only the PRESENT clauses, in Init/Cond/Update order, followed by the always-
// present body — and the node `payload` records which clauses are present. This
// is the MLIR "operand-segment" discipline: the child list stays hole-free for
// generic passes, and the mask is read through the typed `forInit`/
// `loopCondition`/`forUpdate` accessors so no consumer decodes it by hand.
enum class ForClause : std::uint32_t {
    None   = 0,
    Init   = 1u << 0,
    Cond   = 1u << 1,
    Update = 1u << 2,
};
inline constexpr std::uint32_t kForClauseMask = 0b111;  // all valid ForClause bits

[[nodiscard]] inline constexpr ForClause operator|(ForClause a, ForClause b) noexcept {
    return static_cast<ForClause>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] inline constexpr bool has(ForClause mask, ForClause bit) noexcept {
    return (static_cast<std::uint32_t>(mask) & static_cast<std::uint32_t>(bit)) != 0;
}
[[nodiscard]] inline constexpr std::uint32_t clauseCount(ForClause mask) noexcept {
    std::uint32_t const m = static_cast<std::uint32_t>(mask) & kForClauseMask;
    // popcount over 3 bits.
    return (m & 1u) + ((m >> 1) & 1u) + ((m >> 2) & 1u);
}

// `CaseArm` payload bit: set iff the arm is the `default:` arm (which has no
// match-value child). A valued case arm carries its match value as child 0.
inline constexpr std::uint32_t kCaseArmIsDefault = 1u << 0;

// ── Per-kind child-arity (HR3) — the single source of truth ──────────────────
//
// Static child-count bounds the verifier checks against and the `make*` helpers
// satisfy by construction (a test cross-checks the two never drift). `max ==
// kUnboundedArity` means variadic. Kinds with no defined shape yet (the HR4
// declaration kinds, `Extension`) and the `Error` recovery sentinel are
// unconstrained `{0, kUnboundedArity}`. `ForStmt` and `CaseArm` carry ADDITIONAL
// payload-coupled constraints the verifier checks beyond these static bounds.
inline constexpr std::uint32_t kUnboundedArity = 0xFFFFFFFFu;

struct ChildArity {
    std::uint32_t min;
    std::uint32_t max;
};

[[nodiscard]] constexpr ChildArity childArity(HirKind kind) noexcept {
    switch (kind) {
        // ── Expressions (HR2) ──
        case HirKind::Literal: case HirKind::Ref: case HirKind::TypeRef:
            return {0, 0};
        case HirKind::Call:               return {1, kUnboundedArity};  // [callee, args...]
        case HirKind::IntrinsicCall:      return {0, kUnboundedArity};  // [args...]
        case HirKind::ConstructAggregate: return {0, kUnboundedArity};  // [fields...]
        case HirKind::UnaryOp: case HirKind::Cast: case HirKind::MemberAccess:
        case HirKind::Swizzle: case HirKind::SizeOf: case HirKind::AddressOf:
        case HirKind::Deref:
        // FC12a-core: VaStart/VaEnd = [apExpr] — one child (the va_list lvalue).
        case HirKind::VaStart: case HirKind::VaEnd:
            return {1, 1};
        // FC12a-core: VaArg = [apExpr, TypeRef] — the va_list lvalue (value-lowered)
        // PLUS the read TYPE on a SizeOf-style TypeRef child (NEVER value-lowered).
        case HirKind::VaArg:              return {2, 2};
        case HirKind::BinaryOp: case HirKind::Index: case HirKind::LogicalAnd:
        case HirKind::LogicalOr:
            return {2, 2};
        case HirKind::Ternary:            return {3, 3};
        // SeqExpr: [stmt..., resultExpr] — N>=0 statements then the value-
        // yielding expression (always the LAST child). The minimum is 1 (the
        // result expression); a SeqExpr with no statements is degenerate but
        // legal (equals its result). Models GCC statement-expressions / the
        // comma operator: the substrate for value-yielding `++`, assignment-as-
        // expression, and complex-lvalue compound-assign that need a temp.
        case HirKind::SeqExpr:            return {1, kUnboundedArity};
        // ── Statements (HR3) ──
        case HirKind::Block:              return {0, kUnboundedArity};  // [stmts...]
        case HirKind::IfStmt:             return {2, 3};                // [cond, then, else?]
        case HirKind::WhileStmt: case HirKind::DoWhileStmt: case HirKind::AssignStmt:
            return {2, 2};
        case HirKind::ForStmt:            return {1, 4};                // body + 0..3 clauses (+ mask check)
        case HirKind::SwitchStmt:         return {1, kUnboundedArity};  // [discriminant, arms...]
        case HirKind::CaseArm:            return {0, kUnboundedArity};  // [value?, stmts...] (+ default check)
        case HirKind::ExprStmt:           return {1, 1};
        case HirKind::ReturnStmt: case HirKind::VarDecl:
            return {0, 1};                                             // [value/init?]
        case HirKind::BreakStmt: case HirKind::ContinueStmt: case HirKind::Unreachable:
            return {0, 0};
        // ── Unstructured CF (FC5) ──
        case HirKind::GotoStmt:           return {0, 0};   // leaf; target label ordinal in payload
        case HirKind::LabelStmt:          return {1, 1};   // [labeledStmt]; label ordinal in payload
        // ── Declarations (HR4) ──
        case HirKind::Function:      return {1, kUnboundedArity};  // [params…, body Block]
        case HirKind::ExternFunction: return {0, kUnboundedArity}; // [params…] (no body)
        case HirKind::Global:        return {0, 1};                // [init?]
        case HirKind::TypeDecl: case HirKind::ExternGlobal:
            return {0, 0};                                         // leaf (type in typeId)
        case HirKind::Module: case HirKind::ImportGroup:
            return {0, kUnboundedArity};                           // [decls…] / [members…]
        // ── recovery / extension (shape defined by the descriptor at HR5/HR6) ──
        case HirKind::Error: case HirKind::Extension: case HirKind::Count_:
            return {0, kUnboundedArity};
    }
    // No core kind reaches here — every enumerator has a case above. A future
    // kind added without one must NOT silently inherit {0, kUnbounded}
    // (unconstrained = exempt from the verifier's arity rule); the impossible
    // {1, 0} range makes the generic arity check fire for every instance of it,
    // surfacing the omission loudly. (The systemic guard is the tree-wide
    // -Wswitch-enum latch, G-711.)
    return {1, 0};
}

// ── HirFlags: orthogonal per-node markers ────────────────────────────────────
//
// HasError mirrors CST's propagation discipline. Synthetic marks nodes the
// lowering inserted (e.g. structured-CF scaffolding) with no source origin.
// ShaderUsable / HostUsable gate the shader-restriction subverifier (HR6) and
// backend selection — a node may be legal in one execution context and not the
// other. Multiple flags can apply to one node.
enum class HirFlags : std::uint8_t {
    None         = 0,
    HasError     = 1u << 0,   // this node or some descendant is/contains an Error
    Synthetic    = 1u << 1,   // inserted by lowering, not derived from a source node
    ShaderUsable = 1u << 2,   // legal inside a shader-stage subtree
    HostUsable   = 1u << 3,   // legal on the host/native target
    // bits 4-7 reserved
};

[[nodiscard]] inline constexpr HirFlags operator|(HirFlags a, HirFlags b) noexcept {
    using U = std::underlying_type_t<HirFlags>;
    return static_cast<HirFlags>(static_cast<U>(a) | static_cast<U>(b));
}
[[nodiscard]] inline constexpr HirFlags operator&(HirFlags a, HirFlags b) noexcept {
    using U = std::underlying_type_t<HirFlags>;
    return static_cast<HirFlags>(static_cast<U>(a) & static_cast<U>(b));
}
[[nodiscard]] inline constexpr HirFlags operator~(HirFlags v) noexcept {
    using U = std::underlying_type_t<HirFlags>;
    return static_cast<HirFlags>(~static_cast<U>(v));
}
inline constexpr HirFlags& operator|=(HirFlags& a, HirFlags b) noexcept { a = a | b; return a; }
inline constexpr HirFlags& operator&=(HirFlags& a, HirFlags b) noexcept { a = a & b; return a; }

[[nodiscard]] inline constexpr bool any(HirFlags v) noexcept {
    return static_cast<std::underlying_type_t<HirFlags>>(v) != 0;
}
[[nodiscard]] inline constexpr bool has(HirFlags v, HirFlags bit) noexcept { return any(v & bit); }
[[nodiscard]] inline constexpr bool hasError(HirFlags v) noexcept { return has(v, HirFlags::HasError); }

// ── Storage POD ──────────────────────────────────────────────────────────────
//
// Lives inside detail/ so consumers go through `Hir`'s accessors — the fields
// are not all meaningful in isolation (e.g. childStart/childCount address the
// module's child-id pool, not a standalone array; payload's meaning is per-kind:
// extension-kindId for Extension, operator-id for BinaryOp/UnaryOp, literal- or
// intrinsic-index for Literal/IntrinsicCall, etc.).
//
// Layout: 28 bytes today, capped at a 32-byte budget. Parent links DELIBERATELY
// live outside this POD — in `Hir::parentOf_`, a parallel array — to keep the
// node small so the scan-hot passes (verifier, lowering, codegen) that sweep
// every node's kind/type stay dense in cache; a million-node module fits two
// nodes per 64-byte cacheline at 28 bytes. Reverting this and inlining a parent
// HirNodeId (8 bytes) would push the node to 40 and halve scan density.
// SourceSpan + every non-universal attribute live in HirAttribute side-tables
// (HR5; see plan §2.6).
namespace detail {

struct HirNode {
    // Default = Error: a default-constructed node must NEVER silently appear as
    // a real Module/Block in the arena (slot-0 sentinel reads must be visibly
    // bogus). The `Error` recovery sentinel is the only safe default.
    HirKind       kind       = HirKind::Error;
    HirFlags      flags      = HirFlags::None;
    std::uint16_t _pad       = 0;            // explicit padding
    TypeId        typeId     = InvalidType;  // resolved type (every expr node; HR2)
    std::uint32_t childStart = 0;            // offset into Hir's child-id pool
    std::uint32_t childCount = 0;            // consecutive children (0 for leaves)
    std::uint32_t payload    = 0;            // per-kind scalar (see above)
};

static_assert(sizeof(HirNode) <= 32, "detail::HirNode grew unexpectedly — review layout");
static_assert(std::is_trivially_copyable_v<HirNode>);

} // namespace detail

} // namespace dss

// Cross-arena guard wording for the HIR arena (the SH3 / SP1 discipline): a
// HirNodeId minted by one module used against another aborts with both module
// tags in the message. The primary `ArenaNames` template is a must-specialize
// tripwire (arena_tag.hpp), so this is mandatory before instantiating
// ArenaContainer<HirNode, HirNodeId, HirModuleId> or ArenaAttribute<Hir, T>.
namespace dss::substrate {

template <>
struct ArenaNames<HirNodeId, HirModuleId> {
    static constexpr char const* attribute = "HirAttribute";
    static constexpr char const* element   = "HirNodeId";
    static constexpr char const* tag       = "HirModuleId";
    static constexpr char const* access    = "Hir::at";
};

} // namespace dss::substrate
