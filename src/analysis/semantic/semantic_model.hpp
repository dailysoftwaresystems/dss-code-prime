#pragma once

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/compilation_unit/unit_attribute.hpp"
#include "core/export.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_lattice.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The output of phase #8's semantic analysis. Move-only. Holds:
//   - a shared_ptr to the analyzed CompilationUnit (stable address —
//     `UnitAttribute<T>` holds raw Tree* and they must not dangle);
//   - the per-CU TypeLattice (TypeInterner + TypeRegistry);
//   - the symbol table (SymbolId → SymbolRecord) and scope tree;
//   - per-node side tables (NodeId → SymbolId for both decls and uses;
//     NodeId → TypeId for typed expression positions);
//   - the analyzer's own DiagnosticReporter (S_* diagnostics).
//
// Three-pass discipline (semantic_analyzer.cpp): Pass 1 mints every
// declaration into its tree's root scope; Pass 1.5 resolves declared
// types; Pass 2 resolves uses and propagates/checks types. Forward
// references (G-209) fall out for free (all decls minted before any
// use is resolved).

namespace dss {

// C 6.2.3 name spaces. C puts struct/union/enum TAGS (`struct Foo`) in a
// namespace SEPARATE from ordinary identifiers (objects, functions, typedef
// names, enumerators) — so `typedef struct Pair { … } Pair;` is legal (the
// tag `Pair` and the typedef alias `Pair` are distinct names). Each binding
// (and lookup) selects a namespace; the two are independent maps in a scope.
// This is the only axis C 6.2.3 requires for this frontend's subset (label
// and member namespaces are handled elsewhere — labels by the goto pre-scan,
// members by the per-struct field scope).
enum class SymbolNamespace : std::uint8_t {
    Ordinary = 0,   // objects, functions, typedef names, enumerators
    Tag      = 1,   // struct / union / enum TAGS
};

// A scope-tree node. ScopeId is the index into SemanticModel's scope
// vector (slot 0 is the InvalidScope sentinel; slot 1 is the CU root).
// Lookup walks `parent` links; `children` is retained for tooling/tests.
struct DSS_EXPORT ScopeRecord {
    ScopeId  parent{};
    NodeId   anchor{};   // tree node whose subtree opens this scope (or invalid for root)
    TreeId   tree{};
    // name -> SymbolId, for the ORDINARY namespace. Same-scope redeclaration
    // is caught here.
    std::unordered_map<std::string, SymbolId> bindings;
    // C 6.2.3 tag namespace: name -> SymbolId for struct/union/enum TAGS,
    // SEPARATE from `bindings`. A tag and an ordinary symbol of the same name
    // (`typedef struct Pair {…} Pair;`) coexist — one lives here, one in
    // `bindings`. Empty for any scope that declares no tags.
    std::unordered_map<std::string, SymbolId> tagBindings;
    std::vector<ScopeId> children;
};

// One declared symbol. `type` is invalid when the analyzer could not
// determine the symbol's type (e.g. `var x;` with no initializer in a
// language without inferred typing). Pass 2 may upgrade `type` once
// initializer-inference runs.
struct DSS_EXPORT SymbolRecord {
    std::string name;
    ScopeId     scope{};
    NodeId      declNode{};         // the declaration's name node (or the rule node if no name child)
    NodeId      declRuleNode{};     // the declaration rule node itself (for diagnostic spans)
    TreeId      tree{};
    TypeId      type{};
    // The DeclarationRule's `kind` — Variable/Function/Table/Type. Read by
    // type-resolution (a Function symbol carries a FnSig type, etc.).
    DeclarationKind kind = DeclarationKind::Variable;
    // SE4 const-correctness: set when the decl's `constMarker` token was
    // found in the type subtree. A reassignment of a const symbol emits
    // S_ConstViolation.
    bool            isConst = false;
    // c21 (D-CSUBSET-VOLATILE-QUALIFIER): set when the decl's `volatileMarker`
    // token was found in the type subtree — INDEPENDENT of `isConst` (so `const
    // volatile` sets both). Read at CST→HIR access lowering (object Ref + struct
    // MEMBER) and recorded onto the access HIR node via `HirVolatileMap`; HIR→MIR
    // then stamps `MirInstFlags::Volatile` on that access's Load/Store so the
    // (already Volatile-aware) optimizer passes cannot elide or reorder it. A
    // missed access = a silent miscompile, so the threading is exhaustive across
    // every user Load/Store emit site. Default false ⇒ a plain memory access.
    bool            isVolatile = false;
    // SE6: set on a builtin-function symbol declared `variadic` — the
    // call-check skips arg-count enforcement for it.
    bool            variadicBuiltin = false;
    // SE7/D8: copied from the minting DeclarationRule's `warnIfUnused`. After
    // analysis, a symbol with this flag set AND an empty use-set emits
    // S_UnusedVariable (a WARNING) at `declRuleNode`'s span.
    bool            warnIfUnused = false;
    // D5.1: field ordinal within the enclosing composite-type declaration
    // (struct/union). Set on EVERY minted symbol by Pass 1 to its declaration-
    // order index in its declaring scope; meaningful only for field symbols
    // (the inner declarations of a composite-type rule with `fieldChildren`)
    // and read at HIR-lowering time as the `MemberAccess.payload` field index.
    // For all other symbols (Variable/Function/Type/Table outside a composite
    // scope) it is harmless 0-or-positional noise — follows the established
    // SymbolRecord precedent for kind-specific fields (`isConst`,
    // `warnIfUnused`, `variadicBuiltin`).
    std::uint32_t   fieldIndex = 0;
    // D5.1: the inner scope holding this symbol's fields, set by Pass 1 on a
    // Type-kind symbol minted from a declaration with `fieldChildren`. Pass 2's
    // member-access resolution reads this to look up `field` in `obj.field`:
    // TypeId → struct symbol → `structScope` → name lookup. `InvalidScope`
    // (default) for every non-composite symbol.
    ScopeId         structScope{};
    // D5.5: the integer value of a named INTEGER CONSTANT symbol. Set by Pass
    // 1.5 for an enumerator (explicit `= N` overrides the running counter;
    // missing = previous + 1, C99 §6.7.2.2), OR at descriptor injection for a
    // shipped CONSTANT (`isInjectedConstant`). Carries the int64 BIT-PATTERN —
    // for an unsigned-typed constant the uint64 value reinterpreted; the HIR
    // fold re-reads it per the type's signedness. Meaningful only when exactly
    // one of `isEnumerator` / `isInjectedConstant` is set; harmless 0 elsewhere.
    std::int64_t    enumValue = 0;
    // D-CSUBSET-FN-PROTOTYPE: a bare function PROTOTYPE — a function-TYPED object
    // declaration with a function suffix on its NAME and NO body (`int f(int);`).
    // Set by Pass 1 (effectiveKind == Variable + the name carries a function
    // suffix); Pass 1.5 UPGRADES such a symbol's `kind` to Function (it is a
    // function declaration, callable, mergeable with a later definition). A
    // function POINTER (`int (*fp)(int)`) does NOT set this — its suffix sits on
    // the outer declarator, not the name's direct declarator. Default false.
    bool            isProtoDeclaration = false;
    // D-CSUBSET-FN-PROTOTYPE: a proto / redundant function redeclaration that a
    // SURVIVING declaration superseded (proto→def: the proto is absorbed and the
    // definition wins the binding; def→proto / proto→proto: the new redundant
    // decl is absorbed and the prior binding is kept). An absorbed declarator
    // emits NO HIR node — the survivor carries the symbol (the definition emits
    // the body; an unabsorbed proto emits nothing either). Default false.
    bool            isAbsorbedProto = false;
    // D-CSUBSET-EXTERN-DEFINITION-MERGE: TRUE iff this symbol was minted from a
    // NON-DEFINING declaration — a declaration that announces a symbol whose
    // storage/body lives elsewhere (an `extern` declaration in C). Set by Pass 1
    // from the minting DeclarationRule's `nonDefiningDeclaration` flag (config-
    // driven, no rule-name identity). A non-defining declaration of the same name
    // as an in-TU DEFINITION MERGES: the definition WINS the binding and the
    // extern is absorbed (`isAbsorbedProto` set, its HIR ExternFunction/
    // ExternGlobal node suppressed). Two non-defining declarations are idempotent;
    // two definitions still collide (S_RedeclaredSymbol). Default false.
    bool            isExternDeclaration = false;
    // D-CSUBSET-ENUM-INT-CONVERSION (FC8): TRUE iff this symbol IS an enumerator
    // constant (bound under a `compositeKind:"enum"` decl, where `enumValue` was
    // set). DISTINGUISHES an enumerator from a storage-backed `enum E e;` local —
    // BOTH carry `type.kind == Enum`, but only the enumerator may fold to its
    // constant value at HIR Ref-lowering; folding a storage-backed local would be
    // a silent miscompile. Default false (every non-enumerator symbol).
    bool            isEnumerator = false;
    // Item 1 (shipped-header constants): TRUE iff this symbol is a NAMED INTEGER
    // CONSTANT injected from a neutral shipped-lib descriptor's `constants`
    // (e.g. `CHAR_BIT` from `limits.json`). Like an enumerator it folds its Ref
    // to `enumValue` at HIR lowering AND resolves to that value in a constant-
    // expression context (array dim / case / global init) via the const-eval
    // engines' direct-value arm — but its `type` is the constant's OWN integer
    // scalar (NOT an Enum), so the fold derives the literal core from the type
    // directly. INVARIANT: at most one of `isEnumerator` / `isInjectedConstant`
    // is true on any symbol (they share `enumValue` but fold via different cores).
    bool            isInjectedConstant = false;
    // D-CSUBSET-BITFIELD (FC8): the declared bit-field width of a struct/union
    // field, or nullopt for an ordinary field. A TRANSIENT carrier — set at the
    // field's Pass 1.5 resolution (the `: width` const-expr evaluated + validated
    // against the field's integer type there), then READ at the composite's
    // Pass 1.5 type composition to build the `fieldBitWidths` passed to
    // `structType`. After composition the interned TYPE is the authoritative
    // source (layout + codegen read `TypeInterner::fieldBitWidth`); this record
    // field is only the resolution→composition plumbing (cf. `enumValue`). A
    // zero-width (anonymous `int : 0;`) bit-field stores 0 (distinct from nullopt).
    std::optional<std::uint32_t> bitFieldWidth;
};

// FF11 neutral-JSON shipped-library descriptor extern
// (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC). One row per symbol the semantic
// phase MINTED from a resolved shipped-lib descriptor (e.g. `puts` from
// `stdio.json`, pulled in by `#include <stdio.h>`). The semantic phase injects
// each as an extern `SymbolRecord` into scope (so a call resolves like any
// declared function) AND records this row so the CST→HIR lowerer can synthesize
// the matching `ExternFunction`/`ExternGlobal` HIR node + an `HirExternRecord`
// (which FF5 `synthesizeFfiFromSourceDecls` then binds to the library). A
// descriptor symbol that a user declaration already claimed (goal-2) is SKIPPED
// at injection — no row here — so the user's decl is the sole authority and
// nothing is double-declared. The `signature` TypeId is interned in THIS
// model's lattice (the CU interner the lowerer also lowers through).
//
// Kept decoupled from the ffi descriptor enums: `isFunction` selects
// ExternFunction (true) vs ExternGlobal (false) at lowering, the only
// distinction the lowerer needs.
struct DSS_EXPORT ShippedExternSymbol {
    SymbolId    symbol;       // the minted extern symbol
    std::string name;         // the undecorated identifier → HirExternRecord.canonicalName
    TypeId      signature;    // its FnSig (function) or value type (object)
    // Model 3 (2026-06-09): the descriptor's per-object-format `library` map
    // ("pe"/"elf"/"macho" → image name) → HirExternRecord.libraryOverride. The
    // map is carried target-agnostically through HIR; compile_pipeline folds it
    // to a single string for the ACTIVE target's format (where the format is in
    // scope). A missing format key inherits externLibraryByFormat[format].
    std::unordered_map<std::string, std::string> library;
    bool        isFunction = true;  // ExternFunction vs ExternGlobal
};

class DSS_EXPORT SemanticModel {
public:
    // The analyzer is the only producer; construction is by move out of
    // analyze() (declared in semantic_analyzer.hpp).
    SemanticModel(std::shared_ptr<CompilationUnit const> cu,
                  TypeLattice                            lattice,
                  std::vector<ScopeRecord>               scopes,
                  std::vector<SymbolRecord>              symbols,
                  UnitAttribute<SymbolId>                nodeToSymbol,
                  UnitAttribute<TypeId>                  nodeToType,
                  DiagnosticReporter                     diagnostics,
                  std::unordered_map<std::uint32_t, std::vector<NodeId>> usesBySymbol,
                  std::unordered_map<std::uint32_t, ScopeId> compositeScopeByType,
                  UnitAttribute<bool>                    nullPointerConstantNodes,
                  std::vector<ShippedExternSymbol>       shippedExterns,
                  DataModel                              dataModel) noexcept
        : cu_(std::move(cu)),
          lattice_(std::move(lattice)),
          scopes_(std::move(scopes)),
          symbols_(std::move(symbols)),
          nodeToSymbol_(std::move(nodeToSymbol)),
          nodeToType_(std::move(nodeToType)),
          diagnostics_(std::move(diagnostics)),
          usesBySymbol_(std::move(usesBySymbol)),
          compositeScopeByType_(std::move(compositeScopeByType)),
          nullPointerConstantNodes_(std::move(nullPointerConstantNodes)),
          shippedExterns_(std::move(shippedExterns)),
          dataModel_(dataModel) {}

    SemanticModel(SemanticModel const&)            = delete;
    SemanticModel& operator=(SemanticModel const&) = delete;
    SemanticModel(SemanticModel&&)                 = default;
    SemanticModel& operator=(SemanticModel&&)      = default;

    [[nodiscard]] CompilationUnit const&        unit()     const noexcept { return *cu_; }
    [[nodiscard]] TypeLattice const&            lattice()  const noexcept { return lattice_; }
    // Non-const: downstream HIR/MIR lowering interns NEW types (lowered
    // expression types, synthesized signatures) into the same per-CU
    // lattice after analysis, so the interner must stay open past the
    // model boundary. SE1-SE3 themselves do not mutate it post-analyze().
    [[nodiscard]] TypeLattice&                  lattice()        noexcept { return lattice_; }
    [[nodiscard]] DiagnosticReporter const&     diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] bool                          hasErrors() const noexcept { return diagnostics_.hasErrors(); }

    // ── scope tree ──
    // Slot 0 is the InvalidScope sentinel; slot 1 is the CU root scope. Real
    // scopes are dense thereafter. `scopes()` returns the vector for tooling;
    // `recordFor(scope)` is the named lookup.
    [[nodiscard]] std::vector<ScopeRecord> const& scopes() const noexcept { return scopes_; }
    [[nodiscard]] ScopeRecord const&              scopeRecord(ScopeId id) const;

    // ── symbol table ──
    [[nodiscard]] std::vector<SymbolRecord> const& symbols() const noexcept { return symbols_; }
    [[nodiscard]] SymbolRecord const*              recordFor(SymbolId id) const noexcept;

    // ── side-table queries ──
    // `symbolAt(nodeId)` returns the SymbolId bound to a name-node (a
    // declaration's name OR a resolved use). InvalidSymbol when nothing was
    // bound. Aborts via UnitAttribute's CU guard if `nodeId` is not from a
    // tree in this CU.
    [[nodiscard]] SymbolId symbolAt(NodeId id) const;
    [[nodiscard]] TypeId   typeAt(NodeId id)   const;

    // Reverse use-index (SE7): every NodeId that resolved to `symbol`
    // during Pass 2 (the symbol's USE sites — NOT its declaration name
    // node). Returns an empty span for an unknown / never-used symbol.
    // Powers LSP references / rename.
    [[nodiscard]] std::span<NodeId const> usesOf(SymbolId symbol) const noexcept;

    // SP3.a: TypeId→declaring-struct-scope substrate. Composite types
    // (struct/union) carry an associated inner scope holding their
    // field symbols (populated by Pass 1.5 when the struct's TypeId is
    // interned). Returns `InvalidScope` for non-composite types or for
    // composites whose scope didn't get populated (semantic-phase
    // failure). Used by D5.3 designator-position name resolution
    // (look up `.x` in the struct's scope derived from the context
    // type, not the lexical scope) and by future MemberAccess refactors
    // that want a uniform substrate.
    [[nodiscard]] ScopeId compositeScopeFor(TypeId type) const noexcept;

    // R2 (D-SEMANTIC-NULL-CONSTANT-FOLDING): true iff `id` is a source node the
    // analyzer admitted as a FOLDED null-pointer constant (a non-literal integer
    // constant expression with value 0 — `1-1`, `-0`). The CST→HIR lowerer
    // materializes a synthetic Literal 0 in its place so the existing coerce
    // literal-0 arm emits the Cast→Ptr. False for every other node (incl. a
    // structural literal `0`, which the coerce arm admits directly).
    [[nodiscard]] bool isNullPointerConstant(NodeId id) const {
        return nullPointerConstantNodes_.has(id);
    }

    // The full attributes — convenient for tooling / forEach iteration.
    [[nodiscard]] UnitAttribute<SymbolId> const& nodeToSymbol() const noexcept { return nodeToSymbol_; }
    [[nodiscard]] UnitAttribute<TypeId>   const& nodeToType()   const noexcept { return nodeToType_; }

    // FF11 shipped-lib descriptor externs the semantic phase minted (one per
    // injected descriptor symbol; goal-2-skipped names are absent). The CST→HIR
    // lowerer reads this to synthesize the matching extern HIR nodes +
    // HirExternRecords. Empty unless the CU resolved a shipped-lib descriptor.
    [[nodiscard]] std::span<ShippedExternSymbol const> shippedExterns() const noexcept {
        return shippedExterns_;
    }

    // FC3 c1: the data model this analysis ran under (`analyze()`'s
    // parameter — the active format's declared width triple). The HIR
    // lowering reads THIS (never a second parameter), so the two tiers'
    // ladder / UAC resolutions agree by construction.
    [[nodiscard]] DataModel dataModel() const noexcept { return dataModel_; }

private:
    std::shared_ptr<CompilationUnit const> cu_;
    TypeLattice                            lattice_;
    std::vector<ScopeRecord>               scopes_;
    std::vector<SymbolRecord>              symbols_;
    UnitAttribute<SymbolId>                nodeToSymbol_;
    UnitAttribute<TypeId>                  nodeToType_;
    DiagnosticReporter                     diagnostics_;
    // SymbolId.v → its USE-site NodeIds. Built once during analyze().
    std::unordered_map<std::uint32_t, std::vector<NodeId>> usesBySymbol_;
    // SP3.a: composite-TypeId.v → declaring-struct-scope. Populated by
    // Pass 1.5 when a struct's TypeId is interned (see
    // `compositeScopeByType` in semantic_analyzer.cpp's EngineState).
    std::unordered_map<std::uint32_t, ScopeId>             compositeScopeByType_;
    // R2 (D-SEMANTIC-NULL-CONSTANT-FOLDING): source nodes the analyzer admitted as
    // a FOLDED null-pointer constant (`1-1`, `-0`). The CST→HIR lowerer reads
    // `isNullPointerConstant` to materialize a synthetic Literal 0 in place.
    // TREE-KEYED UnitAttribute (NodeId is tree-local — a flat set would alias node
    // indices across a multi-source CU's trees → cross-tree silent miscompile).
    UnitAttribute<bool>                                   nullPointerConstantNodes_;
    // FF11: descriptor externs minted from resolved shipped-lib JSON
    // descriptors (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC). Consumed by the
    // CST→HIR lowerer.
    std::vector<ShippedExternSymbol>                       shippedExterns_;
    // FC3 c1: the analysis-time data model (see `dataModel()`).
    DataModel                                              dataModel_ = DataModel::Lp64;
};

// Pin move-only / non-copyable at compile time so a future refactor
// can't silently make the model copyable (the side-tables would then
// duplicate their per-tree NodeAttribute storage, breaking the
// shared_ptr<CU>-anchors-the-raw-Tree*-pointers invariant).
static_assert(!std::is_copy_constructible_v<SemanticModel>,
              "SemanticModel must be move-only — the side-tables hold raw "
              "Tree* into the bound CU; copying would silently alias them.");
static_assert(!std::is_copy_assignable_v<SemanticModel>,
              "SemanticModel must be move-only.");
static_assert(std::is_move_constructible_v<SemanticModel>);

} // namespace dss
