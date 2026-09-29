// The sibling unit: defines what main.c declares, over the SAME two enumerations with
// fixed underlying types and the same struct (compatible types across units, C 6.2.7).
// Its clause spells `long volatile`: C23 6.7.2.2 makes the underlying type the UNQUALIFIED
// version of what the clause names, so this is main.c's `enum E : long` exactly.

enum E : long volatile { A = 40 };
enum S : unsigned char { SX = 2 };
struct Pair {
    enum S tag;
    enum E value;
};

long take(enum E e, struct Pair p) {
    return (long)e + (long)p.tag + (long)p.value - (long)A;
}

unsigned long pairSize(void) { return sizeof(struct Pair); }

unsigned long valueOffset(void) {
    struct Pair q;
    return (unsigned long)((char *)&q.value - (char *)&q);
}
