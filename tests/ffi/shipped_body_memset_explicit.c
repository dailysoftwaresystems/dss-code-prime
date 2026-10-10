/* DSS's shipped body of C23 7.26.6.2 `memset_explicit`, compiled by THIS LEG'S OWN C COMPILER
 * under a test-only name, for tests/ffi/test_shipped_string_bodies.cpp to call.
 *
 * The shipped file (src/dss-config/runtime/platform/src/memset_explicit.c) is included AS IT IS:
 * the rename is here, in the test, and nothing in the shipped tree knows of it. The C library's
 * header is included first, under its own names — a library that one day declares
 * `memset_explicit` keeps its declaration — and only then does the name of the DEFINITION
 * change. The shipped file's own include is then already satisfied.
 *
 * The test-only name is declared once, in shipped_string_bodies.h, which the cases read too:
 * a body whose signature drifted from that declaration does not compile here.
 */
#include "shipped_string_bodies.h"

#include <string.h>

#undef memset_explicit
#define memset_explicit dss_shipped_body_memset_explicit
#include "../../src/dss-config/runtime/platform/src/memset_explicit.c"
