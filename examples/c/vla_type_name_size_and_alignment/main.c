/* The SIZE and ALIGNMENT a variable-length TYPE NAME answers. `sizeof` of a variable-length typedef's name is the size
   the typedef froze when its declaration was reached (C17 6.7.8p3) — `n` changed afterwards moves nothing — and a
   type name's own suffix multiplies it; `_Alignof` of a variable-length type is its element's alignment and its operand
   is not evaluated (C 6.5.3.4p3). Each helper returns 42 when its answer is C's. */

static int typedefNames(int n) {
    typedef int V[n];
    n = 9;
    if (sizeof(V) != 2 * sizeof(int)) return 1;
    if (sizeof(volatile V) != 2 * sizeof(int)) return 2;
    if (sizeof(V[3]) != 6 * sizeof(int)) return 3;
    if (sizeof(volatile V[3]) != 6 * sizeof(int)) return 4;
    return 42;
}

static int alignments(int n) {
    typedef double W[n][3];
    if (_Alignof(int[n]) != _Alignof(int)) return 5;
    if (_Alignof(W) != _Alignof(double)) return 6;
    if (_Alignof(volatile W) != _Alignof(double)) return 7;
    return 42;
}

int main(void) {
    int const a = typedefNames(2);
    if (a != 42) return a;
    return alignments(4);
}
