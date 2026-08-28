#pragma once

#include "core/export.hpp"
#include "core/types/object_format_kind.hpp"          // ObjectFormatKind
#include "core/types/strong_ids.hpp"                 // TypeId
#include "core/types/type_lattice/core_type.hpp"     // TypeKind

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

// ── CROSS-DESCRIPTOR CONSISTENCY (fail-loud) ────────────────────────────────
//
// ★ ONE ACCUMULATOR, ONE `add`, N INVARIANTS — and that is a design choice, not
// an accident of history. Every invariant here has the SAME shape: shipped
// descriptors are AUTHORED INDEPENDENTLY, several are selected for ONE target,
// and an injection path picks a winner FIRST-WINS BY NAME while the loser's
// DIVERGENT declaration vanishes without a word. A checker per axis would need a
// call site per axis in the descriptor loop, so the next axis would arrive with a
// wiring change instead of a rule. It arrives here instead.
//
// TWO FAMILIES TODAY: (A)/(B) TYPE IDENTITY, and (C) REALIZATION AGREEMENT.
//
// D-LANG-TYPE-IDENTITY-VOCABULARY. Shipped descriptors are AUTHORED INDEPENDENTLY
// but interned into ONE lattice, and two of the injection paths are FIRST-WINS BY
// NAME:
//
//   * a struct/union TAG (`semantic_analyzer.cpp`, the `injectedTags` set) — only
//     the WINNER gets a `compositeScopeByType` field scope, so a second, DIVERGENT
//     declaration of the same tag interns a SECOND TypeId whose members are
//     unreachable. The user sees an INCLUDE-ORDER-DEPENDENT `S000D member access
//     '.' requires a composite-typed operand`;
//   * a typedef NAME (the `injectedNames` set) — the loser silently vanishes, so
//     which WIDTH/IDENTITY `off_t` has depends on include order.
//
// Neither is diagnosed by the per-file reader: `readShippedLibDescriptor` sees ONE
// descriptor at a time and cannot know that a sibling spells the same tag
// differently. Three consecutive fix rounds on this feature ALL failed on exactly
// this class (`struct timeval` declared with `i64` in `sys/resource.json` and
// `i64 "long"` in `sys/time.json`), so it is machine-checked here rather than
// re-asserted in a `$comment`.
//
// TWO invariants, both enforced over the types a descriptor ACTUALLY SELECTED for
// the active target (arch × format × dataModel) — the reader has already collapsed
// `variants` by the time this runs, so "selected for the same target" is exactly
// what is compared:
//
//   (A) CROSS-DECLARATION IDENTITY. Every declaration of a given struct/union TAG
//       NAME — whether a `structs` entry, an INLINE `struct "N" {…}` inside another
//       type's text, or a repeat in a second descriptor — must intern to the SAME
//       TypeId. Likewise every declaration of a given typedef NAME. Interned-TypeId
//       equality IS byte-identical-text equality after resolution, and it is
//       STRICTLY stronger: it also catches two spellings that differ only in a
//       vocabulary tag (`i64` vs `i64 "long"`), which is precisely the defect.
//
//   (B) VOCABULARY-WIDTH AGREEMENT. A vocabulary TAG names a type whose WIDTH is a
//       property of the DATA MODEL, so `i64 "long"` is a PHANTOM on LLP64 (where
//       the language mints `long` as I32): it matches no vocabulary entry at all,
//       and every `_Generic` / pointer-compatibility test against it fails. Each
//       tag a descriptor spells must therefore resolve to the SAME core the ACTIVE
//       LANGUAGE gives that name under the ACTIVE data model.
//
// ── (C) CROSS-DESCRIPTOR REALIZATION AGREEMENT ──────────────────────────────
//
// D-FFI-DUPLICATE-SYMBOL-ACROSS-DESCRIPTORS-SILENTLY-ORDER-RESOLVED. The THIRD
// first-wins-by-name path in the same loop is `injectedNames` over `symbols`, and
// the fact the loser takes with it is not a TYPE — it is the whole REALIZATION:
// which runtime IMAGE the name imports from, whether it is imported at all, which
// base name the linker asks for, which ELF version, whether a shim body is
// synthesized instead. Two descriptors may declare one name with DIFFERENT
// `library` values and the engine resolves it BY ORDER, with no diagnostic.
//
// ✔MEASURED (P42, pe64 exec, `objdump -p`, one three-line program, config mutated
// on a `DSS_CONFIG_ROOT` COPY so `memory.json` said `msvcrt.dll` where
// `string.json` said `ucrtbase.dll`): `#include <string.h>` before `<memory.h>`
// imported `memcpy` from **ucrtbase.dll**; the two includes SWAPPED imported it
// from **msvcrt.dll**; and a HAND-DECLARED `void *memcpy(void*, const void*,
// unsigned long long);` with no include at all imported it from **msvcrt.dll**
// too — because that path (`realizeShippedExternSymbols`) resolves by the corpus
// index's relPath sort, a DIFFERENT order from the include closure. Three
// silently different binaries, rc=0 and ZERO diagnostics on all three. The third
// is the sharpest: it breaks THE PLATFORM REALIZATION ORACLE's own stated law —
// *"a hand-written declaration and an `#include`d one produce a BYTE-IDENTICAL
// import"* — because the two paths disagree about WHICH ORDER decides.
//
// ★★ THE RULE IS AGREEMENT, NOT UNIQUENESS, AND THE DIFFERENCE IS THE WHOLE
// DESIGN. A duplicate is NORMAL and is RELIED ON: `<memory.h>` deliberately
// mirrors `<string.h>`'s `mem*` surface, `<tgmath.h>` mirrors `<math.h>`'s, and
// the first-wins dedup is what makes including both mint exactly one extern.
// ✔MEASURED over the shipped corpus: 53 names are declared by more than one
// descriptor, and **12 of them never co-exist on ANY format** — `io.json` (pe) and
// `unistd.json` (elf/macho) both declare `close`/`read`/`write`/…, with DIFFERENT
// libraries AND different `linkName`s, entirely correctly, because no compile ever
// selects both. A detector that refused duplicates would refuse the shipped
// configuration on day one. So the discriminator is:
//
//   COMPARE ONLY ROWS LIVE ON THE SAME OBJECT FORMAT, after BOTH availability
//   gates (the document's and the symbol's); require every such row to agree on
//   the ENTIRE realization; say nothing about rows that cannot co-exist.
//
// ★★ AND THE RULE IS RUN BY BOTH PICKERS, WHICH IS THE POINT — the defect was
// never "descriptors disagree", it was "TWO PATHS PICK A WINNER BY TWO DIFFERENT
// ORDERS AND NEITHER SAYS SO". `add` serves the `#include` path (include-closure
// order); `addRealizationsOf` serves the HAND-DECLARED path
// (`realizeShippedExternSymbols`, corpus relPath order). One rule, one message,
// one code — so the two orders can no longer produce two different programs
// without a diagnostic, whichever of them a source happened to reach.
//
// ✔That line is measured, not assumed: it leaves 41 names co-live across 4
// descriptor pairs (math+tgmath, memory+string, sys/sysctl+unistd, and
// windows.json's own repeated `CloseHandle` row) = 118 (name, format) live pairs
// in the shipped corpus, and ALL 118 agree on every axis. The check is green on
// the corpus the day it lands, and red the moment a pair diverges.
//
// THE AXES COMPARED ARE THE STRUCT'S OWN, not a hand-picked list: `signature`
// (interned TypeId — strictly stronger than text equality), `library` for the
// active format WITH the per-symbol override merged over the descriptor map,
// `realization` (shipped-source) for the active format merged the same way,
// `synthesize`, `linkName`, `version`, `kind`, `linkage`, `noreturn`,
// `returnsTwice`. Each is a fact injection takes from the winner and discards from
// the loser, so each is a fact two descriptors can silently disagree about.
// `availableObjectFormats` is deliberately NOT an axis — it is the GATE that
// decides which rows are compared, never a thing they must agree on.
//
// ⚠ A row DECLARING an image and a row declaring NONE for this format is a
// DISAGREEMENT, not a shrug: `realizeShippedExternSymbols` treats
// `NoLibraryForFormat` as a usable answer and stops on it, so the sort order would
// silently decide between "import from that image" and "route unbound to the link
// tier". Absence is encoded as its own value and compared like any other.
//
// AGNOSTIC. This checker knows NO spelling and NO data model: the vocabulary is
// passed in as opaque (name → core) rows the CALLER resolved from its own language
// config, and a name the language does not declare is skipped (a descriptor may
// legitimately model a type this language has no word for). Invariant (A) is pure
// TypeId comparison. Invariant (C) reads the format's DECLARED name out of the
// shared `objectFormatKindName` vocabulary and compares strings the CONFIG wrote —
// there is no `if (format == …)`, and no image name appears in this file.

namespace dss {

class DiagnosticReporter;
class TypeInterner;

namespace ffi {

struct ShippedLibDescriptor;
struct ShippedSymbol;

// One row of the ACTIVE LANGUAGE's primitive type VOCABULARY, already resolved for
// the ACTIVE data model (and long-double axis) by the caller. `name` is borrowed —
// it must outlive the checker.
struct DSS_EXPORT VocabularyCore {
    std::string_view name;
    TypeKind         core = TypeKind::Void;
};

// Accumulates what a set of descriptors resolved for ONE target DECLARES — the
// NAMED types for (A)/(B), the per-symbol REALIZATION for (C) — and reports every
// violation. (A)/(B) report `F_ShippedTypeIdentityConflict`; (C) reports
// `F_ShippedCorpusInvariantBroken`, whose subject is exactly this — descriptors
// that are each individually well-formed and decode cleanly, with the fault in the
// RELATIONSHIP between them, a thing no per-file read can see. Both codes are
// unsuppressable: a silent first-wins is a wrong-layout / unreachable-member
// miscompile for (A)/(B) and a wrong-runtime-image import for (C), and a
// suppressible form would sell back exactly the silence these checks buy.
//
// USAGE: one instance per (compilation unit × target). Feed every descriptor the
// unit resolved via `add`; the checker is stateful because (A) is inherently
// cross-file. `add` returns false iff THIS descriptor violated something (earlier
// descriptors' violations are not re-reported).
class DSS_EXPORT ShippedTypeConsistency {
public:
    // `activeFormat` reproduces the analyzer's PER-SYMBOL availability gate: a
    // symbol whose `availableObjectFormats` excludes the active format is never
    // injected, so its signature is not a declaration ON THIS TARGET and must
    // not be compared against one that is (`threads.json` declares three
    // per-format `tss_get` rows, only one of which ships). nullopt = no gate,
    // the reader's own convention for direct-API/unit callers.
    ShippedTypeConsistency(TypeInterner const&             interner,
                           std::span<VocabularyCore const> vocabulary,
                           std::optional<ObjectFormatKind> activeFormat = std::nullopt)
        : in_(&interner), vocabulary_(vocabulary), activeFormat_(activeFormat) {}

    // `origin` names WHERE this descriptor came from, for the diagnostic (the
    // analyzer passes the header spelling, the exhaustive test the file path).
    [[nodiscard]] bool add(std::string_view              origin,
                           ShippedLibDescriptor const&   desc,
                           DiagnosticReporter&           reporter);

    // ── THE HAND-DECLARED PATH'S SLICE OF `add` ─────────────────────────────
    //
    // Invariant (C) ONLY, and only over the symbol rows whose name is in
    // `names`. Same axes, same message, same code, same
    // `duplicateRealizationsCompared()` witness — the rule has ONE owner, and
    // that is the whole reason this is a second entry point rather than a
    // second checker.
    //
    // ★ WHY A NARROWER SLICE AND NOT `add`. `realizeShippedExternSymbols` (C23
    // 7.1.4p2 — a name DECLARED but never `#include`d) reads a descriptor for
    // ONE reason: the caller asked about a name that descriptor happens to
    // declare. It injects NOTHING else from it — not its typedefs, not its tags,
    // not its other symbols — so holding the program to those rows would refuse
    // builds the `#include` path accepts. ✔The concrete case: a TU that
    // hand-declares `memcpy` makes this oracle read BOTH `memory.json` and
    // `string.json`, while `#include <string.h>` alone reads ONE. Running full
    // `add` here would refuse the first program over a `memmove` divergence the
    // second never sees — a NEW spelling-dependent asymmetry, which is precisely
    // the defect class this invariant exists to delete. The slice keeps the two
    // paths refusing the same configurations.
    //
    // ⚠ The caller applies the DOCUMENT availability gate before calling (a
    // header that does not exist here declares nothing here); the SYMBOL gate is
    // applied inside, exactly as `add` applies it.
    [[nodiscard]] bool addRealizationsOf(std::string_view             origin,
                                         ShippedLibDescriptor const&  desc,
                                         std::span<std::string const> names,
                                         DiagnosticReporter&          reporter);

    // How many times invariant (C) actually COMPARED a symbol row against an
    // earlier declaration of the same name selected for this target — i.e. how
    // many co-live duplicates it saw, not how many rows it read.
    //
    // ★ IT EXISTS FOR THE GUARD'S VACUITY PROBLEM, WHICH IS THE ONLY WAY (C) CAN
    // FAIL WITHOUT SAYING SO. (C) is green when descriptors agree AND green when
    // it compared nothing at all — a corpus enumeration that quietly matched no
    // files, an availability gate that widened until no two rows co-exist, or a
    // wiring change that stopped calling `recordRealization` all look EXACTLY like
    // a clean corpus. A sweep asserts a FLOOR on this so "no conflicts" is only
    // ever reported about work that was actually done.
    [[nodiscard]] std::size_t duplicateRealizationsCompared() const noexcept {
        return duplicatesCompared_;
    }

private:
    // The first declaration of a name — the one every later declaration is
    // compared against. `origin` is copied (a descriptor is a loop temporary).
    struct Decl {
        TypeId      type;
        std::string origin;
    };

    // Recursively record/verify every named type REACHABLE from `t`. `operands()`
    // is the universal child accessor (and is qualifier-transparent), so no kind
    // switch is needed; `visited_` makes a self-referential type terminate.
    void walk(TypeId t, std::string_view origin, DiagnosticReporter& reporter,
              bool& ok);

    void recordNamed(std::unordered_map<std::string, Decl>& into,
                     char const* what, std::string name, TypeId t,
                     std::string_view origin, DiagnosticReporter& reporter,
                     bool& ok);

    // ── (C) REALIZATION AGREEMENT ────────────────────────────────────────────
    // One symbol row's realization ON THE ACTIVE FORMAT. `traits` is the
    // CANONICAL, human-readable rendering of every non-type axis — built once,
    // compared as a whole, and quoted verbatim in the diagnostic, so the message
    // shows the author exactly the two rows they must reconcile. The signature is
    // held separately because it compares as an interned TypeId (stronger than
    // text: it also separates `i64` from `i64 "long"`) and renders through the
    // same `render()` the type invariants use.
    struct Realization {
        TypeId      signature;
        std::string traits;
        std::string origin;
    };

    // Record/verify this row against the first row of the same name selected for
    // this target. Called ONLY for rows that passed both availability gates, and
    // ONLY when the active format is known — without a format there is no
    // `library` entry to select, so there is no realization to state and
    // inventing one would be the guess this invariant exists to delete.
    void recordRealization(std::string_view origin, ShippedLibDescriptor const& desc,
                           ShippedSymbol const& sym, std::string_view formatName,
                           DiagnosticReporter& reporter, bool& ok);

    // The hir-text rendering of `t` — the SAME spelling a descriptor author
    // writes, so the diagnostic is directly actionable.
    [[nodiscard]] std::string render(TypeId t, int depth = 0) const;

    // ── THE TWO GATE QUESTIONS, ASKED IN ONE PLACE ──────────────────────────
    // Both entry points must answer them IDENTICALLY: a row the two admitted
    // differently would be compared on one path and not the other, which is the
    // spelling-dependent divergence this invariant exists to delete. They are
    // private helpers rather than repeated conditions for exactly that reason.

    // The map key `library`/`realization` are looked up under, or EMPTY when
    // there is no realization to state (no active format, or the `unknown`
    // sentinel — see the ⚠ at the symbol loop).
    [[nodiscard]] std::string_view activeFormatName() const noexcept;

    // Is this symbol row a declaration ON THIS TARGET? (The PER-SYMBOL
    // availability gate, exactly as the analyzer gates injection.)
    [[nodiscard]] bool symbolSelectedHere(ShippedSymbol const& sym) const noexcept;

    TypeInterner const*             in_;
    std::span<VocabularyCore const> vocabulary_;
    std::optional<ObjectFormatKind> activeFormat_;
    std::unordered_map<std::string, Decl> tags_;      // struct/union/enum TAG ns
    std::unordered_map<std::string, Decl> typedefs_;  // typedef NAME ns
    // (C) The ORDINARY IDENTIFIER namespace (C 6.2.3) — the third first-wins set,
    // keyed by exactly the name `injectedNames` keys, holding the REALIZATION
    // rather than the type.
    std::unordered_map<std::string, Realization> realizations_;
    std::size_t                           duplicatesCompared_ = 0;  // see accessor
    std::unordered_set<std::uint32_t>     visited_;   // TypeId.v, walk memo
};

} // namespace ffi
} // namespace dss
