/* The other unit's tentative definitions of the same two names (see main.c), and its own zero-filled neighbour. */
int shared_counter;
int pair[10];
static int guard_other[4];

int bump(void) { return ++shared_counter; }

/* Whether the `size` bytes at `p` and pair's ten words share a byte. */
static int overlaps(void const *p, unsigned long long size) {
    unsigned long long const lo = (unsigned long long)pair, hi = lo + sizeof pair;
    unsigned long long const a = (unsigned long long)p, b = a + size;
    return a < hi && lo < b;
}

/* 21 when pair is ten words of its own; 0 when another object lies inside them, 1 when filling them changed a
   neighbour, 2 when it changed the counter. */
int last(int const *guard_main, unsigned long long guardSize) {
    if (overlaps(&shared_counter, sizeof shared_counter) || overlaps(guard_main, guardSize)
        || overlaps(guard_other, sizeof guard_other)) {
        return 0;
    }
    for (int i = 1; i < 10; ++i) pair[i] = 100 + i;
    for (int i = 0; i < 4; ++i) {
        if (guard_main[i] != 0 || guard_other[i] != 0) return 1;
    }
    if (shared_counter != 21) return 2;
    pair[9] = 20;
    return pair[9] + pair[0];   /* 20 + 1 */
}
