/* D-CSUBSET-ENUM-INT-CONVERSION (FC8) end-to-end RUNTIME witness across all
 * targets. An enum value is used as an int across every reachable form:
 *   - the enumerator value itself (RED=0/GREEN=1/BLUE=2 auto-increment — folded
 *     to its int at HIR Ref-lowering; a bare enumerator Ref would otherwise hit
 *     the unbound-symbol fail-loud at MIR);
 *   - switch dispatch on an enum discriminant;
 *   - enum->int WIDENING as a call ARG + arithmetic (classify: c + 5);
 *   - enum==int comparison;
 *   - postfix ++ on an enum lvalue;
 *   - compound-assign += (int->enum write-back);
 *   - the explicit (int)enum cast;
 *   - a PACKED enum struct-field store + load (the width witness — see below).
 *
 * base = classify(BLUE) = 2 + 5 = 7 (the BLUE switch arm).
 * step : RED(0) ++ -> 1, += 1 -> 2 ; (int)step = 2.
 * packed: t.payload written FIRST (=100), THEN the 4-byte enum field t.tag
 *   (=GREEN=1). An OVER-WIDE (8-byte) store of the enum field would clobber the
 *   adjacent `payload` at offset 4 (D-LIR-INT-MEMORY-WIDTH-EXACT for enums — a
 *   scalar enum local sits in its own >=8-byte slot and MASKS this, which is
 *   exactly why the earlier scalar-only corpus missed it). Correct width keeps
 *   payload == 100, so (t.payload - 100) == 0 and (int)t.tag - 1 == 0.
 * return base + (int)step - 2 + (t.payload - 100) + (int)t.tag - 1
 *      = 7 + 2 - 2 + 0 + 1 - 1 = 7.
 * A promotion / signedness / fold / memory-width regression in ANY form flips
 * the exit code. The optimizedPipelines arm proves the flow survives
 * Mem2Reg/ConstFold (the baseline arm is the real-codegen memory witness). */
enum Color { RED, GREEN, BLUE };

struct Tagged { enum Color tag; int payload; };   /* packed: tag@0 (4B), payload@4 (4B) */

int classify(enum Color c) { return c + 5; }   /* enum->int widen (arg) + arith */

int main(void) {
    enum Color pick = BLUE;                     /* enumerator value -> Const      */
    int base = 0;
    switch (pick) {                             /* enum switch dispatch           */
        case RED:   base = 1; break;
        case GREEN: base = 2; break;
        case BLUE:  base = classify(pick); break;   /* 7                          */
    }
    if (pick == 2) base = base + 0;             /* enum==int comparison           */
    enum Color step = RED;                      /* 0                              */
    step++;                                     /* postfix ++ on enum -> 1        */
    step += 1;                                  /* compound-assign int->enum -> 2 */

    struct Tagged t;                            /* packed enum-field width witness */
    t.payload = 100;                            /* neighbour written FIRST         */
    t.tag = GREEN;                              /* 4-byte enum store @0 (must not  */
                                                /* clobber payload @4)             */
    return base + (int)step - 2                 /* (int)enum cast: 7 + 2 - 2 = 7   */
         + (t.payload - 100)                    /* 0 iff the enum store was 4-byte */
         + (int)t.tag - 1;                      /* 0 iff the enum load was 4-byte  */
}
