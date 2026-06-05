#include "link/cross_cu_resolve.hpp"

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
    struct Winner { LinkedSymbolKey key; SymbolBinding binding; };
    std::unordered_map<std::string, Winner> table;
    CrossCuResolution out;

    for (auto const& d : defs) {
        if (d.binding == SymbolBinding::Local) continue;  // module-private — excluded
        if (d.name.empty()) continue;                     // producer-guarded; defensive
        auto [it, inserted] = table.try_emplace(d.name, Winner{d.key, d.binding});
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
            if (lessKey(d.key, cur.key)) cur = Winner{d.key, d.binding};
        } else if (newStrong) {        // strong shadows the existing weak
            cur = Winner{d.key, d.binding};
        } else if (!curStrong) {       // both weak — lowest key wins deterministically
            if (lessKey(d.key, cur.key)) cur = Winner{d.key, d.binding};
        }                              // else: existing strong shadows the new weak
    }

    out.winners.reserve(table.size());
    for (auto const& [name, w] : table) out.winners.emplace(name, w.key);
    return out;
}

} // namespace dss::linker
