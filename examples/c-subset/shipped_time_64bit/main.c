/* Runtime witness for shipped <time.h>'s 64-BIT time_t path on EVERY hosted
 * format — including pe, which the pre-existing `shipped_time` example cannot
 * cover because it uses localtime_r (a POSIX name gated [elf,macho]).
 *
 * WHY THIS EXISTS: the pe C runtime migration (D-FFI-PE-CRT-UCRT-MIGRATION
 * Phase 2) rebinds time.json from msvcrt.dll to ucrtbase.dll, where the bare
 * names are NOT exported — `time`/`gmtime`/`mktime` reach UCRT through macros
 * onto `_time64`/`_gmtime64`/`_mktime64`. That mapping is only correct because
 * time_t is declared 64-bit (i64 "long long" on LLP64). If a future edit ever
 * pointed those macros at the 32-bit entry points (`_time32`/`_gmtime32`), or
 * narrowed the time_t typedef, the result would NOT be a link error and NOT a
 * diagnostic — it would be silently wrong dates. This example is the guard.
 *
 * THE DECISIVE ASSERTION is the year-2100 conversion below. A fixed epoch
 * beyond 2^31 seconds cannot survive a 32-bit time_t: it wraps and yields 1901
 * or similar garbage. So a 32-bit regression fails here LOUDLY and specifically,
 * where a "current time looks plausible" check would sail straight past it.
 *
 * Fixed epochs are used throughout (never the wall clock) so every assertion is
 * exact and timezone-independent: gmtime is UTC by definition, so no local-time
 * or DST variation can enter. That keeps the example deterministic on every CI
 * leg and every developer machine.
 *
 * Only the nine struct-tm fields common to all three format variants are read
 * (the elf/macho tm_gmtoff/tm_zone tail is deliberately untouched).
 */
#include <time.h>

/* Mutable globals: keep the epochs out of reach of const-folding so the release
 * arm still performs real dispatched CRT calls (the c99_tgmath precedent). */
time_t g_y2k  = 946684800;    /* 2000-01-01T00:00:00Z — fits in 32 bits      */
time_t g_y2100 = 4102444800;  /* 2100-01-01T00:00:00Z — does NOT fit in 32   */

int main(void) {
    struct tm* t;
    time_t     now;
    char       buf[32];
    size_t     n;

    /* --- 1. a pre-2038 epoch: exact UTC decomposition --- */
    t = gmtime(&g_y2k);
    if (t == 0)            return 60;
    if (t->tm_year != 100) return 61;  /* years since 1900 */
    if (t->tm_mon  != 0)   return 62;  /* January */
    if (t->tm_mday != 1)   return 63;
    if (t->tm_hour != 0)   return 64;
    if (t->tm_min  != 0)   return 65;
    if (t->tm_sec  != 0)   return 66;
    if (t->tm_wday != 6)   return 67;  /* 2000-01-01 was a Saturday */
    if (t->tm_yday != 0)   return 68;

    /* --- 2. THE 64-BIT PROOF: an epoch past 2^31 --- */
    t = gmtime(&g_y2100);
    if (t == 0)            return 70;
    if (t->tm_year != 200) return 71;  /* 2100 — a 32-bit time_t cannot get here */
    if (t->tm_mon  != 0)   return 72;
    if (t->tm_mday != 1)   return 73;
    if (t->tm_hour != 0)   return 74;

    /* --- 3. strftime over that same far-future tm --- */
    n = strftime(buf, sizeof buf, "%Y-%m-%d", t);
    if (n != 10)        return 80;
    if (buf[0] != '2')  return 81;
    if (buf[1] != '1')  return 82;
    if (buf[2] != '0')  return 83;
    if (buf[3] != '0')  return 84;
    if (buf[4] != '-')  return 85;

    /* --- 4. time() returns a sane 64-bit epoch (not 0, not negative, and
     *        past 2020) — the only wall-clock-dependent check, kept loose. --- */
    now = 0;
    time(&now);
    if (now < 1600000000) return 90;   /* 2020-09-13 or later */

    return 42;
}
