void note(int);
__attribute__((constructor)) void ctor_u3(void) { note(3); }
