// Pre-reduced libc <stdio.h> for FFI ingestion (plan 11 FF2).
//
// Hermetic + curated subset — the c-subset grammar accepts only what
// the v1 language needs to express. No varargs (printf vararg form
// awaits a c-subset grammar extension); no size_t (awaits the typedef
// + unsigned integer types). The functions below cover the
// "hello-world via puts" path that FF6's smoke test exercises.
//
// Naming: filenames mirror system <stdio.h> for caller familiarity but
// the content is curated, NOT a verbatim copy.
//
// Owning library: libc.so.6 (Linux glibc) / libSystem.B.dylib (macOS)
// / msvcrt.dll (Windows). The caller specifies the library at
// `readCHeader(..., importLibrary, ...)` time — the header itself
// names symbols only.

extern int puts(const char* s);
extern int putchar(int c);
