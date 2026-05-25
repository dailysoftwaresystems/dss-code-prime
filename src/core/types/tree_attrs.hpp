#pragma once

#include "core/substrate/arena_attribute.hpp"
#include "core/types/tree.hpp"

// `NodeAttribute<T>` — the Tree-bound side-table. Since SP1 it is the
// `ArenaT = Tree` instantiation of the generalized `substrate::ArenaAttribute`
// (HIR/MIR/the CU symbol table instantiate the same template with their own
// arenas). The public API — set/get/has/tryGet/erase, size/empty/isDense,
// clear/reserve, iteration, move-only with reset-on-move, and the cross-tree
// (now cross-arena) guard — is unchanged; the dual sparse↔dense storage and
// promotion heuristics live in arena_attribute.hpp.

namespace dss {

template <class T>
using NodeAttribute = substrate::ArenaAttribute<Tree, T>;

} // namespace dss
