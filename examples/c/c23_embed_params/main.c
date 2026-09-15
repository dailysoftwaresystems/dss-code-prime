/* D-PP-EMBED-PARAMS (C23 6.10.4.2 - 6.10.4.5): end-to-end runtime witness for
 * the standard `#embed` parameters, RUN on the baseline (debug) AND the
 * `release` shippedPipeline arm.
 *
 * The oracle is the ISO C23 text (N3220): gcc 13.3.0, clang 18.1.3 and MSVC
 * 19.51 all refuse `#embed` outright (MEASURED, P60), so no reference compiler
 * can compile this file -- every expectation below is cited to its clause.
 *
 * payload.bin is 40 bytes: {2A 0D 0A 1A 00 FF 80 7F} then 8..39. It is LONGER
 * than every `limit` used, so "limit applied" and "limit ignored" produce
 * different sizes and different bytes.
 *
 * ANTI-FOLD: g_i is a mutable global 0 (runtime-opaque), so the final reads
 * cannot be const-folded away -- the release arm proves the embedded arrays are
 * materialized and indexed at runtime.
 *
 * exit = (sum(head) + all[39] + four[3]) & 0xFF = 203, reached ONLY after every
 * element count and every byte has been checked (a wrong count or a wrong byte
 * returns a distinct small code first).
 */

int g_i = 0;

/* 6.10.4.2p4: limit(8) on a 40-byte resource embeds EXACTLY the first 8 bytes;
 * 6.10.4.4p2 / 6.10.4.3p2: prefix and suffix wrap a NON-empty expansion. */
static const unsigned char head[] = {
#embed "payload.bin" limit(8) prefix(0xAA, 0xBB,) suffix(, 0xCC)
};

/* 6.10.4.1p5: limit(0) makes the resource EMPTY; 6.10.4.5p2: if_empty then
 * REPLACES the whole directive; 6.10.4.4p3 / 6.10.4.3p3: prefix and suffix
 * have NO effect on an empty resource. */
static const unsigned char none[] = {
#embed "payload.bin" limit(0) prefix(1,) suffix(,2) if_empty(0x5A, 0x5A, 0x5A)
};

/* 6.10.4.2p4: a limit LARGER than the resource embeds the whole resource;
 * 6.10.4.5p2: if_empty is ignored for a non-empty one. */
static const unsigned char all[] = {
#embed "payload.bin" limit(1000) if_empty(9)
};

/* 6.10.1p5: `__limit__` behaves exactly like `limit`; 6.10.4.2p3 (EXAMPLE 2):
 * the clause is macro-expanded before it is evaluated. */
#define FOUR 2+2
static const unsigned char four[] = {
#embed "payload.bin" __limit__(FOUR)
};

/* 6.10.2p7 + EXAMPLE 6: __has_embed answers from the same parameters. */
#if __has_embed("payload.bin" limit(8) prefix(1) suffix(2)) != __STDC_EMBED_FOUND__
#error "supported parameters on a found, non-empty resource must answer FOUND"
#endif
#if __has_embed("payload.bin" limit(0)) != __STDC_EMBED_EMPTY__
#error "limit(0) makes the resource EMPTY in __has_embed too (6.10.2 EXAMPLE 6)"
#endif
#if __has_embed("payload.bin" vendor::unknown_parameter(1)) != __STDC_EMBED_NOT_FOUND__
#error "an unsupported prefixed parameter answers NOT_FOUND, never an error (6.10.2p8)"
#endif

/* 6.10.1p9 footnote 196 ("An unrecognized preprocessor PREFIXED parameter is a
 * constraint violation, EXCEPT within has_embed expressions") + 6.10.2p8 NOTE 1
 * ("...instead cause the expression to be evaluated to 0"), AND THE ORDER: the
 * vendor parameter decides BEFORE a sibling limit clause is evaluated. This is
 * C23 6.10.2 EXAMPLE 5's vendor guard crossed with EXAMPLE 6's limit; on an
 * implementation that evaluates the limit first, `limit(2 - 3)` is a 6.10.4.2p1
 * constraint violation and this file does not compile at all. */
#if __has_embed("payload.bin" ds9000::element_type(short) limit(2 - 3)) != __STDC_EMBED_NOT_FOUND__
#error "a vendor parameter answers 0 without evaluating a sibling limit (6.10.2p7)"
#endif
/* ...and the guarded #elif is the branch a portable program then takes. */
#if __has_embed("payload.bin" ds9000::element_type(short) limit(2 - 3))
#error "the vendor branch must not be taken by an implementation without it"
#elif !__has_embed("payload.bin")
#error "the fallback branch must see the plain resource (6.10.2 EXAMPLE 5)"
#endif

int main(void) {
    static const unsigned char expect_head[11] = {
        0xAA, 0xBB, 0x2A, 0x0D, 0x0A, 0x1A, 0x00, 0xFF, 0x80, 0x7F, 0xCC
    };
    unsigned i;
    unsigned sum = 0;

    if (sizeof(head) != 11) return 1;
    if (sizeof(none) != 3)  return 2;
    if (sizeof(all) != 40)  return 3;
    if (sizeof(four) != 4)  return 4;

    for (i = 0; i < 11; ++i) if (head[i] != expect_head[i])    return 5;
    for (i = 0; i < 3; ++i)  if (none[i] != 0x5A)              return 6;
    for (i = 0; i < 8; ++i)  if (all[i] != expect_head[i + 2]) return 7;
    for (i = 8; i < 40; ++i) if (all[i] != i)                  return 8;
    for (i = 0; i < 4; ++i)  if (four[i] != expect_head[i + 2]) return 9;

    for (i = 0; i < 11; ++i) sum += head[i];
    sum += all[39 + g_i];
    sum += four[3 - g_i];
    return (int)(sum & 0xFF);   /* 203 */
}
