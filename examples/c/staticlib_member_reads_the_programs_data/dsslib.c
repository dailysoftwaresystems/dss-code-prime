/* THE REVERSE DIRECTION: an archive member's own CODE reads data, and takes
 * the address of a function, that the PROGRAM defines
 * (D-LK-ELF-AARCH64-EXEC-REFUSES-GOT-RELOCATIONS, P68 round 11).
 *
 * A relocatable object reaches an extern it does not define in whatever way
 * its format declares: directly (ELF x86_64, Mach-O), through a slot of its
 * own (PE's `.refptr`), or through the GOT (ELF aarch64 — DSS's staticlib and
 * every default-built gcc object alike: `adrp :got:` + `ldr :got_lo12:`).
 * The link that pulls this member must honour that shape — before this change
 * the aarch64 exec link refused the GOT pair outright. */
extern int  dss_app_value;
extern int  dss_app_table[4];
extern int  dss_app_twice(int);

int dss_lib_reads_app(void) { return dss_app_value; }

int *dss_lib_row(int i) { return &dss_app_table[i]; }

int (*dss_lib_callback(void))(int) { return dss_app_twice; }
