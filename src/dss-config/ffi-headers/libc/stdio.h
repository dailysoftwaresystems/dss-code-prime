// Pre-reduced libc <stdio.h> for FFI ingestion (plan 11 FF2 §4 Q1).
// Hermetic + curated subset. No varargs (awaits a c-subset grammar
// extension) and no size_t (awaits unsigned integer types) — these
// limitations mean v1 ships the minimum surface for a `puts`-based
// smoke test. Plan 11 FF6's printf path triggers once the grammar
// gains varargs.

extern int puts(const char* s);
extern int putchar(int c);
