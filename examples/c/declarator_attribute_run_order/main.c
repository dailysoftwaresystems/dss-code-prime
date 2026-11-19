/* P69 (lane `cs`, fold f26e) -- D-C-A-C23-SEQUENCE-AFTER-A-WIDTH-OR-AFTER-A-GNU-RUN-IS-ACCEPTED-OTHERWISE-THAN-THE-REFERENCES
 * and D-CSUBSET-ATTRIBUTE-TYPE-POSITION: the attribute runs of a declaration, taken where and in the order the
 * references take them (probes n5o, n5t, n5u: gcc 15.2.0, clang 21.1.8, Apple clang, MinGW-w64 gcc 13.2.0, cl 19.52).
 * Every form below is one gcc 15.2.0 compiles and runs.
 *
 *   (1) a C23 sequence at the END OF A HEADED DECLARATION'S SPECIFIERS (C23 6.7p9) -- file scope, `static`,
 *       `extern`, a typedef-name head, block scope, a for-loop head, a parameter; it appertains to the TYPE, and an
 *       attribute that does not apply to a type is said ignored, as gcc says it;
 *   (2) `aligned` at that slot decorates the head type (gcc's and MinGW-w64 gcc's reading; clang refuses the
 *       spelling there): a typedef, a member and an object are 16-aligned;
 *   (3) an init-declarator's runs in the references' order -- the C23 sequence, then the asm label, then the GNU
 *       run -- and a function declarator's label renames its symbol (another C name bound to the same label calls it);
 *   (4) a typedef's declarator takes the same runs, and a sequence there confers on its own declarator alone;
 *   (5) a parameter takes a C23 sequence after its declarator and a GNU run at the end of its specifiers.
 *
 * exit 42 when every check holds; otherwise 100 + a mask of the failed checks (never 42).
 * RED-ON-DISABLE: refusing any slot is a compile error (no binary); dropping a function's label leaves `f_entry` /
 * `f2_entry` undefined (a loud link failure); dropping the head type's alignment fails the static asserts or the
 * run-time address check. */
#include <stddef.h>
#include <stdint.h>

/* (1) */
int [[vnd::p]] g_unknown = 2;
static int [[deprecated]] g_static = 3;
extern int [[vnd::p]] g_extern;
int g_extern = 4;
typedef int T;
T [[vnd::p]] g_named = 5;

/* (2) */
typedef int [[gnu::aligned(16)]] A16;
_Static_assert(_Alignof(A16) == 16, "a typedef's head type takes the alignment");
struct SA {
    char c;
    int [[gnu::aligned(16)]] v;
};
_Static_assert(offsetof(struct SA, v) == 16, "a member's head type takes the alignment");
_Static_assert(sizeof(struct SA) == 32, "the structure rounds to its member's alignment");
int [[gnu::aligned(16)]] g_aligned = 6;

/* (3) */
int o_c23_label [[maybe_unused]] __asm__("dss_rr_o1") = 7;
int o_c23_gnu [[maybe_unused]] __attribute__((unused)) = 8;
int o_label_gnu __asm__("dss_rr_o3") __attribute__((unused)) = 9;
int o_all [[maybe_unused]] __asm__("dss_rr_o4") __attribute__((unused)) = 10;

int f_label_gnu(int x) __asm__("dss_rr_f1") __attribute__((unused));
int f_label_gnu(int x) { return x + 1; }
int f_entry(int x) __asm__("dss_rr_f1");

int f_slg(int x) [[vnd::p]] __asm__("dss_rr_f2") __attribute__((unused));
int f_slg(int x) { return x + 2; }
int f2_entry(int x) __asm__("dss_rr_f2");

/* (4) */
typedef int TA [[deprecated]], TB;
TB tb_value = 11;
typedef int TL __asm__("dss_rr_t");
TL tl_value = 12;

/* (5) */
static int add_params(int [[vnd::p]] a, int b [[maybe_unused]], int __attribute__((unused)) c) {
    return a + b + c;
}

int main(void) {
    int bad = 0;
    int [[vnd::p]] local = 1;
    int sum = local;
    for (int [[vnd::p]] i = 0; i < 3; ++i) sum += i;
    if (g_unknown + g_static + g_extern + g_named != 14) bad |= 1;
    if (((uintptr_t)&g_aligned & 15u) != 0 || g_aligned != 6) bad |= 2;
    if (o_c23_label + o_c23_gnu + o_label_gnu + o_all != 34) bad |= 4;
    if (f_entry(40) != 41 || f2_entry(40) != 42) bad |= 8;
    if (tb_value + tl_value != 23) bad |= 16;
    if (add_params(1, 2, 3) != 6) bad |= 32;
    if (sum != 4) bad |= 64;
    return bad ? 100 + bad : 42;
}
