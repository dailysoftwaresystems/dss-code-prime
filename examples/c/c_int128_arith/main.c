/* `__int128` / `unsigned __int128` — the 128-bit STANDARD-RANK integer type
 * (D-CSUBSET-UINT128-TYPE). This is the runnable twin of the `_BitInt(128)` corpus:
 * SAME 2-limb multi-limb substrate, DIFFERENT type identity and DIFFERENT alignment.
 *
 * WHY THIS EXAMPLE EXISTS — it re-implements the ACTUAL shapes of sqlite's
 * `sqlite3Multiply128` / `sqlite3Multiply160` (sqlite/src/util.c, the
 * `SQLITE_USE_UINT128` arm), because those two functions are what the corpus probe
 * broke on. The load-bearing shape is a 128-bit value narrowed to a 64-bit scalar
 * IN A FUNCTION-RETURN position — `return (u64)(r>>64);` and its cast-less twin
 * `return r>>64;` — reached from a HELPER function, not from `main`.
 *
 * WHAT BREAKS THE 42 (each `t` term is boolean-gated; any one wrong ⇒ sum ≠ 42):
 *  - t1: a `(u64)(r>>64)` return that drops the wide→narrow truncation, or reads the
 *        LOW limb where the HIGH one is meant. Both operands are seeded so the product
 *        has a NON-ZERO HIGH WORD — a product that fits in 64 bits passes even with a
 *        completely broken high limb, which is the whole trap this term avoids.
 *  - t2: the `sqlite3Multiply160` shape — a wide `+=`, a `>>32` middle-word extract
 *        truncated to `u32` THROUGH A POINTER, and an IMPLICIT (cast-less) wide→`u64`
 *        return. A narrowing applied only at explicit `(T)` casts breaks the return; a
 *        `+=` that drops the carry INTO the high limb breaks the middle word.
 *  - t3: a BY-VALUE 128-bit parameter and 128-bit return (`shl64`), an ARRAY ELEMENT
 *        and a STRUCT MEMBER holding 128-bit values. A by-value 128-bit arg passed as
 *        one register, or an aggregate-width Load/Store, keeps only the low 8 bytes.
 *  - t4: truncating casts to `u64` AND to `u32`, from limb 0 and from `>>32` / `>>64`.
 *        A cast that ignores the shift, or truncates to the wrong width, breaks these.
 *  - t5: the SIGNED `__int128` counterpart — a negative value, `/`, `%`, an ARITHMETIC
 *        `>>` (sign-propagating), and `<`. An unsigned-for-signed shift or compare, or
 *        a `%` that does not take the dividend's sign (C99 6.5.5p6), breaks it.
 *  - t6: the WIDTH probes, in their NON-VACUOUS form (see the vacuity note below).
 *  - t7: the four 64-bit C SPELLINGS — `unsigned long long`, `unsigned long`,
 *        `long long`, `long` — each narrowed from a 128-bit value in a HELPER RETURN.
 *        MEASURED: this is exactly the class that regressed. C spells FOUR distinct
 *        64-bit type names over only TWO kinds (I64, U64), so the interner hands out a
 *        separate TypeId per spelling and `interner.primitive(kind)` is none of them;
 *        a narrowing typed from the KIND instead of the DECLARED TypeId walls with
 *        I_TerminatorTypeMismatch. The sub-64-bit widths do NOT cover this — C gives
 *        those kinds exactly one spelling each, so their declared TypeId happens to BE
 *        the canonical primitive and they pass even while these four are broken.
 *
 * VACUITY NOTE (MEASURED): `(__uint128_t)-1 == 0xFFFFFFFFFFFFFFFFull` is TRUE when the
 * type is mistyped as a 64-bit integer and FALSE for a real 128-bit type — so asserting
 * it TRUE would pass on a broken build. t6 therefore probes the HIGH word directly
 * (`uAll >> 64 == ~0ull`) and the SIGN (`(signed __int128)-1 < 0`), and keeps the trap
 * form only as an explicitly-labelled NEGATIVE control (asserted FALSE).
 *
 * Every operand is seeded through `volatile`, so no const-fold — present or future —
 * can pre-evaluate the 128-bit arithmetic and mask a broken multi-limb implementation.
 * Data-model agnostic: every `long` / `unsigned long` expectation is written as a CAST
 * of the full 64-bit constant, so the LLP64 (Windows, 32-bit `long`) and LP64 legs both
 * hold without a per-target constant. */

/* ── COMPILE-TIME block. Kept as bare `_Static_assert` declarations at file scope and
 * deliberately NOT wired into the exit code: a failed `_Static_assert` aborts
 * translation, so a term computed FROM one could never report a runtime failure. The
 * same three layout facts are independently pinned in C++ by
 * TEST(TypeLayout, Int128AndBitInt128AreIndependentLayouts) and
 * TEST(TypeLayout, Int128AggregateLayoutVsBitInt128), which no translation abort here
 * can mask. ────────────────────────────────────────────────────────────────────────── */

_Static_assert(sizeof(__uint128_t) == 16, "sizeof(unsigned __int128) == 16");
_Static_assert(sizeof(__int128)    == 16, "sizeof(__int128) == 16");

/* ★ THE discriminator against a `_BitInt(128)` binding. `_BitInt(128)` is size 16 /
 * align EIGHT (x86-64 psABI: a bit-precise type aligns to its LIMB, pinned from real C
 * by examples/c/c23_bitint_wide/main.c:24); `__int128` is size 16 / align
 * SIXTEEN (ordinary natural alignment of a 16-byte scalar). If `__uint128_t` were ever
 * bound to `_BitInt(128)`, THIS assert — and nothing else in the size checks — turns
 * red. The two must stay independent in BOTH directions: the align-8 side is pinned
 * over in c23_bitint_wide, and it must STAY 8. */
_Static_assert(_Alignof(__uint128_t) == 16, "_Alignof(unsigned __int128) == 16");
_Static_assert(_Alignof(__int128)    == 16, "_Alignof(__int128) == 16");

/* The align-16 fact PROPAGATED THROUGH AN AGGREGATE — non-vacuous by MEASUREMENT: the
 * `_BitInt(128)` twin of this struct is 520 bytes, not 528. 32*16 = 512 bytes of array,
 * + 8 bytes of `unsigned` pair = 520, rounded UP to the struct's 16-byte alignment =
 * 528. At align 8 (the `_BitInt(128)` rule) the round-up is a no-op and it stays 520,
 * so this single number discriminates the two layouts through a real aggregate.
 * ⚠ MEASURED TRAPS, both worked around here rather than hidden:
 *   - `sizeof(__uint128_t[32])` does NOT parse (P0009 — an abstract array declarator in
 *     `sizeof` is not yet accepted, for `_BitInt` equally); a typedef'd array name is.
 *   - a struct DEFINED inline inside `sizeof` does not parse either (P0009) — the type
 *     must be declared separately, as it is here. */
typedef __uint128_t U128Array32[32];
_Static_assert(sizeof(U128Array32) == 512, "32 * sizeof(unsigned __int128) == 512");

struct N { __uint128_t v[32]; unsigned f1, f2; };
_Static_assert(sizeof(struct N) == 528,
               "the 16-byte alignment propagates through the aggregate (a "
               "_BitInt(128) member would make this 520)");

/* ── A 128-BIT CONSTANT NARROWED BY AN EXPLICIT CAST, IN AN INTEGER-CONSTANT-
 * EXPRESSION CONTEXT (D-CSUBSET-INT128-ICE-CONTEXT-REFUSED). ✔MEASURED at
 * 301e2a63: every line below was `S0029 not an integer constant expression`
 * while clang 18.1.3 (`-std=c23`) and gcc 13.3.0 (`-std=c2x`), probed
 * SEPARATELY, folded all of them — and the `_BitInt` twin two lines down was
 * ALREADY clean, which is what showed the two arms of one law had drifted.
 * ⚠ THE LAST ONE IS THE LOAD-BEARING CELL: the operand does NOT fit 64 bits, so
 * it pins that an EXPLICIT narrowing is the DEFINED modular conversion (C
 * 6.3.1.3p2) rather than a refusal — while a BARE wide value used as a bound
 * (`int a[(__uint128_t)1 << 100];`) must STILL fail loud, which it does. */
_Static_assert((int)((__uint128_t)5) == 5, "wide -> int in an ICE");
_Static_assert((unsigned long long)((__uint128_t)5) == 5, "wide -> u64 in an ICE");
_Static_assert((_BitInt(8))((__uint128_t)5) == 5, "the _BitInt twin, already clean");
_Static_assert((unsigned long long)(((__uint128_t)1 << 100) + 7) == 7,
               "an explicit narrowing of a value that does NOT fit 64 bits is "
               "C 6.3.1.3p2's modular conversion, not a refusal");

/* ── RUNTIME block. Nothing below reads a `_Static_assert`. ───────────────────────── */

typedef unsigned long long u64;
typedef unsigned int       u32;

/* ★ sqlite3Multiply128 (sqlite/src/util.c, SQLITE_USE_UINT128 arm) — VERBATIM shape.
 * `return (u64)(r>>64);` is the exact expression the corpus probe walled on. */
static u64 mul128(u64 a, u64 b, u64 *pLo) {
    __uint128_t r = (__uint128_t)a * b;
    *pLo = (u64)r;                       /* pointer store of a truncating cast */
    return (u64)(r >> 64);               /* ← the failing shape */
}

/* ★ sqlite3Multiply160 (same file) — VERBATIM shape. Adds a wide `+=`, a `>>32`
 * middle-word extract stored through a `u32*`, and a CAST-LESS wide→`u64` return
 * (the implicit conversion, which must narrow exactly as the explicit cast does). */
static u64 mul160(u64 a, u32 aLo, u64 b, u32 *pLo) {
    __uint128_t r = (__uint128_t)a * b;
    r += ((__uint128_t)aLo * b) >> 32;   /* wide compound-assign, carries into limb 1 */
    *pLo = (r >> 32) & 0xffffffff;       /* >>32 + truncation to u32 through a pointer */
    return r >> 64;                      /* implicit (cast-less) wide → u64 */
}

/* A BY-VALUE 128-bit parameter AND a 128-bit return — the two-GPR by-value ABI. */
static __uint128_t shl64(__uint128_t x) { return x << 64; }

/* The four 64-bit C SPELLINGS, each narrowed from a 128-bit value in a helper RETURN
 * (t7). Four separate functions on purpose: the return position is what walled, and
 * `long`/`long long` (and their unsigned twins) are DISTINCT declared types over the
 * same two kinds. */
static unsigned long long hiAsULongLong(__uint128_t r) { return (unsigned long long)(r >> 64); }
static unsigned long      hiAsULong    (__uint128_t r) { return (unsigned long)(r >> 64); }
static long long          hiAsLongLong (__int128 r)    { return (long long)(r >> 64); }
static long               hiAsLong     (__int128 r)    { return (long)(r >> 64); }

struct Box { __uint128_t v; u64 lo; };

/* ── t9: THE MATERIALIZATION SITES THE FIRST ROSTER LEFT OUT ─────────────────────
 * D-CSUBSET-INT128-NARROWING-CAST-SITE-INCOMPLETE lists SEVEN contexts a wide value
 * can be narrowed into. t1..t7 above cover return, assignment, initializer, pointer
 * store, struct MEMBER store and array-element store. The four below are the rest —
 * CALL ARGUMENT, COMPOUND ASSIGNMENT, STRUCT BRACE-INITIALIZER and TERNARY — and
 * they are here because the row's own lesson is that a suite over a SUBSET of a
 * multi-site contract is not proof of the contract, which is exactly how the defect
 * shipped. Every one uses a 64-bit SPELLING (`unsigned long long` / `unsigned long`
 * / `long`), because those are the four names over two kinds that made the declared
 * TypeId differ from the canonical primitive; the sub-64-bit widths cannot see it.
 *
 * ★ THE TERNARY ARM ALSO CARRIES THE `(_Bool)` WITNESS, and it is the shape that
 * found the defect: `c ? (_Bool)(w >> 64) : (_Bool)0` used to wall with
 * `L_UnsupportedLoweringForOpcode Trunc result: TypeKind ordinal 0`, because the
 * `(_Bool)0` arm took HIR→MIR's `mapCast` — which classifies `_Bool` as an 8-bit
 * integer, so an int→`_Bool` cast became a WIDTH TRUNCATION keeping only the low
 * bit. `wHi` here is 2 (an EVEN number, low bit 0) and its truth value is TRUE, so
 * a truncating `(_Bool)` answers false and this term goes to 0. */
static u64  sinkULL(unsigned long long v) { return (u64)v; }
struct Halves { unsigned long hi; long shi; };

static int wideSiteMatrix(__uint128_t w, __int128 sn, int cond) {
    /* CALL ARGUMENT */
    u64 const viaArg = sinkULL((unsigned long long)(w >> 64));
    /* COMPOUND ASSIGNMENT into a 64-bit spelling */
    unsigned long long acc = 1ull;
    acc += (unsigned long long)(w >> 64);
    /* STRUCT BRACE-INITIALIZER, both signednesses */
    struct Halves const h = { (unsigned long)(w >> 64), (long)(sn >> 64) };
    /* TERNARY, whose two arms must agree on the narrowed type */
    unsigned long long const tv = cond ? (unsigned long long)(w >> 64) : 0ull;
    /* `(_Bool)` of a WIDE value whose LOW LIMB IS ZERO, and of an EVEN scalar —
     * both are TRUE, and both are false under a low-bit truncation. */
    _Bool const bWide = (_Bool)(w & ~(__uint128_t)0xFFFFFFFFFFFFFFFFull);
    _Bool const bEven = (_Bool)(int)(w >> 64);
    return (viaArg == 2ull && acc == 3ull
            && h.hi == (unsigned long)2ull && h.shi == (long)-3
            && tv == 2ull && bWide == 1 && bEven == 1) ? 1 : 0;
}

int main(void) {
    /* Runtime seeds — every 128-bit operand descends from one of these, so no
     * const-fold (present or future) can pre-evaluate the wide arithmetic. */
    volatile u64 sMax  = 0xFFFFFFFFFFFFFFFFull;   /* 2^64 - 1 */
    volatile u64 s3    = 3;
    volatile u64 sZero = 0;
    volatile u32 sHalf = 0x80000000u;             /* 2^31 */
    volatile int s1    = 1;
    volatile int s7    = 7;

    /* ── t1: the sqlite3Multiply128 shape. (2^64-1) * 3 = 3*2^64 - 3, i.e. a product
     * whose HIGH word is 2 (NON-ZERO — a 64-bit-fitting product would pass even with a
     * dead high limb) and whose low word is 2^64-3. ─────────────────────────────── */
    u64 lo1 = 0;
    u64 hi1 = mul128(sMax, s3, &lo1);
    int t1 = (hi1 == 2ull && lo1 == 0xFFFFFFFFFFFFFFFDull) ? 6 : 0;

    /* ── t2: the sqlite3Multiply160 shape. r starts at 3*2^64 - 3; the `+=` adds
     * (2^31 * 3) >> 32 == 1, giving 3*2^64 - 2. Middle word = (r>>32) & 0xffffffff =
     * 0xFFFFFFFF; the cast-less return yields the high word 2. ─────────────────── */
    u32 mid2 = 0;
    u64 hi2  = mul160(sMax, sHalf, s3, &mid2);
    int t2 = (hi2 == 2ull && mid2 == 0xFFFFFFFFu) ? 6 : 0;

    /* ── t3: by-value 128-bit param + return, an ARRAY ELEMENT and a STRUCT MEMBER.
     * arr[0] = 3*2^64 - 3 (high word 2); arr[1] = 3 << 64 (high word 3, low 0) built
     * through the by-value helper. Their sum has high word 5, low word 2^64-3. ─── */
    __uint128_t arr[2];
    arr[0] = (__uint128_t)sMax * s3;
    arr[1] = shl64((__uint128_t)s3);
    struct Box box;
    box.v  = arr[0] + arr[1];
    box.lo = (u64)box.v;                          /* struct member ← truncating cast */
    int t3 = (box.lo == 0xFFFFFFFFFFFFFFFDull
              && (u64)(box.v >> 64) == 5ull
              && (u64)(arr[1] >> 64) == 3ull
              && (u64)arr[1] == 0ull) ? 6 : 0;

    /* ── t4: truncating casts to u64 AND u32, from limb 0 and from >>32 / >>64 of a
     * value with a NON-ZERO high word (w = 3*2^64 - 3). ────────────────────────── */
    __uint128_t w = (__uint128_t)sMax * s3;
    u64 c64 = (u64)w;                             /* 0xFFFFFFFFFFFFFFFD */
    u32 c32 = (u32)w;                             /* 0xFFFFFFFD */
    u32 m32 = (u32)(w >> 32);                     /* 0xFFFFFFFF */
    u64 h64 = (u64)(w >> 64);                     /* 2 */
    int t4 = (c64 == 0xFFFFFFFFFFFFFFFDull && c32 == 0xFFFFFFFDu
              && m32 == 0xFFFFFFFFu && h64 == 2ull) ? 6 : 0;

    /* ── t5: the SIGNED __int128 counterpart. sn = -(3*2^64 - 3), a NEGATIVE value
     * whose magnitude needs both limbs. sn / 3 = -(2^64-1) (does not fit in any 64-bit
     * type, so the quotient is compared AS __int128). sn % 7 = -3: 2^64 ≡ 2 (mod 7) so
     * 3*(2^64-1) ≡ 3, and C99 6.5.5p6 gives the remainder the DIVIDEND's sign. The
     * arithmetic `sn >> 64` is -3 (floor), NOT the 2^64-2 a logical shift would give. */
    __int128 sn = -((__int128)sMax * (__int128)s3);
    __int128 sq = sn / (__int128)s3;
    __int128 sr = sn % (__int128)s7;
    __int128 sa = sn >> 64;
    int t5 = (sn < 0 && !(sn > 0)
              && sq == -(__int128)sMax
              && sr == -(__int128)s3
              && sa == -(__int128)s3) ? 6 : 0;

    /* ── t6: the WIDTH probes, NON-VACUOUS forms. `uAll` is 2^128-1 built from a
     * volatile zero (so it is a runtime value, not a literal the folder can shortcut).
     * p1 reads the HIGH word; p2 reads the SIGN of a signed -1.
     * p3 is the NEGATIVE CONTROL: `uAll == (2^64 - 1)` is what a 64-bit-mistyped build
     * answers TRUE — a correct 128-bit type answers FALSE, so it is asserted FALSE. */
    __uint128_t uAll  = ~(__uint128_t)sZero;                  /* 2^128 - 1 */
    __int128    sNeg1 = -(__int128)s1;                        /* -1 */
    int p1 = ((u64)(uAll >> 64) == 0xFFFFFFFFFFFFFFFFull);
    int p2 = (sNeg1 < 0);
    int p3 = !(uAll == (__uint128_t)0xFFFFFFFFFFFFFFFFull);   /* NEGATIVE control */
    int t6 = (p1 && p2 && p3) ? 6 : 0;

    /* ── t7: the four 64-bit SPELLINGS narrowed in a helper RETURN. `wsq` = (2^64-1)^2
     * = 2^128 - 2^65 + 1, whose HIGH word is 2^64-2 — a full-width 64-bit value that
     * does NOT fit in 32 bits, so a narrowing to the wrong width is visible. The
     * `unsigned long` / `long` expectations are written as CASTS of the 64-bit
     * constant, so the LLP64 (32-bit `long`) and LP64 legs both hold. ──────────── */
    __uint128_t wsq = (__uint128_t)sMax * (__uint128_t)sMax;
    int t7 = (hiAsULongLong(wsq) == 0xFFFFFFFFFFFFFFFEull
              && hiAsULong(wsq)  == (unsigned long)0xFFFFFFFFFFFFFFFEull
              && hiAsLongLong(sn) == -3
              && hiAsLong(sn)     == (long)-3) ? 6 : 0;

    /* t8 (TF-C94 self-audit): ++/-- must CARRY and BORROW across the limb
     * boundary. This shape did not compile until the wide-literal kind gate was
     * hoisted above the int64 check -- the synthetic `1` rides the uint64_t arm,
     * so `__uint128_t x; x++;` was refused while both `_BitInt(128)` spellings
     * worked. Written as a MULTIPLICATIVE gate so it is load-bearing without
     * rebalancing t1..t7: if the carry or the borrow is wrong, the exit is 0,
     * not 42. WHAT BREAKS IT: a ++ that drops the carry out of limb 0 leaves
     * inc>>64 == 0; a -- that fails to borrow leaves dec's low limb at 0. */
    __uint128_t inc = (__uint128_t)0xFFFFFFFFFFFFFFFFull;  inc++;
    __int128    dec = (__int128)0xFFFFFFFFFFFFFFFFull + 1;  dec--;
    int t8 = ((u64)(inc >> 64) == 1ull && (u64)inc == 0ull
              && (u64)(dec >> 64) == 0ull
              && (u64)dec == 0xFFFFFFFFFFFFFFFFull) ? 1 : 0;

    /* t9: the four REMAINING materialization sites plus the `(_Bool)` witness —
     * MULTIPLICATIVE, exactly like t8, so it is load-bearing without rebalancing
     * t1..t7. `w` has high word 2 and `sn >> 64` is -3 (both computed above). */
    int t9 = wideSiteMatrix(w, sn, s1 != 0);

    return (t1 + t2 + t3 + t4 + t5 + t6 + t7) * t8 * t9;   /* 7 * 6 * 1 * 1 == 42 */
}
