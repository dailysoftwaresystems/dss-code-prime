/* TF-C93 (D-CSUBSET-ATTRIBUTE-IGNORED-FOR-DECL-KIND-SILENT): the ONE shared
 * decl-kind gate, all FOUR axes of the class, PLUS the negative controls in the
 * same file so the golden asserts both what fires and what must not.
 *
 * Every declaration below was MEASURED SILENT at HEAD 199fe7d (exit 0, zero
 * diagnostics, attribute discarded) except where noted. The `.diag` golden pins
 * the exact SPAN of each warning — the clause the programmer wrote — so a later
 * change that reports at the declaration or at the name instead flips the file.
 */

/* ── the four axes on a DATA OBJECT: each must warn exactly once ── */
__attribute__((noinline))           int a_noinline = 1;
__attribute__((always_inline))      int a_always   = 2;
__attribute__((no_sanitize_thread)) int a_nosan    = 3;
int a_nodiscard __attribute__((warn_unused_result)) = 4;

/* ── the TYPEDEF position: the one `linkageFrom` never reached ── */
typedef __attribute__((noinline)) int IgnoredOnTypedef;

/* ── NEGATIVE CONTROL 3: the TYPEDEF position for the GNU discard spelling, which
 * is APPLICABLE there and must be SILENT. clang's own applicability text lists
 * typedefs among the valid positions and MEASURED emits zero diagnostics here,
 * while its C23 sibling `nodiscard` is function-only (6.7.13.3) and stays loud on
 * the line above's shape. This is the golden's witness for the two-row split: fold
 * the rows back together under either kind set and one of these two flips. ── */
typedef __attribute__((warn_unused_result)) int AppliesToTypedef;

/* ── the DUNDER spelling: the effects table is dunder-normalized ── */
__attribute__((__no_sanitize_thread__)) int a_dunder = 5;

/* ── MULTI-DECLARATOR: ONE spelling, THREE declarators, ONE diagnostic (the
 * clause-node latch). Three warnings here would mean the latch is gone. ── */
__attribute__((noinline)) int m1, m2, m3;

/* ── NEGATIVE CONTROL 1: the FUNCTION position, where these attributes BELONG.
 * Silent — and this is the shape sqlite actually writes (wal.c). ── */
__attribute__((noinline))           static int f_noinline(int k) { return k + 1; }
__attribute__((no_sanitize_thread)) static int f_nosan(int k)    { return k + 2; }

/* ── NEGATIVE CONTROL 2: `aligned` is NOT a member of the class. On an OBJECT it
 * is correct C, silent, and HONORED into the symbol's explicit alignment. Its
 * own sink is the sole authority on that axis — which is why the shared gate
 * must stay quiet here. ── */
__attribute__((aligned(16))) int a_aligned = 6;

int main(void) {
    return a_noinline + a_always + a_nosan + a_nodiscard + a_dunder
         + m1 + m2 + m3 + a_aligned
         + f_noinline(0) + f_nosan(0) + (int)sizeof(IgnoredOnTypedef)
         + (int)sizeof(AppliesToTypedef);
}
