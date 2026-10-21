/* SOURCE of the committed fixtures `tests/link/data/owner_weak_strong_<toolchain>.o` (ELF), kept beside them as
 * `tentative_common.source.c` is kept beside its own. The Mach-O fixtures of the same shape are written in assembly
 * (Apple clang refuses `alias`): `owner_weak_strong.arm64.source.s` and `owner_weak_strong.x86_64.source.s`.
 *
 * WHAT IT IS. OWNERSHIP SHAPE B (`link/test_atom_ownership_shapes`): a VISIBLE WEAK name and a HIDDEN STRONG name of
 * ONE datum — gcc writes both records at one address (`dss_owned_strong` GLOBAL HIDDEN, `dss_owned_weak` WEAK
 * DEFAULT) and reads the datum through each name. A unit that defines `dss_owned_weak` strongly wins the weak NAME
 * and leaves the strong name its body. The readers through a volatile access keep gcc from folding the loads.
 *
 * REBUILD, one object per reference toolchain, each in a directory holding a copy of this file (the source is named
 * RELATIVELY, so the object records no directory):
 *
 *   gcc -O2 -fno-pie -c owner_weak_strong.source.c -o owner_weak_strong_x86_64_elf.o
 *   aarch64-linux-gnu-gcc -O2 -fno-pie -c owner_weak_strong.source.c -o owner_weak_strong_aarch64_elf.o
 *
 * PROVENANCE, MEASURED at the builds (2026-10-15): gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1) and
 * aarch64-linux-gnu-gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), GNU ld 2.42. What the reference linkers make of
 * the shape is in the test's header. Do not read an md5 there as a pin a guard enforces: it records WHICH BYTES
 * were measured.
 */
__attribute__((visibility("hidden"))) int dss_owned_strong = 7;
extern int dss_owned_weak __attribute__((weak, alias("dss_owned_strong")));
int dss_read_owned_strong(void) { return *(volatile int *)&dss_owned_strong; }
int dss_read_owned_weak(void) { return *(volatile int *)&dss_owned_weak; }
