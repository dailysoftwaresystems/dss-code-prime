/* c81 (the sqlite shell.c CLI route): ship <pwd.h> — shell.c expands ~ via
 * `pwent=getpwuid(getuid()); ... pwent->pw_dir` (shell.c:37705); the header
 * was missing -> F001A. LAYOUT-critical: libc returns a pointer to ITS OWN
 * struct passwd, so the reads below probe the glibc offsets directly:
 * pw_uid (u32 @16 — after two char*) must read back 0 for root, and pw_dir
 * (char* @32) must be an absolute path. A wrong field list misreads both.
 * HONEST TOLERANCE: getpwuid resolves through NSS; under qemu-user aarch64
 * the host NSS modules may be unloadable, where getpwuid legitimately
 * returns NULL — that arm exits 42 WITHOUT asserting the layout (the WSL
 * x86_64 leg, where root always resolves, is the layout proof; returning
 * early would hide nothing since NULL means libc never produced a struct).
 * RED-ON-DISABLE: delete pwd.json -> F001A on the include. */
#include <sys/types.h>
#include <pwd.h>

int main(void) {
    struct passwd *p = getpwuid((uid_t)0);   /* root: present on any unix */
    if (p == 0) return 42;                   /* NSS unavailable (qemu cross) — no struct to probe */
    if (p->pw_uid != 0u) return 1;           /* u32 @16: must echo the requested uid */
    if (p->pw_gid > 100000u) return 2;       /* u32 @20: root's gid is a small id (0 on glibc) */
    if (p->pw_dir == 0 || p->pw_dir[0] != '/') return 3;   /* char* @32: absolute home */
    if (p->pw_shell == 0 || p->pw_shell[0] != '/') return 4; /* char* @40: absolute shell */
    return 42;
}
