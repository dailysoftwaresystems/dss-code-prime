// P68 round 12 (lane `cs`) — C23 6.7.3.3p3: an enumerator of an enumeration with a fixed
// underlying type must be representable in that type — and a value at or above 2^63 IS
// representable in `unsigned long long`. Its value carries its own signedness (the type
// of the constant expression, or the previous constant's for an implicit `+ 1`), so it
// is never read as a negative 64-bit number. `bad` counts the checks that fail.

enum AllOnes : unsigned long long { X = 0xFFFFFFFFFFFFFFFFULL };
enum TopBit : unsigned long long { T = 0x8000000000000000ULL };
enum PastMax : unsigned long long { A = 0x7FFFFFFFFFFFFFFFULL, B };

static int check(void) {
    int bad = 0;
    bad += !(X == 0xFFFFFFFFFFFFFFFFULL && X > 0);
    bad += !(T == 0x8000000000000000ULL && T > 0);
    bad += B != 0x8000000000000000ULL;
    bad += (B - A) != 1;
    return bad;
}

int main(void) { return check() == 0 ? 42 : 1; }
