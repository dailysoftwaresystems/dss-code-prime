/* SOURCE of the committed fixtures `tests/link/data/owner_static_hidden_<toolchain>.o` (ELF), kept beside them as
 * `tentative_common.source.c` is kept beside its own. The Mach-O fixtures of the same shape are written in assembly
 * (Apple clang refuses `alias`): `owner_static_hidden.arm64.source.s` and `owner_static_hidden.x86_64.source.s`.
 *
 * WHAT IT IS. OWNERSHIP SHAPE A (`link/test_atom_ownership_shapes`): a MODULE-PRIVATE name and an external HIDDEN
 * name of ONE datum — gcc writes both records at one address (`dss_owned_local` LOCAL, `dss_owned_hidden` GLOBAL
 * HIDDEN), and its code reads the datum through the section. The reader through a volatile access keeps gcc from
 * folding the load.
 *
 * REBUILD, one object per reference toolchain, each in a directory holding a copy of this file (the source is named
 * RELATIVELY, so the object records no directory):
 *
 *   gcc -O2 -fno-pie -c owner_static_hidden.source.c -o owner_static_hidden_x86_64_elf.o
 *   aarch64-linux-gnu-gcc -O2 -fno-pie -c owner_static_hidden.source.c -o owner_static_hidden_aarch64_elf.o
 *
 * PROVENANCE, MEASURED at the builds (2026-10-15): gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1) and
 * aarch64-linux-gnu-gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), GNU ld 2.42. What the reference linkers make of
 * the shape is in the test's header. Do not read an md5 there as a pin a guard enforces: it records WHICH BYTES
 * were measured.
 */
static int dss_owned_local = 7;
extern int dss_owned_hidden __attribute__((alias("dss_owned_local"), visibility("hidden")));
int dss_read_owned_local(void) { return *(volatile int *)&dss_owned_local; }
