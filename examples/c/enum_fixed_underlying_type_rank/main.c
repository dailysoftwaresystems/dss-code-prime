// P68 round 10 (lane `cs`) — C23 6.7.2.2 and C 6.3.1.1p1: an enumeration with a FIXED
// underlying type has that type's conversion RANK, and rank is by NAME (`long long` >
// `long` > `int`). So `enum E : long` beside an `unsigned int` converts exactly as `long`
// does — on LLP64 (Windows) the two share a width and the result is `unsigned long` (C
// 6.3.1.8's fifth conversion); on LP64 `long` is wider and the result is `long` (the
// fourth). The enumeration CONSTANT has the enumerated type too. An enum with no fixed
// type is the `int` control. `bad` counts the checks that fail.

enum E : long { A = 1 };
enum F : long long { B = 1 };
enum G { C = 1 };
// C23's clause is a specifier-QUALIFIER list, and the underlying type is the unqualified
// version of what it names: these two are `long` enums too.
enum Q : const long { QA = 1 };
enum R : long volatile { RA = 1 };

static int check(void) {
    enum E e = A;
    enum F g = B;
    enum G h = C;
    enum Q q = QA;
    enum R r = RA;
    unsigned int u = 1;
    unsigned long ul = 1;
    int const llp64 = sizeof(long) == sizeof(int);
    int bad = 0;

    bad += _Generic(e + u, unsigned long: 1, long: 2, unsigned int: 3, default: 4) != (llp64 ? 1 : 2);
    bad += _Generic(A + u, unsigned long: 1, long: 2, unsigned int: 3, default: 4) != (llp64 ? 1 : 2);
    bad += _Generic(g + ul, unsigned long long: 1, long long: 2, unsigned long: 3, default: 4) != (llp64 ? 2 : 1);
    bad += _Generic(h + u, unsigned int: 1, int: 2, default: 3) != 1;
    bad += _Generic(q + u, unsigned long: 1, long: 2, unsigned int: 3, default: 4) != (llp64 ? 1 : 2);
    bad += _Generic(r + u, unsigned long: 1, long: 2, unsigned int: 3, default: 4) != (llp64 ? 1 : 2);

    // The VALUES agree with the types: an unsigned result wraps -1 to its maximum.
    enum E minus = (enum E)-2;
    bad += ((minus + u) > 0) != llp64;
    return bad;
}

int main(void) { return check() == 0 ? 42 : 1; }
