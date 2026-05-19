#pragma once

#include "core/export.hpp"
#include "core/types/interner.hpp"
#include "core/types/strong_ids.hpp"

namespace dss {

// Interns grammar rule names from the language config ("functionDecl",
// "ifStmt", ...) into stable RuleId values. Slot 0 is the InvalidRule
// sentinel; GrammarSchema::load*() calls freeze() once schema build is
// complete. Implementation lives in interner.hpp.
using RuleInterner = Interner<RuleId>;

} // namespace dss
