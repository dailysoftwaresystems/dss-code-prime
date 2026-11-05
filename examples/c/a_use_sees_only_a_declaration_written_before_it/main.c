/* A USE SEES ONLY A DECLARATION WRITTEN BEFORE IT (C 6.2.1p7; P69 round 1, lane cs, fold f26c). An ordinary
 * identifier's scope begins just after its declarator completes (an enumeration constant's after its enumerator),
 * so before an inner declaration of a name the ENCLOSING one is in scope -- for an object, a typedef and an
 * enumeration constant alike -- and a macro is read where it is expanded, not where it is defined. Every return
 * before 42 names the first fact that failed. */
typedef int T;
int y = 40;

#define ADD (a + b)            /* names parameters declared after this line */
#define SZ ((int)sizeof(L))    /* names a typedef declared after this line */
typedef long long L;

static int outer_object(void) { int r = y; int y = 2; return r + y; }                   /* the file's y, then its own */
static int nested_block(void) { int r = 40; { int s = r; int r = 2; return s + r; } }   /* the outer r, then its own */
static int own_initializer(void) {
    int r = 40;
    { long long r = (long long)sizeof r; if (r != (long long)sizeof(long long)) return 0; }   /* the NEW r */
    return r + 2;
}
static int typedef_later(void) {
    int r = (int)sizeof(T);    /* the file's T: its block's own is declared below */
    typedef char T;
    return r == (int)sizeof(int) && sizeof(T) == 1 ? 42 : 0;
}
static int add(int a, int b) { return ADD; }
enum { A = 40 };
static int enum_own(void) { enum { A = A + 2 }; return A; }   /* the initializer names the outer A */

int main(void) {
    if (outer_object() != 42) return 1;
    if (nested_block() != 42) return 2;
    if (own_initializer() != 42) return 3;
    if (typedef_later() != 42) return 4;
    if (add(40, 2) != 42) return 5;
    if (SZ != 8) return 6;
    if (enum_own() != 42) return 7;
    return 42;
}
