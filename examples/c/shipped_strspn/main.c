/* c51 (D-FFI-STRING-STRSPN): ship strspn + strcspn from <string.h> — sqlite uses
 * `strspn(z, "...")` (sqlite3.c:37894 + json/fts) and `strcspn(...)` (134104),
 * both missing from string.json -> S0001 undeclared. Both: fn(ptr<char>,
 * ptr<char>) -> u64 (returns size_t; sqlite casts the int result). All formats
 * (string.json has the pe msvcrt + elf/macho libc map). RED-ON-DISABLE: remove
 * either symbol -> S0001. Value-correct (libc strspn/strcspn are deterministic). */
#include <string.h>

int main(void) {
    /* strspn = length of the leading run of chars IN the set */
    if (strspn("12 34", "0123456789") != 2) return 1;   /* '1','2' in; ' ' stops */
    /* strcspn = length of the leading run of chars NOT in the set */
    if (strcspn("ab 12", "0123456789") != 3) return 2;  /* 'a','b',' ' not; '1' stops */
    return 42;
}
