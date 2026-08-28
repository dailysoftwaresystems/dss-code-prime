/* c81 (the sqlite shell.c CLI route): ship <utime.h> — shell.c's fileio
 * section includes it under !_WIN32 (shell.c:9841) but USES nothing from it
 * (the active branch calls the already-shipped utimes from <sys/time.h>);
 * only the include has to resolve. The descriptor still ships the header's
 * honest minimal real surface (struct utimbuf {actime,modtime} + utime()) —
 * an empty descriptor fails loud by design — and this example exercises it
 * for real: utime() on a nonexistent path must return -1 (ENOENT) without
 * touching the filesystem (the shipped_utimes precedent), with the utimbuf
 * member writes proving the struct resolves + lays out (2 x i64).
 * RED-ON-DISABLE: delete utime.json -> F001A on the include. */
#include <utime.h>

int main(void) {
    struct utimbuf tb;
    tb.actime  = 0;              /* epoch — never applied (path is absent) */
    tb.modtime = 0;
    int rc = utime("nonexistent_file_c81_probe_xyz", &tb);
    return (rc == -1) ? 42 : 1;
}
