void note(int);
__attribute__((constructor)) void ctor_u1(void) { note(1); }
