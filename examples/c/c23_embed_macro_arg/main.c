/* D-PP-EMBED-MACRO-ARG (C23 6.10.4.1p11): end-to-end runtime witness for the
 * MACRO-EXPANDED `#embed` operand, RUN on the baseline (debug) AND the
 * `release` shippedPipeline arm.
 *
 * 6.10.4.1p11: a `# embed pp-tokens new-line` that matches NEITHER of the two
 * literal forms has the tokens after `embed` "processed just as in normal
 * text" -- every macro name replaced -- and the directive that RESULTS shall
 * match one of the two forms. Three shapes below reach the same handler: an
 * operand that expands to a bare quoted name, one that expands to a name PLUS
 * a parameter, and one where the macro supplies the name and the line supplies
 * a parameter whose own clause holds another macro (6.10.4.2p3).
 *
 * The oracle is the ISO C23 text (N3220): gcc 13.3.0, clang 18.1.3 and MSVC
 * 19.51 all refuse `#embed` outright (MEASURED, P60).
 *
 * payload.bin is 16 bytes: 00 0D 0A 1A FF 10 20 30 40 50 60 70 80 90 A0 B0.
 *
 * exit = (sum(a) + sum(b) + sum(c)) & 0xFF = 133, reached only after every
 * count and every byte has been checked.
 */

#define RESOURCE            "payload.bin"
#define RESOURCE_WITH_LIMIT "payload.bin" limit(3)
#define LIM                 5

int g_i = 0;   /* runtime-opaque index (anti-fold) */

/* The operand expands to `"payload.bin"`: the whole resource. */
static const unsigned char a[] = {
#embed RESOURCE
};

/* The operand expands to `"payload.bin" limit(3)`: the first 3 bytes. */
static const unsigned char b[] = {
#embed RESOURCE_WITH_LIMIT
};

/* The macro supplies the name; the line supplies the parameters, whose limit
 * clause is itself macro-expanded (6.10.4.2p3): the first 5 bytes + 0xEE. */
static const unsigned char c[] = {
#embed RESOURCE limit(LIM) suffix(, 0xEE)
};

/* 6.10.2p5: the second `__has_embed` form is macro-expanded, then re-examined. */
#if __has_embed(RESOURCE) != __STDC_EMBED_FOUND__
#error "__has_embed(MACRO) must expand the operand and then answer FOUND (6.10.2p5)"
#endif
#if __has_embed(RESOURCE_WITH_LIMIT) != __STDC_EMBED_FOUND__
#error "a macro supplying name AND parameter must be re-examined as the first form"
#endif

int main(void) {
    static const unsigned char expect[16] = {
        0x00, 0x0D, 0x0A, 0x1A, 0xFF, 0x10, 0x20, 0x30,
        0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0
    };
    unsigned i;
    unsigned sum = 0;

    if (sizeof(a) != 16) return 1;
    if (sizeof(b) != 3)  return 2;
    if (sizeof(c) != 6)  return 3;

    for (i = 0; i < 16; ++i) if (a[i] != expect[i]) return 4;
    for (i = 0; i < 3; ++i)  if (b[i] != expect[i]) return 5;
    for (i = 0; i < 5; ++i)  if (c[i] != expect[i]) return 6;
    if (c[5] != 0xEE) return 7;

    for (i = 0; i < 16; ++i) sum += a[i];
    for (i = 0; i < 3; ++i)  sum += b[i];
    for (i = 0; i < 6; ++i)  sum += c[i + g_i];
    return (int)(sum & 0xFF);   /* 133 */
}
