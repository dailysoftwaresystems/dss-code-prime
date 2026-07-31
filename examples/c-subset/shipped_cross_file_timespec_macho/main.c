/* TF-C97 — one file-scope struct TAG, one type (C 6.2.3), across the descriptor /
 * source boundary: the sqlite `os_unix.c` proxy-locking shape, reduced.
 *
 * sqlite/src/os_unix.c:7711 declares `struct timespec conchModTime;` and :7731
 * assigns `conchModTime = buf.st_mtimespec;` — a BY-VALUE composite assignment out
 * of a shipped `struct stat`. It was the LAST error on the arm64-macho leg:
 *     error[S0003] S_TypeMismatch  got buf.st_mtimespec
 * MEASURED root cause (not the one first inferred): `sys/stat.json` interns its
 * `st_mtimespec` member against the DESCRIPTOR's `timespec` when the descriptor
 * loads, while `struct timespec conchModTime;` resolves the TAG — and in that TU
 * the tag is claimed by a SOURCE declaration (the macOS SDK's
 * sys/_types/_timespec.h, pulled in through sys/fcntl.h, for which DSS ships no
 * descriptor) whose tree-root binding SHADOWS the descriptor's cuRoot one. Two
 * TypeIds for one file-scope tag ⇒ the assignment is rejected. The tag BINDING was
 * never wrong; the MEMBER was, which is why a tag-resolution test is vacuous here
 * and this example asserts the ASSIGNMENT instead.
 * (D-FFI-DESCRIPTOR-CROSS-FILE-TYPE-IDENTITY)
 *
 * The source declaration below stands in for that SDK header: DSS resolves an
 * angle include to a DESCRIPTOR when one exists, so <time.h> and <sys/stat.h> here
 * are descriptors and this file-scope definition is the only SOURCE declaration of
 * the tag — exactly the pairing os_unix.c hits. `long tv_sec; long tv_nsec;` is the
 * xnu spelling verbatim (__darwin_time_t is `long` on LP64), so it interns as
 * {i64 "long", i64 "long"} against the descriptor's bare {i64, i64}: SAME 16-byte
 * layout, DIFFERENT TypeIds — the case the fix must unify, and the case a
 * TypeId-equality comparison alone can never see as equal.
 *
 * NOT a compile-only witness. `conchModTime` is poisoned to -1 first, so exit 42
 * is reachable ONLY if the 16 bytes really moved: the run reads both nested members
 * back, cross-checks them against `st_mtim_sec`/`st_mtim_nsec` (the c107 explicit-
 * offset OVERLAY — the same 16 bytes at offset 48, reached by their flat names), and
 * only then writes through the nested members to compose 40 + 2.
 *
 * RED-ON-DISABLE (MEASURED, this cycle): revert the member adoption in
 * semantic_analyzer.cpp and this file fails to compile with the original
 * error[S0003] on the `conchModTime = sb.st_mtimespec` line. Narrow the source
 * declaration to `int tv_sec; int tv_nsec;` and the layouts stop agreeing, so the
 * unification is REFUSED and error[F_ShippedTypeIdentityConflict] names both sizes.
 *
 * Single arm64-macho target → the macos-latest CI leg (st_mtimespec exists only in
 * the `when:{format:macho}` variant of `struct stat`). Release arm runs the
 * optimizer over the composite copy.
 */

#include <time.h>
#include <sys/stat.h>

/* The SOURCE declaration of the tag — the xnu sys/_types/_timespec.h shape. */
struct timespec {
    long tv_sec;
    long tv_nsec;
};

int main(void) {
    struct stat     sb;
    struct timespec conchModTime;
    long            sec;
    long            nsec;

    /* Poison: -1 is a value `stat` can never write, so a copy that did not
     * happen cannot masquerade as one that did. */
    conchModTime.tv_sec  = -1;
    conchModTime.tv_nsec = -1;

    if (stat("/", &sb) != 0) return 10;

    /* THE ASSIGNMENT this example exists for: a whole `struct timespec` out of a
     * shipped descriptor's struct, into a SOURCE-declared one of the same tag. */
    conchModTime = sb.st_mtimespec;

    /* Read the nested members back. */
    sec  = conchModTime.tv_sec;
    nsec = conchModTime.tv_nsec;
    if (sec <= 0) return 11;                  /* still -1 ⇒ the copy never happened */

    /* The overlay's flat twin: st_mtim_sec/st_mtim_nsec occupy the SAME 16 bytes
     * at offset 48, so a copy that read the WRONG offset disagrees here. */
    if (sec != sb.st_mtim_sec) return 12;
    if (nsec != sb.st_mtim_nsec) return 13;

    /* Write through the nested members, then read them back for the exit code. */
    conchModTime.tv_sec  = 40;
    conchModTime.tv_nsec = 2;
    return (int)(conchModTime.tv_sec + conchModTime.tv_nsec);   /* 42 */
}
