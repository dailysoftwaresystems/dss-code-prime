/* c16 — shipped <time.h> witness: the struct tm LAYOUT + the time functions on a
 * real libc (linux + macOS — struct tm is byte-identical 56B on both). The layout
 * is SIZE-CRITICAL: localtime_r writes the FULL 56-byte struct into &t, so a short
 * struct would overflow main's frame. The witness reads tm_year (offset 20) back
 * after localtime_r filled it — proving (a) sizeof is the real 56 (no overflow)
 * and (b) tm_year sits at the offset libc wrote (the field-offset layout is right).
 *
 * RED-ON-DISABLE: a wrong struct tm (drop the tm_gmtoff/tm_zone tail → sizeof 40 or
 * a wrong field order) → sizeof != 56 OR libc writes the year somewhere we don't
 * read → t.tm_year stays 0 → exit 1. Runs on the linux + native-arm64 + macos-latest
 * legs (real libc.so.6 / libSystem). */
#include <time.h>

int main(void) {
    struct tm t;
    time_t now;
    long off_year = (long)&t.tm_year - (long)&t;     /* 20 on glibc + Darwin */
    time(&now);
    localtime_r(&now, &t);                            /* libc fills the full 56B */
    return (sizeof(struct tm) == 56 && off_year == 20
            && t.tm_year >= 120) ? 42 : 1;            /* year 2020+ written @20, read @20 */
}
