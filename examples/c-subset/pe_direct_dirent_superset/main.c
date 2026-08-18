/* WITNESS for the declared SUPERSET DEVIATION in direct.json:
 * on `pe`, <direct.h> re-exports <dirent.h> (and, through it, <windows.h>),
 * which NEITHER reference compiler does.  A superset is only compatible if it
 *   (a) never makes LESS available, and
 *   (b) never changes the meaning of anything already visible.
 * (b) is the half that cannot be assumed, so this TU asserts it by EXECUTION.
 *
 * ★ THIS FILE DELIBERATELY NEVER INCLUDES <windows.h>.  Everything Win32 that
 *   reaches it arrives over the new edges; if the chain regresses, this stops
 *   compiling rather than silently losing the deviation.
 *
 * The elf/macho legs compile the SAME source and take the `#else` arm, so the
 * example is a differential over the header GRAPH, not over the program. */

#if defined(_WIN32)
#  include <direct.h>          /* pe: the ONLY header this arm needs */
#else
#  include <unistd.h>          /* the control arm; <direct.h> is pe-only */
#  include <dirent.h>
#endif
#include <sys/stat.h>
#include <string.h>

/* ── (b) A USER'S OWN DECLARATIONS MUST STILL WIN ─────────────────────────
 * The operator predicted these three would collide with the newly-visible
 * shipped surface.  They must not: a user declaration outranks a shipped one.
 * Each is deliberately INCOMPATIBLE with the platform spelling it shadows. */

typedef int HANDLE;              /* Win32's is a void* — ours is an int */

/* ⚠ A COMPLETE user `struct DIR { ... }` is DELIBERATELY NOT PROBED HERE, and
 * that omission is the finding rather than an oversight: on pe, AFTER the
 * <direct.h>→<dirent.h> edge, such a definition is a HARD ERROR
 * (F_ShippedTypeIdentityConflict) where it compiled before the edge. That is a
 * REAL narrowing the deviation introduces — see direct.json. Only the FORWARD
 * declaration stays legal, so that is what is asserted. */
struct DIR;
static struct DIR *opaque_user_ptr;

static int free_slots(int n) {   /* collides with <stdlib.h>'s `free` root */
    return n * 3;
}

int main(void) {
    int f = 0;
    HANDLE h;

    /* the user's HANDLE is an int, 4 bytes — NOT Win32's 8-byte void* */
    h = 7;
    if ((int)sizeof(h) != (int)sizeof(int)) f = f + 1;
    if (h != 7) f = f + 2;

    /* a user FORWARD declaration of the shipped tag stays legal and usable */
    opaque_user_ptr = 0;
    if (opaque_user_ptr != 0) f = f + 4;

    if (free_slots(4) != 12) f = f + 32;

#if defined(_WIN32)
    /* ── (a) THE DEVIATION ITSELF, on pe only ───────────────────────────
     * `DIR *` and the reading half reach us through <direct.h> alone.  On
     * mingw this line does not compile; that is the deviation, witnessed. */
    {
        DIR   *d;
        void  *entry;
        char   cwd[512];

        if (_getcwd(cwd, 512) == 0) return f + 64;
        d = opendir(cwd);
        if (d == 0) return f + 128;
        entry = readdir(d);
        if (entry == 0) f = f + 256;
        if (closedir(d) != 0) f = f + 512;
    }
    /* ── THE CHAIN ITSELF, name by name ────────────────────────────────
     * This TU includes <direct.h> and NOTHING else Win32 or C-library.
     *   `free`  reaches us ONLY via  direct.h -> dirent.h -> windows.h -> stdlib.h
     *   `DWORD` reaches us ONLY via  direct.h -> dirent.h -> windows.h
     * so each name is a live guard on a specific edge: drop edit 1 and `free`
     * goes undeclared; drop edit 2 and `DWORD` does; drop edit 3 and both do. */
    {
        void  *blk;
        DWORD  w;

        blk = malloc(32);
        if (blk == 0) return f + 4096;
        free(blk);
        w = 5;
        if ((int)sizeof(w) != 4) f = f + 8192;
        if (w != 5) f = f + 16384;
    }

    /* S_ISLNK is DECLARED on pe and is the constant 0 — no consumer shim */
    if (S_ISLNK(40960) != 0) f = f + 1024;
    if (S_ISREG(32768) == 0) f = f + 2048;
#else
    /* the control arm: POSIX keeps its own meaning, and <direct.h> is a
     * pe-only header there, so the deviation is scoped to pe by construction */
    if (S_ISLNK(40960) == 0) f = f + 1024;
    if (S_ISREG(32768) == 0) f = f + 2048;
#endif

    if (f != 0) return f;
    return 42;
}
