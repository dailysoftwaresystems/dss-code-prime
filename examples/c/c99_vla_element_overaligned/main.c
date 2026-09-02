// C11/C23 6.7.5 + C99 6.7.6.2 (D-CSUBSET-VLA): a VARIABLE-LENGTH array whose ELEMENT
// type is OVER-ALIGNED — a member `alignas(32)`/`alignas(64)` raises the element's
// alignment above the 16-byte stack alignment the dynamic `sub sp` guarantees.
//
// THIS REPLACES `alignas_vla_element_overaligned_error`, WHOSE SUBJECT THIS IS AND
// WHICH PINNED THE REFUSAL. That example existed because a FIXED-size over-aligned local
// had just become honoured (D-CSUBSET-ALIGNAS-OVERALIGNED-STACK-LOCAL) while the VLA
// half stayed refused, and it named the refusal's premise: honouring it "needs the SIZE
// to carry the headroom and the captured base to be rounded". The premise was right and
// the work is what this example witnesses — the refusal was a gap, not a boundary. gcc
// 13.3.0 and clang 18.1.3, probed separately, both compile these shapes and hand back a
// genuinely aligned base.
//
// TWO INDEPENDENT PROPERTIES, AND THE SECOND IS THE ONE THAT IS EASY TO GET WRONG:
//
//  (1) THE BASE IS ALIGNED. A VLA's base is the post-`sub sp` stack pointer, which is
//      only as aligned as the stack; it has to be ROUNDED UP. Checked directly on every
//      element, since a correctly aligned base aligns them all (an element's size is a
//      multiple of its alignment).
//
//  (2) THE ARRAY STILL FITS IN WHAT WAS RESERVED. Rounding the base upward eats bytes
//      off the front of the reservation, so the SIZE must carry matching headroom or the
//      tail of the array lands on top of whatever was allocated before it. ✔MEASURED:
//      with headroom `elemAlign - stackAlignment` — the bound the FIXED-local mechanism
//      uses — two live VLAs here sat 120 bytes apart while the first was 128 bytes long,
//      an 8-BYTE OVERLAP OF TWO DISTINCT OBJECTS, because the post-`sub sp` SP on
//      x86_64/pe64 is congruent to 8 mod 16 rather than to 0 mod stackAlignment. The
//      reservation must therefore be `elemAlign`, which needs no premise about SP.
//
// ★ AND (2) IS ONLY VISIBLE IF EVERY BYTE IS WRITTEN. An earlier draft of this example
// used `struct { alignas(32) int head; int body[3]; }` and wrote only its named members
// — the overrun landed in the element's 16 bytes of trailing PADDING, nothing observed
// it, and the example passed over a live miscompile. `struct Over` below is 32 bytes of
// addressable ints with NO padding, and every one of them is written. The padded shape
// is still exercised (as `struct Pad`) because it is the shape the withdrawn example
// used, but it is not what carries the overlap witness.
//
// LAYOUT OF THE WITNESS: each VLA is allocated BELOW the previous one, so an overrun
// runs up into its predecessor. `guard` is declared first and re-read last; `av` sits
// above `ap`, so `ap`'s overrun would land in `av`. Every object is re-verified after
// every other object has been fully written.
//
// main is a LEAF (no calls) — a VLA function that calls anything is separately refused
// (L_VlaNonLeafFrameUnsupported, D-CSUBSET-VLA-NONLEAF-CALL-FRAME), so a sink call would
// measure THAT refusal instead of this capability. `volatile` defeats const-folding so
// the bounds are genuinely runtime. Each `return k` is a strict in-program pin.
//
// Red-on-disable: zero `alignHeadroom` in `lowerVlaAlloca` and (2) returns 9; drop the
// `emitAlignUpToPowerOfTwo` on the captured base and (1) returns 1.

struct Over {                  // 32 bytes, every byte addressable
    alignas(32) int cells[8];
};

struct Pad {                   // the withdrawn example's shape: 16 used, 16 padding
    alignas(32) int head;
    int body[3];
};

struct Wide {                  // headroom wider than one element
    alignas(64) int cells[16];
};

int main(void) {
    volatile int vn = 4;
    int n = vn;

    int guard[n * 8];                      // 128 bytes, allocated FIRST (highest)
    for (int i = 0; i < n * 8; ++i) guard[i] = 1000 + i;

    struct Over av[n];                     // 128 bytes, all addressable
    struct Pad  ap[n];                     // 128 bytes, 64 of them padding
    struct Wide aw[n];                     // 256 bytes, 64-byte alignment

    // (1) every element of every over-aligned array is aligned as its type demands.
    for (int i = 0; i < n; ++i) {
        if ((((unsigned long long)(void *)&av[i]) & 31ull) != 0ull) return 1;
        if ((((unsigned long long)(void *)&ap[i]) & 31ull) != 0ull) return 2;
        if ((((unsigned long long)(void *)&aw[i]) & 63ull) != 0ull) return 3;
    }

    // Write EVERY addressable byte of each array, tail included.
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < 8; ++j)  av[i].cells[j] = -1 - (i * 8 + j);
        ap[i].head    = 500 + i;
        ap[i].body[0] = 600 + i;
        ap[i].body[1] = 700 + i;
        ap[i].body[2] = 800 + i;
        for (int j = 0; j < 16; ++j) aw[i].cells[j] = 3000 + i * 16 + j;
    }

    // (2) NOTHING overran into anything allocated before it.
    for (int i = 0; i < n * 8; ++i)
        if (guard[i] != 1000 + i) return 9;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < 8; ++j)
            if (av[i].cells[j] != -1 - (i * 8 + j)) return 10;
    for (int i = 0; i < n; ++i) {
        if (ap[i].head != 500 + i) return 11;
        if (ap[i].body[0] != 600 + i) return 12;
        if (ap[i].body[1] != 700 + i) return 13;
        if (ap[i].body[2] != 800 + i) return 14;
    }
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < 16; ++j)
            if (aw[i].cells[j] != 3000 + i * 16 + j) return 15;

    // The headroom is a lowering detail and must never leak into `sizeof`.
    if (sizeof av != 128u) return 16;
    if (sizeof ap != 128u) return 17;
    if (sizeof aw != 256u) return 18;
    if (sizeof av[0] != 32u) return 19;
    if (sizeof aw[0] != 64u) return 20;

    return 42;
}
