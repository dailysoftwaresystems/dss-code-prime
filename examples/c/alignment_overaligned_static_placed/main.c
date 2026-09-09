/* P64 (D-CSUBSET-THREAD-LOCAL-PE-OVERALIGN, static twin): the POSITIVE runtime
 * witness that an over-aligned STATIC object is PLACED, not refused — and that
 * it is placed CORRECTLY, on every leg this example targets.
 *
 * ⚠⚠ WHY THIS CARRIES EIGHT OBJECTS AND NOT ONE. A single over-aligned static
 * lands at the head of its section and is therefore right BY LUCK: ✔MEASURED in
 * P63, a one-object 8192 probe returned 42 while the real defect was live, and
 * the same defect showed up only once a multi-object subject pushed an object
 * PAST the section head. So every object here is separated from its neighbour by
 * an ODD-sized filler, and the two storage classes are exercised separately —
 * four in `.bss` (no initializer) and four in `.data` (initialized) — because a
 * PE image lays those out as two different sections with two different bases.
 *
 * ★ THE ASSERTION IS THE RUNTIME ADDRESS, never the absence of a diagnostic.
 * The failure this replaces was a CLEAN BUILD placing an object misaligned, so
 * an arm that only checks "the compile stopped erroring" would have passed
 * throughout. Each object's `address % N` is computed from the address the
 * LINKER chose; the alignment constants appear only in the request.
 *
 * ★ AND EACH OBJECT IS WRITTEN AND READ BACK through its far end, so a placement
 * that satisfied the modulus by OVERLAPPING a neighbour would still be caught.
 *
 * ✔THE REFERENCE, each probed SEPARATELY, BUILD **and** RUN with the runtime
 * address asserted (P64): mingw-w64 gcc 13.2.0 and MSVC 19.51 both return 42 on
 * this shape at 16 (control), 32, 4096 and 8192 — MSVC needs `/ALIGN:8192` at
 * the top value — and both refuse 16384 by name (gcc: "requested alignment
 * '16384' exceeds object file maximum 8192"; MSVC: `error C2345`). 8192 is the
 * largest value PE/COFF's four-bit IMAGE_SCN_ALIGN_* field can encode, which is
 * why both land there independently; `examples/c/alignment_static_exceeds_format`
 * is the negative half, one step above.
 *
 * ★★ THE MACH-O ARM IS arm64 ONLY, AND THE REASON IS A DECLARED NUMBER, not a
 * capability. P65 measured that Mach-O cannot honour a static aligned above its
 * image's own mapping granularity at all: `segment_command_64` carries NO
 * alignment field (`section_64.align` is read by the STATIC LINKER, never by
 * dyld), and a base-relative image is slid by a multiple of `segmentPageSize` —
 * so an above-page object keeps the address the linker chose and loses its
 * alignment at load, on about half of all runs. `image.segmentPageSize` is
 * 16384 on `macho64-arm64-darwin-exec` and 4096 on the x86_64 sibling, so this
 * program's 8192 request is PLACED on one and REFUSED BY NAME on the other, from
 * config alone. `examples/c/alignment_static_exceeds_macho_segment_page` is that
 * negative half, and it carries the same source at the same value.
 *
 * ✔MEASURED P65, the DSS-built arm64 image on real Apple Silicon (macOS 26.6.2):
 * `__data addr 0x10000c000` and `__bss addr 0x100012000`, both declaring
 * `align 2^13 (8192)`, and the program returns 42 TWENTY TIMES OUT OF TWENTY
 * with dyld placing __DATA at three different addresses across the runs. Twenty
 * because the failure this arm rules out is a coin flip on the loader's slide,
 * and one run of a coin flip is not a measurement.
 * ✔THE REFERENCE, the same day, same host, Apple clang 21.0.0 / ld
 * PROJECT:ld-1267: this shape returns 42 at 16 / 4096 / 16384; at 32768 ld64
 * WARNS "reducing alignment of section __DATA,__data from 0x8000 to 0x4000
 * because it exceeds segment maximum alignment" and the program then returns 50
 * or 54 BY RUN — never 42. The cap tracks the page (0x1000 on x86_64), and
 * `-Wl,-segalign,0x8000` does not lift it: ld64 refuses that link outright.
 * (D-LINK-MACHO-IMAGE-OVERALIGNED-STATIC-IS-A-LOAD-TIME-COIN-FLIP.)
 *
 * ⚠⚠ THE ELF LEGS WERE NOT ALREADY GREEN, AND THE CLAIM THAT THEY WERE IS WHAT
 * THIS COMMENT USED TO SAY. It read "the ELF legs are here because P63 measured
 * them placing the same objects correctly" — a measurement CARRIED FORWARD from
 * a ONE-OBJECT probe, and the very reason this example carries eight. ✔MEASURED
 * P64: this program returned 54 on `arm64:elf64-aarch64-linux-exec` (= 50 + 4 =
 * `d1`, the first `.data` object) from an image whose `.data` header declared
 * `sh_addralign = 8192` beside `sh_addr = 0x401000`. The ELF image writer chose
 * `.data`'s FILE OFFSET at the requested alignment and derived its ADDRESS by
 * adding the image base — which is only page-aligned — so every request above a
 * page was granted or refused by the accident of a DECLARED base address.
 * x86_64's is 0x400000 and divides 8192; arm64's is 0x3FF000 and does not.
 * ⇒ The legs are here because they are a DIFFERENT WRITER from the pe64 one and
 * this shape had never run on them; a carried-forward "measured correct" is not
 * a measurement of the subject in front of you.
 * (D-LINK-ELF-IMAGE-OVERALIGNED-DATA-PLACED-AT-ALIGNED-FILE-OFFSET.)
 *
 * Exit 42. RED-on-disable, pe64 leg: delete the `alignSectionHeadToItems` pass
 * in `pe::encodeExec` and this example still BUILDS CLEAN and returns 50 —
 * which is the whole point of asserting the address. On the ELF legs the
 * equivalent removal is the address-first placement in the ELF image writer;
 * `tests/link/test_elf_image_overaligned_section_placement` pins that half over
 * both ports and both image arms, because the two ports' declared bases make
 * the luck run in OPPOSITE directions and a single-port pin proves half of it.
 */

#define BIG   8192
#define PAGE  4096

/* --- .bss arm: no initializer, interleaved with odd-sized fillers --- */
__attribute__((aligned(BIG)))  static char b1[3];
static char bpad1[7];
__attribute__((aligned(BIG)))  static char b2[5];
static char bpad2[13];
__attribute__((aligned(PAGE))) static char b3[11];
static char bpad3[9];
__attribute__((aligned(BIG)))  static char b4[17];

/* --- .data arm: initialized, same shape --- */
__attribute__((aligned(BIG)))  static char d1[3]  = {1};
static char dpad1[7]  = {1};
__attribute__((aligned(BIG)))  static char d2[5]  = {1};
static char dpad2[13] = {1};
__attribute__((aligned(PAGE))) static char d3[11] = {1};
static char dpad3[9]  = {1};
__attribute__((aligned(BIG)))  static char d4[17] = {1};

int main(int argc, char **argv) {
    (void)argv;

    char *objs[8];
    unsigned long long wants[8];
    unsigned long long sizes[8];
    int i;

    objs[0] = b1; wants[0] = BIG;  sizes[0] = 3;
    objs[1] = b2; wants[1] = BIG;  sizes[1] = 5;
    objs[2] = b3; wants[2] = PAGE; sizes[2] = 11;
    objs[3] = b4; wants[3] = BIG;  sizes[3] = 17;
    objs[4] = d1; wants[4] = BIG;  sizes[4] = 3;
    objs[5] = d2; wants[5] = BIG;  sizes[5] = 5;
    objs[6] = d3; wants[6] = PAGE; sizes[6] = 11;
    objs[7] = d4; wants[7] = BIG;  sizes[7] = 17;

    /* keep the fillers alive so the layout cannot be collapsed */
    bpad1[0] = (char)argc; bpad2[0] = (char)argc; bpad3[0] = (char)argc;
    dpad1[0] = (char)argc; dpad2[0] = (char)argc; dpad3[0] = (char)argc;

    /* (1) every object sits on the address it asked for */
    for (i = 0; i < 8; ++i) {
        unsigned long long const a = (unsigned long long)(void *)objs[i];
        if (a % wants[i] != 0ull) return 50 + i;
    }

    /* (2) each object owns its own bytes, first AND last, with no overlap */
    for (i = 0; i < 8; ++i) {
        objs[i][0] = (char)(i + 1);
        objs[i][sizes[i] - 1] = (char)(100 + i);
    }
    for (i = 0; i < 8; ++i) {
        if (objs[i][0] != (char)(i + 1)) return 60 + i;
        if (objs[i][sizes[i] - 1] != (char)(100 + i)) return 70 + i;
    }

    /* (3) the fillers survived — a pad that swallowed one would show here */
    if (bpad1[0] != (char)argc) return 80;
    if (dpad3[0] != (char)argc) return 81;

    return 42;
}
