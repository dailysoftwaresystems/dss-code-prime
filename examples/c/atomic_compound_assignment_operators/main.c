/* D-C-ATOMIC-COMPOUND-ASSIGNMENT-AND-INCREMENT-ARE-A-LOAD-THEN-A-SEPARATE-STORE —
 * the VALUE semantics of every compound operator, `++` and `--` on an `_Atomic`
 * object, through the read-modify-write they now lower to.
 *
 * ★★ WHY A SINGLE-THREADED EXAMPLE BESIDE THE RACE WITNESSES. The lowering was
 * REPLACED, not patched: each form is now one retry loop whose update is computed
 * from the value the attempt observed. A race witness proves no update is lost; it
 * cannot prove each update COMPUTES THE RIGHT VALUE — the conversions, the width a
 * sub-word object wraps at, the pointer stride, which value a prefix or postfix
 * form yields, that E2 is evaluated once although the loop may run more than
 * once. Each of those is a distinct way the new lowering could be wrong
 * DIFFERENTLY from the old one, so each has its own arm here.
 *
 * ★ ONE ARM SEPARATES THE REFERENCES ON MEANING, AND C'S TEXT DECIDES IT. For
 * `_Atomic int x = -2; x += 1.5;`, C23 6.5.17.3p4 says `x = x + (1.5)`: the sum
 * is taken in double, -0.5, and truncates to 0. ✔MEASURED gcc 13.3.0 does exactly
 * that; clang 18.1.3 lowers the same statement to `lock add` of `(int)1.5`,
 * leaving -1 — a meaning its own non-atomic `+=` does not have. A reference that
 * silently drops meaning casts no vote, and DSS keeps C's.
 *
 * ⚠ FOLD-RESISTANT: every object is a mutable global or an `_Atomic` local whose
 * accesses are side-effecting atomic ops, so the `release` arm computes the same
 * values at run time.
 *
 * exit = 42; each arm returns its own code on failure.
 */
#include <stdint.h>

enum Colour { Red, Green, Blue };

static _Atomic int            g_int = 100;
static _Atomic unsigned       g_uns = 0xF0F0u;
static _Atomic signed char    g_schar = 127;
static _Atomic unsigned short g_ushort = 65535u;
static _Atomic _Bool          g_bool;
static _Atomic long long      g_wide = 1LL << 40;
static _Atomic enum Colour    g_colour = Red;
static int                    g_cells[8];
static int *_Atomic           g_ptr = g_cells;
static int                    g_calls;

struct Holder { char tag; _Atomic int member; };
static struct Holder g_holder = { 'x', 10 };
static _Atomic int   g_arr[4] = { 1, 2, 3, 4 };

static int next_operand(void) { ++g_calls; return 5; }

/* An `_Atomic` PARAMETER: its storage is the callee's own, and a read-modify-write
 * of it must still address that storage. */
static int param_rmw(_Atomic int v) { v += 7; return v; }

int main(void) {
    /* ── the ten compound operators: the value each YIELDS, and the value left ── */
    if ((g_int += 5) != 105 || g_int != 105)             return 1;
    if ((g_int -= 10) != 95 || g_int != 95)              return 2;
    if ((g_int *= -2) != -190 || g_int != -190)          return 3;
    if ((g_int /= 7) != -27 || g_int != -27)             return 4;   /* toward zero */
    if ((g_int %= 5) != -2 || g_int != -2)               return 5;   /* sign of the dividend */
    if ((g_uns <<= 4) != 0xF0F00u || g_uns != 0xF0F00u)  return 6;
    if ((g_uns >>= 8) != 0xF0Fu || g_uns != 0xF0Fu)      return 7;
    if ((g_uns &= 0xFFu) != 0x0Fu || g_uns != 0x0Fu)     return 8;
    if ((g_uns |= 0x30u) != 0x3Fu || g_uns != 0x3Fu)     return 9;
    if ((g_uns ^= 0x0Au) != 0x35u || g_uns != 0x35u)     return 10;

    /* ── prefix yields the NEW value, postfix the OLD ── */
    g_int = 7;
    if (g_int++ != 7 || g_int != 8)                      return 11;
    if (++g_int != 9 || g_int != 9)                      return 12;
    if (g_int-- != 9 || g_int != 8)                      return 13;
    if (--g_int != 7 || g_int != 7)                      return 14;

    /* ── sub-word objects wrap at their OWN width ── */
    if (g_schar++ != 127 || g_schar != -128)             return 15;
    if ((g_ushort += 2u) != 1u || g_ushort != 1u)        return 16;
    if (g_bool++ != 0 || g_bool != 1)                    return 17;  /* (bool)(0 + 1) */
    if (g_bool++ != 1 || g_bool != 1)                    return 18;  /* (bool)(1 + 1) */
    if (g_bool-- != 1 || g_bool != 0)                    return 19;
    if (g_bool-- != 0 || g_bool != 1)                    return 20;  /* (bool)(0 - 1) */
    if ((g_bool += 2) != 1 || g_bool != 1)               return 21;

    /* ── a 64-bit object, and an enumeration ── */
    if ((g_wide += 3) != (1LL << 40) + 3)                return 22;
    if ((g_wide *= 1000) != ((1LL << 40) + 3) * 1000)    return 23;
    if (g_colour++ != Red || g_colour != Green)          return 24;

    /* ── a POINTER object: the step scales by the pointee ── */
    if (g_ptr++ != g_cells || g_ptr != g_cells + 1)      return 25;
    if ((g_ptr += 3) != g_cells + 4)                     return 26;
    if (--g_ptr != g_cells + 3)                          return 27;
    if ((g_ptr -= 2) != g_cells + 1)                     return 28;
    if ((char *)g_ptr - (char *)g_cells != (long)sizeof(int)) return 29;

    /* ── a floating OPERAND on an integer object: C's meaning, not clang's ── */
    g_int = -2;
    if ((g_int += 1.5) != 0 || g_int != 0)               return 30;

    /* ── E2 is evaluated exactly ONCE, however many times the loop runs ── */
    g_int = 1;
    g_calls = 0;
    g_int += next_operand();
    if (g_int != 6 || g_calls != 1)                      return 31;

    /* ── lvalues that are not plain globals ── */
    if ((g_holder.member += 5) != 15 || g_holder.tag != 'x') return 32;
    int i = 0;
    g_arr[i++] += 10;                                    /* the index runs once */
    if (i != 1 || g_arr[0] != 11 || g_arr[1] != 2)       return 33;
    _Atomic int *const through = &g_arr[2];
    if ((*through *= 3) != 9 || g_arr[2] != 9)           return 34;
    if (param_rmw(35) != 42)                             return 35;

    /* ── an `_Atomic` local stepped in a for-update clause ── */
    _Atomic int steps = 0;
    for (_Atomic int k = 0; k < 10; k += 2) steps++;
    if (steps != 5)                                      return 36;

    return 42;
}
