// The sibling CompilationUnit that DEFINES add5. main.c declares it `extern` and calls
// it; at link the merge binds main.c's reference to THIS definition (the sibling def
// shadows the library fallback) and strips the unused library import.
int add5(int x) {
    return x + 5;
}
