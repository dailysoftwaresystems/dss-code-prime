/* A FUNCTION TYPE CARRIES AN ARRAY OF UNKNOWN SIZE (P69 fold f26b, lane cs).
 *
 * Two function types are compatible when their results and each parameter are (C 6.7.6.3p15), and an array of
 * unknown or variable size is compatible with one of any size of a compatible element (C 6.7.6.2p6), at every level.
 * So a designator returning `int (*)[]` converts to a pointer to a function returning `int (*)[3]` or `int (*)[m]`,
 * and the reverse; a parameter the same; a function returning a pointer to such a function the same; and a call
 * argument, an assignment and a return statement are the same conversion. `_Generic` asks the same relation, and a
 * function may be declared with one and defined with the other. Each check returns its own number on failure. */
int store[8] = {0, 42, 0, 0, 0, 0, 0, 0};

int (*gu(void))[] { return (int (*)[])store; }
int (*g3(void))[3] { return (int (*)[3])store; }
int hu(int (*p)[]) { return (*p)[1]; }
int h3(int (*p)[3]) { return (*p)[1]; }
int apply3(int (*(*cb)(void))[3]) { return (*cb())[1]; }
int (*(*ggu(void))(void))[] { return gu; }
int (*(*pick(void))(void))[3] { return gu; }
int (*hm(int (*p)[2]))[] { return (int (*)[])*p; }

/* a result declared with an array of unknown size, defined with three elements */
int (*gr(void))[];
int (*gr(void))[3] { return (int (*)[3])store; }

/* a parameter pointing to a function, declared one way and defined the other */
int k(int (*(*cb)(void))[]);
int k(int (*(*cb)(void))[3]) { return (*cb())[1]; }

static int check(int m) {
    int (*(*r1)(void))[3] = gu;                 /* a result: unknown -> known size */
    int (*(*r2)(void))[] = g3;                  /* known -> unknown */
    int (*(*r3)(void))[m] = gu;                 /* unknown -> a run-time bound */
    int (*(*r4)(void))[m] = g3;                 /* known -> a run-time bound */
    int (*p5)(int (*)[3]) = hu;                 /* a parameter: unknown -> known */
    int (*p6)(int (*)[]) = h3;                  /* known -> unknown */
    int (*p7)(int (*)[m]) = hu;                 /* unknown -> a run-time bound */
    int (*(*(*n8)(void))(void))[3] = ggu;       /* one function further down */
    int (*(*fu)(void))[] = gu;
    int (*(*f9)(void))[3] = 0;
    f9 = fu;                                    /* a pointer already, assigned */
    int (*(*f22)(int (*)[]))[3] = hm;           /* each side knows a size the other does not */
    if ((*r1())[1] != 42) return 1;
    if ((*r2())[1] != 42) return 2;
    if ((*r3())[1] != 42) return 3;
    if ((*r4())[1] != 42) return 4;
    if (p5((int (*)[3])store) != 42) return 5;
    if (p6((int (*)[])store) != 42) return 6;
    if (p7((int (*)[m])store) != 42) return 7;
    if ((*n8()())[1] != 42) return 8;
    if ((*f9())[1] != 42) return 9;
    if (apply3(gu) != 42) return 10;            /* a call argument */
    if (apply3(fu) != 42) return 11;
    if ((*pick()())[1] != 42) return 12;        /* a return statement */
    if ((*gr())[1] != 42) return 13;
    if (k(g3) != 42) return 14;
    if (!(f9 == gu)) return 16;                 /* == of a pointer and a designator */
    if (_Generic(gu, int (*(*)(void))[3]: 1, default: 0) != 1) return 17;
    if ((*f22((int (*)[])store))[1] != 42) return 22;
    return 42;
}

int main(void) { return check(3); }
