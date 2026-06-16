/* FC7 (D-FC7-MEMBER-ACCESS) — the alloca-SIZING correctness pin (the
 * silent-neighbour-corruption witness, adversarial like deref_store).
 * `struct Big` is 20 bytes (5 ints) > the 16-byte frame-slot stride, so
 * its alloca MUST reserve 2 slots. Writing its FAR field `s.e` (byte
 * offset 16) must land WITHIN s, never clobber the adjacent `guard` local.
 * Pre-FC7 every alloca got ONE 16-byte slot, so `s.e` would overwrite
 * guard. `guard` is pre-seeded to a sentinel; `touch(&s,...)` writes
 * through a pointer (address-taken → not register-promoted in the
 * baseline pipeline, where guard is also memory-resident) so the clobber
 * is observable. A correct frame → guard survives → exit 0; an
 * under-sized slot → guard clobbered → exit 1. RED-ON-DISABLE: revert the
 * lir_callconv per-alloca sizing and this exits 1. */
struct Big { int a; int b; int c; int d; int e; };
int touch(struct Big* p, int v) { p->e = v; return p->a; }
int main(void) {
    struct Big s;
    int guard;
    guard = 12345;
    touch(&s, 99);
    return guard == 12345 ? 0 : 1;
}
