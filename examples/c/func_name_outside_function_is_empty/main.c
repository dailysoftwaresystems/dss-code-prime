// P68 round 9 (lane `cs`) — `__func__` used outside every function body. C 6.4.2.2
// declares it only inside a function definition; gcc 13.3.0 and clang 18.1.3 accept
// the file-scope use with a warning and agree it names the EMPTY string, and DSS
// follows them (S_PredefinedIdentifierOutsideFunction at each use). Inside a body it
// is still that function's own name.
//
// exit = 1 (sizeof "" in an enum) + 1 (__FUNCTION__, the configured alias)
//      + 1 (g[0] == 0) + 34 (a file-scope array of sizeof("") + 33 elements)
//      + 5 ("main" plus its NUL, inside the body) = 42.

enum { N = sizeof(__func__) };
enum { M = sizeof(__FUNCTION__) };
static const char *g = __func__;
static char buf[sizeof(__func__) + 33];

int main(void) {
    return N + M + (g[0] == 0 ? 1 : 0) + (int)sizeof buf + (int)sizeof(__func__);
}
