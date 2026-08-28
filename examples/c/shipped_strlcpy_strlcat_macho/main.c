/* D-CSUBSET-DARWIN-BSD-SYMBOL-CLUSTER — shipped <string.h> strlcpy/strlcat on
 * macOS: the BSD bounded copy/concat pair sqlite os_unix.c's proxy-locking
 * region calls (strlcpy first at os_unix.c:7434, strlcat at :7443), all inside
 * `#if defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE`.
 *
 * THE RETURN VALUE IS THE POINT. BSD semantics: each returns the length of the
 * string it TRIED to create — strlcpy returns strlen(src); strlcat returns the
 * initial strlen(dst) + strlen(src) — NOT the dest pointer strcpy/strcat
 * return, and NOT the number of bytes written. sqlite consumes the returns
 * (`len = strlcpy(...)`, and truncation checks compare them), so a wrong
 * result type or ABI here is a silent lock-path corruption, never a
 * diagnostic. Both calls below are FFI calls into libSystem — opaque to the
 * optimizer, so nothing folds: the values 5 and 8 must actually cross the ABI.
 *
 * Exit arithmetic uses BOTH returns: n1 = 5 (strlen("hello")), n2 = 8
 * (strlen("hello") + strlen("!!!")), (int)(n1 * n2) + 2 = 5*8 + 2 = 42.
 *
 * RED-ON-DISABLE: delete the strlcpy (or strlcat) row from
 * src/dss-config/shippedLibs/string.json and that name is S0001 (undeclared
 * identifier), no binary — the descriptor SHADOWS the real SDK _string.h
 * totally, so the row is the ONLY possible source. The dest-content check
 * guards the write side; the guards + exit arithmetic pin both returns. */
#include <string.h>

int main(void) {
    char buf[32];
    size_t n1 = strlcpy(buf, "hello", 32);  /* 5 = strlen("hello")          */
    size_t n2 = strlcat(buf, "!!!", 32);    /* 8 = 5 + strlen("!!!")        */

    if (strcmp(buf, "hello!!!") != 0) return 1;  /* the WRITE side           */
    if (n1 != 5) return 2;                       /* BSD return: source length */
    if (n2 != 8) return 3;                       /* BSD return: total length  */
    return (int)(n1 * n2) + 2;                   /* 5*8 + 2 = 42              */
}
