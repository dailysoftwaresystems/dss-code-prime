// TF-C79, CU B — the ordinary EXTERNAL definition cu_a.c's inline definition
// defers to. Returning a value distinct from cu_a's inline body is what makes
// the program's exit code identify which body ran.

int pick(void) { return 42; }
