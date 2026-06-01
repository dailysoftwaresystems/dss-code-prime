// Pre-reduced libc <stdlib.h> for FFI ingestion (plan 11 FF2 §4 Q1).
// `int` size is a v1 approximation pending the c-subset grammar's
// unsigned-integer / size_t extension; `void*` works as-is.

extern void* malloc(int size);
extern void  free(void* p);
extern void  exit(int status);
