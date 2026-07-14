// D-CSUBSET-FOR-INIT-SCOPE (C99 6.8.5.3): each for-statement's init clause has its OWN
// scope, so sibling `for (int i = ...)` loops in one block that re-declare the same loop
// name are all valid (the second/third `i` is a distinct object in a distinct scope), and
// a nested `for (int i = ...)` inside a `for (int j = ...)` nests cleanly. This is a
// ubiquitous C idiom that a declare-then-assign test battery never exercises. Exit 42.
int main(void) {
    int s = 0;
    for (int i = 0; i < 4; i++) s = s + i;            // braceless  i#1 -> 0+1+2+3 = 6
    for (int i = 0; i < 4; i++) { s = s + i; }        // braced     i#2 (same name) -> 12
    for (int j = 0; j < 3; j++)                       // nested: an inner for(int i)
        for (int i = 0; i < 2; i++) s = s + i;        // (0+1)*3 = 3 -> 15
    for (int i = 0; i < 4; i++) s = s + i;            // sibling    i#4 -> 21
    s = s + 21;                                       // -> 42
    return s;
}
