/* c51 (D-FFI-SYSTIME-UTIMES): ship utimes from <sys/time.h> — sqlite os_unix
 * `utimes(zLockFile, NULL)` (sqlite3.c:42959, the active #else branch of
 * #ifdef HAVE_UTIME) was missing -> S0001 undeclared. sys/time.json had only
 * `structs` (struct timeval); c51 adds its first `symbols` array + a `library`
 * map. utimes: fn(ptr<char>, ptr<void>) -> i32 (the 2nd arg is `const struct
 * timeval[2]*`, here NULL = "set to now"). Gated [elf,macho] (a pe build uses
 * os_win). RED-ON-DISABLE: remove the symbol -> S0001. Value-correct: utimes on
 * a NONEXISTENT path returns -1 (ENOENT) without touching the filesystem. */
#include <sys/time.h>

int main(void) {
    int rc = utimes("nonexistent_file_c51_probe_xyz", 0);   /* no such file -> -1 */
    return (rc == -1) ? 42 : 1;
}
