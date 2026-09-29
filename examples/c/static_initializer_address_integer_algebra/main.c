// P68 round 13 (lane `cs`, the static-initializer item) — arithmetic on an ADDRESS held in an
// integer exactly as wide as a pointer, in a static initializer: gcc and mingw-w64 fold the
// identity and absorbing elements of each operator, and compare or subtract two such integers
// within one object; c.lang.json names the form
// (`semantics.staticInitializers.otherConstantForms: addressIntegerAlgebra`). Every other
// operation on such an integer (`* 2`, `& 0xFF`, `-x`, `100 - x`) is refused by every reference.
// `bad` counts the checks that fail.

int a[4];
int b[2];

unsigned long long zero = (unsigned long long)&a * 0;          // absorbing
unsigned long long rem = (unsigned long long)&a % 1;           // absorbing
unsigned long long one = (unsigned long long)&a * 1;           // identity ...
unsigned long long orZero = 0 | (unsigned long long)&a;
unsigned long long xorZero = (unsigned long long)&a ^ 0;
unsigned long long shift = (unsigned long long)&a >> 0;
unsigned long long ones = (unsigned long long)&a & ~0ULL;
int isNull = (unsigned long long)&a == 0;                       // an address is not null
int differ = (unsigned long long)&a != (unsigned long long)&b;  // two objects
int above = (unsigned long long)&a[1] > (unsigned long long)&a[0];
unsigned long long span = (unsigned long long)&a[2] - (unsigned long long)&a[0];

static int check(void) {
    unsigned long long const at = (unsigned long long)&a;
    int bad = 0;
    bad += zero != 0 || rem != 0;
    bad += one != at || orZero != at || xorZero != at || shift != at || ones != at;
    bad += isNull != 0 || differ != 1 || above != 1 || span != 2 * sizeof(int);
    return bad;
}

int main(void) { return check() == 0 ? 42 : 1; }
