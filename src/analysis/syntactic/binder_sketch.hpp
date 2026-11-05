#pragma once

#include "core/export.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/source_span.hpp"
#include "core/types/speculation_trail.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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
//   * Shadowing: lookup walks its NAME's bindings newest-first (a per-name
//     chain, P69 L2b1) and skips entries whose scope is no longer live — the
//     innermost live binding wins.
//   * Closed-scope bindings are RETAINED (marked dead via scope-id
//     liveness) rather than truncated, so a Snapshot is a fixed set of
//     integers (every stack is trailed) and restore is truncation plus the
//     journal records since the snapshot — the speculation-rollback contract
//     (SpeculationProbe) costs O(1) per probe and O(changes) per rollback.
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
        // P69 L2a2 (lane `lm`): the rule of the BODY that makes this row a function
        // DEFINITION (its `kindByChild` discriminator, when that names a Function) —
        // invalid for a row that can never be one. See `bindDefinitionParams`.
        RuleId        definitionBodyRule{};
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
    // The scope a `closeScope` closed: its id, and where its bindings begin in the record (every
    // binding a scope holds was recorded after it opened).
    struct ClosedScope {
        std::uint32_t id           = 0;
        std::uint32_t firstBinding = 0;
    };
    // Aborts on global-scope underflow (caller bug).
    ClosedScope closeScope();

    // ── ★ P69 L2a2 (lane `lm`, the L2a review's N9): FUNCTION PROTOTYPE SCOPE ──
    // A parameter list (`declarators.prototypeParamScopeRule`) is a scope of its own (C
    // 6.2.1p4): its parameters are recorded into it and die with it, so a parameter of a
    // nested function declarator — a function pointer, a block-scope prototype, a typedef, a
    // cast's type name — no longer hides a typedef of its spelling for the rest of the block.
    // The list that declares a function DEFINITION's own name gives its names to the body:
    // the parser notes each closed list (`notePrototypeParamScope`), keeps the one the shared
    // structural predicate names at the declarator's end (`keepDefinitionParamsOf`,
    // core/types/prototype_param_scope.hpp, the analyzer's own question), and binds its names
    // into the scope live when the row's definition body opens (`bindDefinitionParams`), where
    // they were bound before. The row's close drops what was kept (`dropDefinitionParams`).
    //
    // ★ P69 L2b1 (lane `lm`, the L2a2 review's A1): WHAT A DECLARATOR OR A ROW CONSUMES IS ITS
    // OWN. Every noted list carries the source offset it starts at. A declarator's end consumes
    // only the lists that start at or after the declarator's own start, and a row's close only
    // those at or after the row's start: lists nest with their tokens, so those are exactly the
    // lists closed inside it, the newest entries. A definition RETURNING a function pointer writes
    // its own list inside its declarator and the returned type's lists after it; each `param` row
    // of a returned list used to drop EVERY noted list, the definition's own included, so its body
    // saw none of its parameters (`static int (*g(int T))(int) { return (T) - 1 ? 0 : h; }` read
    // `(T) - 1` as a cast through a file-scope typedef). ✔MEASURED 2026-11-03 (probe-reference-cc
    // run 20261103-200559-43e50237, gcc 13.3.0 and clang 18.1.3, built and run): that program, the
    // `(void)` and named returned lists, two levels of returned function pointers and the `signal`
    // shape all exit 42 — the parameter, never the typedef.
    [[nodiscard]] bool isPrototypeParamScopeRule(RuleId rule) const noexcept;
    // Whether `rule` is some row's `definitionBodyRule` — the parser asks only then.
    [[nodiscard]] bool isDefinitionBodyRule(RuleId rule) const noexcept;
    void notePrototypeParamScope(ClosedScope scope, NodeId paramList, std::uint32_t listStart);
    // At the end of a declarator that starts at `declaratorStart`: consumes the lists closed
    // inside it and keeps `paramList`'s for a definition body (invalid ⇒ keeps none; an earlier
    // kept list of an enclosing declarator stays).
    void keepDefinitionParamsOf(NodeId paramList, std::uint32_t declaratorStart);
    void bindDefinitionParams();
    // At the close of a declarator-mode row that starts at `rowStart`: drops the lists closed
    // inside it, and the kept list when it is the row's own.
    void dropDefinitionParams(std::uint32_t rowStart) noexcept;

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
    // P69 L2b1 (lane `lm`): O(1) — the declarator-end binding notes its token
    // (`boundAtDeclaratorEnd_`) and this record takes the note back. It was a backward scan of
    // every binding that starts at or after the name, which for a definition is its whole body
    // and for a nest of initializers holding declarations is every level below: Θ(D²).
    void recordUnlessBoundAtItsDeclarator(std::string name, bool isType,
                                          SourceSpan span);
    // The answer for `name`: an ordinary identifier, the only kind recorded.
    // ★ P69 L2b1 (lane `lm`, the L2a2 review's B1): A LOOKUP VISITS ONLY ITS NAME'S BINDINGS. It
    // used to scan every binding ever recorded, newest first and dead ones included, until the
    // name matched — so a lookup cost the bindings recorded since the name's own: a nest that
    // declares a name and probes a typedef at every level cost Θ(D²), and a late `(u8)x` in a long
    // file walked every parameter and local of every earlier function. Now a per-name index holds
    // each name's newest binding and every binding links its same-name predecessor; a lookup walks
    // that chain and moves a DEAD head past itself (its scope closed: no lookup can see it again
    // unless a restore reopens it, and the restore puts the head back), so each binding is passed
    // dead at most once. Every head change is journaled while a snapshot is live and undone by
    // `restore` — except a move past bindings that died before the OUTERMOST live snapshot was
    // taken: they are dead in every state a restore can bring back, so that move is made for good
    // (`passBindingsDeadForEveryLiveSnapshot_`). Journaled, it was undone by every rolled-back probe
    // that looked the name up, and the next lookup walked the same dead bindings again: N
    // definitions whose parameter hides a file-scope variable `T`, then N late `(T)` probes the
    // sketch refutes, cost Θ(N²) (✔MEASURED, P69 L2b1: x63.8 the work for x8 the file). Not
    // `const`: the head moves are the lookup's own work.
    [[nodiscard]] NameKind lookup(std::string_view name) noexcept;

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
    // append-only between snapshots so restore is truncate-to-count.
    // ★ P69 L2a2 (lane `lm`, the L2a review's N10): AND A SNAPSHOT IS O(1) IN THE LEXICAL
    // DEPTH. It used to COPY the live-scope stack (three vectors) — "small (lexical depth) so
    // a full copy is cheap" — once per probe, so a nest of D statement expressions, a probe or
    // more at every level with ~2 scopes per level live, cost Θ(D²) and the parse's token-
    // access count, the parser's work proxy, never saw it (it counts tokens, not copies).
    // ✔MEASURED 2026-10-28 (windows-x86_64-debug, MinGW GCC 13.2 Debug, the parse alone):
    // `int v = ({ ({ … 5; }) … });` 605 / 2385 / 9335 / 39652 ms at 1000 / 2000 / 4000 /
    // 10000 levels (×3.9 per doubling) while its token accesses went 61257 / 122257 / 244257
    // / 610257 (linear). The two stacks a probe can change are TRAILED now
    // (core/types/speculation_trail.hpp, the builder's and the walker's own primitive): a
    // snapshot is two marks and a restore costs the pops since its mark. The journal is armed
    // by a snapshot and retired when the last live one is released (`Snapshot`'s destructor),
    // so it never outgrows the probes that can still rewind to it.
    // ★ P69 L2b1 (lane `lm`, the L2a2 review's A4): O(1) BY CONSTRUCTION, NOT BY A COUNTER. Every
    // field a snapshot holds is one trivially copyable struct (`Fields`), so it CANNOT hold a copy
    // of a stack (a container is not trivially copyable: the static_assert below refuses one), and
    // a copy taken anywhere else is read through the stack's own accessors, which count every entry
    // they hand out (`TrailedStack::reads`, added into `workCount()`).
    struct LiveScope {
        std::uint32_t id           = 0;
        std::uint32_t firstBinding = 0;   // bindings_.size() when it opened (P69 L2b1)
        bool          dominator    = false;
        bool          composite    = false;
    };
    struct ClosedParamScope {
        std::uint32_t scope        = 0;
        std::uint32_t firstBinding = 0;
        std::uint32_t listStart    = 0;   // the list's source offset (P69 L2b1)
        NodeId        list{};
    };
    // Exported on its own (a nested class is not covered by its owner's export on a DLL
    // build): a unit test holds one (P69 L2b1, the cost pins' direct sketch TEST).
    class DSS_EXPORT Snapshot {
    public:
        Snapshot(Snapshot&& other) noexcept;
        Snapshot(Snapshot const&)            = delete;
        Snapshot& operator=(Snapshot const&) = delete;
        Snapshot& operator=(Snapshot&&)      = delete;
        ~Snapshot();

    private:
        friend class BinderSketch;
        Snapshot() = default;
        struct Fields {
            std::size_t      bindingCount            = 0;
            std::size_t      candidateCount          = 0;
            std::size_t      liveScopesMarkSize      = 0;
            std::size_t      liveScopesMarkJournal   = 0;
            std::size_t      closedParamsMarkSize    = 0;   // P69 L2a2
            std::size_t      closedParamsMarkJournal = 0;
            std::size_t      headJournalSize         = 0;   // P69 L2b1
            std::size_t      boundAtEndMark          = 0;   // P69 L2b1
            std::uint32_t    nextScopeId             = 1;
            std::uint64_t    version                 = 0;
            ClosedParamScope definitionParams{};            // scope 0 ⇒ none kept
        };
        static_assert(std::is_trivially_copyable_v<Fields>,
                      "a binder-sketch snapshot holds scalars only: a container member would copy "
                      "a stack per probe (P69 L2b1, the L2a2 review's A4)");
        BinderSketch* owner_ = nullptr;   // null once moved from
        Fields        f{};
    };
    [[nodiscard]] Snapshot snapshot();
    void restore(Snapshot&& s);

    // ── the sketch's own work, counted (P69 L2a2, lane `lm`, the L2a review's N10) ──
    // A DETERMINISTIC proxy for what the sketch does, as the parser's token accesses are for
    // what it reads: the live-scope and parameter-list entries a snapshot copies (none: it
    // marks each stack, one unit apiece), the journal records a restore replays (one unit
    // more), and the bindings and live-scope entries a `lookup` visits. The token accesses
    // cannot see any of it; this can, so a cost the sketch adds per probe or per lookup in
    // proportion to the nesting is super-linear HERE (the parser reports it as
    // `ParseResult::sketchWorkCount`).
    // P69 L2b1: the stack entries are counted where they are READ (`TrailedStack::reads`), not
    // where an author chose to add to a counter; a lookup counts each binding of its name's chain,
    // a re-bind each binding it copies from, a restore each head change it undoes.
    [[nodiscard]] std::uint64_t workCount() const noexcept {
        return work_ + liveScopes_.reads() + closedParamScopes_.reads();
    }

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
    static constexpr std::uint32_t kNoBinding = 0xFFFFFFFFu;
    struct Binding {
        std::string   name;
        std::uint32_t scope  = 0;     // owning scope id (0 = global)
        bool          isType = false;
        // Span of the declaring name-token. Empty for oracle-injected seeds
        // (they carry no source token) — see globalTypeBindings.
        SourceSpan    span   = SourceSpan::empty(0);
        // P69 L2b1: the previous binding of the same name (kNoBinding ⇒ none), set when recorded.
        std::uint32_t prevSameName = kNoBinding;
    };

    // The per-name index (P69 L2b1, B1): a name's newest binding still linked. Heterogeneous so a
    // lookup by `string_view` allocates nothing.
    struct NameHash {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };
    std::unordered_map<std::string, std::uint32_t, NameHash, std::equal_to<>> heads_;
    // A head change, undone by `restore`: the binding whose name it was, and the head before.
    // A head is an element of `heads_`, which is never erased from, so its address is stable.
    struct HeadChange {
        std::uint32_t* head = nullptr;
        std::uint32_t  old  = kNoBinding;
    };
    std::vector<HeadChange> headJournal_;   // armed while a snapshot is live (`liveMarks_`)
    void setHead_(std::uint32_t& head, std::uint32_t to);
    // Every binding enters through here: appended, linked to its name's chain, made the head.
    void pushBinding_(Binding b);
    // A DEAD binding (`b`, at `index`) that died before the outermost live snapshot was taken: its
    // scope's last close (`closedAt_`) is older than that snapshot (`stableBefore_`) and so is the
    // binding (`stableBindingCount_`). No restore while that snapshot lives reopens the scope or
    // drops the binding — a restore goes back to a state after it — so passing it is never undone.
    // With no snapshot live every dead binding is one.
    [[nodiscard]] bool diedBeforeEveryLiveSnapshot_(Binding const& b,
                                                    std::uint32_t index) const noexcept;
    // Moves `head` past the dead bindings `diedBeforeEveryLiveSnapshot_` answers for, unjournaled,
    // each counted once: they are never walked again.
    void passBindingsDeadForEveryLiveSnapshot_(std::uint32_t& head) noexcept;
    // Per scope id, the number (`versionSource_`'s, never given twice) its latest close was given;
    // 0 ⇒ never closed. Not trailed: a scope a restore reopens is live, which `lookup` asks first,
    // and its next close stamps it again. `stableBefore_` / `stableBindingCount_`: `versionSource_`
    // and the binding count when the outermost live snapshot was taken (read only while one lives).
    std::vector<std::uint64_t> closedAt_;
    std::uint64_t              stableBefore_       = 0;
    std::size_t                stableBindingCount_ = 0;

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
    // One live scope (`LiveScope`, above). `dominator`: a declarator-dominator (a TYPE tag
    // recorded while it is live floats past it to the namespace). `composite`: a composite BODY
    // (a lifted field name recorded while it is live floats past it to the namespace).
    // Stack of live scopes; [0] is the global scope (id 0), never popped. Its ids STRICTLY
    // ASCEND from the bottom (a scope opens with `nextScopeId_++`, above every live id, and a
    // restore brings back a stack and the `nextScopeId_` that was above it), which `lookup`'s
    // binary search reads and `openScope` checks. Trailed (see `Snapshot`); every entry read
    // from it counts into `workCount()` (`TrailedStack::reads`).
    TrailedStack<LiveScope>                       liveScopes_;
    std::uint32_t                                 nextScopeId_ = 1;
    [[nodiscard]] bool scopeIsLive_(std::uint32_t id) const noexcept;
    // The snapshots not yet released; the journals retire when it reaches 0.
    std::size_t                                   liveMarks_ = 0;
    // `workCount()`; mutable because a `lookup`, which reads, counts what it visits.
    mutable std::uint64_t                         work_ = 0;
    void releaseMark_() noexcept;

    std::vector<AmbiguousTypeNameCandidate>       candidates_;

    // `version()`: the current state's number, and where fresh ones come from
    // (never restored, so a number is never given twice).
    std::uint64_t                                 version_       = 0;
    std::uint64_t                                 versionSource_ = 0;

    // P69 L2a2 (lane `lm`): function prototype scope (see `isPrototypeParamScopeRule`).
    // `closedParamScopes_`: each parameter list closed and not yet consumed by the declarator or
    // the row it closed inside (P69 L2b1), with its node and start; `definitionParams_`: the one
    // kept for a definition's body (scope 0 ⇒ none — the global scope's id is never a list's).
    RuleId                                        protoParamScopeRule_{};
    std::unordered_set<std::uint32_t>             definitionBodyRules_;
    TrailedStack<ClosedParamScope>                closedParamScopes_;   // trailed, as liveScopes_
    ClosedParamScope                              definitionParams_{};
    // Pops the entries whose list starts at or after `start` (the newest ones, by nesting);
    // returns the one whose node is `keep`, if any.
    ClosedParamScope consumeParamScopesFrom_(std::uint32_t start, NodeId keep);
    // The declaring tokens (their start offsets) of names bound at the end of their declarator
    // whose row has not closed yet (see `recordUnlessBoundAtItsDeclarator`). Trailed.
    TrailedSet<std::uint32_t>                     boundAtDeclaratorEnd_;
};

} // namespace dss
