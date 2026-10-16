/* The test-only names of the <string.h> bodies DSS ships — ONE declaration of each, read by
 * BOTH sides: the C wrapper unit that defines it (shipped_body_strndup.c,
 * shipped_body_memset_explicit.c) and the C++ cases that call it
 * (test_shipped_string_bodies.cpp).
 *
 * WHY ONE HEADER AND NOT A DECLARATION ON EACH SIDE: a C function has no type in its link name,
 * so a definition and a caller that disagree about a parameter LINK and then misbehave. Here the
 * wrapper includes this header before it includes the shipped body, so a body whose signature
 * drifted from the declaration the cases call does not compile.
 *
 * C and C++ both read it: plain ISO C, with the linkage block C++ needs.
 */
#ifndef DSS_TESTS_FFI_SHIPPED_STRING_BODIES_H
#define DSS_TESTS_FFI_SHIPPED_STRING_BODIES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* src/dss-config/runtime/platform/src/strndup.c — C23 7.26.2.7 */
char *dss_shipped_body_strndup(const char *s, size_t n);

/* src/dss-config/runtime/platform/src/memset_explicit.c — C23 7.26.6.2 */
void *dss_shipped_body_memset_explicit(void *s, int c, size_t n);

#ifdef __cplusplus
}
#endif

#endif
