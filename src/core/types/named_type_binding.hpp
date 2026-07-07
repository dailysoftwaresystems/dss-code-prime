#pragma once

#include "core/types/strong_ids.hpp"

#include <string_view>

// c82 (D-FFI-DESCRIPTOR-VA-LIST-TYPE): one caller-supplied NAME → TypeId
// binding for the hir-text TYPE decoder (`parseTypeFromText`). A span of
// these lets a caller resolve a bare identifier that is not a builtin type
// keyword — e.g. the semantic tier binds C's per-calling-convention
// `va_list` so a shipped-library descriptor can spell an ABI-defined alias
// (`vfprintf(..., va_list)`) arch-neutrally and land the exact TypeId a
// user-written prototype gets.
//
// Lives in `core/types` (alongside `TypeId`) because three tiers consume it
// — the hir-text decoder (lookup), the ffi descriptor reader (thread-
// through), and the semantic analyzer (supplier) — the `extern_import.hpp`
// hoist precedent. Content-blind: nothing here knows what a name means.
//
// The `name` view borrows the caller's storage, which must outlive the
// parse call it is passed to (all shipped callers pass string literals or
// locals that span the call).

namespace dss {

struct NamedTypeBinding {
    std::string_view name;
    TypeId           type;
};

} // namespace dss
