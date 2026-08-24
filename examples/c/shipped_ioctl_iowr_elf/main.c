/* TF-C92 fold — the ELF half of the shipped <sys/ioctl.h> request-ENCODING
 * macros. The `shipped_ioctl_iowr_macho` sibling pins the Darwin arm's exact
 * numbers; before this file the elf arms of the same four `when:{format}` rows
 * were pinned only as `replacement` TEXT (tests/ffi/test_shipped_lib_descriptor
 * .cpp compares the string), so NO ELF IOCTL VALUE WAS EVER COMPUTED anywhere in
 * the suite.
 *
 * WHY A TEXT PIN IS NOT ENOUGH HERE, precisely. The failure that corrupts a
 * syscall is a WRONG NUMBER, and the elf and macho arms encode the DIRECTION
 * FIELD INCOMPATIBLY — Darwin uses one bit each (IOC_OUT 0x40000000 for read,
 * IOC_IN 0x80000000 for write); Linux uses a 2-bit CODE at bit 30 with the
 * opposite sense (_IOC_READ 2 → 0x80000000, _IOC_WRITE 1 → 0x40000000). So a
 * copy-paste between the two per-format arms SWAPS read and write while leaving
 * both replacement strings looking entirely plausible. The value asserts below
 * are what makes that swap loud; two of them are written as explicit
 * `!= <the macho number>` so the swap is named rather than merely excluded.
 *
 * PROVENANCE OF EVERY NUMBER. The formula is musl's, read off disk at
 * <emsdk>/upstream/emscripten/system/lib/libc/musl/arch/generic/bits/ioctl.h —
 * the Linux asm-generic ABI, verbatim:
 *     #define _IOC(a,b,c,d) ( ((a)<<30) | ((b)<<8) | (c) | ((d)<<16) )
 *     _IOC_NONE 0U   _IOC_WRITE 1U   _IOC_READ 2U
 * Each expected value below was then MEASURED by compiling the same expression
 * against that real header with the host /usr/bin/clang — these are differential
 * results, not derivations. Three of the four are additionally PUBLISHED Linux
 * F2FS numbers, which is an independent witness that the transcription is right:
 *     _IO (0xf5,  1)               = 0x0000f501  = F2FS_IOC_START_ATOMIC_WRITE
 *     _IOW(0xf5, 13, unsigned int) = 0x4004f50d  = F2FS_IOC_SET_PIN_FILE
 *     _IOR(0xf5, 12, unsigned int) = 0x8004f50c  = F2FS_IOC_GET_FEATURES
 * and 0xf5 IS the magic sqlite writes on this arm (os_unix.c:392-396, under
 * `#ifdef __linux__`: four `_IO(F2FS_IOCTL_MAGIC, …)` plus the `_IOR(…, 12, u32)`).
 * DERIVATIONS, stated for the two values that are not published constants:
 *     _IOW(0xf5,13,unsigned int): dir _IOC_WRITE 1 → 1<<30 = 0x40000000;
 *         type 0xf5 → 0xf500; nr 13 → 0x0d; size 4 → 4<<16 = 0x00040000.
 *     _IOWR('z',23,struct ByteRangeLockPB2): dir _IOC_READ|_IOC_WRITE = 3 →
 *         3<<30 = 0xc0000000; size 32 → 0x00200000; 'z' = 0x7a → 0x7a00;
 *         nr 23 → 0x17.  ⇒ 0xc0207a17.
 *
 * ★ THE _IOWR COINCIDENCE, RECORDED SO IT IS NOT MISTAKEN FOR A DISCRIMINATOR:
 * 0xc0207a17 is BYTE-IDENTICAL to the macho sibling's `_IOWR` value. That is not
 * an error and not a shared implementation — Darwin's IOC_INOUT (IOC_IN|IOC_OUT)
 * and Linux's dir code 3 both land on 0xc0000000, and the size/type/nr fields
 * happen to occupy the same bits. So `_IOWR` alone can NEVER witness which
 * per-format arm was selected; only `_IO`/`_IOR`/`_IOW` can, and all three are
 * asserted against their macho twins below.
 *
 * ★★ ON THE `u` SUFFIX — TWO MEASUREMENTS THAT REFUTE THE OBVIOUS PIN, RECORDED
 * BECAUSE THE PLAUSIBLE VERSION OF THIS FILE WOULD CLAIM MORE THAN IT PROVES.
 * The elf direction constants carry an explicit `u` (`0u`/`1u`/`2u`/`3u`) and the
 * usual justification is that `2 << 30` overflows a signed int, whose UB the two
 * compilers resolve differently. The interop half of that is TRUE and MEASURED:
 * with the `u` dropped, host clang folds the `_IOR` shape to 0xffffffff8004f50c
 * (it sign-extends into the `unsigned long` request), so a signed spelling would
 * make DSS disagree with every native consumer of the same header. But the part
 * a file like this one would like to assert is NOT available:
 *   (1) DSS DOES NOT SIGN-EXTEND. MEASURED with the shipped CLI on this target:
 *       `_Static_assert((unsigned long)(2 << 30) == 0x80000000ul)` PASSES, and the
 *       `_IOR` body spelled `((2) << 30 | …)` folds to the SAME 0x8004f50c as the
 *       `((2u) << 30 | …)` spelling. So dropping the `u` from `_IOR`/`_IOW`/`_IOWR`
 *       is INVISIBLE from inside a DSS-compiled TU — no `_Static_assert` here can
 *       fire on it. (The macho sibling records the mirror-image non-red for a
 *       different reason: C 6.4.4.1p5 already makes `0xc0000000` unsigned there.)
 *   (2) The `_IO` arm has no `sizeof`, so its TYPE follows the direction constant
 *       and unsigned-ness would be observable via modular wraparound
 *       (`_IO(0xf5,1) - 0xf502 > 0` is true iff the expression is unsigned).
 *       That probe is UNUSABLE: MEASURED, DSS's constant evaluator does not apply
 *       C 6.2.5p9 wraparound — `_Static_assert(0u - 1u == 0xffffffffu)` FAILS
 *       under DSS while host clang accepts it (the runtime path is correct; only
 *       the folder is wrong). Reported separately as its own defect; a guard
 *       built on it would be pinning that bug, not the `u`.
 * SO WHAT THESE ASSERTS ACTUALLY GUARD, stated exactly: that DSS computes the
 * number host clang computes from the real musl header. That catches every
 * shift/mask/direction/size regression and the arm-to-arm copy-paste — i.e. every
 * way the config can diverge EXCEPT the one where DSS's UB choice happens to land
 * on the right answer anyway. The `!= 0xffffffff…` lines below are FORWARD guards
 * in the macho sibling's sense: they hold today and turn red the day any fold
 * starts sign-extending into the request parameter.
 *
 * ★ THE ELF SIZE FIELD IS UNMASKED, unlike macho's `& 0x1fffu` — faithful to
 * musl, and a documented hazard rather than a bug: at `sizeof(t) >= 0x4000` the
 * size field overflows into the bit-30 DIRECTION field and silently rewrites it
 * (a 0x4000-byte `_IOR` encodes as `_IOWR`). The guard that would stop this,
 * `_IOC_TYPECHECK`, is KERNEL-INTERNAL (`#ifdef __KERNEL__`) and absent from every
 * userspace header — verified: zero hits for `_IOC_TYPECHECK` across the emsdk
 * musl sysroot. Recorded in full on `src/dss-config/shippedLibs/sys/ioctl.json`'s
 * `$comment`. Nothing here is near the wall (32/8/4/0 bytes), so no assert claims
 * a bound nobody tests.
 *
 * HOST REALITY, STATED PLAINLY RATHER THAN LEFT TO BE DISCOVERED. This example
 * targets elf only and declares `runOn: ["linux"]`, so on the macOS development
 * host the runner COMPILES both arms and reports SkippedCrossHost for the run.
 * That is why every value pin is a `_Static_assert`: a compile-time assert makes
 * this file load-bearing on the host that actually builds it, and the `main`
 * below re-checks the same numbers through the real `unsigned long` request
 * parameter on the Linux CI leg. A `#else`/fallback path is deliberately absent —
 * a regression must fail, never degrade into a passing exit code. */
#include <sys/ioctl.h>

/* Verbatim from sqlite/src/os_unix.c:2976 — 3×8 + 1 + 1 + pad2 + 4 = 32 bytes.
 * Reused from the macho sibling ON PURPOSE: the same declaration through the two
 * per-format arms is what makes the `_IOWR` coincidence above verifiable. */
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

/* The size field of the ELF encoding, isolated: a TYPE-NAME travelling through a
 * function-like macro parameter into `sizeof`. This is the shape whose absence
 * killed the whole TU as `error[P0009] … got 'struct'` before c92 — and note the
 * ABSENCE of macho's `& 0x1fffu`, which is the per-format divergence itself. */
#define SZ(t) (sizeof(t) << 16)

_Static_assert(sizeof(struct ByteRangeLockPB2) == 32, "AFP lock PB2 is 32 bytes");
_Static_assert(SZ(struct ByteRangeLockPB2) == 0x00200000ul, "sizeof(struct T) through a macro param");
_Static_assert(SZ(union DssIoctlProbeU)     == 0x00080000ul, "sizeof(union T) through a macro param");
_Static_assert(SZ(enum DssIoctlProbeE)      == 0x00040000ul, "sizeof(enum T) through a macro param");

/* os_unix.c:2986's expression, evaluated under the ELF arm. */
#define dssByteRangeLock2Elf  _IOWR('z', 23, struct ByteRangeLockPB2)

/* Published Linux F2FS numbers (see the header comment) + the two derived ones. */
#define DSS_ELF_IOR_F2FS   0x8004f50cul  /* F2FS_IOC_GET_FEATURES        */
#define DSS_ELF_IO_F2FS    0x0000f501u   /* F2FS_IOC_START_ATOMIC_WRITE  */
#define DSS_ELF_IOW_F2FS   0x4004f50dul  /* F2FS_IOC_SET_PIN_FILE        */
#define DSS_ELF_IOWR_PB2   0xc0207a17ul  /* dir 3 | 32<<16 | 'z'<<8 | 23 */

_Static_assert(_IOR(0xf5, 12, unsigned int) == DSS_ELF_IOR_F2FS, "_IOR elf encoding");
_Static_assert(_IO(0xf5, 1)                 == DSS_ELF_IO_F2FS,  "_IO elf encoding");
_Static_assert(_IOW(0xf5, 13, unsigned int) == DSS_ELF_IOW_F2FS, "_IOW elf encoding");
_Static_assert(dssByteRangeLock2Elf         == DSS_ELF_IOWR_PB2, "_IOWR elf encoding");

/* ── THE PER-FORMAT SELECTION PIN ────────────────────────────────────────────
 * The same three expressions under the MACHO arm, as measured against the real
 * SDK sys/ioccom.h and pinned by the macho sibling. Each `!=` here fails the
 * instant an elf target picks up a macho variant — the copy-paste this file
 * exists to catch. `_IOR`/`_IOW` are the direction SWAP (0x8… ↔ 0x4…); `_IO` is
 * Darwin's dedicated IOC_VOID bit, which Linux does not have at all. */
#define DSS_MACHO_IOR_Q9   0x40047109ul
#define DSS_MACHO_IOW_Q7   0x80087107ul
#define DSS_MACHO_IO_Q11   0x2000710bu

_Static_assert(_IOR('q', 9, enum DssIoctlProbeE)  == 0x80047109ul, "elf _IOR('q',9,enum) = dir 2");
_Static_assert(_IOR('q', 9, enum DssIoctlProbeE)  != DSS_MACHO_IOR_Q9,
               "elf _IOR must NOT carry Darwin's IOC_OUT — read/write swapped");
_Static_assert(_IOW('q', 7, union DssIoctlProbeU) == 0x40087107ul, "elf _IOW('q',7,union) = dir 1");
_Static_assert(_IOW('q', 7, union DssIoctlProbeU) != DSS_MACHO_IOW_Q7,
               "elf _IOW must NOT carry Darwin's IOC_IN — read/write swapped");
_Static_assert(_IO('q', 11)                       == 0x0000710bu, "elf _IO('q',11) = dir 0");
_Static_assert(_IO('q', 11)                       != DSS_MACHO_IO_Q11,
               "elf _IO must NOT set Darwin's IOC_VOID bit — Linux has no such bit");

/* FORWARD sign-extension guards, in the macho sibling's sense: the value must
 * widen to the `unsigned long` request parameter by ZERO-extension. These hold
 * today (see the two measurements in the header comment — DSS does not
 * sign-extend) and turn RED the day any fold starts to. */
#define DSS_ELF_IOR_SIGN_EXTENDED  0xffffffff8004f50cul
#define DSS_ELF_IOWR_SIGN_EXTENDED 0xffffffffc0207a17ul

_Static_assert((unsigned long)_IOR(0xf5, 12, unsigned int) == DSS_ELF_IOR_F2FS,
               "widens WITHOUT sign extension");
_Static_assert((unsigned long)_IOR(0xf5, 12, unsigned int) != DSS_ELF_IOR_SIGN_EXTENDED,
               "must not sign-extend");
_Static_assert((unsigned long)dssByteRangeLock2Elf != DSS_ELF_IOWR_SIGN_EXTENDED,
               "must not sign-extend");

int main(void) {
    struct ByteRangeLockPB2 pb;
    /* Recompute at RUNTIME through the `unsigned long` request type, so the pins
     * survive the release optimizer rather than living only in the folder. */
    unsigned long rreq  = _IOR(0xf5, 12, unsigned int);
    unsigned long vreq  = _IO(0xf5, 1);
    unsigned long wreq  = _IOW(0xf5, 13, unsigned int);
    unsigned long rwreq = dssByteRangeLock2Elf;

    pb.offset        = 0;
    pb.length        = 0;
    pb.retRangeStart = 0;
    pb.unLockFlag    = 1;
    pb.startEndFlag  = 0;
    pb.fd            = -1;

    /* A real variadic libc `ioctl` call carrying the encoded request through its
     * `unsigned long` parameter. fd -1 → EBADF, no side effect, no fd touched;
     * the point is that the value reaches the ABI boundary for real. */
    ioctl(-1, rreq, &pb);

    if (rreq  != DSS_ELF_IOR_F2FS)  return 1;
    if (vreq  != DSS_ELF_IO_F2FS)   return 2;
    if (wreq  != DSS_ELF_IOW_F2FS)  return 3;
    if (rwreq != DSS_ELF_IOWR_PB2)  return 4;
    if (rreq  == DSS_MACHO_IOR_Q9)  return 5;   /* wrong per-format arm */
    if (rreq  == DSS_ELF_IOR_SIGN_EXTENDED) return 6;  /* signed-fold regression */
    if (pb.fd != -1)                return 7;
    return 42;
}
