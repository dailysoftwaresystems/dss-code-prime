/* GUARD FORM 4 — the guard is NOT the FIRST conditional in the file, and its
   `#define` is NOT the next line. MEASURED: `netinet6/in6.h` opens with
   `#ifndef __KAME_NETINET_IN_H_INCLUDED_` (an umbrella check with no matching
   `#define`) and only then opens its real guard; libc++'s `inttypes.h` and
   `stdint.h` put the guard's `#define` five logical lines down. A detector that
   assumes "first conditional, define on the next line" refuses both. */
#ifndef REENTRY_UMBRELLA_CHECK
/* deliberately defines nothing — this is not the guard */
#endif

#ifndef REENTRY_D_H

/* a banner comment, a blank line and a nested conditional all sit between the
   guard and its `#define` */

#if 1
#endif

#define REENTRY_D_H

#define D_VALUE 12

/* A real DEFINITION, not just a macro: if the guard failed to empty a
   re-entered copy of this header, this would be defined twice and the compile
   would fail on the duplicate rather than quietly producing the right number. */
static int reentry_d_value(void) { return D_VALUE; }

/* ★ BACK EDGE 2 and 3 — both already on the include stack. */
#include "reentry_a.h"
#include "reentry_b.h"

#endif /* REENTRY_D_H */
