/* <dlfcn.h>'s pseudo-handles: dlsym takes RTLD_DEFAULT (the default search) or RTLD_NEXT (the objects loaded after
 * the caller) in place of a library handle. Each C library spells its own: glibc RTLD_DEFAULT ((void *) 0) and
 * RTLD_NEXT ((void *) -1l); Apple's RTLD_DEFAULT ((void *) -2), RTLD_NEXT ((void *) -1), plus RTLD_SELF ((void *) -3)
 * and RTLD_MAIN_ONLY ((void *) -5). The program prints each value and resolves `puts` through both. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    void *const viaDefault = dlsym(RTLD_DEFAULT, "puts");
    void *const viaNext = dlsym(RTLD_NEXT, "puts");
    printf("RTLD_DEFAULT=%ld RTLD_NEXT=%ld default-finds-puts=%d next-finds-puts=%d",
           (long)(intptr_t)RTLD_DEFAULT, (long)(intptr_t)RTLD_NEXT, viaDefault != NULL, viaNext != NULL);
#ifdef RTLD_SELF
    printf(" RTLD_SELF=%ld", (long)(intptr_t)RTLD_SELF);
#endif
#ifdef RTLD_MAIN_ONLY
    printf(" RTLD_MAIN_ONLY=%ld", (long)(intptr_t)RTLD_MAIN_ONLY);
#endif
    printf("\n");
    return viaDefault != NULL && viaNext != NULL ? 42 : 1;
}
