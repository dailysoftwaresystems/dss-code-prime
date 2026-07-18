/* C14 (C 6.7.9p14 + 6.2.5p15, D-CSUBSET-STRING-LITERAL-ARRAY-ZERO-FILL extended
 * to the signed/unsigned character types): a string literal may initialize an
 * array of ANY character type — plain `char`, `signed char`, AND `unsigned
 * char` (C 6.2.5p15: the three character types). Before C14 only a plain
 * `char[N]` was admitted; `unsigned char[N] = "…"` / `signed char[N] = "…"`
 * raised S0003, and the aggregate form raised H_VerifierFailure.
 *
 * This is the real sqlite shape: test_hexio's `const unsigned char zHex[] =
 * "0123456789ABCDEF";` and test_func's charmap `unsigned char` tables.
 *
 * SENSITIVE: reads BOTH the string bytes (must be correct) AND the trailing
 * zero-fill (must be ZERO — a wrong pad or an OOB read of adjacent memory shows
 * a nonzero byte where C mandates zero → a distinct non-42 exit at the failing
 * byte). The `hex`/`sc` arrays are LOCALS (runtime byte-wise init from the
 * retyped rodata literal), so the release pipeline exercises the real init copy
 * + loads, not a folded constant. exit 42 == every byte verified.
 *
 * RED-ON-DISABLE: revert the C14 widening (type_rules stringLiteralArrayInit-
 * Compatible back to char-on-both-sides) and this no longer compiles —
 *   - `unsigned char hex[18] = "…"` / `signed char sc[6] = "abc"` → S0003;
 *   - the aggregate `unsigned char zTag[7]` rows → H_VerifierFailure.
 * Pure c-subset → all 4 targets; the pe target RUNS on Windows.
 */
typedef unsigned char u8;

/* An unsigned-char[7] field inside a static const aggregate, initialized by
 * names SHORTER than 7 (the C 6.7.9p14 zero-fill through the aggregate producer). */
static const struct {
  u8            nName;
  unsigned char zTag[7];
  int           rID;
} aTags[] = {
  { 3, "day",   300 },   /* "day"\0  + 3 trailing zero bytes */
  { 5, "month", 400 },   /* "month"\0 + 1 trailing zero byte  */
};

int main(void) {
  /* --- LOCAL unsigned char array from a narrow literal (runtime init copy).
   *     "0123456789ABCDEF" = 16 chars → char[17]; N=18 → NUL at 16, pad at 17. --- */
  unsigned char hex[18] = "0123456789ABCDEF";
  if (hex[0]  != '0') return 1;
  if (hex[10] != 'A') return 2;
  if (hex[15] != 'F') return 3;
  if (hex[16] != 0)   return 4;   /* implicit NUL */
  if (hex[17] != 0)   return 5;   /* trailing zero pad */

  /* --- LOCAL signed char array: 'a','b','c', NUL, then 2 zero-pad bytes. --- */
  signed char sc[6] = "abc";
  if (sc[0] != 'a') return 6;
  if (sc[2] != 'c') return 7;
  if (sc[3] != 0)   return 8;    /* NUL */
  if (sc[5] != 0)   return 9;    /* zero pad */

  /* --- AGGREGATE unsigned char[7] field, zero-fill (aggregate producer path). --- */
  if (aTags[0].zTag[0] != 'd') return 10;
  if (aTags[0].zTag[2] != 'y') return 11;
  if (aTags[0].zTag[3] != 0)   return 12;   /* NUL */
  if (aTags[0].zTag[6] != 0)   return 13;   /* pad (an OOB read would be nonzero) */
  if (aTags[1].zTag[0] != 'm') return 14;
  if (aTags[1].zTag[4] != 'h') return 15;
  if (aTags[1].zTag[6] != 0)   return 16;   /* pad */
  if (aTags[0].rID   != 300)   return 17;   /* neighbouring field intact */
  if (aTags[1].nName != 5)     return 18;

  /* Accumulate from RUNTIME-loaded bytes so the release pipeline cannot fold the
   * whole function to a constant 42 pre-codegen. Every difference is exactly 0. */
  int acc = (hex[1] - '1') + (sc[1] - 'b') + ((int)aTags[1].zTag[1] - 'o');
  if (acc != 0) return 60;

  return 42;
}
