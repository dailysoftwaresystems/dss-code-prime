/* D-CSUBSET-ATOMIC-MONOMORPH-I32 — the `<stdatomic.h>` OBJECT SURFACE at widths
 * other than `int`, end to end, RUN on BOTH the baseline (debug) and `release`.
 *
 * WHAT WAS WRONG, PRECISELY. The `_Atomic` QUALIFIER was never truncated — a
 * bare `_Atomic long long g; g = 0x1FFFFFFFF;` already ran correctly. It was
 * the `<stdatomic.h>` ACCESSOR SURFACE that was monomorphized to i32, because a
 * builtin's config row carries ONE fixed signature text while C defines these as
 * GENERIC functions (§7.17.1p6). `atomic_store_explicit(&aWideAtomic, …)` was refused
 * `S_TypeMismatch` while gcc 13.3.0 and clang 18.1.3 both compiled and ran it.
 * The fix declares the SHAPE (`genericPointee` in `sources/c.lang.json`): the
 * row's signature is an EXEMPLAR and the real one is derived per call site from
 * the argument's pointee.
 *
 * WHY EVERY VALUE HERE EXCEEDS 32 BITS WHERE IT CAN. A specialization that
 * silently failed would coerce the value to the exemplar's `int`, and a value
 * that FITS in 32 bits would survive that unharmed — the test would pass while
 * proving nothing. `0x1FFFFFFFF` does not fit, so a lost specialization is a
 * wrong answer rather than a silent pass. (While this example was being written
 * the specialization DID silently fail to reach the lowering, and the MIR
 * verifier's write-typing belt caught it as `I_StoreValueTypeMismatch`.)
 *
 * SUB-WORD IS HERE TOO because it exercises different machinery: the byte/half
 * `store_seqcst` / `load_acquire` / `store_release` encoding slots (previously
 * absent — `_Atomic char c; c = 1;` died at the ENCODER with
 * `A_NoMatchingEncodingVariant` at width 8), and the integer PROMOTION the RMW
 * ALU step takes because neither target declares a byte/half ALU form
 * (D-CSUBSET-32BIT-ALU-FORMS refuses those by name).
 *
 * ★★ AND ONE ARM HERE FOUND A SILENT MISCOMPILE, which is why the guard bytes
 * around `g_flag` are load-bearing rather than padding. A one-byte
 * compare-exchange took the REGISTER-plumbing width function instead of the
 * MEMORY-access one, which maps `_Bool` to the width DEFAULT — so it emitted
 * `lock cmpxchg QWORD PTR`, an EIGHT-byte read-modify-write over a ONE-byte
 * object, comparing and clobbering seven bytes of its neighbours. ✔MEASURED: with
 * ZERO neighbours the program still exits 42 and nothing is visible; the defect
 * only becomes a wrong answer when something real sits beside the object. It was
 * pre-existing and UNREACHABLE before this cycle (the only CAS consumers were the
 * i32/i64 `_InterlockedCompareExchange` intrinsics), and the RMW family plus the
 * width-generic accessors are what opened the path.
 *
 * RED-ON-DISABLE: delete the `genericPointee` block from
 * `atomic_store_explicit`'s row in `sources/c.lang.json` → the wide-object arms
 * fail. Delete the `atomic_llong` typedef from `shippedLibs/stdatomic.json` →
 * the `atomic_llong` arms fail. Put `lowerAtomicCas` back on
 * `widthFlagsForType` → the guard-byte arms fail. Delete
 * `atomicCasCompareOperand`'s narrowing → the signed sub-word arms fail on
 * arm64 `--config=release`. Delete `arrayToPointerDecay` from
 * `specializeGenericPointeeSignature` → the array-binding arms fail to COMPILE.
 * Delete `pointerDifferenceParams` from the two `atomic_fetch_{add,sub}_explicit`
 * rows → the atomic-pointer-arithmetic arms fail to compile.
 *
 * Every arm has its OWN non-42 exit. exit = 42.
 */
#include <stdatomic.h>

/* Mutable globals = runtime-opaque (anti-fold). */
/* ⚠ `long` IS 32 BITS ON LLP64 (the pe64 leg) AND 64 ON LP64, so every arm that
 * must exceed 32 bits uses `long long` / `size_t`, whose widths are the same on
 * every leg this example declares. ✔MEASURED: an earlier draft asserted
 * 0x1FFFFFFFF through an `_Atomic long` and exited 1 on pe64 — DSS was RIGHT and
 * the test was wrong, which is the `long`-is-not-64-bits trap in its usual form.
 * `atomic_long` still gets its own arm below, at a value that fits either width. */
_Atomic long long g_qual  = 0;   /* the QUALIFIER, above 32 bits         */
atomic_long       g_long  = 0;   /* the data-model-dependent typedef     */
atomic_llong      g_llong = 0;   /* always 64 bits                       */
atomic_short   g_short = 0;
atomic_char    g_char  = 0;
atomic_bool    g_bool  = 0;
atomic_uint    g_uint  = 0;
atomic_size_t  g_size  = 0;
/* ⚠ THE NEIGHBOURS ARE LOAD-BEARING, NOT PADDING. A CAS that used the width
 * DEFAULT instead of the object's own byte size emitted an EIGHT-byte
 * `lock cmpxchg` over the ONE-byte `g_flag` — it compared and clobbered seven
 * bytes of whatever sat beside it. ✔MEASURED: with zero neighbours the program
 * still exits 42 and the defect is invisible; these NON-ZERO neighbours are what
 * turn it into a wrong answer. Keep them, and keep them non-zero. */
_Alignas(8) atomic_bool g_flag  = 0;
unsigned char  g_guard0 = 0xAA;
unsigned char  g_guard1 = 0xBB;
unsigned char  g_guard2 = 0xCC;
unsigned char  g_guard3 = 0xDD;
int            g_pa = 1, g_pb = 2;
int * _Atomic  g_ptr;
/* ★★ SIGNED SUB-WORD, WITH NEGATIVE VALUES — THE ARM THE GUARD BYTES ABOVE
 * COULD NOT REACH. Every sub-word CAS in the first cut of this example carried a
 * NON-NEGATIVE comparand (`_Bool` 0/1), which is exactly the case where a
 * container-width compare happens to be right. ✔MEASURED: an
 * `_Atomic signed char` compare-exchange against `-1` under `--config=release`
 * on arm64 returned `true` WITHOUT EXCHANGING — the in-CAS compare saw a
 * SIGN-extended materialized constant (0xFFFFFFFF) against a ZERO-extended
 * `ldaxrb` (0x000000FF), skipped the `stlxr`, and then a second compare one tier
 * up said the values were equal. gcc -O2 and clang -O2 both give the right
 * answer. A NEGATIVE comparand is the whole witness; keep the values negative,
 * and keep the `--config=release` arm, because debug canonicalises the comparand
 * through memory and hides it. */
_Atomic signed char g_schar  = -1;
_Atomic short       g_sshort = -300;
/* R3 / the BINDING position: an ARRAY of atomic objects, passed BY ARRAY NAME
 * and by `arr + n`. Both decay to a pointer in C (6.3.2.1p3) and both references
 * run them; DSS refused both until the decay was applied before the generic
 * accessors' pointee binding. */
_Atomic long long g_arr[3];
/* R5 / atomic POINTER arithmetic: C §7.17.1p6 makes the operand `ptrdiff_t` and
 * §7.17.7.5 makes the computation the `+` OPERATOR — i.e. SCALED by the element
 * size. The two element sizes below (4 and 8) are what makes an UNSCALED
 * (byte-wise) implementation a wrong answer rather than a coincidence. */
int               g_ints[4]  = {10, 11, 12, 13};
long long         g_wides[4] = {20, 21, 22, 23};
int       * _Atomic g_pi;
long long * _Atomic g_pw;

int main(void) {
    /* ── the row's own probe: a value that does NOT fit in 32 bits ── */
    atomic_store_explicit(&g_qual, 0x1FFFFFFFF, memory_order_seq_cst);
    if (atomic_load_explicit(&g_qual, memory_order_seq_cst) != 0x1FFFFFFFF)
        return 1;

    /* ── the GENERIC (non-`_explicit`) accessors, C §7.17.7.1-.3 ── */
    atomic_store(&g_long, 0x2FFFFL);
    if (atomic_load(&g_long) != 0x2FFFFL)                return 2;

    /* `atomic_init` is spelled as an assignment through the pointer, which in
     * DSS IS a seq_cst atomic store (✔MEASURED byte-identical to
     * `atomic_store`). C23 §7.17.2.1 requires only that the object end up
     * holding `value`, and its p3 says concurrent access during initialization
     * is a DATA RACE — so the stronger form is conforming and no conforming
     * program can observe the difference. ⚠ An earlier draft of this comment
     * claimed the opposite ("DELIBERATELY a plain assignment ... NOT an atomic
     * store"); it was false the day it was written. */
    atomic_init(&g_llong, 0x3FFFFFFFF);
    if (atomic_load(&g_llong) != 0x3FFFFFFFF)            return 3;

    /* ── the RMW family above 32 bits ── */
    if (atomic_fetch_add(&g_qual, 0x100000000LL) != 0x1FFFFFFFF) return 4;
    if (atomic_load(&g_qual) != 0x2FFFFFFFF)             return 5;
    if (atomic_exchange(&g_llong, 7) != 0x3FFFFFFFF)     return 6;

    /* ── sub-word: byte/half encodings + the promoted ALU step ── */
    if (atomic_fetch_add(&g_short, 40) != 0)             return 7;
    if (atomic_load(&g_short) != 40)                     return 8;
    if (atomic_fetch_add(&g_char, 2) != 0)               return 9;
    if (atomic_load(&g_char) != 2)                       return 10;
    atomic_store(&g_bool, 1);
    if (!atomic_load(&g_bool))                           return 11;

    /* ── unsigned, and a `_t` typedef whose width is data-model-dependent ── */
    if (atomic_fetch_or(&g_uint, 0xF0000000u) != 0u)     return 12;
    if (atomic_load(&g_uint) != 0xF0000000u)             return 13;
    atomic_store(&g_size, 0x123456789ULL);
    if (atomic_load(&g_size) != 0x123456789ULL)          return 14;

    /* ── a ONE-BYTE compare-exchange, with live neighbours on both sides ── */
    _Bool flagExpected = 0;
    if (!atomic_compare_exchange_strong(&g_flag, &flagExpected, 1)) return 18;
    if (!atomic_load(&g_flag))                           return 19;
    if (g_guard0 != 0xAA || g_guard1 != 0xBB
     || g_guard2 != 0xCC || g_guard3 != 0xDD)            return 20;
    if (atomic_exchange(&g_flag, 0) != 1)                return 21;
    if (g_guard0 != 0xAA || g_guard1 != 0xBB)            return 22;
    /* unsigned sub-word wraparound: the promoted ALU step must truncate back. */
    if (atomic_fetch_add(&g_char, 250) != 2)             return 23;
    if ((unsigned char)atomic_load(&g_char) != 252)      return 24;

    /* ── an atomic OBJECT POINTER: a type `_Generic` could never enumerate ── */
    atomic_store(&g_ptr, &g_pa);
    if (atomic_load(&g_ptr) != &g_pa)                    return 25;
    int *ptrExpected = &g_pa;
    if (!atomic_compare_exchange_strong(&g_ptr, &ptrExpected, &g_pb)) return 26;
    if (atomic_load(&g_ptr) != &g_pb)                    return 27;

    /* ── compare-exchange on a 64-bit object, BOTH outcomes ── */
    long long expected = 7;
    if (!atomic_compare_exchange_strong(&g_llong, &expected, 0x4FFFFFFFF))
                                                         return 15;
    expected = 7;                                        /* now stale */
    if (atomic_compare_exchange_strong(&g_llong, &expected, 1))
                                                         return 16;
    if (expected != 0x4FFFFFFFF)                         return 17;

    /* ── SIGNED SUB-WORD compare-exchange, NEGATIVE comparand, BOTH outcomes ──
     * `-1` and `-300` are MATERIALIZED CONSTANTS, which is the producer the
     * broken enumeration omitted. Both the succeeding and the failing direction
     * are asserted: a compare that disagrees with the exchange shows up as
     * "reported success, object unchanged" in the first and as a stale
     * write-back in the second. */
    signed char scExpected = -1;
    if (!atomic_compare_exchange_strong(&g_schar, &scExpected, -2)) return 28;
    if (atomic_load(&g_schar) != -2)                     return 29;
    scExpected = -1;                                     /* now stale */
    if (atomic_compare_exchange_strong(&g_schar, &scExpected, -3))  return 30;
    if (scExpected != -2)                                return 31;

    short shExpected = -300;
    if (!atomic_compare_exchange_strong(&g_sshort, &shExpected, -30000)) return 32;
    if (atomic_load(&g_sshort) != -30000)                return 33;
    shExpected = -300;                                   /* now stale */
    if (atomic_compare_exchange_strong(&g_sshort, &shExpected, 1))  return 34;
    if (shExpected != -30000)                            return 35;

    /* signed sub-word RMW that CROSSES the sign boundary (the RMW family takes
     * its comparand from its own AtomicLoad, so it escaped the defect above —
     * pin it anyway, because "escaped by luck" is not a property). */
    if (atomic_fetch_sub(&g_schar, 126) != -2)           return 36;
    if (atomic_load(&g_schar) != -128)                   return 37;
    if (atomic_fetch_add(&g_sshort, 30000) != -30000)    return 38;
    if (atomic_load(&g_sshort) != 0)                     return 39;

    /* ── an ARRAY in the BINDING position: bare name, and `arr + n` ── */
    atomic_store_explicit(g_arr, 0x5FFFFFFFFLL, memory_order_seq_cst);
    if (atomic_load_explicit(g_arr, memory_order_seq_cst) != 0x5FFFFFFFFLL)
                                                         return 40;
    atomic_store_explicit(g_arr + 2, 0x6FFFFFFFFLL, memory_order_seq_cst);
    if (atomic_load_explicit(&g_arr[2], memory_order_seq_cst) != 0x6FFFFFFFFLL)
                                                         return 41;
    /* the OFFSET is load-bearing: a decay that dropped the `+ 2` would write
     * element 0 and this would catch it. */
    if (atomic_load_explicit(&g_arr[0], memory_order_seq_cst) != 0x5FFFFFFFFLL)
                                                         return 43;

    /* ── atomic POINTER arithmetic, SCALED (C §7.17.1p6 + §7.17.7.5) ── */
    atomic_store(&g_pi, &g_ints[0]);
    if (atomic_fetch_add(&g_pi, 2) != &g_ints[0])        return 44;
    if (atomic_load(&g_pi) != &g_ints[2])                return 45;
    if (*atomic_load(&g_pi) != 12)                       return 46;
    if (atomic_fetch_sub(&g_pi, 1) != &g_ints[2])        return 47;
    if (atomic_load(&g_pi) != &g_ints[1])                return 48;
    /* a DIFFERENT element size, so a byte-wise implementation cannot pass both */
    atomic_store(&g_pw, &g_wides[0]);
    if (atomic_fetch_add(&g_pw, 3) != &g_wides[0])       return 49;
    if (atomic_load(&g_pw) != &g_wides[3])               return 50;
    if (*atomic_load(&g_pw) != 23)                       return 51;
    if (atomic_fetch_sub(&g_pw, 2) != &g_wides[3])       return 52;
    if (atomic_load(&g_pw) != &g_wides[1])               return 53;
    return 42;
}
