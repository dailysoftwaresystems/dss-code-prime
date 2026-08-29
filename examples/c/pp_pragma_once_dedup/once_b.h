// FORM 2 — a `#pragma once` header that ITSELF includes another one, so
// `once_a.h` is reached through TWO different paths (directly from main.c and
// transitively through here). This is the ordinary real-world shape: the second
// arrival is a SIBLING, never a cycle, which is exactly the case the
// pre-existing include-stack guard could not catch.
#pragma once

#include "once_a.h"

#define B_VALUE 11

int once_b_value(void) { return B_VALUE; }
