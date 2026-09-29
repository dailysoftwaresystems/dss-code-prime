// P68 round 11 (lane `cs`) — enumerations with FIXED underlying types (C23 6.7.2.2) crossing
// a translation-unit boundary: a function taking one and a struct holding two, declared in
// both units, defined in def.c. The whole-program merge re-interns every unit's types into
// one host, and an enum keeps the underlying type it was declared with through that — so
// the merged program agrees with each unit about every size, offset and value below.
// `sizeof(enum E)` is `long`'s on both data models; the struct's padding follows. Returns 42
// only when every check holds: 40 from the call plus the tag.

enum E : long { A = 40 };
enum S : unsigned char { SX = 2 };
struct Pair {
    enum S tag;
    enum E value;
};

long take(enum E e, struct Pair p);
unsigned long pairSize(void);
unsigned long valueOffset(void);

int main(void) {
    struct Pair p = { SX, A };
    struct Pair q;
    unsigned long const here = (unsigned long)((char *)&q.value - (char *)&q);
    if (pairSize() != sizeof(struct Pair)) return 1;
    if (valueOffset() != here) return 2;
    if (sizeof(enum E) != sizeof(long) || sizeof(enum S) != 1) return 3;
    return (int)take(A, p);
}
