// FC16 C11/C23 6.5.1.1: a `_Generic` whose controlling type (`double`) matches
// NEITHER typed association (`int`, `char`) and has NO `default` — a constraint
// violation. The analyzer fails loud with S_GenericSelectionNoMatch at the
// `_Generic` expression's span.
int main(void) {
    double d = 0;
    return _Generic(d, int: 1, char: 2);
}
