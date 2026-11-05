/* D-LK-ARCHIVE-SEARCH-FETCHES-A-MEMBER-FOR-A-WEAK-REFERENCE (P69 round 5): the
 * IMAGE half. It links one object input, `f.o` — the RELOCATABLE link of f.c
 * beside a static library whose member defines `hook`, which f.c holds only as a
 * WEAK reference (the target's `dependsOn`, and that entry's own `dependsOn`).
 * The library is not on this link.
 *
 * What the relocatable link did with the weak reference is its members' format's
 * `archiveWeakReferenceSearch`, the same answer an image link reads, and every
 * reference linker's `-r` agrees with its own image link (✔MEASURED 2026-10-08):
 *   * ELF — GNU ld 2.42 and ld.lld 18 on x86_64, GNU ld on aarch64: nothing is
 *     fetched and `hook` stays a weak undefined name of `f.o`; this link has
 *     nothing that defines it, so it resolves to nothing and f() is 0: 19.
 *   * PE — GNU ld's PE linker: the same, 19 (link.exe and lld-link have no
 *     relocatable-output mode).
 *   * Mach-O — ld64, arm64 and x86_64: `-r` fetches the member, `f.o` defines
 *     `hook`, and f() is 23: 42.
 * Round 4 answered a relocatable link from its own document, which states
 * nothing, so the link that makes `f.o` was refused on every format. */
extern int f(void);

int main(void) {
    return f() + 19;
}
