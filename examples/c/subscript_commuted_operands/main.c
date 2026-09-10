// P53 (D-C-SUBSCRIPT-OPERANDS-ARE-NOT-COMMUTATIVE, C 6.5.3.2p1).
//
// C defines `E1[E2]` as `*((E1)+(E2))` and states the constraint symmetrically
// — "one operand shall be a pointer to a complete object type, the other shall
// have integer type" — naming neither position. Addition is commutative, so
// `2[p]` IS `p[2]`. ✔MEASURED 2026-09-02, each probed SEPARATELY: gcc 13.3.0
// `-std=c2x`, clang 18.1.3 `-std=c23` and MSVC 19.51 `/std:clatest` all compile
// every spelling below, and the gcc/clang images exit 42.
//
// ★★ THE EXIT CODE DISCRIMINATES, WHICH A COMPILE-ONLY PIN COULD NOT.
// The failure this guards against is not "the reversed spelling is refused" —
// that one is loud. It is a fix that PARSES the reversed spelling and then
// resolves the WRONG operand, which compiles clean and reads the wrong memory.
// So every check below compares a reversed read against the value it must equal
// AND against the value the other index would have produced: the array holds
// distinct values at every slot, so resolving `i` where `p` was meant (or
// indexing by the wrong operand) lands on a different element and returns a
// distinct non-42 code naming the exact check that broke.
//
// ⚠ THE FORWARD SPELLING IS THE CONTROL AND IT IS IN THE SAME PROGRAM. `p[i]`
// already worked at this cycle's base, so a fix that merely stopped asking
// which operand is the pointer — or that swapped unconditionally — would pass a
// reversed-only pin. Both directions are asserted on the same data.
//
// ★ RED-ON-DISABLE (REMOVE direction), ✔EXERCISED P53: delete the
// `IndexContainerOperand::Subscript` arm from `indexContainerOperand`
// (src/analysis/semantic/type_rules.hpp) — the law then answers `Neither` for
// every reversed spelling — and this example FAILS IN BOTH RUNNERS while
// `array_storage_index`, `agg_string_index` and `sizeof_value_array_dim` stay
// green: the control that says the mutant is targeted, not a blanket break.

enum Idx { IDX_TWO = 2 };

static int data[8] = {100, 110, 120, 130, 140, 150, 160, 170};
static const char text[4] = {'x', 'y', '*', 0};

// Distinct rows so a 2-D commuted read that transposes its subscripts lands on
// a different value instead of a coincidentally-equal one.
static int grid[2][3] = {{1, 2, 3}, {4, 5, 42}};

int main(void) {
    int *p = data;
    int i = 3;
    enum Idx e = IDX_TWO;

    // 1. Both directions agree, for a variable, a literal and an enumerator.
    if (i[p] != p[i]) return 1;
    if (2[p] != p[2]) return 2;
    if (e[p] != p[e]) return 3;

    // 2. The reversed reads land on the SLOT THEY NAME, not merely on some
    //    slot. data[3] = 130 and data[2] = 120, so swapping the operands'
    //    ROLES (rather than their positions) is visible here.
    if (i[p] != 130) return 4;
    if (e[p] != 120) return 5;
    if (2[p] != 120) return 6;

    // 3. An ARRAY container on the right (no explicit pointer in sight): the
    //    array-to-pointer decay must happen on whichever operand is the
    //    container.
    if (i[data] != 130) return 7;
    if (e[data] != 120) return 8;

    // 4. A STRING LITERAL container — the K&R `2["abc"]` idiom, and a const
    //    char array read through the reversed spelling.
    if (2["xy*"] != '*') return 9;
    if (e[text] != '*') return 10;

    // 5. Nested / 2-D: the outer subscript's container is itself a subscript
    //    result, written in the reversed order at both levels.
    {
        int r = 1, c = 2;
        if (c[r[grid]] != 42) return 11;
        if (c[r[grid]] != grid[r][c]) return 12;
    }

    // 6. The reversed spelling as a MODIFIABLE LVALUE, an address-of operand,
    //    and an increment target — the three consumers that route through the
    //    lvalue path rather than the rvalue one.
    {
        static int slot[4] = {0, 0, 7, 0};
        int k = 2;
        k[slot] = 40;
        if (slot[2] != 40) return 13;
        ++k[slot];
        if (slot[2] != 41) return 14;
        {
            int *q = &k[slot];
            *q += 1;
            if (slot[2] != 42) return 15;
        }
    }

    // 7. The TYPE of a reversed subscript, read by the three constructs that
    //    consume the semantic type oracle rather than the lowering. `_Generic`
    //    is the one that failed SILENTLY at this cycle's base — it compiled
    //    clean and selected `default`.
    {
        static double dd[2] = {0.0, 1.0};
        double *dp = dd;
        int j = 1;
        if (sizeof(j[p]) != sizeof(int)) return 16;
        if (sizeof(j[dp]) != sizeof(double)) return 17;
        if (_Generic(j[dp], double: 1, default: 0) != 1) return 18;
        if (_Generic(j[p], int: 1, default: 0) != 1) return 19;
        {
            typeof(j[dp]) v = 2.5;
            if (v != 2.5) return 20;
        }
    }

    // 8. The forward control once more, AFTER every reversed read, so a fix
    //    that damaged the ordinary spelling late cannot hide behind an early
    //    pass.
    if (p[0] != 100 || p[7] != 170) return 21;
    if (data[i] != 130) return 22;

    return 42;
}
