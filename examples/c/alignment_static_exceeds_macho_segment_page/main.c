/* P65 [[D-LINK-MACHO-IMAGE-OVERALIGNED-STATIC-IS-A-LOAD-TIME-COIN-FLIP]], the
 * NEGATIVE half: a statically allocated object aligned more strictly than the
 * IMAGE's own mapping granularity is refused BY NAME, never placed and shipped.
 *
 * ★★ THE SAME SOURCE IS THE POSITIVE HALF ON THE OTHER DARWIN PORT, and that is
 * the whole point of this pair. `image.segmentPageSize` is 4096 on
 * `macho64-x86_64-darwin-exec` and 16384 on `macho64-arm64-darwin-exec`, so a
 * request of 8192 is REFUSED here and PLACED there — by a number in a
 * `.format.json`, with no architecture test anywhere in the writer.
 * `examples/c/alignment_overaligned_static_placed` carries the arm64 arm and
 * runs the same request to 42.
 *
 * ★★ WHY MACH-O REFUSES WHERE ELF PLACES. An ELF PT_LOAD carries `p_align`, so
 * the ELF writer can PROMISE a mapping granularity that covers its strongest
 * member and the kernel honours it — `elf64-x86_64-linux-exec` runs this shape
 * at 8192, 16384 and 65536. `segment_command_64` has NO alignment field at all,
 * and `section_64.align` is read by the STATIC LINKER, never by dyld. A
 * base-relative Mach-O image (every darwin image here: MH_PIE / MH_DYLIB) is
 * slid by a multiple of the VM page size, so an object asking for more keeps the
 * address the linker chose and LOSES its alignment at load.
 *
 * ✔THE REFERENCE, Apple Silicon / macOS 26.6.2 / Apple clang 21.0.0 / ld
 * PROJECT:ld-1267, each port probed SEPARATELY, BUILD **and** RUN, twenty runs
 * per cell because one run decides nothing about a coin flip:
 *   x86_64 (page 4096):  align 4096  -> 42 every run.
 *                        align 8192  -> ld64 WARNS "reducing alignment of
 *                        section __DATA,__data from 0x2000 to 0x1000 because it
 *                        exceeds segment maximum alignment", then 50/52 BY RUN.
 *   arm64  (page 16384): align 16384 -> 42 twenty times out of twenty.
 *                        align 32768 -> the same warning, 0x8000 -> 0x4000, then
 *                        50/54 BY RUN — never 42.
 * `-Wl,-segalign,0x8000` does not lift the cap either: ld64 refuses the link
 * outright with "chained fixups, page_size not 4KB or 16KB in segment #2".
 * ⇒ No reference makes an above-page static WORK on Mach-O, so under
 * `DSS = (gcc U clang U MSVC)` — the union being over what WORKS, not what is
 * ACCEPTED — nothing votes to place it. DSS is one notch stricter than ld64 on
 * purpose: ld64 warns and ships the coin flip, and a warning is not an answer
 * where the outcome is a silent, non-deterministic miscompile.
 *
 * ★ TWO OBJECTS BEHIND AN ODD FILLER, not one, and in both storage classes. A
 * single over-aligned static sits at its section head and is right BY LUCK —
 * the measurement that hid the ELF sibling for months — so a subject that could
 * only ever be checked by a REFUSAL still carries the shape that would be
 * needed if the refusal were ever lifted.
 *
 * ⓘ NOT AN ELF OR PE ARM, and that is measured rather than conservative: this
 * exact shape at 8192 BUILDS AND RUNS to 42 on `elf64-x86_64-linux-exec` and on
 * `pe64-x86_64-windows-exec` (PE pads within the section; ELF raises `p_align`),
 * so a gate on either would refuse what DSS demonstrably gets right.
 * `positioned: false` because a link-tier diagnostic carries no source span.
 */

#define BIG 8192

__attribute__((aligned(BIG))) static char b1[3];
static char bpad1[7];
__attribute__((aligned(BIG))) static char b2[5];

__attribute__((aligned(BIG))) static char d1[3] = {1};
static char dpad1[7] = {1};
__attribute__((aligned(BIG))) static char d2[5] = {1};

int main(int argc, char **argv) {
    (void)argv;
    char *objs[4];
    int i;

    objs[0] = b1; objs[1] = b2; objs[2] = d1; objs[3] = d2;
    bpad1[0] = (char)argc;
    dpad1[0] = (char)argc;

    for (i = 0; i < 4; ++i) {
        unsigned long long const a = (unsigned long long)(void *)objs[i];
        if (a % (unsigned long long)BIG != 0ull) return 50 + i;
    }
    return 42;
}
