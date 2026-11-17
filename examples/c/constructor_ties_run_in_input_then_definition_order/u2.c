void note(int);
__attribute__((constructor)) void ctor_u2(void) { note(2); }
