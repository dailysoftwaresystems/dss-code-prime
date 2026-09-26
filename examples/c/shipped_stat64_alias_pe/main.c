/*
 * `struct stat64`, `stat64()` and `fstat64()` on pe -- the POSIX spellings of the
 * UCRT's 64-bit status record and entry points, which a Windows reference accepts:
 *
 *   mingw-w64 `_mingw_stat64.h`   #define stat64   _stat64  (for POSIX)
 *                                 #define fstat64  _fstat64 (for POSIX)
 *
 * The UCRT's own headers spell neither name, so MSVC refuses this program while
 * mingw-w64 gcc builds and runs it -- and under the disjunction one working
 * reference makes it required. DSS models the reference's own mechanism: two
 * `when:{format:pe}` rows in sys/stat.json's `macros`, beside the `__stat64` row
 * that aliases the same record, so `struct stat64` IS `struct _stat64` and the two
 * calls ARE `_stat64` and `_fstat64`.
 *
 * ★ WHAT EACH CELL DISCRIMINATES. A missing alias is a REFUSED compile (an
 * incomplete `struct stat64`, an undeclared `stat64`), so deleting either row makes
 * this example fail to BUILD. The cells aim at the other failure available here, an
 * alias that is present but WRONG:
 *   - `oneRecordNotTwo`: a pointer crosses between the two spellings in BOTH
 *     directions with no cast, which two look-alike records would refuse;
 *   - `statSeesTheDirectory`: `stat64(".")` fills the record the UCRT fills, so
 *     the mode reads as a directory;
 *   - `fstatSeesTheWrittenSize`: `fstat64` on a stream this program wrote three
 *     bytes to reads `st_size == 3` and a regular-file mode -- a record laid out
 *     differently from `_fstat64`'s would read the size from the wrong offset.
 * Three cells, each 1; one wrong cell moves the exit code off 42 by 7.
 */
#include <stdio.h>
#include <sys/stat.h>

static int oneRecordNotTwo(void) {
    struct stat64 viaPosix;
    struct _stat64 viaUcrt;
    struct _stat64 *ucrtPointsAtPosix = &viaPosix;
    struct stat64 *posixPointsAtUcrt = &viaUcrt;
    return ucrtPointsAtPosix != 0 && posixPointsAtUcrt != 0
           && sizeof(struct stat64) == sizeof(struct _stat64);
}

static int statSeesTheDirectory(void) {
    struct stat64 st;
    return stat64(".", &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static int fstatSeesTheWrittenSize(void) {
    struct stat64 st;
    int seen;
    FILE *f = fopen("shipped_stat64_alias_pe.tmp", "wb");
    if (f == NULL) return 0;
    seen = fwrite("abc", 1, 3, f) == 3 && fflush(f) == 0
           && fstat64(_fileno(f), &st) == 0
           && st.st_size == 3 && (st.st_mode & S_IFMT) == S_IFREG;
    if (fclose(f) != 0) seen = 0;
    if (remove("shipped_stat64_alias_pe.tmp") != 0) seen = 0;
    return seen;
}

int main(void) {
    int cells = oneRecordNotTwo() + statSeesTheDirectory() + fstatSeesTheWrittenSize();
    return 42 + (cells - 3) * 7;
}
