// FC15c (`__has_include` + `__has_c_attribute` -- C23 6.10.1p4) runtime witness.
// The controlling expression of the `#if` combines BOTH operators:
//
//   #if __has_include("pp_has_include_defs.h") && __has_c_attribute(deprecated)
//
//   * __has_include("pp_has_include_defs.h") -> 1: the header exists in the
//     example dir (QUOTE-form self-dir search -- the SAME resolution `#include`
//     uses). The taken branch then `#include`s it (so the exit code depends on
//     the header truly resolving, not merely the probe folding to 1).
//   * __has_c_attribute(deprecated) -> 202311 (truthy): `deprecated` is a known
//     C23 standard attribute in the config's knownCAttributes.
//
// Both are non-zero -> the `#if` is TAKEN -> main returns PP_HAS_INCLUDE_ANSWER
// (42, from the included header). The `#else` returns 1, so a wrong fold of
// EITHER operator changes the exit code (fold-resistant -- the result feeds the
// live conditional-compilation decision AND the included macro value).
//
// RED-ON-DISABLE: with the `hasIncludeOperator` config stripped (or the
// `__has_include` arm reverted) the operator folds to 0 as an ordinary
// identifier -> the `#if` is not taken -> exit 1, not 42. Likewise stripping
// `__has_c_attribute` folds it to 0 -> exit 1.

#if __has_include("pp_has_include_defs.h") && __has_c_attribute(deprecated)
#include "pp_has_include_defs.h"
int main(void) {
    return PP_HAS_INCLUDE_ANSWER;   // 42
}
#else
int main(void) {
    return 1;
}
#endif
