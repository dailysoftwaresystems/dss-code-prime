/* GUARD FORM 2 — `#if !defined(X)`. MEASURED in 10 SDK headers
   (`_inttypes.h`, the `odmodule` family) and sqlite's
   `ext/expert/sqlite3expert.h`. */
#if !defined(REENTRY_B_H)
#define REENTRY_B_H

#define B_VALUE 11

#include "reentry_c.h"

/* ★ BACK EDGE 1 — reentry_a.h is ON THE INCLUDE STACK right now. Legal C: a
   real cpp re-enters it, the guard is already satisfied, and it expands to
   nothing. Before TF-C87 DSS refused this and the program did not compile. */
#include "reentry_a.h"

#endif /* REENTRY_B_H */
