#pragma once

#include "core/export.hpp"
#include "core/types/symbol_attrs.hpp"  // SymbolBinding, DuplicateMatch
#include "link/symbol_kind.hpp"         // LinkedSymbolKey

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// Cross-CU DEFINITION resolution — the PURE, tier-neutral winner-selection kernel
// (Cycle 24 extraction from `linker.cpp::resolveCrossCuSymbols`). Given every
// externally-visible symbol DEFINITION across the linked CompilationUnits — each as a
// `(name, binding, key)` triple — it computes the WINNING definition per name after
// weak-vs-strong resolution, plus the names that have an ambiguous (two-strong)
// definition.
//
// **Why a separate, pure function** (the Cycle-25 whole-program-MIR-merge
// prerequisite): the cross-CU symbol policy (strong-shadows-weak / two-strong is
// ambiguous / all-weak lowest-key wins, order-independent) is the SINGLE source of
// truth both the linker AND a future whole-program MIR merge must agree on. Extracting
// it as a pure value→value function lets a direct unit test pin the policy (the
// `test_cross_cu_resolve` tripwire) so the two consumers can never silently diverge.
//
// **Conflict-as-data, not conflict-as-diagnostic.** This function emits NO
// diagnostics, takes NO `DiagnosticReporter`, and depends on NO `AssembledModule` /
// target / object-format / source-language type. A two-strong collision is reported by
// RECORDING a `CrossCuConflict` — the name plus the colliding key PAIR (the existing
// winner-so-far + the incoming duplicate) — in `conflicts` (data); the CALLER (the
// linker) turns each recorded conflict into its `K_SymbolRedefinedAcrossUnits`
// diagnostic, which names BOTH defining CompilationUnits. Carrying the pair (not just
// the name) lets the caller reproduce the original both-CUs-named wording AND gives the
// Cycle-25 whole-program MIR merge the colliding keys it needs to fold. This keeps the
// kernel reusable by any tier (the MIR merge has its own diagnostic vocabulary) and
// keeps it trivially testable. **No `if (target/format/lang == …)` — name+binding
// only.** (The standing source/target/linker agnosticism veto.)

namespace dss::linker {

// The ranks a caller states for a weak definition today (`CrossCuDef::weakRank`). Only
// their ORDER means anything to the kernel.
inline constexpr std::uint8_t kYieldingWeakDefinition = 0;   // every other weak definition of the name replaces it
inline constexpr std::uint8_t kUnrankedWeakDefinition = 1;   // the default: folds as weak definitions always did

// ── WHAT A WEAK DEFINITION IS BESIDE A STRONG ONE OF ITS NAME ─────────────────
//
// P69 fold 3 (lane `xa`). Until it, the kernel knew one answer — the strong
// definition replaces the weak one — and that answer is a FORMAT's, not a law:
// a PE select-any definition (a COMDAT) beside a strong definition of its name
// is a duplicate, refused by every PE linker measured (✔MEASURED 2026-10-12, 16
// of 16 cells: link.exe 14.44 LNK2005, lld-link 19.1.5 "duplicate symbol", GNU
// ld 2.42 "multiple definition", both object orders, on cl's, clang's and MinGW
// gcc's objects), where DSS linked the pair in silence and the program ran on
// the strong definition's bytes. The CALLER states the answer per definition,
// read from the link's document (`linker::weakBesideAStrongDefinition`,
// link/common_yield.hpp); the kernel still knows no kind and no format.
//   * `Yields`   — the strong definition replaces it. THE DEFAULT, so a caller
//                  that states nothing folds exactly as before.
//   * `Collides` — the pair is a duplicate definition: recorded as data
//                  (`CrossCuResolution::collisions`), the caller's to refuse.
//   * `Unstated` — the document was asked and does not say: recorded
//                  (`CrossCuResolution::undecided`), the caller's to refuse by
//                  the key's name rather than guess between the two.
enum class WeakBesideStrongAnswer : std::uint8_t { Yields, Collides, Unstated };

// ── WHAT A COMMON IS BESIDE A WEAK DEFINITION OF ITS NAME ─────────────────────
//
// A TENTATIVE definition (`CrossCuDef::tentative`) is a COMMON. Beside a weak
// definition of its name the formats split — ELF's common outranks every weak
// definition, Mach-O's yields to every one, PE's stands between its two kinds —
// so, again, the caller states the document's answer for THIS weak definition
// (`linker::commonBesideAWeakDefinition`, link/common_yield.hpp, the one reader
// of `commonYieldsTo`) and the kernel compares nothing it does not understand.
//   * `Yields`   — the weak definition replaces the common;
//   * `Outranks` — the common is the name's definition and the weak one is
//                  ignored;
//   * `Unstated` — THE DEFAULT, deliberately: a caller that hands the kernel a
//                  tentative definition and never said what a weak one is beside
//                  it gets the pair back as `undecided`, not a guess.
enum class CommonBesideWeakAnswer : std::uint8_t { Unstated, Yields, Outranks };

// One externally-visible DEFINITION the resolver ranks. `name` is the cross-CU match
// key (the raw declared identifier); `binding` is Global (strong) or Weak; `key` is the
// definition's compound `(cuId, SymbolId)`. A `Local` binding may be passed — it is
// EXCLUDED (module-private), so callers need not pre-filter. An empty `name` is skipped
// (defensive; producers guard against it upstream).
struct CrossCuDef {
    std::string     name;
    SymbolBinding   binding = SymbolBinding::Global;
    LinkedSymbolKey key{};
    // ── WHAT THIS WEAK DEFINITION PROMISES ABOUT ITS DUPLICATES ───────────
    //    D-LK-COFF-COMDAT-SAME-SIZE-EXACT-MATCH-UNCHECKED
    //
    // Two of COFF's COMDAT selections carry a duty the linker is SPECIFIED to
    // discharge: SAME_SIZE (3) requires every definition of the name to have
    // the same byte length, EXACT_MATCH (4) requires them to be byte-identical,
    // and either way a violation is a multiply-defined-symbol ERROR. Before
    // this, all three of ANY / SAME_SIZE / EXACT_MATCH were folded lowest-key
    // with no comparison at all, so DSS silently accepted exactly the input the
    // format tells it to reject.
    //
    // ★ THE VERIFICATION LIVES HERE, WITH THE POLICY, RATHER THAN IN THE
    // LINKER. This kernel is already the single source of truth for "which
    // definition wins"; "…and under what conditions the losers were allowed to
    // exist" is the same question's other half, and splitting them would leave
    // the MIR merge folding weak duplicates by a rule the linker had privately
    // strengthened. It stays PURE and diagnostic-free: a violation is RECORDED
    // as data (`duplicateMismatches`) exactly as a two-strong collision is, and
    // the caller owns the wording.
    //
    // ⚠ `body` IS A NON-OWNING VIEW and the caller guarantees it outlives the
    // call. That is the one concession this struct makes to the tier above it,
    // and it is deliberately a `span<const byte>` rather than an
    // `AssembledFunction` or an `AssembledData`: the kernel still depends on no
    // module type, no target, no object format, and no source language.
    //
    // ★★ `bodySize` IS SEPARATE FROM `body.size()` AND THAT IS NOT REDUNDANCY —
    // it is the ZERO-FILL case, and getting it wrong is a silent hole rather
    // than a loud one. A `.bss` definition occupies `reservedSize` bytes in its
    // section and stores NONE of them (`AssembledData::sizeInSection` is the
    // producer-side statement of exactly this). Comparing `body.size()` would
    // compare 0 against 0 for two zero-fill definitions of DIFFERENT declared
    // sizes and report that a SAME_SIZE promise was kept. So:
    //   * `bodySize` is the size the definition OCCUPIES — always meaningful;
    //   * `body` is the STORAGE, empty for a zero-fill definition, and
    //     otherwise `body.size() == bodySize`.
    // A zero-fill definition's content is implicitly all-zero, which is what
    // lets EXACT_MATCH compare a zero-fill against a file-backed run of zeros
    // and get the right answer rather than refusing to look.
    //
    // Both default to "nothing declared", which is why `Any` (the default duty)
    // skips the comparison entirely and the pre-existing behaviour is preserved
    // bit for bit for every producer that sets neither.
    DuplicateMatch                 duplicateMatch = DuplicateMatch::Any;
    std::size_t                    bodySize = 0;
    std::span<std::uint8_t const>  body{};
    // ── WHERE THIS WEAK DEFINITION STANDS AMONG THE WEAK DEFINITIONS OF ITS NAME ──
    //    D-LK-WEAK-EXTERNAL-BODY-OUTRANKED-A-SELECT-ANY-DEFINITION-BY-LINK-ORDER
    //
    // Read only when `binding == Weak`. A weak definition of a HIGHER rank
    // replaces one of a lower rank whatever their keys and whatever the order
    // they arrive in; weak definitions of ONE rank fold as weak definitions
    // always did (the promise checked, the lowest key kept). A definition
    // outranked by another weak definition of its name is not one of the copies
    // the fold compares, so nothing is asked of it.
    //
    // ★ THE CALLER STATES THE RANK; THIS KERNEL COMPARES NUMBERS. Whether a
    // format ranks its weak definitions at all, and which kind stands where, is
    // the link document's fact, read by `linker::weakDefinitionRank`
    // (`link/common_yield.hpp`). Nothing here knows a kind or a format, which is
    // what lets the cross-unit MIR merge fold by the same rule. The default is
    // the rank of a definition nobody ranked, so a caller that states nothing
    // folds exactly as before.
    std::uint8_t                   weakRank = kUnrankedWeakDefinition;
    // Read only when `binding == Weak`: what this definition is beside a STRONG
    // one of its name, and what a COMMON is beside it — each the caller's
    // reading of the link's document (see the two enums above).
    WeakBesideStrongAnswer         besideStrong = WeakBesideStrongAnswer::Yields;
    CommonBesideWeakAnswer         commonBesideIt = CommonBesideWeakAnswer::Unstated;
    // ── A TENTATIVE DEFINITION — A COMMON ─────────────────────────────────────
    //    D-C-TENTATIVE-DEFINITIONS-OF-ONE-NAME-IN-TWO-UNITS-REFUSED
    //
    // Read only when `binding == Global`: this definition is one its unit never
    // completed (C 6.9.2p2 — `int x;` with no initializer anywhere in the
    // unit). It DEFINES the name, and it is not a strong definition:
    //   * a STRONG definition of the name replaces it, in silence — the
    //     initialized definition is the object (✔MEASURED 2026-10-12, 16 of 16
    //     PE cells and 8 of 8 ELF cells, both orders);
    //   * TWO of them are ONE object, the larger (`bodySize`, which a caller
    //     passing a tentative definition states), the lowest key on a tie
    //     (✔MEASURED the same day, 16 of 16 and 8 of 8) — until this field the
    //     kernel had only "two strong definitions", and DSS refused the program
    //     cl links;
    //   * beside a WEAK definition, that definition's `commonBesideIt` decides.
    // A weak tentative definition (`__attribute__((weak)) int t;`) is a WEAK
    // definition — its kind decides everything — so the flag is not read there.
    //
    // ⓘ A thread-local tentative definition is NOT one of these and its caller
    // passes false: no reference writes it as a common (✔MEASURED 2026-10-12:
    // cl and clang put it in `.tls$`, gcc and clang on ELF in `.tbss`, MinGW gcc
    // in its emulated-TLS control object) and two units holding one are refused
    // by every linker measured, as two strong definitions are.
    bool                           tentative = false;
};

// One two-strong collision event. `name` is the colliding cross-CU name; `existing` is
// the winner-so-far's key at the moment the duplicate was seen, and `incoming` is the
// duplicate strong definition's key — exactly the pair the caller names in the
// `K_SymbolRedefinedAcrossUnits` diagnostic ("CU #existing and CU #incoming"). Recording
// the PAIR (not just the name) is what lets the linker reproduce the both-CUs-named
// wording and what the Cycle-25 MIR merge consumes to fold the colliding definitions.
struct CrossCuConflict {
    std::string     name;
    LinkedSymbolKey existing{};
    LinkedSymbolKey incoming{};
};

// One BROKEN DUPLICATE-MATCH PROMISE — the anchor, whole on its own line:
//   D-LK-COFF-COMDAT-SAME-SIZE-EXACT-MATCH-UNCHECKED
// Recorded when two definitions of `name` are folded and the
// governing duty (`required`, the STRICTER of the pair's two — see
// `stricterDuplicateMatch`) is not satisfied by their bodies. `existing` /
// `incoming` are the same winner-so-far / duplicate pair `CrossCuConflict`
// carries, and `existingSize` / `incomingSize` are their body lengths, so the
// caller can name both members AND state the numbers without re-deriving them.
//
// ⚠ `required` is reported rather than inferred by the caller BECAUSE THE
// DUTY MAY COME FROM EITHER MEMBER. A definition that promises nothing folded
// against one that promises EXACT_MATCH is governed by EXACT_MATCH, so a
// diagnostic that read the duty off `incoming` alone would name the wrong
// selection roughly half the time.
struct CrossCuDuplicateMismatch {
    std::string     name;
    LinkedSymbolKey existing{};
    LinkedSymbolKey incoming{};
    DuplicateMatch  required = DuplicateMatch::Any;
    std::size_t     existingSize = 0;
    std::size_t     incomingSize = 0;
};

// A weak definition beside a strong definition of its name where the link's
// document says the two COLLIDE (`WeakBesideStrongAnswer::Collides`). `strong`
// is the name's winning strong definition, `weak` the definition that may not
// stand beside it — the pair the caller names.
struct CrossCuCollision {
    std::string     name;
    LinkedSymbolKey strong{};
    LinkedSymbolKey weak{};
};

// A pair the link's document was asked about and did not decide. `question`
// says which fact is missing, so the caller can name the key that states it;
// `definition` is the strong definition or the common, `weak` the weak
// definition beside it.
enum class CrossCuQuestion : std::uint8_t {
    WeakBesideStrong,   // is a strong definition beside this weak one a replacement or a duplicate?
    CommonBesideWeak,   // does a common yield to this weak definition, or outrank it?
};
struct CrossCuUndecided {
    std::string     name;
    LinkedSymbolKey definition{};
    LinkedSymbolKey weak{};
    CrossCuQuestion question = CrossCuQuestion::WeakBesideStrong;
};

// The resolution outcome. `winners[name]` is the winning definition's compound key for
// every externally-visible name (a strong def shadows weak; among all-weak the
// lexicographically-lowest `(cuId, SymbolId)` wins; among multiple strongs the lowest
// key is recorded so `winners` stays order-independent even for an ambiguous name).
// `conflicts` lists one entry PER two-strong collision event — K strong definitions of
// one name yield K-1 entries (mirrors the former per-pair diagnostic count exactly, so
// the caller's diagnostic count is unchanged) — each carrying the colliding key pair.
struct CrossCuResolution {
    std::unordered_map<std::string, LinkedSymbolKey> winners;
    std::vector<CrossCuConflict>                     conflicts;
    // Every folded weak pair whose duplicate-match promise was BROKEN. Empty
    // for every link in which no producer declared a duty (the overwhelmingly
    // common case, and the pre-existing behaviour bit for bit).
    std::vector<CrossCuDuplicateMismatch>            duplicateMismatches;
    // Every weak definition that stands beside a STRONG definition of its name
    // and stated `Collides` — a duplicate definition, the caller's to refuse.
    // One entry per such weak definition, in the order the definitions were
    // handed in; `strong` is the name's winner.
    std::vector<CrossCuCollision>                    collisions;
    // Every pair the kernel could not decide because the caller's document did
    // not say: a weak definition beside a strong one (`besideStrong` Unstated),
    // or the name's weak winner beside a common (`commonBesideIt` Unstated).
    // The winner recorded for such a name is the strong definition, or the
    // common — a module to report against, never a verdict.
    std::vector<CrossCuUndecided>                    undecided;
};

// Resolve the winning definition per name. ORDER-INDEPENDENT: permuting `defs` yields
// the same `winners`; the `conflicts` multiset is likewise stable as a SET of
// {name, {existing, incoming}} pairs (the recorded existing/incoming may swap roles
// under a permutation, but the unordered pair of colliding keys is preserved). Pure —
// no side effects.
[[nodiscard]] DSS_EXPORT CrossCuResolution
resolveCrossCuDefs(std::span<CrossCuDef const> defs);

} // namespace dss::linker
