/* D-C-ATOMICS-RUNTIME-PE64-BUS-LOCKS-EVERY-ACCESS-TO-A-CACHE-LINE-STRADDLING-OBJECT
 * — the LIBRARY half of the two-image race. The runner builds this into a DLL
 * and `main.c` into an exe resolving against it.
 *
 * ★★★ THE POINT IS THAT THIS IMAGE CARRIES ITS OWN COPY OF THE ATOMICS RUNTIME.
 * The accesses below go through a pointer to a PACKED struct, so their lvalue's
 * provable alignment is 1 and each lowers to `__atomic_store` / `__atomic_load`.
 * On pe64 those resolve against the shipped runtime ARCHIVE at THIS image's
 * link, so the DLL and the exe that loads it each contain a separate body. Two
 * bodies can only be mutually atomic on a line-crossing object if they take the
 * SAME lock, which is why the body's lock is the process heap's (one per
 * process) and never a table of its own (one per image). `main.c` races the two
 * copies against each other; a per-image arbiter tears there.
 */
struct __attribute__((packed)) Packed {
    char              c;
    _Atomic unsigned  a;
};

void dss_two_image_store(struct Packed *p, unsigned v) {
    p->a = v;
}

unsigned dss_two_image_load(struct Packed *p) {
    return p->a;
}
