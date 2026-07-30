/* TF-C92 — the shipped <sys/ioctl.h> request-ENCODING macros on macOS: the
 * sqlite `os_unix.c` AFP-locking shape, reduced to its essentials.
 *
 * sqlite/src/os_unix.c:2986 writes, live on every Darwin build:
 *     #define afpfsByteRangeLock2FSCTL  _IOWR('z', 23, struct ByteRangeLockPB2)
 * and consumes it at os_unix.c:3013 as `fsctl(path, afpfsByteRangeLock2FSCTL, …)`.
 * Before c92, `sys/ioctl.json` shipped the `ioctl` SYMBOL and ZERO macros (its own
 * `$comment` claimed sqlite used none of this header's macros — measurably false).
 * An angle include resolves to DESCRIPTORS ONLY, so the real <sys/ioccom.h> text is
 * never read: `_IOWR` reached the PARSER as an ordinary call whose third argument is
 * a TYPE-NAME, and `struct` cannot start an expression —
 *     error[P0009] … got 'struct'   at os_unix.c:2986:56
 * the sole diagnostic in that whole TU. c92 ships `_IO`/`_IOR`/`_IOW`/`_IOWR` as
 * per-format `variants` (macho ← the SDK sys/ioccom.h bit layout; elf ← the Linux
 * asm-generic one), which is the ONLY place that per-OS divergence may live.
 *
 * This example pins the EXACT ENCODED VALUES, not merely "it parses" — a
 * parse-only test would be vacuous here, because the failure mode that actually
 * corrupts a syscall is a WRONG NUMBER, and a wrong number is silent.
 *
 * Also exercised deliberately: a TYPE-NAME reaching `sizeof` THROUGH a
 * function-like macro PARAMETER (`SZ(t)` → `sizeof(t)`) for all three tag
 * flavours — `struct`, `union`, `enum`. That is precisely the shape the c92 defect
 * proves was untested, and it is the mechanism every `_IOxx` macro depends on.
 *
 * ON THE UNSIGNED DIRECTION BITS — measured, so stated precisely rather than
 * assumed. Every direction constant in the descriptor carries an explicit `u`,
 * mirroring Darwin's `(__uint32_t)` casts and musl's `0U/1U/2U`. On the MACHO side
 * that is EXPLICITNESS, not a repair: `0xc0000000` already has type `unsigned int`
 * by C 6.4.4.1p5 (it does not fit in `int`), and DSS and clang were both measured
 * to agree. On the ELF side it is LOAD-BEARING: `2 << 30` overflows a signed int,
 * and the two compilers DIVERGE on that UB — clang widens the fold to
 * 0xffffffff80000000 while DSS widens it to 0x80000000, so an ioctl number
 * computed by DSS would not match the one every native consumer of the same
 * header computes. `2u << 30` makes them agree. The `WIDENED`/`SIGN_EXTENDED`
 * asserts below are a FORWARD guard: they hold today and would turn RED the day
 * any fold starts sign-extending into the `unsigned long` request parameter.
 *
 * RED-ON-DISABLE — each of these was actually RUN, and what it produced is
 * recorded (no claimed-but-unobserved red):
 *   1. Delete the `_IOWR` row from `src/dss-config/shippedLibs/sys/ioctl.json`:
 *      `_IOWR` is unshipped, reaches the parser as a call whose argument is a
 *      type-name → `error[P0009] … got 'struct'`, no binary. On sqlite os_unix.c
 *      the SAME edit restores exactly the original single diagnostic at
 *      os_unix.c:2986:56. (MEASURED.)
 *   2. Delete just the macho VARIANT of `_IOWR` (leaving the elf one): no variant
 *      matches the active format, the macro is not injected, and P0009 returns —
 *      so the per-format SELECTION is pinned, not merely the row's presence.
 *      (MEASURED.)
 *   3. Give the macho `_IOR` the elf direction code (0x40000000u → 0x80000000u):
 *      `error[S0029]: static assertion failed: "_IOR macho encoding"`. The
 *      exact-value pin is what catches a plausible copy-paste between the two
 *      per-format arms. (MEASURED.)
 *   4. A regression in `sizeof` through a macro parameter turns the SZ_* asserts
 *      RED; a shift/mask regression turns the four encoding asserts RED. (These
 *      are engine behaviours, not config rows — stated as what the asserts pin.)
 * NOTE: dropping the `u` from the macho `0xc0000000u` was also run and is NOT red
 * — correctly so, per the C 6.4.4.1p5 escalation above. It is recorded here so
 * nobody mistakes that suffix for the thing these asserts are guarding.
 * A `#else` fallback is deliberately absent: the only program here is the ioctl
 * one, so a regression cannot silently degrade into a passing exit code. */
#include <sys/ioctl.h>

/* Verbatim from sqlite/src/os_unix.c:2976 — the layout drives the encoded size
 * field, so `sizeof` here is load-bearing: 3×8 + 1 + 1 + pad2 + 4 = 32 bytes. */
struct ByteRangeLockPB2 {
    unsigned long long offset;
    unsigned long long length;
    unsigned long long retRangeStart;
    unsigned char      unLockFlag;
    unsigned char      startEndFlag;
    int                fd;
};

union DssIoctlProbeU { int i; double d; };          /* sizeof 8 */
enum DssIoctlProbeE { DSS_IOCTL_PROBE_E0 = 1 };     /* sizeof 4 */

/* The size field of the encoding, isolated: a TYPE-NAME travelling through a
 * function-like macro parameter into `sizeof`. This is the shape that broke. */
#define SZ(t) ((sizeof(t) & 0x1fffu) << 16)

_Static_assert(sizeof(struct ByteRangeLockPB2) == 32, "AFP lock PB2 is 32 bytes");
_Static_assert(SZ(struct ByteRangeLockPB2) == (32u << 16), "sizeof(struct T) through a macro param");
_Static_assert(SZ(union DssIoctlProbeU)     == (8u  << 16), "sizeof(union T) through a macro param");
_Static_assert(SZ(enum DssIoctlProbeE)      == (4u  << 16), "sizeof(enum T) through a macro param");

/* os_unix.c:2986, verbatim. */
#define afpfsByteRangeLock2FSCTL  _IOWR('z', 23, struct ByteRangeLockPB2)

/* Every value below was measured by compiling the same expression against the
 * REAL /usr/include/sys/ioccom.h with Apple clang — these are not derivations. */
#define DSS_IOWR_EXPECTED  0xc0207a17u   /* IOC_INOUT | 32<<16 | 'z'<<8 | 23  */
#define DSS_IOW_EXPECTED   0x80087107u   /* IOC_IN    |  8<<16 | 'q'<<8 |  7  */
#define DSS_IOR_EXPECTED   0x40047109u   /* IOC_OUT   |  4<<16 | 'q'<<8 |  9  */
#define DSS_IO_EXPECTED    0x2000710bu   /* IOC_VOID  |  0<<16 | 'q'<<8 | 11  */
#define DSS_SIGN_EXTENDED  0xffffffffc0207a17ul  /* what a SIGNED 0xc0000000 gives */

_Static_assert(afpfsByteRangeLock2FSCTL == DSS_IOWR_EXPECTED, "_IOWR macho encoding");
_Static_assert(_IOW('q',  7, union DssIoctlProbeU) == DSS_IOW_EXPECTED, "_IOW macho encoding");
_Static_assert(_IOR('q',  9, enum DssIoctlProbeE)  == DSS_IOR_EXPECTED, "_IOR macho encoding");
_Static_assert(_IO('q',  11)                       == DSS_IO_EXPECTED,  "_IO macho encoding");

/* THE interop pin: the widening the real `ioctl`/`fsctl` request parameter forces.
 * Unsigned direction bits zero-extend; a signed one sign-extends and silently
 * passes a different syscall request. Both directions are asserted. */
_Static_assert((unsigned long)afpfsByteRangeLock2FSCTL == 0xc0207a17ul, "widens WITHOUT sign extension");
_Static_assert((unsigned long)afpfsByteRangeLock2FSCTL != DSS_SIGN_EXTENDED, "must not sign-extend");

int main(void) {
    struct ByteRangeLockPB2 pb;
    /* Recompute at RUNTIME through the `unsigned long` request type, so the pin
     * survives the release optimizer rather than living only in the folder. */
    unsigned long req  = afpfsByteRangeLock2FSCTL;
    unsigned long wreq = _IOW('q',  7, union DssIoctlProbeU);
    unsigned long rreq = _IOR('q',  9, enum DssIoctlProbeE);
    unsigned long vreq = _IO('q',  11);

    pb.offset        = 0;
    pb.length        = 0;
    pb.retRangeStart = 0;
    pb.unLockFlag    = 1;
    pb.startEndFlag  = 0;
    pb.fd            = -1;

    /* A real variadic libSystem `ioctl` call carrying the encoded request through
     * its `unsigned long` parameter. fd -1 → EBADF, no side effect, no fd touched;
     * the point is that the value reaches the ABI boundary for real. */
    ioctl(-1, req, &pb);

    if (req  != 0xc0207a17ul) return 1;
    if (wreq != 0x80087107ul) return 2;
    if (rreq != 0x40047109ul) return 3;
    if (vreq != 0x2000710bul) return 4;
    if (req == DSS_SIGN_EXTENDED) return 5;      /* signed-fold regression */
    if (pb.fd != -1) return 6;
    return 42;
}
