/* GUARD FORM 3 — a COMPOUND condition of MIXED polarity. MEASURED in the sqlite
   corpus itself: `ext/misc/windirent.h` is
       #if defined(_WIN32) && defined(_MSC_VER) && !defined(SQLITE_WINDIRENT_H)
   and `ext/session/sqlite3session.h` is
       #if !defined(__SQLITESESSION_H_) && defined(SQLITE_ENABLE_SESSION)
   A detector that pattern-matches "the condition is a negation" fails both. */
#if defined(REENTRY_GATE) && !defined(REENTRY_C_H)
#define REENTRY_C_H

#define C_VALUE 9

#include "reentry_d.h"

#endif /* REENTRY_C_H */
