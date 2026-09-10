/* D-CSUBSET-PACKED-BITFIELD-INTERACTION — the end-to-end witness that a `packed`
 * aggregate carrying BIT-FIELDS compiles, lays out to the references' bytes, and
 * runs.
 *
 * Until this cycle DSS REFUSED this program outright, twice: the semantic front
 * door `S_PackedBitfieldUnsupported` (now a RETIRED code) and a `packed &&
 * anyBitfield -> nullopt` belt in `computeLayout`'s struct AND union arms. The
 * refusal was a conformance divergence, not a guard — ✔MEASURED, probed
 * SEPARATELY, every reference accepts the construct:
 *   gcc 13.3.0 (x86_64-linux)      __attribute__((packed))     exit 42
 *   clang 18.1.3 (x86_64-linux)    __attribute__((packed))     exit 42
 *   mingw-w64 gcc 13.2.0 (Windows) __attribute__((packed))     exit 42
 *   cl.exe 19.51 (Windows)         #pragma pack(1)             exit 42
 * MSVC has no attribute spelling at all, so `#pragma pack(1)` is ITS form of the
 * same request — and DSS has supported that spelling, on this same code path,
 * since TF-C97. That equivalence is the whole story: the two spellings are
 * MEASURED byte-identical, so lifting the refusal was a DELETION rather than a
 * new packing algorithm.
 *
 * THE SHAPE IS CHOSEN SO ONE EXIT CODE COVERS ALL FOUR TARGETS. `#pragma pack` +
 * bit-fields is normally where the two shipped bit-field strategies diverge (see
 * examples/c/packed_bitfield_align, which is deliberately GNU-only for exactly
 * that reason). Here every bit-field's declared type is `unsigned char`, so its
 * allocation unit is ONE byte under gnu_packed AND under msvc_straddle, and no
 * field straddles: the two strategies land on the identical 4/1 layout. That is
 * checked, not assumed — all four references above are compiled and RUN.
 *
 * ANTI-FOLD: `arr` is a mutable file-scope array written at runtime and `one` is
 * `volatile`, so `&arr[one]` is a real address computation carrying the packed
 * STRIDE into generated code rather than a `sizeof` the const-folder answers.
 *
 * exit = sizeof(4) + _Alignof(1) + stride(4) + ver(5) + ihl(6) + flags(3)
 *      + ttl(17) + neighbours-intact(2) = 42.
 */

/* An IP-header-shaped record: two nibble bit-fields, an ordinary 16-bit member
 * that packing pulls back to byte 1, then two more bit-fields. */
struct Hdr {
    unsigned char  ver   : 4;
    unsigned char  ihl   : 4;
    unsigned short len;            /* packed: byte 1; unpacked: byte 2 */
    unsigned char  flags : 3;
    unsigned char  ttl   : 5;
} __attribute__((packed));

/* The UNPACKED twin, member-for-member identical. Its 6/2 is what `packed` is
 * being measured AGAINST — without it, 4/1 could be a number that happens to
 * match rather than evidence that the attribute did anything. */
struct Plain {
    unsigned char  ver   : 4;
    unsigned char  ihl   : 4;
    unsigned short len;
    unsigned char  flags : 3;
    unsigned char  ttl   : 5;
};

_Static_assert(sizeof(struct Hdr)    == 4, "packed drops the padding before `len`: 4");
_Static_assert(_Alignof(struct Hdr)  == 1, "packed drops the aggregate alignment: 1");
_Static_assert(sizeof(struct Plain)  == 6, "the unpacked twin pads to 6");
_Static_assert(_Alignof(struct Plain) == 2, "...and aligns to its widest member: 2");

static struct Hdr arr[3];

/* volatile: the index must not fold, so the stride below is computed at runtime
   from the laid-out element size. */
volatile int one = 1;

int main(void) {
    arr[0].ver = 5;  arr[0].ihl = 6;  arr[0].len = 1000; arr[0].flags = 1; arr[0].ttl = 2;
    arr[1].ver = 3;  arr[1].ihl = 4;  arr[1].len = 2000; arr[1].flags = 3; arr[1].ttl = 17;
    arr[2].ver = 9;  arr[2].ihl = 10; arr[2].len = 3000; arr[2].flags = 5; arr[2].ttl = 21;

    int const stride = (int)((char const *)&arr[one] - (char const *)&arr[0]);

    /* A packed element is 4 bytes, so element 1 begins where element 0 ends with
     * no gap to absorb a sloppy write. If a bit-field read-modify-write reached
     * past its own element, THESE are the bytes it would land in. */
    int const intact = (arr[1].len == 2000 ? 1 : 0) + (arr[2].ver == 9 ? 1 : 0);

    return (int)sizeof(struct Hdr)      /*  4 */
         + (int)_Alignof(struct Hdr)    /*  1 */
         + stride                       /*  4 */
         + arr[0].ver                   /*  5 */
         + arr[0].ihl                   /*  6 */
         + arr[1].flags                 /*  3 */
         + arr[1].ttl                   /* 17 */
         + intact;                      /*  2 */
}
