// P68 round 12 (lane `cs`) — C23 6.7.3.4p6: an enumeration with a FIXED underlying type
// is complete immediately after its enum type specifier, so its own tag names a complete
// type INSIDE its enumerator list (6.7.3.3's EXAMPLE 3 spells `m40 = sizeof(enum E4)`
// as valid), and its constants have the enumerated type there (6.7.3.3p12). `bad`
// counts the checks that fail.

enum Big : long long { B_SIZE = sizeof(enum Big), B_ONE = 1,
                       B_KIND = _Generic(B_ONE, enum Big: 7, default: 9),
                       B_CAST = sizeof((enum Big)B_ONE) };
enum Small : unsigned char { S_SIZE = sizeof(enum Small) + 41 };

static int check(void) {
    int bad = 0;
    bad += B_SIZE != 8;
    bad += B_KIND != 7;
    bad += B_CAST != 8;
    bad += S_SIZE != 42;
    return bad;
}

int main(void) { return check() == 0 ? 42 : 1; }
