int b(void) { return f(40) + 2; }
int c(void) { return f(1) + 41; }
int f(int v) { return v; }

/* A CALL BEFORE ANY DECLARATION OF ITS CALLEE DECLARES IT IMPLICITLY, AND THE LATER DEFINITION IS THE FUNCTION CALLED
 * (C90 6.3.2.2; P69 round 1, lane cs, fold f26c). `f` is called twice before its definition: the first call declares
 * it, warned once (gcc's -Wimplicit-function-declaration and cl's C4013 are said once per name), the second takes the
 * same declaration. The code sits on the file's first lines so the warning's position is line 1. Every return before
 * 42 names the first fact that failed. */
int main(void) {
    if (b() != 42) return 1;
    if (c() != 42) return 2;
    return 42;
}
