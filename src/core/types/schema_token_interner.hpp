#pragma once

#include "core/export.hpp"
#include "core/types/interner.hpp"
#include "core/types/strong_ids.hpp"

namespace dss {

// Interns schema-resolved token meaning names ("SumOperator",
// "GenericDefinitionOpener", "Whitespace", ...) declared by the language
// config. Distinct namespace from RuleId — the schema can define a
// "block" rule AND a "BlockOpen" token; they get different ids in
// different interners.
using SchemaTokenInterner = Interner<SchemaTokenId>;

} // namespace dss
