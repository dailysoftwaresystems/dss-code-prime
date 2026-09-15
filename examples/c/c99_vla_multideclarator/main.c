// D-CSUBSET-VLA-MULTIDECLARATOR-STATEMENT-TEARDOWN + D-CSUBSET-VLA-FOR-INIT-MULTIDECL:
// ONE DECLARATION, MANY DECLARATORS.
//
// C 6.2.4 gives every object of `int a[n], b[n];` the scope of the ENCLOSING block —
// the declaration itself is not a scope. HIR nevertheless has to hand the statement
// position ONE node, so cst_to_hir wraps the N declarators in a Block; read as a
// scope, that Block freed both objects at the end of the STATEMENT, while they were
// still live, and the next VLA in the same block was allocated on top of the first.
// The same wrapper in a `for`-init clause was RECOGNISED and refused LOUD instead.
// One producer, two shapes, opposite failure modes — so this example moves in BOTH
// directions at once: the statement arms must give the RIGHT ANSWER, and the for-init
// arms must COMPILE AND RUN AT ALL.
//
// ★★ EVERY VALUE ARM RE-READS ITS GROUP **AFTER** THE NEXT OBJECT HAS BEEN WRITTEN.
// That placement IS the test. An arm that reads a[0] before the corrupting store
// stays green with the fix removed: nothing has landed on `a` yet. Every `total = ...`
// that re-reads a group therefore sits BELOW the store to the object declared after
// it, never above.
//
// ★ EVERY VLA-DECLARING FUNCTION HERE IS A LEAF (`main` makes no call). That is
// deliberate and it is what makes this example a witness for THIS defect rather than
// for the VLA frame model: with no call, `outgoingArgAreaSize` is 0, no frame bias is
// emitted, and the reproduction is byte-identical with and without the non-leaf VLA
// frame work (D-CSUBSET-VLA-NONLEAF-CALL-FRAME).
//
// The three 400000-iteration arms are CRASH-on-leak witnesses in the other direction:
// the fix widens a declaration group's teardown OUT to the enclosing scope, and if
// that widening ever escaped to a real block body (arms 7, 8, 12) the `sub sp` would
// accumulate per iteration and STATUS_STACK_OVERFLOW / SIGSEGV.
//
//   (1)  plain group + third object, re-read after      =>          21
//   (2)  VLA-typedef group + third object               =>          60
//   (3)  three declarators in one statement             =>          15
//   (4)  mixed VLA / scalar / VLA in one statement       =>          13
//   (5)  multi-declarator `for`-init, live across iters  =>          30
//   (6)  `for`-init group + body VLA, 400000 iters       =>     2400000
//   (7)  group in a loop body, 400000 iters              =>      800000
//   (8)  REAL all-declaration nested block, 400000 iters =>      400000
//   (9)  labelled declaration group                      =>           6
//  (10)  labelled single declarator                      =>           9
//  (11)  `goto` FORWARD past a group in the same block   =>           6
//  (12)  `goto` OUT of a nested group, 400000 iters      =>      800000
//   total = 4400169  => return 42   (4400160 + 9 from arm 13)
// Ten parameters on purpose: see arm (13). `volatile` on the accumulator keeps the callee
// from being folded away, so the stores really happen.
static int sink10(int a1, int a2, int a3, int a4, int a5,
                  int a6, int a7, int a8, int a9, int a10) {
    volatile int acc = 0;
    acc = a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10;
    return acc;
}

int main(void) {
    volatile int n = 8;   // runtime VLA length; volatile defeats constant-folding
    int i;
    long total;
    total = 0;

    // (1) THE P0 SHAPE. `a` and `b` are declared in ONE statement; `c` follows in the
    // SAME scope. Freeing a+b at the end of their declaration puts `c` on top of `a`,
    // so the reads BELOW the store to `c` are the ones that catch it.
    {
        int a[n], b[n];
        int c[n];
        a[0] = 1; a[n - 1] = 2;
        b[0] = 3; b[n - 1] = 4;
        c[0] = 5; c[n - 1] = 6;                    // the corrupting stores
        total = total + c[0] + c[n - 1];           // 11
        total = total + a[0] + a[n - 1];           //  3  <- AFTER c is written
        total = total + b[0] + b[n - 1];           //  7  <- AFTER c is written
    }

    // (2) The same shape through a VLA TYPEDEF (`R e, f;`), the form the defect was
    // first caught in. The alias froze its size once; the teardown must not care.
    {
        typedef int R[n];
        R e, f;
        R g;
        e[0] = 10;
        f[0] = 20;
        g[0] = 30;                                 // the corrupting store
        total = total + g[0];                      // 30
        total = total + e[0] + f[0];               // 30  <- AFTER g is written
    }

    // (3) THREE declarators in one statement: one watermark, taken before the FIRST
    // object, must cover all three. A per-declarator watermark restores to the
    // shallowest anyway, so the count only shows up as dead StackSaves — but a
    // restore-to-the-LAST would leave p and q dangling here.
    {
        int p[n], q[n], r[n];
        int s[n];
        p[0] = 1; q[0] = 2; r[0] = 3;
        s[0] = 4; s[n - 1] = 5;                    // the corrupting stores
        total = total + s[0] + s[n - 1];           //  9
        total = total + p[0] + q[0] + r[0];        //  6  <- AFTER s is written
    }

    // (4) MIXED declarators: VLA, scalar, VLA in one statement. The scalar opens no
    // watermark, so `w` has to recognise that `u` already opened the group's.
    {
        int u[n], v = 7, w[n];
        int x[n];
        u[0] = 1; w[0] = 2;
        x[0] = 3;                                  // the corrupting store
        total = total + x[0] + v;                  // 10
        total = total + u[0] + w[0];               //  3  <- AFTER x is written
    }

    // (5) THE OPPOSITE DIRECTION: a multi-declarator `for`-init used to be a LOUD
    // REFUSAL of a program gcc and clang both run. It must now compile AND run, with
    // for-SCOPE lifetime: `fa`/`fb` are written once at k==0 and read at k==1,2, so a
    // teardown on the BACK-EDGE (the block-scope reading of the wrapper) is a
    // use-after-free that shows up as a wrong total, not merely as a leak.
    {
        int k;
        k = 0;
        for (int fa[n], fb[n]; k < 3; k = k + 1) {
            if (k == 0) { fa[0] = 7; fb[0] = 8; }
            else { total = total + fa[0] + fb[0]; }        // += 15 at k=1,2 => 30
        }
    }

    // (6) A `for`-init GROUP and a BODY VLA are torn down at DIFFERENT points: the
    // body VLA every iteration (back-edge), the init group only at the loop exit.
    // 400000 outer iterations, so getting either one wrong overflows the stack.
    for (i = 0; i < 400000; i = i + 1) {
        int k;
        k = 0;
        for (int ga[n], gb[n]; k < 2; k = k + 1) {
            int hb[n];
            hb[0] = 1;
            if (k == 0) { ga[0] = 2; gb[0] = 3; }
            else { total = total + ga[0] + gb[0] + hb[0]; }   // += 6 per outer iter
        }
    }

    // (7) A declaration group AS a loop body's own declaration: still block-scoped,
    // still reclaimed on every back-edge. This is the arm the widening must NOT reach.
    for (i = 0; i < 400000; i = i + 1) {
        int la[n], lb[n];
        la[0] = 1; lb[0] = 1;
        total = total + la[0] + lb[0];                       // += 2 per iteration
    }

    // (8) CONTROL for the conservative half of the recogniser. `{ int na[n]; int nb[n]; }`
    // is a REAL compound statement that happens to hold nothing but declarations, so it
    // is indistinguishable from a declarator group at the MIR tier and is treated as
    // one: its stack is reclaimed at the ENCLOSING scope's exit instead of its own.
    // The enclosing scope here is the loop body, which still frees every iteration —
    // that is the bound this arm asserts, and without it the widening would overflow.
    for (i = 0; i < 400000; i = i + 1) {
        { int na[n]; int nb[n]; }
        total = total + 1;                                   // += 1 per iteration
    }

    // (9) A LABELLED declaration group (C23 6.8.1 — a label may precede a declaration;
    // gcc 13.3.0 and clang 18.1.3 both compile and RUN it, clang calling the C17
    // spelling a C23 extension rather than an error). The label is a second non-scope
    // wrapper between the declaration and its block, and it hid the same early free.
    {
        int y;
        y = 0;
YG:     int ya[n], yb[n];
        int yc[n];
        ya[0] = 1; yb[0] = 2;
        yc[0] = 3;                                 // the corrupting store
        total = total + yc[0];                     //  3
        total = total + ya[0] + yb[0];             //  3  <- AFTER yc is written
        if (y == 0) { y = 1; }
    }

    // (10) A labelled SINGLE declarator — the shape that used to fail loud with "a
    // variable-length array in this declaration position is not yet torn down at
    // scope exit". It has NO wrapper Block at all (one declarator never wraps), so it
    // is the arm that proves the walk-out is through the LABEL and not merely through
    // the group. ⚠ Its discriminator is COMPILATION, not the values: with nothing but
    // a label between the declaration and its block there is no early free to catch,
    // so removing the label leg of the walk-out reddens this arm by REFUSING it.
    {
ZS:     int za[n];
        int zb[n];
        za[0] = 4;
        zb[0] = 5;
        total = total + zb[0];                     //  5
        total = total + za[0];                     //  4
    }

    // (11) A `goto` FORWARD over a declaration group, staying inside the group's own
    // block. The group is STILL LIVE at the label, so this edge must free NOTHING.
    // The teardown decides that by looking the group up in its block's child list; a
    // frame anchored at the inner VarDecl instead of at the group is not in that list,
    // reads as "does not enclose the label", and frees ba/bb here — after which `bc`
    // lands on them and the reads below go wrong. This arm is that half's witness.
    {
        int ba[n], bb[n];
        ba[0] = 1; bb[0] = 2;
        if (ba[0] == 1) { goto GF; }
        total = total + 1000;                      // unreachable
GF:     ;
        {
            int bc[n];
            bc[0] = 3;
            total = total + bc[0];                 //  3
        }
        total = total + ba[0] + bb[0];             //  3  <- AFTER bc is written
    }

    // (12) A `goto` OUT of a nested block holding a group DOES free it — 400000 times,
    // so a missed restore on that edge overflows rather than merely leaking.
    for (i = 0; i < 400000; i = i + 1) {
        {
            int ca[n], cb[n];
            ca[0] = 1; cb[0] = 1;
            total = total + ca[0] + cb[0];         // += 2 per iteration
            goto GN;
        }
GN:     ;
    }

    // (13) THE WIDENED SHAPE: a CALL after the declarator group. This is the arm lane
    // `md` could not write -- at its base ANY call inside a VLA function was refused
    // (L_VlaNonLeafFrameUnsupported), so every VLA function in this file had to be a leaf.
    // [[D-CSUBSET-VLA-NONLEAF-CALL-FRAME]] closed in the same cycle and made it expressible.
    // ten arguments overflow ms_x64's 4 GPRs, sysv's 6 and aapcs64's 8, so the tail of the
    // list is STORED into the outgoing-args area -- a SECOND, independent writer over a
    // wrongly-freed group, and the one a real program is most likely to have.
    // The reads sit BELOW the call deliberately: above it they would pass with the fix gone.
    {
        int da[n], db[n];
        da[0] = 4; db[0] = 5;
        if (sink10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) != 55) { return 8; }
        total = total + da[0] + db[0];             //  9  <- AFTER the outgoing-arg stores
    }

    if (total == 4400169) { return 42; }
    return 7;
}
