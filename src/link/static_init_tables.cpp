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
    // Ascending priority, and a TIE KEEPS THE SCHEDULE'S ORDER: each unit's
    // definition order, the units in the link's input order (the header says
    // why, and what was decided). A STABLE sort, and no SymbolId read: an
    // unstable sort, or a tie broken by id, makes the order a fact of how the
    // module was numbered -- the host-dependent order this replaced.
    std::stable_sort(out.begin(), out.end(), [](StaticInitOrderEntry const& a,
                                                StaticInitOrderEntry const& b) {
        return a.priority < b.priority;
    });
    // The after-entry channel runs the same sequence BACKWARD (measured — see the
    // header). Reversing HERE, once, is what keeps every consumer in agreement.
    if (phase == StaticInitPhase::AfterEntry) std::ranges::reverse(out);
    return out;
}

} // namespace dss::linker
