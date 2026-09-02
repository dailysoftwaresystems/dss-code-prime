/* D-FFI-DARWIN-MALLOC-ZONE-TAIL-OPAQUE — the RUNTIME witness for the 22 members
 * that used to be one opaque 176-byte array.
 *
 * WHY IT EXISTS. Until P48, `src/dss-config/shippedLibs/malloc/malloc.json`
 * declared `struct _malloc_zone_t` at its true 200 bytes but typed only
 * `reserved1`, `reserved2` and `size`; the remaining 176 bytes were one
 * `__opaque_tail` byte array. Every member past `size` therefore failed loud as a
 * missing member — the right failure, but a wall. All 25 are typed now, and the
 * unit pin (tests/ffi/test_darwin_struct_shapes.cpp) asserts what the DESCRIPTOR
 * says. That is not the same question as whether the descriptor matches the real
 * libmalloc: a self-consistent wrong table passes a descriptor test perfectly.
 * This example asks the other question, and it can only be asked at RUN time,
 * against a zone that Apple's own allocator built.
 *
 * ★★ EVERY OFFSET HERE IS READ THROUGH A POINTER THE SYSTEM ALLOCATOR RETURNED.
 * `malloc_default_zone()` hands back a real `malloc_zone_t *` owned by
 * libsystem_malloc. So `z->version` at 104 is not our own arithmetic read back to
 * us — it is a field of a struct this program did not lay out, and a wrong offset
 * yields a wrong VALUE from live memory rather than a consistent lie. The
 * version, name and introspection guards are what turn "the offset arithmetic is
 * internally consistent" into "the offset arithmetic agrees with Apple's".
 *
 * ★ THE 4-BYTE TAIL PAD IS THE SUBTLE PART, so it is pinned twice. `unsigned
 * version` is 4 bytes at 104, and the next member must 8-align, so `memalign`
 * sits at 112 and bytes 108..111 are padding. A descriptor that had modelled
 * `version` as 8 bytes, or dropped the pad, would put `memalign` at 108 or 112-4
 * and every later member would slide. Guard 8 catches it, and the exit sum
 * re-states it as `offMemalign - offVersion - 8 == 0`.
 *
 * ★ THE CALLABLE MEMBERS ARE CALLED, not merely addressed. Taking `&z->malloc`
 * proves the offset; INVOKING it proves the signature, and a wrong parameter list
 * on a member that Apple's allocator implements is exactly the silent-miscompile
 * class the opaque tail existed to avoid. `size`, `malloc`, `calloc`, `realloc`,
 * `free` and `memalign` are each driven through the live default zone and their
 * results cross-checked against invariants (usable >= requested; realloc
 * preserves; calloc really zeroes; memalign really aligns) or against the shipped
 * free function `malloc_size`, never against a literal.
 *
 * HERMETIC BY CONSTRUCTION. No size, address, page size, allocator version or
 * zone name is compared to a literal. `version` is only required to be in a sane
 * open range, the name and introspection table only to be non-null, and every
 * block is freed back to the zone it came from. `memalign` is gated on
 * `version >= 5`, which is the version Apple documents it from, so an older
 * allocator skips it rather than failing. The zone itself is never destroyed —
 * it is the process's default zone.
 *
 * NOT DESTROYED, NOT NAMED, NOT PRESSURED: `destroy`, `batch_malloc`,
 * `batch_free`, `pressure_relief`, `claimed_address`, `try_free_default`,
 * `malloc_with_options` and the five `malloc_type_*` entries are typed by the
 * descriptor but NOT called here. They are optional-and-may-be-NULL by Apple's
 * own comments, version-gated above the live allocator, or destructive; calling
 * them would trade a hermetic witness for a fragile one. Their offsets are still
 * covered — `malloc_type_malloc_with_options` is the LAST member, so guard 9
 * pins the far end of the table and every member between two pinned offsets is
 * bounded by them.
 *
 * RED-ON-DISABLE: restore the `__opaque_tail` form of `malloc.json`'s
 * `structs[0]` -> every `z-><member>` past `size` is a missing member and there
 * is no binary; the descriptor SHADOWS the real SDK header totally, so this file
 * has no other source for the names. Move any single offset in the descriptor and
 * the matching guard (4..9) returns its own number. Re-model `version` as 8 bytes
 * and guard 8 returns. Drop the `x86_64` variant arm and the arm64 build stays
 * green while an x86_64 one stops compiling.
 *
 * BOTH DARWIN ARCHES ARE TARGETED, and that is load-bearing rather than
 * thorough: the descriptor's `structs[0]` carries TWO variant arms (mach
 * `boolean_t` makes `claimed_address` return `int` on arm64 and `unsigned int`
 * on x86_64), the variant selector requires exactly one match, and a typo in the
 * arm nobody builds is invisible from every direction. ✔MEASURED 2026-09-01 on
 * the operator's Mac (macOS 26.6.2, arm64) BEFORE the manifest listed the second
 * target: the arm64 artifact exits 42 natively and the x86_64 artifact exits 42
 * under Rosetta, in both the debug and the release arm. The release arm re-runs
 * every member call under the full optimizer, where a wrong member type would be
 * folded rather than loaded.
 */
#include <malloc/malloc.h>
#include <stdlib.h>

/* A mutable global: a runtime-opaque operand the exit sum cannot fold. */
int g_bump = 7;

int main(void) {
    malloc_zone_t *z;
    char          *b;
    void          *p;
    void          *q;
    void          *c;
    void          *m;
    size_t         szP;
    size_t         szQ;
    size_t         szC;
    size_t         szM;
    size_t         szFree;
    size_t         szNull;
    int            offSize;
    int            offZoneName;
    int            offIntrospect;
    int            offVersion;
    int            offMemalign;
    int            offLast;
    int            total;
    int            i;

    /* (a) THE SIZE PIN. A consumer that embeds a zone by value, or writes
    ** `sizeof(malloc_zone_t)`, gets whatever the descriptor declares — silently.
    ** Both spellings must agree, or the typedef and the tag interned apart. */
    if (sizeof(malloc_zone_t) != 200)        return 1;
    if (sizeof(struct _malloc_zone_t) != 200) return 2;

    z = (malloc_zone_t *)malloc_default_zone();
    if (z == 0)                              return 3;
    b = (char *)z;

    /* (b) THE OFFSETS, from pointer arithmetic on a zone LIBSYSTEM built. */
    offSize       = (int)((char *)&z->size       - b);
    offZoneName   = (int)((char *)&z->zone_name  - b);
    offIntrospect = (int)((char *)&z->introspect - b);
    offVersion    = (int)((char *)&z->version    - b);
    offMemalign   = (int)((char *)&z->memalign   - b);
    offLast       = (int)((char *)&z->malloc_type_malloc_with_options - b);
    if (offSize       != 16)                 return 4;
    if (offZoneName   != 72)                 return 5;
    if (offIntrospect != 96)                 return 6;
    if (offVersion    != 104)                return 7;
    if (offMemalign   != 112)                return 8;   /* the 4-byte tail pad */
    if (offLast       != 192)                return 9;   /* the far end         */

    /* (c) THE OFFSETS AGREE WITH APPLE, not merely with themselves. Each of these
    ** reads live memory the allocator wrote, so a slid offset gives a wrong
    ** value rather than a consistent one. */
    if (z->version < 8)                      return 10;  /* v8 introduced later
                                                         ** members this file
                                                         ** relies on the shape of */
    if (z->version > 1000)                   return 11;  /* not a slid pointer  */
    if (z->zone_name == 0)                   return 12;
    if (z->introspect == 0)                  return 13;

    /* (d) THE CALLABLE MEMBERS, INVOKED. `size` is the one sqlite reaches
    ** (sqlite3.c mem1.c); the rest were unreachable until the tail was typed. */
    if (z->size == 0)                        return 14;
    if (z->malloc == 0)                      return 15;
    if (z->calloc == 0)                      return 16;
    if (z->realloc == 0)                     return 17;
    if (z->free == 0)                        return 18;

    p = z->malloc(z, 100);
    if (p == 0)                              return 19;
    szP    = z->size(z, p);
    szFree = malloc_size(p);                 /* the shipped free function */
    if (szP < 100)                           return 20;  /* usable >= requested */
    if (szP != szFree)                       return 21;  /* two sources agree   */

    /* Seeded from a mutable global, so no pass can precompute the pattern. The
    ** check after the realloc recomputes the SAME formula: it asserts SURVIVAL,
    ** never a particular byte value. */
    for (i = 0; i < 100; i = i + 1) {
        ((unsigned char *)p)[i] = (unsigned char)(i * 3 + g_bump);
    }

    q = z->realloc(z, p, 4096);
    if (q == 0)                              return 22;
    szQ = z->size(z, q);
    if (szQ < 4096)                          return 23;
    for (i = 0; i < 100; i = i + 1) {
        if (((unsigned char *)q)[i] != (unsigned char)(i * 3 + g_bump)) return 24;
    }
    z->free(z, q);                           /* void: witnessed by what follows */

    c = z->calloc(z, 4, 32);
    if (c == 0)                              return 25;  /* zone alive after free */
    szC = z->size(z, c);
    if (szC < 128)                           return 26;
    for (i = 0; i < 128; i = i + 1) {
        if (((unsigned char *)c)[i] != 0)    return 27;  /* calloc really zeroes */
    }
    z->free(z, c);

    /* memalign is documented "present in version >= 5"; gated rather than
    ** assumed, so an older allocator skips instead of failing. */
    if (z->version >= 5) {
        if (z->memalign == 0)                return 28;
        m = z->memalign(z, 64, 128);
        if (m == 0)                          return 29;
        if ((((unsigned long)m) & 63) != 0)  return 30;  /* really 64-aligned */
        szM = z->size(z, m);
        if (szM < 128)                       return 31;
        z->free(z, m);
    }

    /* The zone's own size() of a NON-allocation is 0 — it INSPECTS rather than
    ** echoing its argument. Deliberately unguarded: the exit sum is its only
    ** assertion, so no pass can fold it away. */
    szNull = z->size(z, 0);

    total = (int)szNull                             /*  0 */
          + (int)(sizeof(malloc_zone_t) - 200)      /*  0 */
          + (offMemalign - offVersion - 8)          /*  0 = the 4-byte tail pad */
          + (offLast / 8)                           /* 24 */
          + (offIntrospect / 8)                     /* 12 */
          + (offSize / 8)                           /*  2 */
          + g_bump - 3;                             /*  4 */
    return total;                                   /* 24 + 12 + 2 + 4 = 42 */
}
