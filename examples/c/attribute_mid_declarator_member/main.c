// P63 (D-CSUBSET-ATTRIBUTE-MID-DECLARATOR-POSITION-REFUSED): a GNU attribute run
// between a struct/union member's TYPE HEAD and its DECLARATOR.
//
//   struct s { char pad; char __attribute__((__aligned__(16))) v; };
//
// ✔MEASURED before this cycle, through the shipped CLI:
//   `error[P_UnexpectedToken]: expected 'EndStatement' — got 'v'`
// while gcc 13.3.0, clang 18.1.3 and mingw-w64 gcc 13.2.0 all BUILD AND RUN it.
//
// ★★ THE TWO NEIGHBOURING POSITIONS ARE RETAINED HERE AS CONTROLS, and they are
// what make the isolation exact rather than merely suggestive: DSS already agreed
// with all three references on both, so if either moved, the change would be about
// something other than the position that was refused.
//   LEADING  — `__attribute__((...)) char v;`  rides `structMemberDeclSpecifiers`
//   TRAILING — `char v __attribute__((...));`  rides `structMemberAttrList`
//
// ★★★ WHAT A MID-DECLARATOR ATTRIBUTE DECORATES — MEASURED, AND THE ANCHOR ROW'S
// OWN PRESCRIPTION WAS WRONG. The row said it "binds to the DECLARATOR it
// precedes". A TWO-declarator subject refutes that, and only a two-declarator
// subject can, because the two candidate answers give DIFFERENT numbers:
//     per-declarator (the row's claim) : a@0  b@1   sizeof 16
//     per-declaration (measured)       : a@0  b@16  sizeof 32
// ✔MEASURED 2026-09-07, gcc 13.3.0 AND clang 18.1.3 AGREEING, undecorated control
// `struct { char a, b; }` = a@0 b@1 sizeof 2 on both: the SECOND declarator is
// aligned too. The file-scope twin `char __attribute__((aligned(16))) a, b;`
// aligns BOTH on both references — and that is DSS's already-shipped `declAttrRun`
// slot with `appertainsTo: declaration`, which is exactly the rule this position
// now reuses. One attribute must not mean two things on two sides of a declarator.
//
// ⓘ RESIDUE, named and still loud: the MID-LIST position (a declarator OTHER than
// the first — `char a, __attribute__((aligned(16))) b;`) is a DIFFERENT slot. gcc
// REFUSES it and clang accepts it; DSS still refuses, and a unit test pins that so
// the refusal cannot rot into a silent drop.

struct mid   { char pad; char __attribute__((__aligned__(16))) v; };
struct lead  { __attribute__((__aligned__(16))) char v; char pad; };
struct trail { char pad; char v __attribute__((__aligned__(16))); };
struct pair  { char __attribute__((__aligned__(16))) a, b; };
struct plain { char pad; char v; };
union  umid  { char pad; char __attribute__((__aligned__(16))) v; };

static struct mid  g_mid;
static struct pair g_pair;

static unsigned long long addr_of(const void *p) {
    return (unsigned long long)p;
}

int main(int argc, char **argv) {
    (void)argv;

    /* THE SUBJECT: the alignment reaches the LAYOUT, not merely the parser. The
       decorated member follows a `char`, so its offset and the aggregate size both
       move; a parse-and-drop leaves them at 1 and 2. */
    if (__builtin_offsetof(struct mid, pad) != 0u)  return 1;
    if (__builtin_offsetof(struct mid, v)   != 16u) return 2;
    if (sizeof(struct mid)   != 32u) return 3;
    if (_Alignof(struct mid) != 16u) return 4;

    /* THE UNDECORATED CONTROL — the same member shape with no attribute. Without
       it the four numbers above are consistent with "this struct is always 32". */
    if (__builtin_offsetof(struct plain, v) != 1u) return 5;
    if (sizeof(struct plain)   != 2u) return 6;
    if (_Alignof(struct plain) != 1u) return 7;

    /* CONTROL 1 — the TRAILING position, unchanged by this cycle. */
    if (__builtin_offsetof(struct trail, v) != 16u) return 8;
    if (sizeof(struct trail)   != 32u) return 9;
    if (_Alignof(struct trail) != 16u) return 10;

    /* CONTROL 2 — the LEADING position, unchanged by this cycle. Its member ORDER
       differs (the decorated member is FIRST), so its sizeof is 16, not 32; that
       difference is the reason the reference probe returns a different code for it
       and all four toolchains agree on it. */
    if (__builtin_offsetof(struct lead, v)   != 0u) return 11;
    if (__builtin_offsetof(struct lead, pad) != 1u) return 12;
    if (sizeof(struct lead)   != 16u) return 13;
    if (_Alignof(struct lead) != 16u) return 14;

    /* THE BINDING. Per-declarator binding would put `b` at 1 with sizeof 16. */
    if (__builtin_offsetof(struct pair, a) != 0u)  return 15;
    if (__builtin_offsetof(struct pair, b) != 16u) return 16;
    if (sizeof(struct pair)   != 32u) return 17;
    if (_Alignof(struct pair) != 16u) return 18;

    /* THE UNION HALF — `unionField` is a separate config row and so a separate
       omission; a language where struct honours this and union does not is one no
       reference implements. */
    if (_Alignof(union umid) != 16u) return 19;
    if (sizeof(union umid)   != 16u) return 20;

    /* And the addresses really are 16-aligned at run time — the linker's answer,
       not the type metadata the checks above read. */
    if (addr_of(&g_mid.v)  % 16ull) return 21;
    if (addr_of(&g_pair.a) % 16ull) return 22;
    if (addr_of(&g_pair.b) % 16ull) return 23;

    g_mid.v  = (char)argc;        /* argc is a runtime value */
    g_pair.a = (char)(argc + 1);
    g_pair.b = (char)(argc + 2);
    int const score = (int)g_mid.v + (int)g_pair.a + (int)g_pair.b;  /* 6 */
    if (score != 6) return 24;
    return score * 8 - 6;   /* 42, derived from argc */
}
