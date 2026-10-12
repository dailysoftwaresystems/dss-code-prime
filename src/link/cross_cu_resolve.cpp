#include "link/cross_cu_resolve.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dss::linker {

namespace {

// Lexicographic order on the compound key: (cuId, SymbolId), cuId major. The
// all-weak tie-break + the order-independent strong-among-strongs winner both use it.
// (Same comparator the former in-linker loop used; kept local — it is pure + trivial.)
[[nodiscard]] bool lessKey(LinkedSymbolKey a, LinkedSymbolKey b) noexcept {
    return (a.cuId.v != b.cuId.v) ? (a.cuId.v < b.cuId.v)
                                  : (a.symbol.v < b.symbol.v);
}

} // namespace

CrossCuResolution resolveCrossCuDefs(std::span<CrossCuDef const> defs) {
    // The running winner per name. `binding` is retained so the next same-name def can
    // apply weak-vs-strong against the CURRENT winner, not just its key.
    // `duplicateMatch` + `body` ride along for the same reason: the promise a
    // weak duplicate has to keep is checked against the CURRENT winner
    // (D-LK-COFF-COMDAT-SAME-SIZE-EXACT-MATCH-UNCHECKED).
    struct Winner {
        LinkedSymbolKey               key;
        SymbolBinding                 binding;
        DuplicateMatch                duplicateMatch;
        std::size_t                   bodySize;
        std::span<std::uint8_t const> body;
        CommonBesideWeakAnswer        commonBesideIt;
    };
    auto winnerOf = [](CrossCuDef const& d) {
        return Winner{d.key, d.binding, d.duplicateMatch, d.bodySize, d.body,
                      d.commonBesideIt};
    };
    // ── THREE CLASSES OF DEFINITION PER NAME, EACH WITH ITS OWN RUNNING WINNER ──
    //
    // P69 fold 3. The table held ONE running winner per name and decided each
    // new definition against it, which is enough for two classes (strong, weak)
    // and not for three: a tentative definition — a COMMON — is neither, and
    // what it is beside a weak definition is not known until the name's weak
    // winner is. So each class folds among its own, in the order the
    // definitions arrive and by the rule it always had, and the classes are set
    // against each other ONCE, after the walk:
    //   strong  — the lowest key; every further strong definition is a conflict;
    //   weak    — the top rank only; the duplicate-match promise checked, the
    //             lowest key kept; a weak definition that arrives after a strong
    //             one is shadowed and not folded (as before);
    //   common  — the LARGEST (`bodySize`), the lowest key on a tie.
    struct NameState {
        std::optional<Winner> strong;
        std::optional<Winner> weak;
        std::optional<Winner> common;
        bool                  settled = false;
    };
    std::unordered_map<std::string, NameState> table;
    CrossCuResolution out;

    // ── THE DUPLICATE-MATCH PROMISE ────────────────────────────────────────
    //    D-LK-COFF-COMDAT-SAME-SIZE-EXACT-MATCH-UNCHECKED
    //
    // Called for every WEAK-vs-WEAK pair the fold puts in contact, BEFORE the
    // tie-break decides which of them survives — the duty is a property of the
    // pair, not of the winner, so checking it after the swap would let the
    // check depend on the input order the tie-break exists to eliminate.
    //
    // ★ THE GOVERNING DUTY IS THE STRICTER OF THE TWO. A definition promising
    // nothing does not dilute a sibling that promised EXACT_MATCH; see
    // `stricterDuplicateMatch`.
    //
    // ⚠ `Any` RETURNS EARLY AND THAT IS LOAD-BEARING FOR MORE THAN SPEED: the
    // overwhelming majority of weak definitions in this tree are DSS's own,
    // they carry no body span at all, and comparing two empty spans would
    // "pass" for a reason that has nothing to do with the bodies. The early
    // return keeps the pre-existing behaviour bit-for-bit rather than
    // accidentally asserting something about modules that declared nothing.

    // Byte `i` of a definition. ★ A ZERO-FILL DEFINITION STORES NOTHING AND ITS
    // CONTENT IS ALL ZERO -- `body` is empty while `bodySize` states the extent.
    // Reading through this instead of indexing `body` directly is what lets
    // EXACT_MATCH compare a `.bss` COMDAT against a `.data` run of zeros and
    // answer correctly, rather than crashing on an empty span or silently
    // declaring every zero-fill pair identical.
    //
    // ⚠ THE BOUND IS ON `body.size()`, NOT ON `bodySize`, AND THAT IS NOT
    // BELT-AND-BRACES. The loop below walks `[0, bodySize)`; `body` is either
    // empty (zero-fill) or exactly `bodySize` long BY THE CALLER'S INVARIANT —
    // and an invariant supplied by a producer is precisely the thing this
    // kernel cannot check up front, because it takes a span and a number from
    // an arbitrary caller. A producer that ever hands over a SHORT span turns
    // this into an out-of-bounds READ inside a pure function, which is the
    // failure class that corrupts the heap and reddens something unrelated
    // several tests later rather than failing here. Reading past the storage as
    // an implicit zero is the same answer the zero-fill arm already gives, so
    // the guard costs one comparison and removes the class.
    auto byteAt = [](std::span<std::uint8_t const> body,
                     std::size_t i) -> std::uint8_t {
        return i < body.size() ? body[i] : std::uint8_t{0};
    };

    auto checkDuplicatePromise = [&](Winner const& cur, CrossCuDef const& d) {
        DuplicateMatch const required =
            stricterDuplicateMatch(cur.duplicateMatch, d.duplicateMatch);
        if (required == DuplicateMatch::Any) return;
        bool const sizesAgree = cur.bodySize == d.bodySize;
        bool       ok         = sizesAgree;
        if (ok && required == DuplicateMatch::ExactContent) {
            for (std::size_t i = 0; i < cur.bodySize; ++i) {
                if (byteAt(cur.body, i) != byteAt(d.body, i)) { ok = false; break; }
            }
        }
        if (ok) return;
        out.duplicateMismatches.push_back(
            CrossCuDuplicateMismatch{d.name, cur.key, d.key, required,
                                     cur.bodySize, d.bodySize});
    };

    // ── THE HIGHEST RANK AMONG THE WEAK DEFINITIONS OF EACH NAME ──────────────
    //    D-LK-WEAK-EXTERNAL-BODY-OUTRANKED-A-SELECT-ANY-DEFINITION-BY-LINK-ORDER
    //
    // Found BEFORE the fold, so that the fold never depends on the order the
    // definitions arrive in: a weak definition below its name's highest rank is
    // skipped outright. It can never be the winner — a weak definition of the
    // higher rank is in the link — and it is not one of the copies whose
    // duplicate-match promise the fold checks: had it been folded until the
    // higher rank arrived, two outranked definitions would have been compared
    // in one order of the link and not in another.
    std::unordered_map<std::string_view, std::uint8_t> topWeakRank;
    for (auto const& d : defs) {
        if (d.binding != SymbolBinding::Weak || d.name.empty()) continue;
        auto const [it, fresh] = topWeakRank.try_emplace(d.name, d.weakRank);
        if (!fresh && d.weakRank > it->second) it->second = d.weakRank;
    }

    // The weak definitions that stated something other than "a strong
    // definition replaces me" — by index, in arrival order, so what is recorded
    // of them below is in the order the caller handed them in and not in a hash
    // table's.
    std::vector<std::size_t> answersOwedBesideStrong;

    for (std::size_t i = 0; i < defs.size(); ++i) {
        CrossCuDef const& d = defs[i];
        if (d.binding == SymbolBinding::Local) continue;  // module-private — excluded
        if (d.name.empty()) continue;                     // producer-guarded; defensive
        NameState& state = table.try_emplace(d.name).first->second;
        if (d.binding == SymbolBinding::Weak) {
            // Asked of EVERY weak definition, outranked or not: whether it may
            // stand beside a strong definition does not depend on which other
            // weak definitions the link holds.
            if (d.besideStrong != WeakBesideStrongAnswer::Yields) {
                answersOwedBesideStrong.push_back(i);
            }
            if (d.weakRank < topWeakRank.at(d.name)) continue;   // outranked by another weak definition
            if (state.strong.has_value()) continue;              // the strong definition shadows it
            if (!state.weak.has_value()) { state.weak = winnerOf(d); continue; }
            // both weak — lowest key wins deterministically
            checkDuplicatePromise(*state.weak, d);
            if (lessKey(d.key, state.weak->key)) state.weak = winnerOf(d);
            continue;
        }
        if (d.tentative) {
            // A COMMON: the larger extent is the object, the lowest key on a tie.
            if (!state.common.has_value()
                || d.bodySize > state.common->bodySize
                || (d.bodySize == state.common->bodySize
                    && lessKey(d.key, state.common->key))) {
                state.common = winnerOf(d);
            }
            continue;
        }
        if (!state.strong.has_value()) {
            // ⚠ NO PROMISE CHECK AGAINST THE WEAK DEFINITIONS, AND IT IS NOT AN
            // OMISSION. A COMDAT selection describes how the copies of a WEAK
            // definition relate to EACH OTHER; a strong definition is not one of
            // those copies, and the format asks nothing of it. What a strong
            // definition IS beside a weak one — a replacement or a duplicate —
            // is each weak definition's own statement (`besideStrong`), settled
            // after the walk.
            state.strong = winnerOf(d);
            continue;
        }
        // Two strong definitions of one name — ambiguous. Record the conflict (one
        // entry per collision event: K strongs → K-1 entries, matching the former
        // per-pair diagnostic count) carrying the colliding key PAIR: `existing` is
        // the winner-so-far's key at THIS moment, `incoming` is the duplicate — the
        // exact pair the linker names ("CU #existing and CU #incoming"). Capture
        // before the swap below. Then keep the lowest key as the winner so `winners`
        // is order-independent even across the conflicting strongs.
        out.conflicts.push_back(CrossCuConflict{d.name, state.strong->key, d.key});
        if (lessKey(d.key, state.strong->key)) state.strong = winnerOf(d);
    }

    // ── A WEAK DEFINITION BESIDE A STRONG ONE: A REPLACEMENT, OR A DUPLICATE ───
    for (std::size_t const i : answersOwedBesideStrong) {
        CrossCuDef const& d     = defs[i];
        NameState const&  state = table.at(d.name);
        if (!state.strong.has_value()) continue;   // no strong definition: nothing was asked
        if (d.besideStrong == WeakBesideStrongAnswer::Collides) {
            out.collisions.push_back(CrossCuCollision{d.name, state.strong->key, d.key});
        } else {
            out.undecided.push_back(CrossCuUndecided{
                d.name, state.strong->key, d.key, CrossCuQuestion::WeakBesideStrong});
        }
    }

    // ── THE CLASSES SET AGAINST EACH OTHER, ONCE PER NAME ─────────────────────
    // Walked in arrival order (and each name settled at its first definition) so
    // that `undecided` is a function of the input's order, as `conflicts` is.
    out.winners.reserve(table.size());
    for (auto const& d : defs) {
        if (d.binding == SymbolBinding::Local || d.name.empty()) continue;
        NameState& state = table.at(d.name);
        if (state.settled) continue;
        state.settled = true;
        if (state.strong.has_value()) {            // strong shadows weak and common alike
            out.winners.emplace(d.name, state.strong->key);
            continue;
        }
        if (!state.common.has_value()) {           // all weak
            out.winners.emplace(d.name, state.weak->key);
            continue;
        }
        if (!state.weak.has_value()) {             // all tentative: one object
            out.winners.emplace(d.name, state.common->key);
            continue;
        }
        // A common beside the name's weak winner: that definition's statement.
        switch (state.weak->commonBesideIt) {
            case CommonBesideWeakAnswer::Yields:
                out.winners.emplace(d.name, state.weak->key);
                break;
            case CommonBesideWeakAnswer::Outranks:
                out.winners.emplace(d.name, state.common->key);
                break;
            case CommonBesideWeakAnswer::Unstated:
                out.undecided.push_back(CrossCuUndecided{
                    d.name, state.common->key, state.weak->key,
                    CrossCuQuestion::CommonBesideWeak});
                out.winners.emplace(d.name, state.common->key);
                break;
        }
    }
    return out;
}

} // namespace dss::linker
