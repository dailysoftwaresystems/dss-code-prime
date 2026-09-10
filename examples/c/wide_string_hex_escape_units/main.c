// C11/C23 6.4.5 (D-CSUBSET-WIDE-HEX-OCTAL-ESCAPE-VALUE, STRING half): a `\x` hex
// or `\ooo` octal escape in a wide/UTF string literal names a RAW CODE-UNIT
// VALUE, and DSS assembles it directly instead of re-reading it as UTF-8.
//
// ★★ THIS IS THE EXAMPLE THAT REPLACES A SILENT MISCOMPILE. The original defect
// pushed each escape's byte through the UTF-8 re-decoder, so `u"\xC3\xA9"` — two
// escapes naming the units 0xC3 and 0xA9 — collapsed into ONE wrong 0x00E9 unit.
// A refusal stood in for the fix for several cycles. Both are now gone: the
// literal is TWO units, asserted below by VALUE, and by `sizeof` so the semantic
// tier's array length is pinned with it. ✔MEASURED 2026-09-02, gcc 13.3.0, clang
// 18.1.3, mingw-w64 gcc 13.2.0 and MSVC 19.51 each compiled and RUN with their
// emitted units read back: every value here is what all four produce.
//
// ★ THE CASES ARE CHOSEN AGAINST THE TEMPTING WRONG IMPLEMENTATION, which is to
// treat the escape as a code POINT and re-encode it. That shortcut passes
// `u"\xFFFF"` and fails every one of these:
//   u"\xD800"      a LONE SURROGATE — a legal unit, an illegal code point
//   U"\xFFFFFFFF"  PAST U+10FFFF — not a Unicode scalar value at all
//   u"\777"        octal above 255, an ordinary char16_t unit
//   u"\xF" "F"     phase 5 decodes per SEGMENT and phase 6 joins the bytes, so
//                  the escape must land at its rebased offset — 0x000F then
//                  0x0046, never a raw-token merge into `\xFF`
//   u"\xFF\u00E9"  an escape and a UCN in one body: the first is a raw unit, the
//                  second a code point, and they must not be routed through one
//                  another
//   u"\U0001F600"  an astral UCN still becomes a SURROGATE PAIR under char16_t —
//                  the pre-existing behaviour, unchanged by any of the above

// ⚠⚠ THE ONE DELIBERATE DIVERGENCE FROM A REFERENCE, PINNED HERE ON PURPOSE.
// For `u8"\xNN"` gcc, clang, mingw-w64 and ISO C 6.4.5 all make the escape ONE
// RAW BYTE, while MSVC 19.51 treats it as a CODE POINT and UTF-8-encodes it
// (`u8"\xFF"` → C3 BF, `u8"\xC3\xA9"` → C3 83 C2 A9). That is a split on what a
// program MEANS, so it was ruled rather than assumed: ISO C is a named vertex of
// the union and its text is explicit; MSVC is the sole outlier. The arms below
// exist so a later cycle applying "the union" by reflex cannot quietly flip it —
// they would go red, and the reader would come here and find this paragraph.

extern int puts(const char* s);

static const unsigned char  u8raw[]  = u8"\xFF";
static const unsigned char  u8two[]  = u8"\xC3\xA9";
static const unsigned short exact[]  = u"\xFFFF";
static const unsigned int   astral[] = U"\x1F600";
static const unsigned short pair[]   = u"\xC3\xA9";
static const unsigned short mixed[]  = u"a\xFFz";
static const unsigned short octal[]  = u"\777";
static const unsigned short surro[]  = u"\xD800";
static const unsigned int   full32[] = U"\xFFFFFFFF";
static const unsigned short split[]  = u"\xF" "F";
static const unsigned short withUcn[] = u"\xFF\u00E9";
static const unsigned short astUcn[]  = u"\U0001F600";

int main(void) {
    if (sizeof(u"\xFFFF") / 2 != 2)          return 1;
    if (exact[0] != 0xFFFFu || exact[1] != 0) return 2;
    if (sizeof(U"\x1F600") / 4 != 2)         return 3;
    if (astral[0] != 0x1F600u)               return 4;
    // The collapse case: TWO units, not one.
    if (sizeof(u"\xC3\xA9") / 2 != 3)        return 5;
    if (pair[0] != 0xC3u || pair[1] != 0xA9u || pair[2] != 0) return 6;
    // A byte escape must not disturb the ordinary characters around it.
    if (sizeof(u"a\xFFz") / 2 != 4)          return 7;
    if (mixed[0] != 'a' || mixed[1] != 0xFFu || mixed[2] != 'z') return 8;
    if (octal[0] != 0x1FFu)                  return 9;
    if (surro[0] != 0xD800u)                 return 10;
    if (full32[0] != 0xFFFFFFFFu)            return 11;
    if (sizeof(u"\xF" "F") / 2 != 3)         return 12;
    if (split[0] != 0x000Fu || split[1] != 0x0046u) return 13;
    if (sizeof(u"\xFF\u00E9") / 2 != 3)      return 14;
    if (withUcn[0] != 0xFFu || withUcn[1] != 0xE9u) return 15;
    if (sizeof(u"\U0001F600") / 2 != 3)      return 16;
    if (astUcn[0] != 0xD83Du || astUcn[1] != 0xDE00u) return 17;
    // The u8 ruling: ONE RAW BYTE, not a UTF-8-encoded code point.
    if (sizeof(u8"\xFF") != 2)                       return 18;
    if (u8raw[0] != 0xFFu || u8raw[1] != 0)          return 19;
    if (sizeof(u8"\xC3\xA9") != 3)                   return 20;
    if (u8two[0] != 0xC3u || u8two[1] != 0xA9u)      return 21;
    puts("wide");
    return 42;
}
