// P68 round 9 (lane `cs`) — an INDEX DESIGNATOR is compared with its array's bound at
// FULL width, never narrowed to 32 bits first (negative / diagnostic).
//
// C 6.7.9p6: a designator `[ constant-expression ]` of an array of known size must
// name an element of it. DSS narrowed the index to 32 bits before asking, so an index
// of 2^32 or more WRAPPED to a small one that was in range and the element was stored
// there SILENTLY. Every refused element below is refused by gcc 13.3.0 and clang
// 18.1.3 (`-std=c17 -pedantic-errors` and `-std=c2x`) and by mingw-w64 13.2.0; MSVC
// 19.51 refuses the top-level ones (C2078) and builds the nested one while dropping
// its value. The same shapes IN range, in `accepted`, draw nothing — the expectation
// is EXACT SET EQUALITY, so a check that refused every designated element would fail
// this entry as surely as one that refused none.

static int g[2] = { [4294967296] = 7 };            // file scope: wrapped to g[0]

static int refused(void) {
    int a[2] = { [4294967296] = 7 };               // wrapped to a[0]
    int b[2] = { [4294967297] = 7 };               // wrapped to b[1]
    int c[2][2] = { [0][4294967296] = 7 };         // a nested index: wrapped to c[0][0]
    int d[2] = { [(long long)1 << 32] = 7 };       // a constant expression: wrapped to d[0]
    return a[0] + b[1] + c[0][0] + d[0];
}

static int accepted(void) {
    int a[2] = { [1] = 7 };
    int c[2][2] = { [0][1] = 7 };
    int e[] = { [3] = 7 };
    return a[1] + c[0][1] + e[3];
}

int main(void) { return refused() + accepted() + g[0]; }
