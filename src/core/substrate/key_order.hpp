#pragma once

#include <algorithm>
#include <vector>

// THE ONE ORDER A HASH TABLE'S ENTRIES ARE WALKED IN WHEN WHAT THE WALK DOES IS SEEN (P69 fold 3f).
//
// An unordered container's iteration order is its C++ library's, and it moves with the bucket count: the
// order differs between the libraries that build DSS (libstdc++, libc++, MSVC's) and between two sizes of
// one table. A walk whose effects are SEEN -- a refusal reported per entry, the first entry a check stops
// at, an entry appended to output -- made what the compiler says a fact of the toolchain that BUILT it
// rather than of its input. Such a walk takes its entries from here, in ascending key order. A map's keys
// are unique, so that order is total and the same on every host.
//
// A walk whose effect does not depend on its order (a keyed insert, a count, any/all) needs nothing.
//
// The entries point INTO `map`, valid until `map` is next modified.

namespace dss::substrate {

template <class Map>
[[nodiscard]] std::vector<typename Map::value_type const*> entriesInKeyOrder(Map const& map) {
    std::vector<typename Map::value_type const*> entries;
    entries.reserve(map.size());
    for (auto const& entry : map) entries.push_back(&entry);
    std::sort(entries.begin(), entries.end(),
              [](auto const* a, auto const* b) { return a->first < b->first; });
    return entries;
}

}  // namespace dss::substrate
