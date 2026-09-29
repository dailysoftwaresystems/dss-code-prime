// [[D-C-DOLLAR-IN-IDENTIFIERS-REFUSED]] — `$` starts and continues a C name.
//
// C23 6.4.2.1 lets an implementation add "other implementation-defined
// characters" to identifiers, and every reference compiler adds `$`.
// ✔MEASURED 2026-09-22: gcc 13.3.0, clang 18.1.3, MinGW gcc and MSVC VS 18
// accept every name below. DSS refused each one with P_IllegalChar until C's
// document declared `identifierClass { extraStart "$", extraContinue "$" }`.
//
// Runtime witness: exits 42 only if every `$` name is its own object with its
// own value — leading, inside, trailing, alone, before a digit, a function,
// a struct tag and member, a macro name tested by `#if`, a stringized
// argument, a pasted name, and two names with EXTERNAL linkage, so the symbol
// table of every object format carries a `$` too.

#define S(x) #x
#define CAT(a, b) a##b
#define TEN$ 10
#define ON$ 1

static int $ = 2;        // alone
static int $lead = 3;    // leading
static int in$ide = 4;   // inside
static int trail$ = 5;   // trailing
static int $1 = 6;       // before a digit: a name, not a number

int f$(int v) { return v + 1; }   // external linkage: a `$` symbol
int CAT(x, $y) = 7;               // pasted to `x$y`, external linkage

struct $pair { int $first; int second$; };

int main(void) {
    struct $pair p = { 1, 2 };
    int sum = $ + $lead + in$ide + trail$ + $1 + f$(0) + x$y   // 2+3+4+5+6+1+7 = 28
            + p.$first + p.second$;                            // 31
#if ON$
    sum += TEN$;                                                // 41
#else
    sum = 0;
#endif
    char const *s = S(a$b);   // the spelling survives: "a$b"
    if (s[0] != 'a' || s[1] != '$' || s[2] != 'b' || s[3] != '\0') return 1;
    return sum + 1;           // 42
}
