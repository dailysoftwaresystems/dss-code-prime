/* glibc exports ONE environment object under three names — `environ`, `__environ` and `_environ`. Its <unistd.h>
 * declares `__environ` always and `environ` only under _GNU_SOURCE, and never `_environ`, so the program declares the
 * two it needs itself, as C 7.1.4 allows for a library name. DSS's <unistd.h> describes the object with ONE row and its
 * measured aliases; every name must reach the same object. */
#include <stdio.h>
#include <unistd.h>

extern char **environ;
extern char **_environ;

int main(void) {
    int const same = environ == __environ && environ == _environ && environ != NULL;
    printf("environ, __environ and _environ name one object: %d\n", same);
    return same ? 42 : 1;
}
