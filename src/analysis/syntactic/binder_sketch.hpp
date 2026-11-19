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

// One UNKNOWN-NAME REFUSAL recorded during a parse (the type-name
// triage's rule 4): a speculative type-name-guarded probe built its type
// child on a LONE identifier the sketch had no entry for, and the triage
// refused the type reading (the guard's polarity is RequireKnownType, or
// under PreferType an operator followed), so the parser took the
// competing reading. Rollback-convergent: a record made inside a probe
// that later rolls back is erased with it (the sketch restores its
// count) and the winning parse makes its own, so the records are exactly
// the surviving parse's refusals.
//
// Engine-generic: nothing here says "cast" — any language whose grammar
// declares a `commitRequiresTypeName` shape gets the same machinery.
struct DSS_EXPORT UnknownNameRefusal {
    std::string name;          // the lone identifier's text
    SourceSpan  span;          // its source span — where it is named
    std::size_t fromPos  = 0;  // the refused probe's first token (TokenStream position)
    std::size_t reachPos = 0;  // how far that probe had read when it was refused
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
//   * Shadowing: each name's LIVE bindings form one chain, newest first, and
//     a lookup reads its head — the newest live binding wins (P69 L2b2).
//   * A scope's close UNLINKS its bindings from their names' chains; they stay
//     in the binding store (its own list keeps them, for a definition's
//     re-bind), dead. Every container is trailed, so a Snapshot is a fixed set
//     of integers and restore rewinds each container's journal since the
//     snapshot — the speculation-rollback contract (SpeculationProbe) costs
//     O(1) per probe and O(changes) per rollback.
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
    // A binding holds its name's entry in `heads_` by address, and the head journal holds heads by
    // address: a copy would hold the other sketch's (P69 L2b2).
    BinderSketch(BinderSketch const&)            = delete;
    BinderSketch& operator=(BinderSketch const&) = delete;
    BinderSketch(BinderSketch&&)                 = delete;
    BinderSketch& operator=(BinderSketch&&)      = delete;

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
    // P69 L2b2: where a TYPE recorded while it is the innermost scope binds, and where a LIFTED
    // name does, are worked out here once from its parent's (`LiveScope`), so a record walks no
    // stack.
    void openScope(RuleId rule);
    // The scope a `closeScope` closed.
    struct ClosedScope {
        std::uint32_t id = 0;
    };
    // P69 L2b2 (lane `lm`, the L2b1 review's W1): the close UNLINKS each binding the scope holds
    // from its name's chain, so no lookup meets it again until a restore rewinds the unlinking.
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
    // OWN — the lists closed inside it. A definition RETURNING a function pointer writes its own
    // list inside its declarator and the returned type's lists after it; each `param` row of a
    // returned list used to drop EVERY noted list, the definition's own included, so its body saw
    // none of its parameters (`static int (*g(int T))(int) { return (T) - 1 ? 0 : h; }` read
    // `(T) - 1` as a cast through a file-scope typedef). ✔MEASURED 2026-11-03 (probe-reference-cc
    // run 20261103-200559-43e50237, gcc 13.3.0 and clang 18.1.3, built and run): that program, the
    // `(void)` and named returned lists, two levels of returned function pointers and the `signal`
    // shape all exit 42 — the parameter, never the typedef.
    // ★★ P69 L2b2 (lane `lm`, the L2b1 review's D1): "CLOSED INSIDE IT" IS PARSE ORDER, NEVER A
    // SOURCE OFFSET. L2b1 told a construct's lists by the offset they start at — at or after the
    // construct's own start — and a macro's tokens carry the `#define` line's offsets: a returned
    // list written in a macro BODY starts before a definition whose own list is the macro's
    // argument, so `#define RET_FNPTR(d) int (*d)(int)` / `static RET_FNPTR(g(int T)) { … }` lost
    // g's parameters again (✔MEASURED 2026-11-04: gcc 13.3.0, clang 18.1.3 and cl 19.51 run the
    // four macro forms to 42; DSS refused them S_UnknownType, leg run 20261104-204404-5ece0c4a).
    // Every noted list takes the next number of a counter that only climbs (`paramListMark`); the
    // parser reads the counter when a declarator's or a declarator-mode row's frame OPENS, and its
    // end consumes the lists numbered from that mark on: exactly the ones noted while it was open.
    [[nodiscard]] bool isPrototypeParamScopeRule(RuleId rule) const noexcept;
    // Whether `rule` is some row's `definitionBodyRule` — the parser asks only then.
    [[nodiscard]] bool isDefinitionBodyRule(RuleId rule) const noexcept;
    // The number the next noted list takes: a frame's mark, read when it opens.
    [[nodiscard]] std::uint64_t paramListMark() const noexcept { return nextListSequence_; }
    void notePrototypeParamScope(ClosedScope scope, NodeId paramList);
    // At the end of a declarator whose frame opened at `declaratorMark`: consumes the lists noted
    // since and keeps `paramList`'s for a definition body (invalid ⇒ keeps none; an earlier kept
    // list of an enclosing declarator stays).
    void keepDefinitionParamsOf(NodeId paramList, std::uint64_t declaratorMark);
    void bindDefinitionParams();
    // At the close of a declarator-mode row whose frame opened at `rowMark`: drops the lists
    // noted since, and the kept list when it is one of them.
    void dropDefinitionParams(std::uint64_t rowMark) noexcept;

    // ── bindings ──
    // `liftToEnclosingScope` (a lifted field row's name, e.g. an enumeration
    // constant) binds past the composite's own body scope and any
    // declarator-dominator, into the nearest enclosing namespace scope.
    void record(std::string name, bool isType, bool liftToEnclosingScope = false);
    // ★ P69 L2 (lane `lm`): a DECLARATOR-MODE row's name at the END OF ITS
    // DECLARATOR — C 6.2.1p7: "Any other identifier has scope that begins just after
    // the completion of its declarator" — recorded while the row's frame is still
    // open, so the rest of the declaration (an initializer, the next declarator) and
    // a function definition's body see it. It binds where the row's close-time
    // record binds it: past the row's own scope when `rowOwnsInnermostScope` (a
    // declarator-dominator row such as c's topLevelDecl, whose scope is the innermost
    // live one while its declarators close), then as `record` places a name.
    // `nameNode` is the declaring name token's node: the note the row's close takes back.
    void recordAtDeclaratorEnd(std::string name, bool isType, bool rowOwnsInnermostScope,
                               NodeId nameNode);
    // The close-time record of a declarator-mode row: a name already bound at the
    // end of its declarator (its declaring token's node noted) is not bound again; any
    // other is recorded as `record` records it.
    // P69 L2b1 (lane `lm`): O(1) — the declarator-end binding notes its token
    // (`boundAtDeclaratorEnd_`) and this record takes the note back. It was a backward scan of
    // every binding that starts at or after the name, which for a definition is its whole body
    // and for a nest of initializers holding declarations is every level below: Θ(D²).
    // P69 L2b2 (the L2b1 review's D1): the note is the token's NODE, not its source offset — two
    // tokens of one macro body share an offset (`#define DECL_T(i) int T = i` /
    // `DECL_T(({ DECL_T(40); T + 1; }))`: the nested row took the outer one's note back, and the
    // outer row's close bound its name a second time).
    void recordUnlessBoundAtItsDeclarator(std::string name, bool isType, NodeId nameNode);
    // The answer for `name`: an ordinary identifier, the only kind recorded.
    // ★ P69 L2b1 (lane `lm`, the L2a2 review's B1): A LOOKUP VISITS ONLY ITS NAME'S BINDINGS. It
    // used to scan every binding ever recorded, newest first and dead ones included, until the
    // name matched — so a lookup cost the bindings recorded since the name's own: a nest that
    // declares a name and probes a typedef at every level cost Θ(D²), and a late `(u8)x` in a long
    // file walked every parameter and local of every earlier function.
    // ★★ P69 L2b2 (lane `lm`, the L2b1 review's W1): AND IT VISITS ONE. L2b1's per-name chain held
    // dead bindings too, and a lookup moved its name's head past the dead ones it met — for good
    // only past those that died before the OUTERMOST live snapshot; a binding that died after it
    // was passed under the journal, every inner probe that rolled back put it back, and the next
    // lookup walked it again: N bindings dying under a live probe, then N lookups each in a probe
    // the sketch refutes, cost Θ(N²) — the third cost of one class this round, each closed by making
    // a dead binding cheaper to pass. The chain holds LIVE bindings only now: a scope's close
    // unlinks its own (`closeScope`), every link is trailed, so a lookup reads the head and there is
    // no dead binding to pass; a head that is not live is a broken close, and aborts by name.
    [[nodiscard]] NameKind lookup(std::string_view name) const noexcept;

    // Seed a TYPE binding into the GLOBAL scope before parsing — the shipped
    // descriptors' typedef names, which no token of the parse declares
    // (`ParserConfig::seedGlobalTypeNames`).
    void seedGlobalType(std::string name);
    // Seed a VALUE binding into the GLOBAL scope before parsing — the shipped
    // descriptors' functions, objects and constants, which the semantic tier
    // binds and no token of the parse declares (`ParserConfig::seedGlobalValueNames`).
    void seedGlobalValue(std::string name);

    // ── the unknown-name refusals (triage rule 4) ──
    void recordRefusal(UnknownNameRefusal r);
    [[nodiscard]] std::span<UnknownNameRefusal const>
        refusals() const noexcept { return refusals_; }

    // ── speculation safety ──
    // Captured/restored exactly like the parser's other four state
    // machines (SpeculationProbe), LAST IN FIRST OUT: a restore of any snapshot but the innermost
    // live one aborts by name (P69 L2b2) — the parser's probes nest, and the trails rewind exactly
    // only in that order.
    // ★ P69 L2a2 (lane `lm`, the L2a review's N10): AND A SNAPSHOT IS O(1) IN THE LEXICAL
    // DEPTH. It used to COPY the live-scope stack (three vectors) — "small (lexical depth) so
    // a full copy is cheap" — once per probe, so a nest of D statement expressions, a probe or
    // more at every level with ~2 scopes per level live, cost Θ(D²) and the parse's token-
    // access count, the parser's work proxy, never saw it (it counts tokens, not copies).
    // ✔MEASURED 2026-10-28 (windows-x86_64-debug, MinGW GCC 13.2 Debug, the parse alone):
    // `int v = ({ ({ … 5; }) … });` 605 / 2385 / 9335 / 39652 ms at 1000 / 2000 / 4000 /
    // 10000 levels (×3.9 per doubling) while its token accesses went 61257 / 122257 / 244257
    // / 610257 (linear). Every container a probe can change is TRAILED now
    // (core/types/speculation_trail.hpp, the builder's and the walker's own primitive): a
    // snapshot is their marks and a restore costs the records since its marks. The journals are
    // armed by a snapshot and retired when the last live one is released (`Snapshot`'s
    // destructor), so they never outgrow the probes that can still rewind to them.
    // ★ P69 L2b1 (lane `lm`, the L2a2 review's A4): O(1) BY CONSTRUCTION, NOT BY A COUNTER. A
    // snapshot is ONE trivially copyable struct (`Fields`, its owner inside it) and nothing else —
    // the two static_asserts below refuse a container in `Fields` and a member beside it (P69 L2b2,
    // the L2b1 review's R1) — so it CANNOT hold a copy of a stack, and a copy taken anywhere else is
    // read through the stack's own accessors, which count every entry they hand out
    // (`TrailedStack::reads`, added into `workCount()`); a trailed stack cannot be copied at all.
    // ★ P69 L2b2 (lane `lm`, the L2b1 review's W1): where a TYPE binds when this scope is the
    // innermost (the nearest scope that is no declarator-dominator: `typeTarget`) and where a
    // LIFTED name does (the nearest that is neither a dominator nor a composite body:
    // `liftTarget`), worked out once at its open from its parent's — a record walked the live stack
    // for them, Θ(D) per enumeration constant in a nest of D composite bodies.
    struct LiveScope {
        std::uint32_t id         = 0;
        std::uint32_t typeTarget = 0;
        std::uint32_t liftTarget = 0;
    };
    struct ClosedParamScope {
        std::uint32_t scope    = 0;
        std::uint64_t sequence = 0;   // its number, in the order lists are noted (P69 L2b2)
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
        struct TrailMark {
            std::size_t size    = 0;
            std::size_t journal = 0;
        };
        struct Fields {
            BinderSketch*    owner           = nullptr;   // null once moved from
            std::size_t      depth           = 0;   // the live snapshots taken before it (P69 L2b2)
            TrailMark        bindings{};                  // P69 L2b2
            TrailMark        scopes{};                    // P69 L2b2
            TrailMark        liveScopes{};
            TrailMark        closedParams{};              // P69 L2a2
            std::size_t      refusalCount    = 0;
            std::size_t      headJournalSize = 0;         // P69 L2b1
            std::size_t      boundAtEndMark  = 0;         // P69 L2b1
            std::uint32_t    nextScopeId     = 1;
            std::uint64_t    version         = 0;
            ClosedParamScope definitionParams{};          // scope 0 ⇒ none kept
        };
        static_assert(std::is_trivially_copyable_v<Fields>,
                      "a binder-sketch snapshot holds scalars only: a container member would copy "
                      "a stack per probe (P69 L2b1, the L2a2 review's A4)");
        Fields f{};
    };
    static_assert(sizeof(Snapshot) == sizeof(Snapshot::Fields),
                  "a binder-sketch snapshot is its Fields and nothing else: a member beside them "
                  "could copy a stack per probe (P69 L2b2, the L2b1 review's R1)");
    [[nodiscard]] Snapshot snapshot();
    void restore(Snapshot&& s);

    // ── the sketch's own work, counted (P69 L2a2, lane `lm`, the L2a review's N10) ──
    // A DETERMINISTIC proxy for what the sketch does, as the parser's token accesses are for
    // what it reads: one unit per snapshot (it marks each trail and copies nothing), the journal
    // records a restore replays (one unit more), and every entry read from a container. The token
    // accesses cannot see any of it; this can, so a cost the sketch adds per probe or per lookup
    // in proportion to the nesting is super-linear HERE (the parser reports it as
    // `ParseResult::sketchWorkCount`).
    // P69 L2b1: the stack entries are counted where they are READ (`TrailedStack::reads`), not
    // where an author chose to add to a counter. P69 L2b2 (the L2b1 review's P1): and so are the
    // binding store's and the scope table's — every container the sketch walks is a trailed one,
    // so a scan of the bindings is counted however it is written; a lookup adds one unit.
    [[nodiscard]] std::uint64_t workCount() const noexcept {
        return work_ + bindings_.reads() + scopes_.reads() + liveScopes_.reads()
             + closedParamScopes_.reads();
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

    // The per-name index (P69 L2b1, B1): each name's spelling and its newest LIVE binding
    // (kNoBinding ⇒ none). Heterogeneous so a lookup by `string_view` allocates nothing. Never
    // erased from, so an entry's address is stable: a binding holds its name's entry by address.
    struct NameHash {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };
    using Heads     = std::unordered_map<std::string, std::uint32_t, NameHash, std::equal_to<>>;
    using HeadEntry = Heads::value_type;
    Heads heads_;

    // ★ P69 L2b2 (lane `lm`, the L2b1 review's W1 and P1): ONE BINDING RECORD, TRIVIALLY COPYABLE,
    // IN A TRAILED STORE. A binding is linked into its name's chain of LIVE bindings (`olderLive`,
    // `newerLive`) when it is recorded and unlinked when its scope closes (any position: a newer
    // binding of the same name may sit in an OUTER scope, floated or lifted there), and into its
    // scope's list (`nextInScope`, oldest first), which outlives the close: a definition's
    // parameters are copied from their list's own. Every link change is a journaled write of the
    // store (`TrailedStack::assign`), so a restore relinks exactly what it must.
    struct Binding {
        HeadEntry*    name        = nullptr;
        std::uint32_t scope       = 0;          // owning scope id (0 = global)
        std::uint32_t olderLive   = kNoBinding;
        std::uint32_t newerLive   = kNoBinding;
        std::uint32_t nextInScope = kNoBinding;
        bool          isType      = false;
    };
    TrailedStack<Binding> bindings_;
    // Per scope id (a scope's id is its index: ids are minted in order, and a restore rewinds the
    // table with `nextScopeId_`): its bindings' list and whether it is live. Trailed.
    struct ScopeRecord {
        std::uint32_t first = kNoBinding;
        std::uint32_t last  = kNoBinding;
        bool          live  = false;
    };
    TrailedStack<ScopeRecord> scopes_;

    // A head change, undone by `restore`: the head, and its value before.
    struct HeadChange {
        std::uint32_t* head = nullptr;
        std::uint32_t  old  = kNoBinding;
    };
    std::vector<HeadChange> headJournal_;   // armed while a snapshot is live (`liveMarks_`)
    void setHead_(std::uint32_t& head, std::uint32_t to);
    [[nodiscard]] HeadEntry& entryFor_(std::string name);
    // Every binding enters through here: appended, made its name's head, linked into its scope's
    // list. Aborts when the scope is not live.
    void pushBinding_(HeadEntry& name, std::uint32_t scope, bool isType);
    // Takes the binding at `index` out of its name's chain of live bindings.
    void unlink_(std::uint32_t index);

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

    // Stack of live scopes (`LiveScope`, above); [0] is the global scope (id 0), never popped.
    // Trailed (see `Snapshot`); every entry read from it counts into `workCount()`.
    TrailedStack<LiveScope>                       liveScopes_;
    std::uint32_t                                 nextScopeId_ = 1;
    // The snapshots not yet released; the journals retire when it reaches 0.
    std::size_t                                   liveMarks_ = 0;
    // `workCount()`; mutable because a `lookup`, which reads, counts what it does.
    mutable std::uint64_t                         work_ = 0;
    void releaseMark_() noexcept;

    std::vector<UnknownNameRefusal>               refusals_;   // `recordRefusal`; truncated on restore
    // `semantics.libraryBuiltins` of the schema the sketch was built from (outlives it).
    LibraryBuiltins const*                        libraryBuiltins_ = nullptr;

    // `version()`: the current state's number, and where fresh ones come from
    // (never restored, so a number is never given twice).
    std::uint64_t                                 version_       = 0;
    std::uint64_t                                 versionSource_ = 0;

    // P69 L2a2 (lane `lm`): function prototype scope (see `isPrototypeParamScopeRule`).
    // `closedParamScopes_`: each parameter list closed and not yet consumed by the declarator or
    // the row it closed inside, with its node and number; `definitionParams_`: the one kept for a
    // definition's body (scope 0 ⇒ none — the global scope's id is never a list's).
    // `nextListSequence_`: the next list's number — it only climbs, a restore included (P69 L2b2).
    RuleId                                        protoParamScopeRule_{};
    std::unordered_set<std::uint32_t>             definitionBodyRules_;
    TrailedStack<ClosedParamScope>                closedParamScopes_;   // trailed, as liveScopes_
    ClosedParamScope                              definitionParams_{};
    std::uint64_t                                 nextListSequence_ = 0;
    // Pops the entries numbered `mark` or later (the newest ones, by nesting); returns the one
    // whose node is `keep`, if any.
    ClosedParamScope consumeParamScopesFrom_(std::uint64_t mark, NodeId keep);
    // The declaring tokens' nodes of names bound at the end of their declarator whose row has not
    // closed yet (see `recordUnlessBoundAtItsDeclarator`). Trailed.
    TrailedSet<std::uint32_t>                     boundAtDeclaratorEnd_;
};

} // namespace dss
