// FORM 4 — a header in a SUBDIRECTORY, reached through two different spellings
// (`sub/once_d.h` and `./sub/once_d.h`). Its own `#include` of `once_a.h` uses a
// `../` spelling, so the parent header arrives here under a THIRD spelling that
// must still reduce to the identity already recorded.
#pragma once

#include "../once_a.h"

#define D_VALUE 12

int once_d_value(void) { return D_VALUE; }
