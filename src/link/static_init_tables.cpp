#include "link/static_init_tables.hpp"

#include <algorithm>

namespace dss::linker {

std::vector<StaticInitOrderEntry>
staticInitOrder(AssembledModule const& module, StaticInitPhase phase) {
    std::vector<StaticInitOrderEntry> out;
    for (auto const& e : module.staticInitSchedule) {
        auto const prio = e.schedule.priorityFor(phase);
        if (!prio.has_value()) continue;
        out.push_back(StaticInitOrderEntry{e.symbol, *prio});
    }
    // Ascending priority; ties broken by merged SymbolId so the image is
    // DETERMINISTIC. See this file's header for why determinism — not agreement
    // with any one reference — is the contract for equal priorities.
    std::ranges::sort(out, [](StaticInitOrderEntry const& a,
                              StaticInitOrderEntry const& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.symbol.v < b.symbol.v;
    });
    // The after-entry channel runs the same sequence BACKWARD (measured — see the
    // header). Reversing HERE, once, is what keeps every consumer in agreement.
    if (phase == StaticInitPhase::AfterEntry) std::ranges::reverse(out);
    return out;
}

} // namespace dss::linker
