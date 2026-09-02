/* D-CORE-COMPOSITE-OVERLAP-CLAIM-BLIND-TO-UNIONS — a union brace-initializer,
 * RUN, on every shipped target.
 *
 * `compositeFieldsOverlap` is THE authority for "do this composite's members
 * share bytes", and it used to answer `false` for a union carrying neither
 * explicit offsets nor bit-fields — the plainest wrong answer the question has,
 * since every union member sits at byte 0 by definition. It now answers `true`.
 *
 * Both callers consult that authority before deciding whether a positional
 * member-wise write is meaningful, so the truthful answer would have made BOTH
 * of them refuse `union U u = {1};` — a construct gcc, clang and MSVC all accept.
 * They route unions past the gate instead, because the gate's real question is
 * "would a later positional write clobber an earlier one" and a union
 * initializer names exactly ONE member (C 6.7.9p17): one write, at offset 0, with
 * no sibling to lose.
 *
 * ★ TWO TIERS, AND THEY ARE DIFFERENT CODE — the same split
 * `examples/c/overlap_struct_zero_init/` documents. Blocks (1)-(3) declare
 * LOCALS, whose stores the MIR brace-init lowering emits at run time
 * (`hir_to_mir.cpp` `lowerAggregateInitIntoSlot`). Blocks (4)-(5) read
 * FILE-SCOPE objects whose bytes the STATIC-DATA encoder produced at compile time
 * (`asm.cpp` `encodeAggregateValue`). Each tier carries its own copy of the
 * overlap gate and its own copy of the union route; no local declaration can
 * reach the second one, so without blocks (4)-(5) half the rule has no
 * end-to-end witness.
 *
 * ★ AND IT IS TARGET-AGNOSTIC ON PURPOSE. Nothing here is behind a `_WIN32` or
 * `__APPLE__` arm: a union needs no shipped descriptor to be spelled, which is
 * exactly what made this reachable where the explicit-offset overlay was not.
 * Every target runs the SAME program and the same expected exit code.
 *
 * THE SCORE IS A CHECKSUM, NOT A TALLY: 8 + 8 + 9 + 8 + 8 = 41, so dropping or
 * short-circuiting any ONE block changes the exit code even when every check that
 * ran passed.
 *
 * RED-ON-DISABLE, per tier and separately observable:
 *   * delete the union route in `lowerAggregateInitIntoSlot` and blocks (1)-(3)
 *     fail to compile with error[H_UnsupportedLoweringForKind]
 *     "brace-initialization of an overlapping explicit-offset struct";
 *   * delete it in `encodeAggregateValue` and blocks (4)-(5) fail with
 *     error[K_NoMatchingObjectFormat] "static initialization of an overlapping
 *     explicit-offset struct".
 * The `release` arm re-runs all of it through the shipped optimizer. */

/* Two sizeable members, neither explicit offsets nor bit-fields: the exact shape
 * the authority used to call disjoint. `b` is WIDER than `a`, so the union's
 * layout size (8) exceeds the initialized member's width (4) — which is what
 * makes block (3)'s tail check a real question rather than a tautology. */
union U {
    unsigned           a;
    unsigned long long b;
};

/* A union nested inside a struct: the struct's own members do NOT overlap (the
 * union occupies one contiguous range of the struct), so the STRUCT still takes
 * the ordinary member-wise path while its union member takes the union route on
 * the recursion. A fix that keyed on "contains a union" rather than "IS a union"
 * would change this shape's lowering and block (2) would notice. */
struct Wrap {
    unsigned  tag;
    union U   u;
};

/* (4) FILE-SCOPE, NON-ZERO — the static-data encoder's arm. MUTABLE and written
 * below on purpose: a `const` copy could be folded at every use, which would
 * prove nothing about the bytes that actually reached the data section. */
static union U  g_one  = {1};
/* (5) FILE-SCOPE, ALL-ZERO — the same encoder's other outcome. A union takes the
 * ONE-member route here too, NOT the whole-object zero-fill an overlapping
 * struct gets; `buf` is pre-zeroed to the layout size, so the tail is zero either
 * way and this block's job is to prove the encoder still accepted it. */
static union U  g_zero = {0};
static struct Wrap g_wrap = {7, {9}};

int main(void) {
    int score = 0;

    /* (1) The plain declaration form — the construct the row is named for. The
     * FIRST member is the one initialized (C 6.7.9p17), so `a` reads back 1. */
    union U z = {1};
    if (z.a != 1u) return 1;
    score += 8;

    /* (2) The union as a struct MEMBER, reached through the nested-aggregate
     * recursion rather than at the top level. `tag` must survive: if the struct
     * had taken the union route (or the union the struct route) the two members'
     * writes would not both land. */
    struct Wrap w = {7, {9}};
    if (w.tag != 7u)  return 2;
    if (w.u.a != 9u)  return 3;
    score += 8;

    /* (3) THE LIVE-OVERLAY WITNESS, and it separates "one member was written"
     * from "the object is a 4-byte husk". Dirty all 8 bytes through the WIDE
     * member, then re-initialize through the brace path with a value that fills
     * only the NARROW one, and check the four bytes that ARE specified. Then
     * prove the storage is genuinely shared, in BOTH directions.
     *
     * ⚠ EVERY CHECK HERE IS ENDIAN-AGNOSTIC ON PURPOSE. `a` occupies b's LOW
     * bytes on a little-endian target and its HIGH bytes on a big-endian one, so
     * `d.b & 0xFFFFFFFF` would be a target test wearing a portability claim —
     * exactly the shape the source/target-agnostic rule forbids. Asking only
     * "did writing one member change the other" needs no byte order at all, and
     * the bytes beyond `a` are unspecified for a partially-initialized union
     * anyway, so there is nothing here worth asserting that an endian assumption
     * would have bought. */
    union U d;
    d.b = 0xFFFFFFFFFFFFFFFFull;
    d = (union U){0x11223344u};
    if (d.a != 0x11223344u) return 4;       /* the four SPECIFIED bytes */
    d.b = 0ull;
    if (d.a != 0u) return 5;                /* writing b reached a  */
    d.a = 0x55667788u;
    if (d.b == 0ull) return 6;              /* and writing a reached b */
    score += 9;

    /* (4) THE STATIC-DATA ARM, non-zero — read the COMPILE-TIME bytes back out of
     * the data section. No runtime store put them there; the encoder did. Then
     * clear the wide member and read the narrow one back: the emitted object must
     * still be a LIVE 8-byte overlay, not a zeroed husk. */
    if (g_one.a != 1u) return 7;
    if (g_wrap.tag != 7u || g_wrap.u.a != 9u) return 8;
    g_one.b = 0ull;
    if (g_one.a != 0u) return 9;
    score += 8;

    /* (5) THE STATIC-DATA ARM, all-zero — the other outcome of the same gate, and
     * the one that carries the SIZE assertion. A union `{0}` must still be
     * ACCEPTED (it was before, for the wrong reason), and C 6.7.9p21 zero-fills
     * the remainder of a static aggregate, so `g_zero.b` reading 0 covers the
     * bytes an emission that stopped at the initialized member's width would
     * leave holding whatever the section's next object contains. Reading the
     * WIDE member is what makes it a size check, and it needs no byte order. */
    if (g_zero.a != 0u) return 10;
    if (g_zero.b != 0ull) return 11;
    g_zero.a = 5u;
    if (g_zero.a != 5u) return 12;
    score += 8;

    return score;                            /* 41 */
}
