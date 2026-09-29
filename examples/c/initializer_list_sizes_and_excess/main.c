/* Three initializer-list rules, each worth 7:
 *   C 6.7.9p22 - an array of unknown size is as long as the LARGEST index its list
 *                initializes, plus one: a designator moves the index, positional
 *                elements count on from it (a compound literal too);
 *   C 6.7.9p14 - a character array may be initialized by a string literal
 *                "optionally enclosed in braces";
 *   C 6.7.9p2  - an EXCESS positional element is a constraint violation that the
 *                accepting compilers warn about and DROP, never evaluating it.
 * exit 42 = every size, every value, and no excess element ran. */
struct P { int x, y; };

static int calls = 0;
static int bump(void) { return ++calls; }

int main(void) {
    int sum = 0;

    int a[] = { [3] = 5 };
    if (sizeof a == 4 * sizeof(int) && a[3] == 5 && a[0] == 0) sum += 7;

    int b[] = { 1, [5] = 2, 3 };
    if (sizeof b == 7 * sizeof(int) && b[5] == 2 && b[6] == 3) sum += 7;

    struct P ps[] = { [2].y = 7 };
    if (sizeof ps == 3 * sizeof(struct P) && ps[2].y == 7 && ps[2].x == 0) sum += 7;

    char s[] = { "abc" };
    if (sizeof s == 4 && s[0] == 'a' && s[2] == 'c' && s[3] == 0) sum += 7;

    int *cp = (int[]){ [2] = 9 };
    if (sizeof((int[]){ [2] = 9 }) == 3 * sizeof(int) && cp[2] == 9) sum += 7;

    int e[2] = { 1, 2, bump() };
    struct P q = { 3, 4, bump() };
    if (e[0] + e[1] == 3 && q.x + q.y == 7 && calls == 0) sum += 7;

    return sum;
}
