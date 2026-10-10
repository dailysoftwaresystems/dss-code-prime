/* DSS's shipped body of C23 7.26.2.7 `strndup`, compiled by THIS LEG'S OWN C COMPILER under a
 * test-only name, for tests/ffi/test_shipped_string_bodies.cpp to call.
 *
 * The shipped file (src/dss-config/runtime/platform/src/strndup.c) is included AS IT IS: the
 * rename is here, in the test, and nothing in the shipped tree knows of it. The C library's
 * headers are included first, under their own names — so the library's own `strndup`, where it
 * has one, keeps its declaration — and only then does the name of the DEFINITION change. The
 * shipped file's own includes are then already satisfied.
 *
 * The test-only name is declared once, in shipped_string_bodies.h, which the cases read too:
 * a body whose signature drifted from that declaration does not compile here.
 */
#include "shipped_string_bodies.h"

#include <stdlib.h>
#include <string.h>

#undef strndup
#define strndup dss_shipped_body_strndup
#include "../../src/dss-config/runtime/platform/src/strndup.c"
