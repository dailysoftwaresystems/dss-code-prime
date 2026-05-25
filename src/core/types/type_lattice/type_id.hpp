#pragma once

#include "core/substrate/arena_tag.hpp"
#include "core/types/strong_ids.hpp"

// The CU-scoped type lattice stores its TypeRecords in a
// substrate::ArenaContainer/ArenaBuilder whose arena tag is the owning
// CompilationUnitId — so every TypeId carries its owning-CU provenance and a
// TypeId from one CU used against another CU's lattice aborts via the
// cross-arena guard (the same SH3 discipline as Tree's NodeId). This
// specialization gives that guard type-system-flavored wording.

namespace dss::substrate {

template <>
struct ArenaNames<TypeId, CompilationUnitId> {
    static constexpr char const* attribute = "TypeAttribute";
    static constexpr char const* element   = "TypeId";
    static constexpr char const* tag       = "CompilationUnitId";
    static constexpr char const* access    = "TypeInterner::get";
};

} // namespace dss::substrate
