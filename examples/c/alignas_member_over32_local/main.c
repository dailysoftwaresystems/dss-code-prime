// C11/C23 6.7.5 (D-CSUBSET-ALIGNAS-OVERALIGNED-STACK-LOCAL): the MEMBER-alignas
// route to an over-aligned stack local. No `alignas` appears on the declaration at
// all — the object's alignment is raised to 32 by an alignment specifier on a MEMBER
// of its type, which the layout folds into the whole struct's alignment. This is the
// second of the two ways C reaches this case, and it travels a different channel
// (`CompositeFields.fieldAligns` → the type's computed layout alignment → the
// alloca's effective alignment) than a declaration-site `alignas` does.
//
// It also proves the frame honours the alignment of the OBJECT, not of the
// declaration: a fix that only read a declaration-site specifier would compile this
// and silently under-align it — the exact silent-miscompile class the row names.
//
// The struct is deliberately larger than one frame slot, so the alloca spans several
// and the headroom is added after a multi-slot span rather than a single one.
//
// Red-on-disable: zero `frameSlotAlignHeadroom` or drop the
// `emitAlignUpToPowerOfTwo` call in `emitLeaFrameSlot` and this returns 1, not 42.
// Holds under the baseline AND the shipped release pipeline, on all four targets.

struct Over {
    alignas(32) int head;   // raises the WHOLE object's alignment to 32
    int  body[7];
    char tail;
};

// Non-inlinable sink: keeps the struct address-taken and live across a real call.
int sink(struct Over *p, int *w);

int sink(struct Over *p, int *w) {
    return p->head + p->body[6] + (int)p->tail + *w;
}

int check(void) {
    char pad = 3;                  // a preceding local: nonzero running offset
    struct Over s;
    int witness = 9;               // a following local: above the headroom

    s.head    = 10;
    s.body[6] = 20;
    s.tail    = (char)pad;

    if (sink(&s, &witness) != 42)  // 10 + 20 + 3 + 9
        return 1;

    if (((unsigned long long)(&s) & 31ull) != 0ull)
        return 1;                  // the object is under-aligned → loud failure
    if (witness != 9)
        return 1;                  // the reservation overran its neighbour
    if (s.head != 10 || s.body[6] != 20)
        return 1;                  // the multi-slot span moved under us

    return 0;
}

int main(void) {
    if (check() != 0)
        return 1;
    return 42;
}
