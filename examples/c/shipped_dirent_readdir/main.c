/* c81 (the sqlite shell.c CLI route): ship <dirent.h> — shell.c's fileio
 * extension holds a `DIR *pDir` struct member, walks opendir/readdir/closedir
 * and reads entry->d_name (the lsdir/fsdir table); the header was missing
 * -> F001A. This proves the whole surface end-to-end on real libc:
 *   - DIR resolves as an OPAQUE type name (typedef to the empty named struct,
 *     the stdio FILE precedent) — `DIR *d` below is shell.c's exact shape;
 *   - struct dirent is glibc-exact (d_ino u64, d_off i64, d_reclen u16,
 *     d_type u8, d_name char[256] @19; verified bits/dirent.h, x86_64 ==
 *     arm64): readdir returns LIBC'S OWN struct, so finding "." by d_name
 *     equality proves the d_name OFFSET — a wrong layout reads garbage and
 *     never matches;
 *   - every directory contains "." (POSIX), so the loop must find it.
 * RED-ON-DISABLE: delete dirent.json -> F001A on the include. */
#include <dirent.h>
#include <string.h>

int main(void) {
    DIR *d = opendir(".");
    struct dirent *e;
    int found_dot = 0;
    if (d == 0) return 1;
    while ((e = readdir(d)) != 0) {
        if (strcmp(e->d_name, ".") == 0) found_dot = 1;
    }
    if (closedir(d) != 0) return 2;
    return found_dot ? 42 : 3;
}
