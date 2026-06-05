#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_lattice.hpp"

#include <cstdint>
#include <unordered_map>

// Type re-intern walker (Cycle 25, Stage A) — the foundation for a whole-program
// MIR merge that unifies N per-CU type interners into one host TypeLattice.
//
// Each TypeInterner is CU-scoped: its TypeIds are interner-relative and stamped
// with the owning CompilationUnitId. To merge MIR from several CUs into one
// module, every TypeId that crosses a CU boundary must be re-interned into a
// single destination host lattice so the host's hash-consing can canonicalize
// structurally-identical types from different CUs to one TypeId.
//
// `reinternType` does exactly that: given a TypeId from source interner `src`,
// it returns the equivalent TypeId interned into `dstHost`'s lattice, recursing
// bottom-up (types are referential — a pointer's pointee, a fnSig's
// result/params, a struct's fields are themselves TypeIds that must be
// re-interned first). The `remap` memo guarantees a given source TypeId maps to
// a stable destination TypeId (and breaks the recursion's repeated work);
// structurally-identical types collapse in the host because each host builder
// hash-conses.
//
// AGNOSTIC: the walker keys on `TypeKind` alone — no language / target / format
// branch. FAIL-LOUD: every TypeKind in core_type.hpp is handled explicitly; an
// unhandled / never-interned kind aborts with the kind name rather than silently
// mis-reinterning.

namespace dss {

// Re-intern `srcId` (an interner-relative TypeId from `src`) into `dstHost`,
// returning the host-stamped equivalent TypeId. Recurses on every operand
// TypeId first (bottom-up), then rebuilds via the matching `dstHost` builder
// with the remapped operands plus the same scalars / name / extensionKind.
//
// `remap` is the caller-owned memo keyed by `srcId.v`: a hit returns the stored
// host TypeId; otherwise the result is stored before returning. Re-using one
// `remap` across calls keeps mappings stable AND lets independent source
// TypeIds that turn out structurally identical share a single host TypeId.
//
// An invalid / sentinel `srcId` (`!srcId.valid()`) re-interns to an invalid host
// TypeId (identity — InvalidType is CU-agnostic). Any TypeKind without a known
// interner encoding (FnPtr / Param / Bind — none have a public builder, so none
// can legitimately appear in an interner's arena) aborts loud with the kind
// name: a never-interned kind reaching here is interner corruption, not a type
// to silently pass through.
[[nodiscard]] DSS_EXPORT TypeId reinternType(
    TypeInterner const& src, TypeId srcId, TypeLattice& dstHost,
    std::unordered_map<std::uint32_t, TypeId>& remap);

} // namespace dss
