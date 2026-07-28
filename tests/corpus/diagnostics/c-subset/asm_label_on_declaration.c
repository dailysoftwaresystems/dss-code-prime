// TF-C80: the GNU asm-LABEL-on-a-declaration form the grammar does not admit.
// This is the `sys/cdefs.h` `__DARWIN_ALIAS(sym)` shape, reduced and made
// SDK-independent, and it is the parse-failure CASCADE the TF-C80 renderer
// crash was layered on top of.
//
// This file pins the LOUD FAILURE. Admitting asm labels on declarations is a
// SEPARATE feature with its own anchor — if that lands, this golden changes
// deliberately. What must never happen again is the compiler ABORTING on this
// cascade (uncaught std::out_of_range from the diagnostic renderer) instead of
// reporting it. The `inline` definition is present because TF-C79's C99 inline
// support is what makes the surrounding real-header path reachable at all.
#define __STRING(x) #x
#define __DARWIN_SUF_UNIX03 ""
#define __DARWIN_ALIAS(sym) __asm("_" __STRING(sym) __DARWIN_SUF_UNIX03)
int aliased_fn(void) __DARWIN_ALIAS(aliased_fn);
inline int sputc(int c) { return c; }
int main(void) { return 42; }
