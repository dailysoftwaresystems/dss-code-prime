/* POSIX's <unistd.h> typedefs, from <unistd.h> ALONE (P69, D-C-UNISTD-H-LACKS-SEVEN-POSIX-TYPEDEFS): size_t, ssize_t,
 * off_t, pid_t, intptr_t — and uid_t/gid_t where the platform's <unistd.h> has them (glibc and Apple; mingw-w64's has
 * neither). Each type is classified by _Generic BEFORE any other header is included, so every name below comes from
 * <unistd.h>; the codes are printed afterwards. 1 int, 2 unsigned int, 3 long, 4 unsigned long, 5 long long,
 * 6 unsigned long long. */
#include <unistd.h>

#define KIND(T) _Generic((T)0, int: 1, unsigned int: 2, long: 3, unsigned long: 4, long long: 5, \
                         unsigned long long: 6, default: 0)

static int const kinds[] = {KIND(size_t), KIND(ssize_t), KIND(off_t), KIND(pid_t), KIND(intptr_t)};
static int const sizes[] = {(int)sizeof(size_t), (int)sizeof(ssize_t), (int)sizeof(off_t), (int)sizeof(pid_t),
                            (int)sizeof(intptr_t)};
#if !defined(_WIN32)
static int const ids[] = {KIND(uid_t), KIND(gid_t)};
#endif

#include <stdio.h>

int main(void) {
    printf("size_t=%d/%d ssize_t=%d/%d off_t=%d/%d pid_t=%d/%d intptr_t=%d/%d", kinds[0], sizes[0], kinds[1], sizes[1],
           kinds[2], sizes[2], kinds[3], sizes[3], kinds[4], sizes[4]);
#if !defined(_WIN32)
    printf(" uid_t=%d gid_t=%d", ids[0], ids[1]);
#endif
    printf("\n");
    return 42;
}
