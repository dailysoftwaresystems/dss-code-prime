/* A PASTED NAME IS ORDERED WHERE IT IS WRITTEN (P69 round 1, lane cs, fold f26c). The C
 * front end asks "is A written before B" at many places, and answered it by BYTE OFFSET --
 * which a token the preprocessor materializes does not keep: the product of a paste (`##`)
 * lives in the synth buffer's product tail, past every source byte. So a struct whose tag was
 * pasted was incomplete for every later function (its parameter, its result, a call's
 * argument), a declaration whose name was pasted was not seen by a later `sizeof`, and a const
 * object whose initializer was pasted was "not visible" to a later static initializer
 * (MEASURED, probes fp3-fp6). gcc 13.3.0, clang 18.1.3, mingw-w64 gcc 13.2.0 and Apple clang
 * compile and run every form here to 42: a paste yields one preprocessing token, and every
 * later phase sees the program the plain spelling writes. Every return before 42 names the
 * first form that failed. */
#define CAT(a, b) a##b

/* A composite completed by a pasted tag, before a definition's parameter and result and a
 * call's argument and result. */
struct CAT(S, 1) { int v; };
int get(struct S1 s);
struct S1 mk(int v);

/* An array of unknown size completed by a declaration whose name was pasted. */
extern int arr1[];
int CAT(arr, 1)[3] = {1, 2, 39};

/* A const object whose initializer was pasted, read by a later static initializer. */
static const int k = CAT(4, 2);
static int g = k;

volatile int one = 1;

int main(void) {
    struct S1 x = {one * 42};
    if (get(x) != 42) return 1;
    if (mk(one * 42).v != 42) return 2;
    if (sizeof arr1 != 3 * sizeof(int)) return 3;
    if (arr1[0] + arr1[1] + arr1[2] != 42) return 4;
    if (g != 42) return 5;
    return 42;
}

int get(struct S1 s) { return s.v; }
struct S1 mk(int v) { struct S1 r = {v}; return r; }
