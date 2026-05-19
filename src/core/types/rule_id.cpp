// Intentionally empty.
// RuleInterner is now `using RuleInterner = Interner<RuleId>;` in rule_id.hpp;
// the template body lives in interner.hpp. This translation unit is preserved
// (rather than deleted) so that no caller has to update an `#include` path,
// and so an explicit instantiation could land here if profiling later shows
// the header-only template inflates compile time.
