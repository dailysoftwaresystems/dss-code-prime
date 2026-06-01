// Pre-reduced libc <stdlib.h> for FFI ingestion (plan 11 FF2).
//
// Same curation rules as stdio.h — only declarations the c-subset
// grammar accepts. malloc/free use `void*` and an `int` size for v1
// (a future grammar extension introducing `size_t` will widen this
// to the ABI-correct unsigned long; the FF6 smoke test pins the v1
// shape).

extern void* malloc(int size);
extern void  free(void* p);
extern void  exit(int status);
