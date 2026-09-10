/* D-PP-EMBED-ANGLE (C23 6.10.4.1p8): end-to-end runtime witness for the ANGLE
 * form of `#embed`, RUN on the baseline (debug) AND the `release`
 * shippedPipeline arm.
 *
 * The resource `blob.bin` lives ONLY in `res/`, which the project manifest
 * declares as an include directory (`includes: ["res"]`, the file-driven
 * `-I`). It is deliberately NOT beside this file: the angle form searches the
 * same places the angle `#include` searches -- the system directories, then
 * the include directories -- and never the including file's own directory
 * (the 6.10.3p2 parity), so this program compiles ONLY through that search.
 *
 * The oracle is the ISO C23 text (N3220): gcc 13.3.0, clang 18.1.3 and MSVC
 * 19.51 all refuse `#embed` outright (MEASURED, P60).
 *
 * blob.bin is 12 bytes: 44 53 53 00 0D 0A 1A FE 7F 80 01 C3 (a NUL, CR, LF, SUB
 * and 0xFE among them -- the binary-read canaries). `suffix(, 0)` appends a
 * terminating zero (6.10.4.3p2), so the array has 13 elements.
 *
 * exit = sum(blob) & 0xFF = 220, reached only after the count and every byte
 * have been checked.
 */

int g_i = 0;   /* runtime-opaque index (anti-fold) */

static const unsigned char blob[] = {
#embed <blob.bin> suffix(, 0)
};

/* 6.10.2p7: __has_embed's angle form runs the same search as the directive. */
#if __has_embed(<blob.bin>) != __STDC_EMBED_FOUND__
#error "the angle form must resolve through the include directories (6.10.4.1p8)"
#endif
#if __has_embed(<blob.bin> limit(0)) != __STDC_EMBED_EMPTY__
#error "limit(0) makes the resource EMPTY in __has_embed too (6.10.2 EXAMPLE 6)"
#endif
#if __has_embed(<no_such_resource.bin>) != __STDC_EMBED_NOT_FOUND__
#error "an absent angle resource answers NOT_FOUND (6.10.2p7)"
#endif

int main(void) {
    static const unsigned char expect[12] = {
        0x44, 0x53, 0x53, 0x00, 0x0D, 0x0A, 0x1A, 0xFE, 0x7F, 0x80, 0x01, 0xC3
    };
    unsigned i;
    unsigned sum = 0;

    if (sizeof(blob) != 13) return 1;
    for (i = 0; i < 12; ++i) if (blob[i] != expect[i]) return 2;
    if (blob[12] != 0) return 3;

    for (i = 0; i < 13; ++i) sum += blob[i + g_i];
    return (int)(sum & 0xFF);   /* 220 */
}
