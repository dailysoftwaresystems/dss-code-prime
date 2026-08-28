/* GUARD FORM 1 — the canonical `#ifndef X` / `#define X`.
   MEASURED the shape of 2942 of 3100 macOS SDK headers and 35 of 52 sqlite
   headers, and the shape of `mach/mach_types.h`, whose re-entry cost the macho
   corpus leg 4 F001A before TF-C87. */
#ifndef REENTRY_A_H
#define REENTRY_A_H

#define A_VALUE 10
/* Gates FORM 3's compound guard below — if this does not reach it, that
   header's whole body is skipped and C_VALUE never exists. */
#define REENTRY_GATE 1

#include "reentry_b.h"

#endif /* REENTRY_A_H */
