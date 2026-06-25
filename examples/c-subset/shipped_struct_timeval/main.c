/* Cluster G — the struct-body descriptor mechanism (the FIRST named-field struct
 * shipped via a descriptor). `#include <sys/time.h>` resolves to shippedLibs/sys/
 * time.json (the c6 subdir resolver) whose `structs` surface declares
 * `struct timeval { i64 tv_sec; i64 tv_usec; }`. The semantic phase injects the
 * tag (TAG namespace) + a field scope + compositeScopeByType, so by-NAME field
 * access resolves and the layout engine derives the offsets (tv_sec@0, tv_usec@8).
 *
 * exit 42 is the witness that (a) `struct timeval` resolved from the descriptor,
 * (b) tv_sec/tv_usec field access compiles, (c) the read/write lands at the right
 * offsets, on every ABI. RED-ON-DISABLE: without the `structs` surface +
 * injection, `struct timeval` is an unknown type → the compile fails (no binary). */
#include <sys/time.h>

int main(void) {
    struct timeval tv;
    tv.tv_sec  = 40;
    tv.tv_usec = 2;
    return (int)(tv.tv_sec + tv.tv_usec);   /* 42 */
}
