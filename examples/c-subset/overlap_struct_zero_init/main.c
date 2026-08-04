/* D-MIR-OVERLAP-STRUCT-ZERO-INIT — zero-initializing a struct whose members
 * SHARE BYTES (a c107 / D-FFI-DESCRIPTOR-UNION-OVERLAY explicit-offset overlay).
 *
 * `{0}` and `{}` denote a WHOLE OBJECT of zero bytes, which is unambiguous no
 * matter how many members alias those bytes — read through either spelling,
 * zeroed bytes are zero. So the lowering zero-fills the struct's FULL size and
 * skips member-wise assignment entirely. A NON-zero brace initializer stays
 * refused (that one genuinely is ambiguous: a later member would clobber an
 * earlier one), which is why this example only ever zero-initializes.
 *
 * An overlapping struct cannot be SPELLED in C — the explicit offsets arrive
 * only from a shipped-library descriptor. Exactly two shipped overlays exist
 * (MEASURED, 2026-08-04), and neither is reachable from an elf target:
 *   • pe    — `windows.json`'s ULARGE_INTEGER {QuadPart u64@0, LowPart u32@0,
 *             HighPart u32@4}: both 32-bit halves live INSIDE the 64-bit whole.
 *   • macho — `sys/stat.json`'s `struct stat` variant: st_mtimespec (a 16-byte
 *             timespec) @48 overlays the flat twins st_mtim_sec @48 +
 *             st_mtim_nsec @56, so the field may be spelled either way. This is
 *             the shape that BLOCKED sqlite's CLI: `shell.c:25283`
 *             `struct stat x = {0};`.
 * The elf arms therefore compile the trivial `#else` (no elf-reachable overlay
 * exists to exercise) and prove only that the change costs them nothing.
 *
 * ★ TWO TIERS, AND THEY ARE DIFFERENT CODE. Blocks (1)–(4) declare LOCALS: the
 * zero-fill is emitted as runtime stores by the MIR brace-init lowering. Block
 * (5) declares a FILE-SCOPE STATIC, whose bytes are produced at COMPILE time by
 * the static-data encoder (`src/asm/asm.cpp` `encodeAggregateValue`) directly
 * into the object file. That encoder carries its OWN copy of the refusal and no
 * local declaration can reach it, so without block (5) the static-data half of
 * this rule has no end-to-end witness at all.
 *
 * THE SIZE WITNESS (why this is not a "something was emitted" test): each arm
 * first DIRTIES every byte of the object, then re-zeroes it through the
 * brace-init path via a compound literal. A fill that stopped at the first
 * member's width — or anywhere short of the full layout size — leaves the dirty
 * bytes visible through the ALIASING member and the exit code changes. The
 * `release` arm re-runs all of it through the optimizer.
 *
 * THE SCORE IS A CHECKSUM, NOT A TALLY: the five blocks contribute 8 + 8 + 10 +
 * 8 + 8 = 42, so dropping or short-circuiting ANY ONE of them changes the exit
 * code even when every check that did run passed.
 *
 * RED-ON-DISABLE: restore the guard to refuse every overlapping brace-init and
 * the pe + macho compiles fail — blocks (1)–(4) with
 * error[H_UnsupportedLoweringForKind] "brace-initialization of an overlapping
 * explicit-offset struct", block (5) with error[K_NoMatchingObjectFormat]
 * "static initialization of an overlapping explicit-offset struct". */

#ifdef _WIN32
#include <windows.h>

/* (5) FILE-SCOPE STATIC — the STATIC-DATA encoder's arm. MUTABLE, and written
 * below on purpose: a `const` copy could be folded at every use, which would
 * prove nothing about the bytes that actually reached the data section. */
static ULARGE_INTEGER g_zero = {0};

int main(void) {
    int score = 0;

    /* (1) The plain declaration form — sqlite's `struct stat x = {0};` shape. */
    ULARGE_INTEGER z = {0};
    if (z.QuadPart != 0) return 1;          /* the 8-byte whole             */
    if (z.LowPart  != 0) return 2;          /* alias of bytes [0,4)         */
    if (z.HighPart != 0) return 3;          /* alias of bytes [4,8)         */
    score += 8;

    /* (2) The C23 empty initializer (6.7.10p11), same object, same guarantee —
     * in BOTH spellings, and the second one is dirtied first so it witnesses the
     * fill SIZE and not merely a lucky zero stack slot. */
    ULARGE_INTEGER e = {};
    if (e.QuadPart != 0) return 4;
    if (e.LowPart  != 0) return 5;
    if (e.HighPart != 0) return 6;
    e.QuadPart = 0xFFFFFFFFFFFFFFFFull;
    e = (ULARGE_INTEGER){};
    if (e.QuadPart != 0) return 14;
    if (e.HighPart != 0) return 15;
    score += 8;

    /* (3) THE SIZE WITNESS. Dirty all 8 bytes, then zero through the brace-init
     * path. A short fill leaves 0xFFFFFFFF in the high half, which is visible
     * through HighPart AND through QuadPart. */
    ULARGE_INTEGER d;
    d.QuadPart = 0xFFFFFFFFFFFFFFFFull;
    d = (ULARGE_INTEGER){0};
    if (d.QuadPart != 0) return 7;
    if (d.HighPart != 0) return 8;          /* the byte range a short fill misses */
    if (d.LowPart  != 0) return 9;
    score += 10;

    /* (4) The overlay is still LIVE afterwards — zero-filling must not have
     * turned the aliasing members into dead storage. Write the two halves and
     * read the whole back (x86_64 is little-endian). */
    d.LowPart  = 7u;
    d.HighPart = 9u;
    if (d.QuadPart != 0x0000000900000007ull) return 10;
    score += 8;

    /* (5) THE STATIC-DATA ARM — read the COMPILE-TIME bytes of `g_zero` back out
     * of the data section. No runtime store put them there; the encoder did.
     * HighPart covers bytes [4,8), the range an emission that stopped at the
     * first member's width would leave holding whatever the section's next
     * object contains. Then write both halves and read the whole back: the
     * emitted object must still be a LIVE 8-byte overlay, not a zeroed husk. */
    if (g_zero.QuadPart != 0) return 16;
    if (g_zero.LowPart  != 0) return 17;
    if (g_zero.HighPart != 0) return 18;
    g_zero.LowPart  = 3u;
    g_zero.HighPart = 4u;
    if (g_zero.QuadPart != 0x0000000400000003ull) return 19;
    score += 8;

    return score;                            /* 42 */
}

#elif defined(__APPLE__)
#include <sys/stat.h>

/* (5) FILE-SCOPE STATIC — the STATIC-DATA encoder's arm, on the very type that
 * blocked sqlite's CLI. MUTABLE and written below (see the pe twin's note). */
static struct stat g_zero = {0};

int main(void) {
    int score = 0;

    /* (1) VERBATIM the construct that blocked sqlite's CLI (shell.c:25283). */
    struct stat z = {0};
    if (z.st_size != 0) return 1;
    if (z.st_mtim_sec  != 0) return 2;       /* flat twin @48                */
    if (z.st_mtim_nsec != 0) return 3;       /* flat twin @56                */
    if (z.st_mtimespec.tv_sec  != 0) return 4;  /* the timespec overlaying both */
    if (z.st_mtimespec.tv_nsec != 0) return 5;
    score += 8;

    /* (2) The C23 empty initializer (6.7.10p11), same object, same guarantee —
     * in BOTH spellings, and the second one is dirtied first so it witnesses the
     * fill SIZE and not merely a lucky zero stack slot. */
    struct stat e = {};
    if (e.st_size != 0) return 6;
    if (e.st_mtim_sec != 0 || e.st_mtimespec.tv_nsec != 0) return 7;
    e.st_mtimespec.tv_sec = -1;
    e.st_birthtim_sec     = -1;
    e = (struct stat){};
    if (e.st_mtim_sec != 0) return 15;
    if (e.st_birthtim_sec != 0) return 16;
    score += 8;

    /* (3) THE SIZE WITNESS. `struct stat` is 144 bytes and the overlay sits at
     * byte 48 — far past the first member — so a fill that stopped early leaves
     * the dirtied timespec visible through BOTH spellings. */
    struct stat d;
    d.st_size             = -1;
    d.st_mtimespec.tv_sec  = -1;
    d.st_mtimespec.tv_nsec = -1;
    d.st_birthtim_sec      = -1;             /* byte 96+ — the far tail       */
    d = (struct stat){0};
    if (d.st_size != 0) return 8;
    if (d.st_mtim_sec  != 0) return 9;
    if (d.st_mtim_nsec != 0) return 10;
    if (d.st_mtimespec.tv_sec != 0) return 11;
    if (d.st_birthtim_sec != 0) return 12;   /* the byte range a short fill misses */
    score += 10;

    /* (4) The overlay is still LIVE afterwards — write through the timespec
     * spelling and read back through the flat twins. */
    d.st_mtimespec.tv_sec  = 7;
    d.st_mtimespec.tv_nsec = 9;
    if (d.st_mtim_sec  != 7) return 13;
    if (d.st_mtim_nsec != 9) return 14;
    score += 8;

    /* (5) THE STATIC-DATA ARM — read the COMPILE-TIME bytes of `g_zero` back out
     * of the data section. No runtime store put them there; the encoder did.
     * `st_birthtim_sec` sits in the far tail, the range an emission that stopped
     * short would leave holding whatever the section's next object contains.
     * Then write through the timespec spelling and read back through the flat
     * twins: the emitted object must still be a LIVE overlay. */
    if (g_zero.st_size != 0) return 17;
    if (g_zero.st_mtim_sec != 0 || g_zero.st_mtim_nsec != 0) return 18;
    if (g_zero.st_mtimespec.tv_sec != 0) return 19;
    if (g_zero.st_birthtim_sec != 0) return 20;
    g_zero.st_mtimespec.tv_sec  = 3;
    g_zero.st_mtimespec.tv_nsec = 4;
    if (g_zero.st_mtim_sec != 3 || g_zero.st_mtim_nsec != 4) return 22;
    score += 8;

    return score;                            /* 42 */
}

#else
/* elf: no shipped descriptor declares an OVERLAPPING explicit-offset struct that
 * an elf target can reach (MEASURED — the only two overlays are pe-gated and
 * macho-gated), so there is nothing here to exercise. This arm exists to prove
 * the change is inert for elf x86_64 + arm64.
 *
 * ★ IT RETURNS 99, NOT 42, AND THAT IS THE POINT. The elf targets carry a
 * per-target `exitCode: 99` override in expected.json. Were this arm to return
 * the same 42 the real arms compute, a pe or darwin build that fell through to
 * `#else` — a lost `_WIN32` / `__APPLE__` predefine, or a descriptor that
 * stopped being reachable — would still exit 42, and this example would report
 * GREEN while compiling none of the code it exists to test. With 99 here,
 * exactly one arm can produce each target's expected exit code. */
int main(void) { return 99; }
#endif
