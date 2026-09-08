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
 * ⓘ NOT A MACH-O ARM, and the reason is ownership rather than doubt: the Mach-O
 * writer carries its own thread-local ceiling under its own anchor, and this
 * lane may not touch that file. The ELF legs are here because P63 measured them
 * placing the same objects correctly, so a regression there would be visible.
 *
 * Exit 42. RED-on-disable: delete the `alignSectionHeadToItems` pass in
 * `pe::encodeExec` and this example still BUILDS CLEAN and returns 50 on the
 * pe64 leg — which is the whole point of asserting the address.
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
