/* A FOREIGN ARCHIVE READS WHAT THE PROGRAM DEFINES — THROUGH THE GOT
 * (P68 round 11: D-LK-ELF-READER-REFUSES-GOTPCREL-BLOCKS-REAL-GLIBC-MEMBERS and
 * D-LK-ELF-AARCH64-EXEC-REFUSES-GOT-RELOCATIONS).
 *
 * The prebuilt archives `tests/link/data/libforeign_got_members_{x86_64,aarch64}_elf.a`
 * hold three gcc 13.3 -O2 members each (x86_64: gcc; aarch64:
 * aarch64-linux-gnu-gcc), built from these three files:
 *
 *   lib_default.c — the DEFAULT codegen (PIE on Ubuntu):
 *       extern int  dss_app_value;
 *       extern int  dss_app_table[4];
 *       extern int  dss_app_twice(int);
 *       int  dss_lib_reads_app(void)        { return dss_app_value; }
 *       int *dss_lib_row(int i)             { return &dss_app_table[i]; }
 *       int (*dss_lib_callback(void))(int)  { return dss_app_twice; }
 *
 *   lib_calls.c — -fPIC -fno-plt:
 *       extern int dss_app_value;
 *       extern int dss_app_twice(int);
 *       int dss_libx_calls_app(void) { return dss_app_twice(dss_app_value); }
 *
 *   lib_plain.c — -fPIC, and on x86_64 -Wa,-mrelax-relocations=no: the
 *   `lib_default.c` body with the functions renamed dss_libp_*.
 *
 * x86_64: the default member takes the function's address through
 * R_X86_64_REX_GOTPCRELX, the -fno-plt member calls through R_X86_64_GOTPCRELX
 * and loads the datum through REX_GOTPCRELX, and the plain member reaches all
 * three through a PLAIN R_X86_64_GOTPCREL. aarch64: every reference is the
 * R_AARCH64_ADR_GOT_PAGE + R_AARCH64_LD64_GOT_LO12_NC pair. ✔MEASURED
 * 2026-09-24 before this change: DSS refused the x86_64 archive at READ
 * (type 42 undeclared) and the aarch64 one at the link's kind unifier; gcc
 * (-no-pie and -pie), clang 18 and aarch64 gcc under qemu link each to 42.
 *
 * The Mach-O twins, `tests/link/data/libforeign_got_members_{arm64,x86_64}_macho.a`
 * (P68 round 11), hold the same three files built by clang 18 for macOS 11:
 * arm64 reaches every extern through ARM64_RELOC_GOT_LOAD_PAGE21 +
 * GOT_LOAD_PAGEOFF12, x86_64 through X86_64_RELOC_GOT_LOAD and X86_64_RELOC_GOT
 * (the element address, and the -fno-plt call `callq *...@GOTPCREL(%rip)`),
 * each x86_64 field holding its remainder in place. DSS refused both archives
 * at READ before the Mach-O documents declared the four relocations.
 *
 * Each check returns its own code when a member's view disagrees with the
 * program's; 17 + 25 + 0 = 42 only if every reference reached its definition. */
int dss_app_value = 17;
int dss_app_table[4] = {1, 2, 25, 4};
int dss_app_twice(int x) { return 2 * x; }

int  dss_lib_reads_app(void);
int *dss_lib_row(int i);
int (*dss_lib_callback(void))(int);
int  dss_libx_calls_app(void);
int  dss_libp_reads_app(void);
int *dss_libp_row(int i);
int (*dss_libp_callback(void))(int);

int main(void) {
    if (dss_lib_reads_app() != 17) return 1;                  /* default: datum */
    if (dss_lib_row(2) != &dss_app_table[2]) return 2;         /* default: element address */
    if (dss_lib_callback() != dss_app_twice) return 3;         /* default: function address */
    if (dss_libx_calls_app() != 34) return 4;                  /* -fno-plt: call through the GOT */
    if (dss_libp_reads_app() != 17) return 5;                  /* plain GOTPCREL: datum */
    if (dss_libp_row(2) != &dss_app_table[2]) return 6;        /* plain GOTPCREL: element address */
    if (dss_libp_callback() != dss_app_twice) return 7;        /* plain GOTPCREL: function address */
    return dss_app_value + *dss_libp_row(2) + dss_lib_callback()(0);
}
