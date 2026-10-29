#pragma once

#include "core/export.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss {

// One ambiguous type-name site recorded during a parse (FC2 cast
// disambiguation, triage rule 4): a speculative type-name-guarded probe
// structurally succeeded on a LONE identifier the sketch had no entry
// for, AND the token following the type position was an operator — so
// the parser rolled back to the value reading and recorded the site.
// The compilation-unit oracle (UnitBuilder::finish) resolves the name
// against every tree's exported global type names; a hit triggers ONE
// whole-file reparse with the name seeded as a global type.
//
// Engine-generic: nothing here says "cast" — any language whose grammar
// declares a `commitRequiresTypeName` shape gets the same machinery.
struct DSS_EXPORT AmbiguousTypeNameCandidate {
    std::string name;   // the lone identifier's text
    SourceSpan  span;   // its source span (tests / future diagnostics)
};

// Parser-side scoped name→kind(type|value) map — the "binder sketch"
// (FC2 Part A). A deliberately SHALLOW mirror of the semantic analyzer's
// pass-1 binding walk, fed by the SAME config vocabulary
// (`semantics.declarations` rows + `semantics.scopes` rules), maintained
// DURING the parse so the speculative type-name triage can answer "is
// this identifier a type or a value here?" at probe-commit time.
//
//   * Languages with no `declarations` → `enabled() == false` → the
//     parser skips every hook; zero behavior + zero cost (toy / tsql).
//   * Recording is token-ordered (a binder rule's frame closes after its
//     tokens) — C's declare-before-use falls out naturally.
//   * Scopes are RULE-driven (the same `semantics.scopes` rules the
//     analyzer uses), NOT token-driven: `funcDefTail` opens one scope
//     covering params + body, so a param shadows a file-scope typedef
//     inside the body exactly like the analyzer's scope tree.
//   * Shadowing: lookup walks bindings newest-first and skips entries
//     whose scope is no longer live — the innermost live binding wins.
//   * Closed-scope bindings are RETAINED (marked dead via scope-id
//     liveness) rather than truncated, so a Snapshot is four integers +
//     a depth-sized stack copy and restore is pure truncation — the
//     speculation-rollback contract (SpeculationProbe) stays O(depth)
//     per probe.
//
// KNOWN SHALLOWNESS (by design — the sketch only needs to be right
// about TYPE-vs-VALUE for names it has SEEN; everything else routes to
// the Unknown arm whose commit/rollback decision is follower-driven and
// whose misses fail LOUD at semantic analysis):
//   * No pass-1 style pre-binding: a name declared LATER in the file is
//     Unknown at an earlier use site (correct for C; languages with
//     hoisting would extend this).
//   * `kindByChild` discriminators are not evaluated — they only flip
//     Variable→Function today, and both are VALUES to the triage.
//
// ★★ TWO VALUES THE SOURCE NEVER DECLARES WHERE THEY ARE USED ARE MIRRORED
// (P68 round 9, lane `cs`), AND THE OLD CLAIM THAT "Unknown IS SAFE" FOR THEM
// WAS FALSE. `sizeof ( X )` has no operand after its type position, so the
// follower test has nothing to read and an Unknown lone identifier COMMITS AS A
// TYPE — the semantic tier then refused it S_UnknownType. ✔MEASURED 2026-09-23,
// each reference separately and RUN: `sizeof(__func__)`, `sizeof(__FUNCTION__)`
// and `sizeof(E)` for an enumerator of an anonymous, a named and a block-scope
// enum build and exit 42 on gcc 13.3.0, clang 18.1.3, mingw-w64 13.2.0 and MSVC
// 19.51, and DSS refused all five. So:
//   * the language's PREDEFINED identifiers (`semantics.predefinedFunctionNames`)
//     are recorded as VALUES in the global scope at construction — they are
//     ordinary identifiers, never type names, wherever they are visible at all;
//   * a field row its composite LIFTS (`fieldChildren.liftToEnclosingScope`, the
//     C enumeration constant) binds its name in the nearest enclosing NAMESPACE
//     scope — past the composite's own body scope and any declarator-dominator —
//     exactly where the analyzer republishes it.
// A type the sketch cannot see (a typedef from another tree of the compilation
// unit) still reaches the Unknown arm and still commits as a type.
class DSS_EXPORT BinderSketch {
public:
    enum class NameKind : std::uint8_t { Unknown, Type, Value };

    // Compact per-rule view over a `semantics.declarations` row — only
    // the fields name extraction needs (built once at parser ctor; no
    // ad-hoc deep reaches into SemanticConfig mid-parse).
    struct BinderDecl {
        std::uint32_t nameChild = 0;     // visible-child index (post specifier-strip)
        bool          isType    = false; // DeclarationKind::Type (static kind)
        NameMatchMode nameMatch = NameMatchMode::Self;
        RuleId        specifierPrefixRule{};   // invalid ⇒ no prefix to strip
        // FC4 c1: declarator-mode rows have no positional nameChild — the
        // name(s) live inside recursive declarators. `carrierChild` is the
        // visible-child index (post specifier-strip) of the row's
        // declarator-list / single-declarator subtree; the parser runs the
        // SHARED declarator walk (core/types/declarator_walk.hpp) below it
        // at frame close and records EVERY extracted name (an
        // initDeclarator LIST binds multiple — `typedef int A, *B;` binds
        // both A and B as types).
        bool          declaratorMode = false;
        std::uint32_t carrierChild   = 0;
        // A field row whose composite republishes its names in the enclosing
        // scope (`fieldChildren.liftToEnclosingScope` — C's enumeration
        // constants). See `record`.
        bool          liftToEnclosingScope = false;
        // A COMPOSITE's row (it carries `fieldChildren`, the analyzer's one gate for
        // binding a name as a tag): the name it declares is a TAG, never recorded.
        bool          namesATag = false;
        // A composite's FIELD row whose names the composite does not republish (a
        // struct or union member, never an enumeration constant): the name it
        // declares is a MEMBER, never recorded.
        bool          namesAMember = false;
    };

    // ★ P69 L2 (lane `lm`): THE SKETCH RECORDS ORDINARY IDENTIFIERS ONLY — the one
    // name space the type-name triage asks about. C 6.2.3 keeps a TAG apart from an
    // ordinary identifier (and so does the analyzer, `SymbolNamespace::Tag`), and gives
    // each structure or union its own name space of MEMBERS; the parser records
    // neither (`BinderDecl::namesATag`, `namesAMember`), so a tag never makes its
    // spelling a type name and a member never hides one. While a composite's tag was recorded as
    // an ordinary TYPE, a tag newer than an object or a function of its spelling
    // turned `(name)` into a cast. ✔MEASURED 2026-10-20 (macos-arm64-debug's dsscp,
    // pe64 images run): `int g(int); struct g {…}; (g)(2)` and `int T = 44; enum T
    // {A = 2}; (T) - A` were refused S_UnknownType, and `typedef int T; int T = 44;
    // { struct T {…}; return (T) - 2; }` RAN -2 — gcc 13.3.0 and clang 18.1.3 build
    // all three and they exit 42. A structure or union MEMBER is no ordinary
    // identifier either — each such type has its own name space of members (C 6.2.3)
    // — and while members were recorded as ordinary values, `typedef int P; struct S
    // { int P; char a[(P) 42]; };` was refused P_UnexpectedToken at `42` (✔MEASURED
    // on this lane's tree, run 20261021-185729-1127e757; both references build and
    // run it). A binding nothing would read is no binding: a tag or a member
    // recorded apart would answer no `lookup` and move no `version()`.

    explicit BinderSketch(GrammarSchema const& schema);

    // True when the schema declares at least one name-bearing
    // declaration row — the parser consults this before every hook so
    // binder-less languages pay nothing.
    [[nodiscard]] bool enabled() const noexcept { return !byRule_.empty(); }

    // The binder view row for `rule`, or nullptr when `rule` declares
    // no name.
    [[nodiscard]] BinderDecl const* binderFor(RuleId rule) const noexcept;

    // True when `rule` opens a lexical scope (`semantics.scopes`).
    [[nodiscard]] bool isScopeRule(RuleId rule) const noexcept;

    // ── scope events (driven by the parser's frame open/close) ──
    // `rule` is the scope-opening frame's rule — used to classify the new
    // scope as a DECLARATOR-DOMINATOR (a `declarations`-rule scope that is
    // NOT a composite-type body, e.g. c's topLevelDecl, which opens a
    // scope only to dominate a function's params). A composite TYPE tag minted
    // in such a scope's HEAD (a file-scope `struct P { … } v;`) floats PAST it
    // to the enclosing namespace at `record` time (C11 6.2.1) — mirrors the
    // analyzer's `floatToNamespaceScope`.
    void openScope(RuleId rule);
    void closeScope();   // aborts on global-scope underflow (caller bug)

    // ── bindings ──
    // `span` is the source span of the declaring name-token — carried so the
    // CU oracle can tell a name's OWN defining occurrence apart from a USE of
    // it (globalTypeBindings / D-CSUBSET-FN-TYPE-TYPEDEF-PAREN-NAME).
    // `liftToEnclosingScope` (a lifted field row's name, e.g. an enumeration
    // constant) binds past the composite's own body scope and any
    // declarator-dominator, into the nearest enclosing namespace scope.
    void record(std::string name, bool isType, SourceSpan span,
                bool liftToEnclosingScope = false);
    // ★ P69 L2 (lane `lm`): a DECLARATOR-MODE row's name at the END OF ITS
    // DECLARATOR — C 6.2.1p7: "Any other identifier has scope that begins just after
    // the completion of its declarator" — recorded while the row's frame is still
    // open, so the rest of the declaration (an initializer, the next declarator) and
    // a function definition's body see it. It binds where the row's close-time
    // record binds it: past the row's own scope when `rowOwnsInnermostScope` (a
    // declarator-dominator row such as c's topLevelDecl, whose scope is the innermost
    // live one while its declarators close), then as `record` places a name.
    void recordAtDeclaratorEnd(std::string name, bool isType, SourceSpan span,
                               bool rowOwnsInnermostScope);
    // The close-time record of a declarator-mode row: a name already bound at the
    // end of its declarator (same spelling, same declaring token, ordinary) is not
    // bound again; any other is recorded as `record` records it.
    void recordUnlessBoundAtItsDeclarator(std::string name, bool isType,
                                          SourceSpan span);
    // The answer for `name`: an ordinary identifier, the only kind recorded.
    [[nodiscard]] NameKind lookup(std::string_view name) const noexcept;

    // Seed a TYPE binding into the GLOBAL scope before parsing — the
    // compilation-unit oracle's cross-file typedef channel (FC2 A3).
    void seedGlobalType(std::string name);

    // Global-scope TYPE names bound during this parse, in binding order
    // (seeds included — on a seeded reparse the export surface is not
    // re-consumed; the oracle runs once per CU). The CU oracle unions
    // these across all trees. A tag is never recorded, so never exported: it is
    // no type name in another tree either.
    [[nodiscard]] std::vector<std::string> globalTypeNames() const;

    // Global-scope TYPE bindings paired with the source span of their
    // declaring name-token (a superset of globalTypeNames carrying the
    // definition site). The CU oracle's SELF-DEFINITION channel: a typedef
    // name is not in scope within its own declarator (C 6.2.1p7), so an
    // ambiguous-type-name candidate whose span EQUALS its own binding span
    // must NOT be oracle-seeded — otherwise `typedef int (F);` reparses with
    // F seeded and mis-commits `(F)` as an abstract function-suffix param.
    // Seeds (scope-0, oracle-injected on a reparse) carry an empty span and
    // never coincide with a real token, so they are inert here.
    [[nodiscard]] std::vector<std::pair<std::string, SourceSpan>>
        globalTypeBindings() const;

    // ── ambiguous-site records (triage rule 4) ──
    void recordCandidate(AmbiguousTypeNameCandidate c);
    [[nodiscard]] std::span<AmbiguousTypeNameCandidate const>
        candidates() const noexcept { return candidates_; }
    [[nodiscard]] std::vector<AmbiguousTypeNameCandidate> takeCandidates();

    // ── speculation safety ──
    // Captured/restored exactly like the parser's other four state
    // machines (SpeculationProbe): bindings + candidates are
    // append-only between snapshots so restore is truncate-to-count;
    // the live-scope stack is small (lexical depth) so a full copy is
    // cheap.
    struct Snapshot {
        std::size_t                bindingCount   = 0;
        std::size_t                candidateCount = 0;
        std::vector<std::uint32_t> liveScopes;
        std::vector<bool>          liveScopeDominator;   // parallel to liveScopes
        std::vector<bool>          liveScopeComposite;   // parallel to liveScopes
        std::uint32_t              nextScopeId    = 1;
        std::uint64_t              version        = 0;
    };
    [[nodiscard]] Snapshot snapshot() const;
    void restore(Snapshot&& s);

    // ── the lookup state's identity (cycle P69 L2, lane `lm`) ──
    // Two moments with the same version answer every `lookup` alike. A binding and
    // a closed scope (which may hide bindings) take a fresh number, never reused;
    // an opened scope hides nothing and keeps it (a scope is closed by
    // the frame that opened it, so a probe that starts with more empty scopes
    // open reads exactly what one without them reads); `restore` brings back the
    // number of the state it restores. The parser keys a probe's failure by it
    // when the failure asked the sketch (`failedProbes_`).
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }

private:
    struct Binding {
        std::string   name;
        std::uint32_t scope  = 0;     // owning scope id (0 = global)
        bool          isType = false;
        // Span of the declaring name-token. Empty for oracle-injected seeds
        // (they carry no source token) — see globalTypeBindings.
        SourceSpan    span   = SourceSpan::empty(0);
    };

    std::unordered_map<std::uint32_t, BinderDecl> byRule_;
    std::unordered_set<std::uint32_t>             scopeRules_;
    // Scope rules that open a DECLARATOR-DOMINATOR scope (a `declarations`
    // row that is NOT a Type/composite body). A composite TYPE tag declared
    // in such a scope floats past it to the enclosing namespace (see openScope).
    std::unordered_set<std::uint32_t>             dominatorScopeRules_;
    // Scope rules that open a COMPOSITE BODY scope (a scope rule whose binder
    // row is a Type — c's structSpec / unionSpec / enumSpec). A LIFTED field
    // name floats past it (see `record`).
    std::unordered_set<std::uint32_t>             compositeScopeRules_;

    // Chronological, append-only between snapshot/restore pairs. Closed
    // scopes do NOT truncate (liveness is scope-id-based) — see class doc.
    std::vector<Binding>                          bindings_;
    // Stack of live scope ids; [0] is the global scope (id 0), never popped.
    std::vector<std::uint32_t>                    liveScopes_;
    // Parallel to liveScopes_: true iff that scope is a declarator-dominator
    // (a TYPE tag recorded while it is live floats past it to the namespace).
    std::vector<bool>                             liveScopeIsDominator_;
    // Parallel to liveScopes_: true iff that scope is a composite BODY (a lifted
    // field name recorded while it is live floats past it to the namespace).
    std::vector<bool>                             liveScopeIsComposite_;
    std::uint32_t                                 nextScopeId_ = 1;

    std::vector<AmbiguousTypeNameCandidate>       candidates_;

    // `version()`: the current state's number, and where fresh ones come from
    // (never restored, so a number is never given twice).
    std::uint64_t                                 version_       = 0;
    std::uint64_t                                 versionSource_ = 0;
};

} // namespace dss
