/* c62 (C 6.7.9p14, D-CSUBSET-STRING-LITERAL-ARRAY-ZERO-FILL): a STRING LITERAL
 * initializing a `char[N]` array zero-fills the trailing N-len bytes. This is the
 * sqlite `aXformType[]` (sqlite3.c:26283) shape — a `static const struct { u8 n;
 * char z[7]; ... } arr[]` whose rows carry names SHORTER than the 7-byte field —
 * plus the scalar `char s[7] = "hi"` the SAME mechanism fixes.
 *
 * SENSITIVE: the test reads BOTH the string bytes (must be correct) AND the
 * trailing padding bytes (must be ZERO). A wrong padding or an OOB read of
 * adjacent rodata (the latent miscompile Option A avoids by materializing the
 * rodata global PADDED to N) would leave a nonzero byte where C 6.7.9p14 mandates
 * zero -> the test returns a distinct non-42 code at the exact failing byte.
 * Success -> exit 42.
 *
 * RED-ON-DISABLE: revert the c62 fix and this no longer compiles —
 *   - the aggregate rows ("hour"/"day"/"month"/"year") -> H_VerifierFailure
 *     (ConstructAggregate child type Array<char,M> != field Array<char,7>);
 *   - the scalar `char s[7]="hi"` -> S0003 (got "hi").
 * Pure c-subset -> all 4 targets (the pe target RUNS on Windows).
 */
typedef unsigned char u8;

/* The faithful sqlite aXformType miniature: a static const array-of-struct whose
 * char[7] field is initialized by names of length 6 (exact fit), 4, 3, 5, 4. */
static const struct {
  u8   nName;
  char zName[7];
  int  rLimit;
} aXformType[] = {
  { 6, "second", 100 },   /* 6 chars + NUL = 7 -> exact fit (sameType) */
  { 4, "hour",   200 },   /* "hour"\0 + 2 trailing zero bytes */
  { 3, "day",    300 },   /* "day"\0  + 3 trailing zero bytes */
  { 5, "month",  400 },   /* "month"\0 + 1 trailing zero byte  */
  { 4, "year",   500 },   /* "year"\0 + 2 trailing zero bytes */
};

int main(void) {
  /* --- Scalar `char s[7] = "hi"`: 'h','i', then 5 zero-padding bytes. --- */
  char s[7] = "hi";
  if (s[0] != 'h') return 1;
  if (s[1] != 'i') return 2;
  if (s[2] != 0)   return 3;   /* implicit NUL */
  if (s[3] != 0)   return 4;   /* zero pad */
  if (s[4] != 0)   return 5;
  if (s[5] != 0)   return 6;
  if (s[6] != 0)   return 7;   /* last padding byte */

  /* --- aXformType[1] = "hour": bytes 0-3 'h','o','u','r', byte 4 NUL,
   *     bytes 5-6 zero padding. A wrong padding / OOB read shows here. --- */
  if (aXformType[1].zName[0] != 'h') return 10;
  if (aXformType[1].zName[1] != 'o') return 11;
  if (aXformType[1].zName[2] != 'u') return 12;
  if (aXformType[1].zName[3] != 'r') return 13;
  if (aXformType[1].zName[4] != 0)   return 14;   /* NUL */
  if (aXformType[1].zName[5] != 0)   return 15;   /* pad (OOB read would be nonzero) */
  if (aXformType[1].zName[6] != 0)   return 16;   /* pad */

  /* --- aXformType[2] = "day": shortest name, 3 trailing zero bytes. --- */
  if (aXformType[2].zName[0] != 'd') return 20;
  if (aXformType[2].zName[2] != 'y') return 21;
  if (aXformType[2].zName[3] != 0)   return 22;   /* NUL */
  if (aXformType[2].zName[4] != 0)   return 23;   /* pad */
  if (aXformType[2].zName[5] != 0)   return 24;   /* pad */
  if (aXformType[2].zName[6] != 0)   return 25;   /* pad */

  /* --- aXformType[3] = "month": 1 trailing zero byte (byte 6). --- */
  if (aXformType[3].zName[0] != 'm') return 30;
  if (aXformType[3].zName[4] != 'h') return 31;
  if (aXformType[3].zName[5] != 0)   return 32;   /* NUL */
  if (aXformType[3].zName[6] != 0)   return 33;   /* pad */

  /* --- aXformType[0] = "second": EXACT fit, all 7 bytes are the string +
   *     its NUL (no separate padding). byte 6 is the NUL. --- */
  if (aXformType[0].zName[0] != 's') return 40;
  if (aXformType[0].zName[5] != 'd') return 41;
  if (aXformType[0].zName[6] != 0)   return 44;   /* "second"'s own NUL */

  /* The neighbouring fields must be intact (the padding never bled into them). */
  if (aXformType[1].nName  != 4)   return 50;
  if (aXformType[1].rLimit != 200) return 51;
  if (aXformType[2].rLimit != 300) return 52;

  return 42;
}
