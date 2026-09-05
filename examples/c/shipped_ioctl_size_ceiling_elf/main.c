/* D-FFI-IOCTL-SIZE-FIELD-OVERFLOW-SILENT — the ELF half of the per-format SIZE
 * CEILING now carried inside the shipped <sys/ioctl.h> request-encoding macros.
 *
 * THE DEFECT THIS CLOSES. musl's `_IOC` does not mask the length
 * (`sizeof(t) << 16`, verbatim in the elf arms of `src/dss-config/shippedLibs/
 * sys/ioctl.json`), so the size field simply kept growing: at
 * `sizeof(t) >= 0x4000` bit 30 lit up and the value collided with the 2-bit
 * DIRECTION field — a 0x4000-byte `_IOR` came out as 0x80000000 | 0x40000000,
 * which IS `_IOWR`, so the kernel would copy the argument THE WRONG WAY. No
 * diagnostic at any layer; the first symptom is a corrupted syscall. (Darwin's
 * arm overflows differently — it MASKS to 13 bits, so an 8192-byte type encodes
 * size ZERO — which is why the ceiling is per-format and lives in the same
 * `when:{format}` arms as the encodings themselves.)
 *
 * ★ WHAT THIS FILE ADDS OVER `tests/ffi/test_shipped_ioctl_size_ceiling.cpp`,
 * WHICH ALREADY PINS BOTH FORMATS ON EVERY HOST. That test drives the FRONT END
 * and stops at the compile verdict. This one carries the boundary encodings
 * through the RELEASE OPTIMIZER and out across a real variadic libc `ioctl`
 * call, so a fold that is correct in the constant evaluator and wrong after
 * optimization has somewhere to fail. The two are not redundant: the compile
 * fact is host-independent, the runtime fact is not.
 *
 * ★ THE REFUSAL HALF IS DELIBERATELY NOT HERE. A corpus example asserts a
 * successful build and an exit code; there is no compile-failure convention in
 * `examples/`, so "a 16384-byte type must NOT compile" is pinned in the unit
 * test and nowhere else. What IS here is the other half of the same contract —
 * that the ceiling refuses ONLY what it must, and every size under it still
 * encodes exactly the number the native header computes.
 *
 * ★ 8192 IS THE INTERESTING SIZE, AND IT IS HERE ON PURPOSE. It is one past
 * Darwin's IOCPARM_MASK (8191) and less than half of Linux's _IOC_SIZEMASK
 * (16383): the SAME type this file compiles and runs is REFUSED on macho. That
 * asymmetry is the property no single global ceiling — and no engine branch —
 * could produce, and it is the reason the guard belongs in the per-format arms.
 *
 * VALUES. Every number below is the published Linux/asm-generic encoding for
 * the stated (dir, size, magic, nr), with 0xf5 = F2FS_IOCTL_MAGIC, the magic
 * sqlite itself writes on this arm (os_unix.c's `#ifdef __linux__` region):
 *   _IOR (dir 2) | size<<16 | 0xf5<<8 | nr
 *   _IOW (dir 1) | size<<16 | 0xf5<<8 | nr
 *   _IOWR(dir 3) | size<<16 | 0xf5<<8 | nr
 *
 * HOST REALITY: elf-only targets with `runOn: ["linux"]`, so a Windows or macOS
 * host COMPILES both arms and reports SkippedCrossHost for the run. That is why
 * every value pin is a `_Static_assert` — it is load-bearing on the host that
 * builds it — and why `main` re-checks the same numbers through the real
 * `unsigned long` request parameter on the Linux legs. A `#else` fallback is
 * deliberately absent: a regression must fail, never degrade into a passing
 * exit code.
 *
 * RED-ON-DISABLE (MEASURED, not claimed): delete the ` | (0u * sizeof(struct {
 * char dss_ioctl_arg_size_exceeds_IOC_SIZEMASK_16383[...]; }))` term from the
 * three elf arms and this file stays GREEN — correctly so, because removing a
 * ceiling cannot change a value under it; that removal is caught by the unit
 * test's refusal arm. What this file catches is the opposite mistake: a guard
 * that stops contributing zero, or a ceiling copied from the macho arm. Both
 * turn the asserts below RED.
 */
#include <sys/ioctl.h>

/* Exactly Linux's _IOC_SIZEMASK: the largest length the size field can hold. */
struct DssIoctlCeilingArg { char b[0x3fff]; };      /* 16383 */

/* Over Darwin's IOCPARM_MASK, far under Linux's — the per-format split, as a
 * type this program actually passes to a syscall. */
struct DssIoctlSplitArg   { char b[0x2000]; };      /* 8192  */

/* sqlite's own scale, as the control: the ceiling must move nothing here. */
struct DssIoctlSmallArg   { char b[32]; };

_Static_assert(sizeof(struct DssIoctlCeilingArg) == 0x3fff, "16383 bytes");
_Static_assert(sizeof(struct DssIoctlSplitArg)   == 0x2000, "8192 bytes");
_Static_assert(sizeof(struct DssIoctlSmallArg)   == 32,     "32 bytes");

#define DSS_CEIL_IOR    0xbffff50cul   /* dir 2 | 16383<<16 | 0xf5<<8 | 12 */
#define DSS_CEIL_IOW    0x7ffff50dul   /* dir 1 | 16383<<16 | 0xf5<<8 | 13 */
#define DSS_CEIL_IOWR   0xfffff50eul   /* dir 3 | 16383<<16 | 0xf5<<8 | 14 */
#define DSS_SPLIT_IOR   0xa000f50cul   /* dir 2 |  8192<<16 | 0xf5<<8 | 12 */
#define DSS_SMALL_IOR   0x8020f50cul   /* dir 2 |    32<<16 | 0xf5<<8 | 12 */

/* The ceiling itself is ACCEPTED and encodes exactly the native number. */
_Static_assert(_IOR(0xf5, 12, struct DssIoctlCeilingArg)  == DSS_CEIL_IOR,
               "elf _IOR at the size ceiling");
_Static_assert(_IOW(0xf5, 13, struct DssIoctlCeilingArg)  == DSS_CEIL_IOW,
               "elf _IOW at the size ceiling");
_Static_assert(_IOWR(0xf5, 14, struct DssIoctlCeilingArg) == DSS_CEIL_IOWR,
               "elf _IOWR at the size ceiling");

/* The type macho refuses. Green here IS the per-format claim. */
_Static_assert(_IOR(0xf5, 12, struct DssIoctlSplitArg) == DSS_SPLIT_IOR,
               "elf accepts 8192, which is one past Darwin's IOCPARM_MASK");

/* And the ceiling moved nothing at sqlite's own scale. */
_Static_assert(_IOR(0xf5, 12, struct DssIoctlSmallArg) == DSS_SMALL_IOR,
               "a 32-byte argument encodes exactly as it did before the ceiling");

/* FORWARD sign-extension guard, in the sibling examples' sense: the value must
 * widen into the `unsigned long` request parameter by ZERO extension. It holds
 * today and turns RED the day any fold starts to sign-extend. `_IOWR` at the
 * ceiling is the worst case — its top TWO bits are set. */
#define DSS_CEIL_IOWR_SIGN_EXTENDED 0xfffffffffffff50eul
_Static_assert((unsigned long)_IOWR(0xf5, 14, struct DssIoctlCeilingArg)
                   != DSS_CEIL_IOWR_SIGN_EXTENDED,
               "must not sign-extend into the request parameter");

/* File scope, not the stack: 16 KiB of automatic storage would be measuring the
 * stack rather than the encoding, and a static object also denies the release
 * optimizer the easy answer of deleting the buffer outright. */
static struct DssIoctlCeilingArg dss_ceiling_arg;
static struct DssIoctlSplitArg   dss_split_arg;

int main(void) {
    /* Recompute at RUNTIME through the `unsigned long` request type, so the
     * pins survive the release optimizer rather than living only in the fold. */
    unsigned long rceil  = _IOR(0xf5, 12, struct DssIoctlCeilingArg);
    unsigned long wceil  = _IOW(0xf5, 13, struct DssIoctlCeilingArg);
    unsigned long rwceil = _IOWR(0xf5, 14, struct DssIoctlCeilingArg);
    unsigned long rsplit = _IOR(0xf5, 12, struct DssIoctlSplitArg);
    unsigned long rsmall = _IOR(0xf5, 12, struct DssIoctlSmallArg);

    dss_ceiling_arg.b[0]          = 1;
    dss_ceiling_arg.b[0x3fff - 1] = 2;
    dss_split_arg.b[0]            = 3;
    dss_split_arg.b[0x2000 - 1]   = 4;

    /* Real variadic libc `ioctl` calls carrying the encoded requests through
     * the `unsigned long` parameter. fd -1 → EBADF, no side effect, no fd
     * touched; the point is that the values reach the ABI boundary for real. */
    ioctl(-1, rceil,  &dss_ceiling_arg);
    ioctl(-1, rsplit, &dss_split_arg);

    if (rceil  != DSS_CEIL_IOR)  return 1;
    if (wceil  != DSS_CEIL_IOW)  return 2;
    if (rwceil != DSS_CEIL_IOWR) return 3;
    if (rsplit != DSS_SPLIT_IOR) return 4;
    if (rsmall != DSS_SMALL_IOR) return 5;
    if (rwceil == DSS_CEIL_IOWR_SIGN_EXTENDED) return 6;  /* signed-fold regression */
    /* The direction field must still be READABLE as itself: an oversized length
     * is exactly what used to bleed into it, so read it back explicitly rather
     * than trusting the whole-word compare above to have covered it. */
    if ((rceil  >> 30) != 2ul) return 7;
    if ((wceil  >> 30) != 1ul) return 8;
    if ((rwceil >> 30) != 3ul) return 9;
    if (dss_ceiling_arg.b[0x3fff - 1] != 2) return 10;
    if (dss_split_arg.b[0x2000 - 1]   != 4) return 11;
    return 42;
}
