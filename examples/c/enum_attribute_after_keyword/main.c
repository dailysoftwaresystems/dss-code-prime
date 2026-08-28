/* D-CSUBSET-ATTRIBUTE-TYPE-POSITION — the AFTER-KEYWORD attribute slot on an
 * `enum`, in both spellings, honored end-to-end and WITHOUT changing a byte of
 * the program's behaviour.
 *
 * WHAT WAS BROKEN. `enumSpec` was the last composite row in the shipped C
 * grammar with no after-keyword attribute surface at all — `structSpec` and
 * `unionSpec` have carried `compositeAttrLead` since TF-C73. So the C23 spelling
 * `enum [[deprecated]] E { ... }` was error[P0009] and the GNU twin
 * `enum __attribute__((deprecated)) E { ... }` was error[P0001]: a PARSE failure
 * on the position C23 6.7.3.1 defines for a tag's attribute, which gcc 13.3.0 and
 * clang 19.1.1 both accept and both warn on at every USE (MEASURED 2026-08-25).
 *
 * WHY IT IS RUNNABLE AND NOT ONLY A DIAGNOSTIC GOLDEN. The corpus golden pins
 * that the warnings fire at the right spans. It cannot pin the other half of the
 * contract: that admitting the slot did not change what the enum IS. An enum
 * whose tag index shifted by one — which is exactly what an always-emitted lead
 * slot does — binds ANONYMOUSLY if the row's `name` index was not moved with it,
 * and a tag that binds anonymously still compiles: a program that only ever used
 * the ENUMERATORS would sail through with the tag silently discarded. Here every
 * tag is used in a TYPE position (`enum E1 e1`), so a discarded tag cannot reach
 * an exit code at all.
 *
 * THE LAYOUT-ATTRIBUTE CONTRAST IS DELIBERATE AND LOAD-BEARING. `packed` and
 * `aligned(N)` on an enum are ACCEPTED by both references, so DSS may not refuse
 * them; but DSS's enum has one layout channel — the C23 `enum E : T` underlying
 * type — that neither attribute feeds. They are therefore accepted and reported
 * IGNORED (warning[S005F]) rather than silently dropped, and this example proves
 * the "accepted" half really produces a working program: P1/P2 are declared,
 * used, and arithmetic on them lands in the exit code. RECORDED DIVERGENCE: the
 * enum stays 4 bytes here where a gcc `packed` enum is 1. Before this cycle the
 * same source did not compile at all, so nothing regresses.
 *
 * WHAT BREAKS IT: the lead slot removed => no binary (parse error, both arms);
 * the row's `name` index not moved with it => the tags bind anonymously and the
 * `enum E1 e1` declarations fail; the composite scan not reaching enum => the
 * deprecation warnings vanish (the golden catches that half); the gate becoming
 * an ERROR => no binary, and the runner asserts rc==0 AND errorCount()==0.
 *
 * Operands are volatile-seeded so neither arm can const-fold the sums away. */

enum [[deprecated]] E1 { A1 = 5, A1b = 6 };
enum __attribute__((deprecated)) E2 { A2 = 7, A2b = 8 };
enum __attribute__((packed)) P1 { B1 = 9 };
enum __attribute__((aligned(16))) P2 { B2 = 10 };

/* The C23 attribute must not disturb the C23 underlying-type clause that sits
 * immediately after the tag — the two are adjacent in the shape. */
enum [[deprecated]] E3 : unsigned char { A3 = 11 };

/* An ANONYMOUS decorated enum: the lead slot is always emitted, so the tag index
 * must stay right even when there IS no tag. */
enum [[deprecated]] { A4 = 12 };

int main(void) {
    volatile int one = 1;

    enum E1 e1 = A1;                /* tag used in TYPE position */
    enum E2 e2 = A2;
    enum P1 p1 = B1;
    enum P2 p2 = B2;
    enum E3 e3 = A3;

    int t1 = (int)e1 * (int)one;                   /*  5 */
    int t2 = (int)e2 * (int)one;                   /*  7 */
    int t3 = (int)p1 * (int)one;                   /*  9 */
    int t4 = (int)p2 * (int)one;                   /* 10 */
    int t5 = (int)e3 * (int)one;                   /* 11 */
    int t6 = ((int)A1b + (int)A2b + (int)A4 == 26) /* 6+8+12 */
             ? 0 : 100;

    /* 5+7+9+10+11 == 42 */
    return t1 + t2 + t3 + t4 + t5 + t6;
}
