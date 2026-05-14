#pragma once

#include "core/export.hpp"

// CP2 fills this in: SchemaCursor, ScopeKind, GrammarSchema, loaders, expectedAt, ...
// For now an empty class lets Tree hold a shared_ptr<GrammarSchema const>.

namespace dss {

class DSS_EXPORT GrammarSchema {
    // Intentionally empty in checkpoint 1.
    // Plan §5.12 specifies the full API; implementation lands in checkpoint 2.
};

} // namespace dss
