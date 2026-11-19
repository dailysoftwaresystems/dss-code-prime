/* P69 (lane `cs`) — D-C-A-READ-THROUGH-AN-ADDRESS-CONSTANT-IN-A-STATIC-INITIALIZER-IS-REFUSED:
 * a static initializer may read THROUGH a pointer whose value is an address constant, when the
 * object it lands in is const (or a string literal) and its value is a constant — `*p`, `p[i]`,
 * `p->m`. Exit 42 when every read holds its value, otherwise a bitmask:
 *   1 `s[0]` of a const pointer to a string literal, 2 `p[1]` of a const pointer to a const
 *   array, 4 `*p` of `a + 1`, 8 `*(a + 1)`, 16 `*p` of a pointer to a const char array,
 *   32 `*"*"`, 64 `*p` of `&s.b` beside `s.a`, 128 `p->b` of a pointer to a const structure,
 *   256 a block-scope const pointer read in a static local.
 * clang 18.1.3 builds every one; gcc 13.3.0 none (it folds only `*&k`) — one working
 * reference makes each required. */
static const char *const str = "*";
static const int arr[2] = { 1, 42 };
static const int *const parr = arr;
static const int *const pend = arr + 1;
static const char carr[] = "*";
static const char *const pc = carr;
static const struct S { int a, b; } cs = { 40, 2 };
static const int *const pb = &cs.b;
static const struct S *const ps = &cs;

char r1 = str[0];
int r2 = parr[1];
int r3 = *pend;
int r4 = *(arr + 1);
char r5 = *pc;
int r6 = *"*";
int r7 = cs.a + *pb;
int r8 = ps->b;

int main(int argc, char **argv) {
    (void)argv;
    int bad = 0;
    if (r1 != '*') bad |= 1;
    if (r2 != 42) bad |= 2;
    if (r3 != 42) bad |= 4;
    if (r4 != 42) bad |= 8;
    if (r5 != '*') bad |= 16;
    if (r6 != '*') bad |= 32;
    if (r7 != 42) bad |= 64;
    if (r8 != 2) bad |= 128;
    const char *const s = "*";
    static char r9 = s[0];
    if (r9 != '*') bad |= 256;
    return bad == 0 ? 42 + (argc - argc) : bad;
}
