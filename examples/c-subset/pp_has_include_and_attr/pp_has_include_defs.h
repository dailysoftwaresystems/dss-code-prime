// FC15c witness header for examples/c-subset/pp_has_include_and_attr. Its mere
// EXISTENCE is what `__has_include("pp_has_include_defs.h")` tests -- the gate in
// main.c takes its 42 branch only because this file resolves (self-dir search,
// the same resolution `#include "..."` uses). It also defines the answer used
// by the taken branch so the exit code depends on the header actually being
// included, not just probed.
#define PP_HAS_INCLUDE_ANSWER 42
