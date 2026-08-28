/* D-CSUBSET-ENUM-GLOBAL-CODEGEN — an ENUM-TYPED object with STATIC storage
 * duration: the end-to-end RUNTIME witness on every target, and the registry
 * row's closing evidence (the row was opened 2026-07-17 from TF arc C6 probing).
 *
 * ⚠ WHAT THIS PINS, AND WHY IT IS NOT AN EXOTIC CORNER: before this example the
 * compiler REFUSED `enum E g = B;` at file scope outright —
 *   error[K_NoMatchingObjectFormat] lowerMirGlobalsToDataItems: global
 *   SymbolId={..} has TypeKind=24 — non-primitive global types ...
 * TypeKind 24 is Enum. The data-item producer dispatched on the DECLARED kind,
 * and `TypeKind::Enum` is a NOMINAL-IDENTITY marker with no width of its own
 * (C 6.7.2.2: an enum's representation IS its underlying integer's), so the
 * width lookup returned "not a scalar" and the global was rejected. gcc 13
 * -std=c2x and clang 18 -std=c23 both compile AND run every declaration below.
 *
 * EVERY SHAPE THE PRODUCER REACHES IS HERE, because the refusal was ONE gate
 * shared by all of them — covering a subset would have left the rest latent,
 * which is exactly how a multi-site defect survives a green suite:
 *   g_mut     mutable initialized enum scalar   -> writable .data
 *   g_const   const initialized enum scalar     -> .rodata
 *   g_tent    tentative (zero-init) enum scalar -> .bss, size reserved
 *   g_small   enum with a C23 FIXED underlying type (`: unsigned char`)
 *   g_arr     ARRAY of enum        -> the aggregate-leaf encoder
 *   g_pair    STRUCT with an enum member        -> aggregate leaf at an offset
 *   g_uni     UNION whose first member is an enum
 *   g_packed  a 1-byte-backed enum member at offset 1 of a 2-byte struct
 *
 * ★ THE EXIT CODE DISCRIMINATES BY CONSTRUCTION. It is the raw SUM of every
 * enumerator byte the image must carry, minus one constant:
 *   3 + 5 + 0 + 200 + 5 + 3 + 1 + 3 + 40 + 5 + 2 + 200 = 467,  467 - 425 = 42.
 * A global emitted as ZERO (the classic silent data-item bug) subtracts its own
 * value from the total and the exit moves; there is no term whose loss cancels.
 *
 * ★ g_small / g_packed.s are the WIDTH witnesses specifically. Their underlying
 * is `unsigned char`, so the producer must take the width from the enum's
 * underlying scalar and not from a hard-coded `int`. `g_packed` puts that 1-byte
 * enum at offset 1 of a 2-byte struct: a 4-byte write there overruns the item's
 * byte extent and fails LOUD at the layout-vs-encoder check, so "assume enums
 * are int" cannot pass this example quietly.
 *
 * ★ `g_arr[idx]` indexes with `argc` — an OS-supplied RUNTIME value — so at
 * least one enum-array element load survives the shipped `release` pipeline and
 * the array's BYTES must really be in the image, not const-folded out of it.
 *
 * All widths are data-model-INDEPENDENT (int 4 / unsigned char 1 under both LP64
 * and LLP64), so the single exit code holds on all four targets. */

enum E { A = 1, B = 3, C = 5 };
enum Small : unsigned char { S_LOW = 7, S_HIGH = 200 };

enum E       g_mut    = B;              /* 3   — mutable  -> .data              */
const enum E g_const  = C;              /* 5   — const    -> .rodata            */
enum E       g_tent;                    /* 0   — tentative-> .bss (size 4)      */
enum Small   g_small  = S_HIGH;         /* 200 — 1-byte underlying              */
enum E       g_arr[3] = { C, B, A };    /* 5,3,1 — aggregate-leaf enum elements */

struct Pair { enum E e; int n; };       /* e@0 (4B), n@4 (4B) — sizeof 8        */
struct Pair  g_pair   = { B, 40 };      /* 3, 40                                */

union U { enum E e; int n; };
union U      g_uni    = { C };          /* 5 — union first-member init          */

struct Packed { char tag; enum Small s; };  /* tag@0, s@1 — sizeof 2            */
struct Packed g_packed = { 2, S_HIGH };     /* 2, 200                           */

int main(int argc, char **argv) {
    (void)argv;
    int idx = argc;                     /* 1 at runtime; blocks const-folding   */

    int raw = (int)g_mut                /*   3 */
            + (int)g_const              /*   5 */
            + (int)g_tent               /*   0 */
            + (int)g_small              /* 200 */
            + (int)g_arr[0]             /*   5 */
            + (int)g_arr[idx]           /*   3 (runtime index)                  */
            + (int)g_arr[2]             /*   1 */
            + (int)g_pair.e             /*   3 */
            + g_pair.n                  /*  40 */
            + (int)g_uni.e              /*   5 */
            + (int)g_packed.tag         /*   2 */
            + (int)g_packed.s;          /* 200 */

    return raw - 425;                   /* 467 - 425 = 42 */
}
