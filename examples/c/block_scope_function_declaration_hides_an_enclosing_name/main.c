/* A BARE BLOCK-SCOPE FUNCTION DECLARATION HAS ITS BLOCK'S SCOPE AND THE LINKAGE `extern` GIVES IT (C 6.2.1p4,
 * 6.2.2p5; P69 round 1, lane cs, fold f26b). Inside its block it HIDES a name of an enclosing scope -- an object of an
 * enclosing block (lane lm's probe k9h), a file-scope typedef (k9i) -- and a file-scope definition of the name is the
 * function it declares. Every return before 42 names the first fact that failed. */

typedef int T;

static int call_through_a_hiding_declaration(void) {
    int x = 1;
    {
        int x(void);            /* hides the object: `x()` calls the file-scope function below */
        return x() + 0 * 1;
    }
}

static int a_hidden_typedef_is_no_cast(void) {
    {
        char T(int);            /* hides the typedef: `(T)(2)` calls a function returning char, it is no cast */
        return sizeof ((T)(2)) == 1;
    }
}

int x(void) { return 42; }

int main(void) {
    if (call_through_a_hiding_declaration() != 42) return 1;
    if (!a_hidden_typedef_is_no_cast()) return 2;
    return 42;
}
