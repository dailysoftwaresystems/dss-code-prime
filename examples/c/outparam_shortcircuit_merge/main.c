/* See helper.c for what this example holds and why it is two TUs. */
struct V { int pad[34]; struct B *link; };
struct B { long tag; };

extern int get_it(void *ctx, const char *s, void **out);
extern int getint(int *out);
extern int consumer(struct V *p, int i, int op, int flags, void *o);

int probe(void *ctx, int objc, void **objv) {
    struct V *p = 0;
    int idx = 0;
    int flags = 0;
    /* The `a || b` shape: the out-param write lives in the LEFT arm, so `p` is
     * written on a path the merge has to keep live, and the right arm writes
     * through a pointer into a sibling local. */
    if (get_it(ctx, (const char *)objv[objc - 2], (void **)&p)
        || getint(&idx)) {
        return 1;
    }
    /* Argument 1 is the merged local; argument 5 rides the stack slot. */
    return consumer(p, idx, 1, flags, (void *)&flags);
}

int main(void) {
    /* objc==5, so objv[objc-2] is objv[3] — the entry the lookup accepts. */
    void *objv[5] = {0, 0, 0, (void *)"handle", 0};
    return probe(0, 5, objv) == 0 ? 42 : 7;
}
