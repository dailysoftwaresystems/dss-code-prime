int main(void) { int l = 42; static int *p = &l; return *p; }
