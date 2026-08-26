#include "link/cross_cu_resolve.hpp"

#include <cstddef>
#include <string>
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
    };
    auto winnerOf = [](CrossCuDef const& d) {
        return Winner{d.key, d.binding, d.duplicateMatch, d.bodySize, d.body};
    };
    std::unordered_map<std::string, Winner> table;
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

    for (auto const& d : defs) {
        if (d.binding == SymbolBinding::Local) continue;  // module-private — excluded
        if (d.name.empty()) continue;                     // producer-guarded; defensive
        auto [it, inserted] = table.try_emplace(d.name, winnerOf(d));
        if (inserted) continue;
        Winner& cur = it->second;
        bool const newStrong = (d.binding == SymbolBinding::Global);
        bool const curStrong = (cur.binding == SymbolBinding::Global);
        if (newStrong && curStrong) {
            // Two strong definitions of one name — ambiguous. Record the conflict (one
            // entry per collision event: K strongs → K-1 entries, matching the former
            // per-pair diagnostic count) carrying the colliding key PAIR: `existing` is
            // the winner-so-far's key at THIS moment, `incoming` is the duplicate — the
            // exact pair the linker names ("CU #existing and CU #incoming"). Capture
            // before the swap below. Then keep the lowest key as the winner so `winners`
            // is order-independent even across the conflicting strongs.
            out.conflicts.push_back(CrossCuConflict{d.name, cur.key, d.key});
            if (lessKey(d.key, cur.key)) cur = winnerOf(d);
        } else if (newStrong) {        // strong shadows the existing weak
            // ⚠ NO PROMISE CHECK HERE, AND IT IS NOT AN OMISSION. A COMDAT
            // selection describes how the copies of a WEAK definition relate to
            // EACH OTHER; a strong definition is not one of those copies, it
            // OVERRIDES them all, and the format asks nothing of it. Checking
            // here would refuse the ordinary and legal "a real definition
            // overrides an inline/selectany one" shape.
            cur = winnerOf(d);
        } else if (!curStrong) {       // both weak — lowest key wins deterministically
            checkDuplicatePromise(cur, d);
            if (lessKey(d.key, cur.key)) cur = winnerOf(d);
        }                              // else: existing strong shadows the new weak
    }

    out.winners.reserve(table.size());
    for (auto const& [name, w] : table) out.winners.emplace(name, w.key);
    return out;
}

} // namespace dss::linker
