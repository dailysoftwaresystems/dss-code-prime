/* THE PROGRAM DEFINES WHAT ITS ARCHIVE READS
 * (D-LK-ELF-AARCH64-EXEC-REFUSES-GOT-RELOCATIONS, P68 round 11).
 *
 * `dsslib.c` is built into a DSS static library by the manifest's `dependsOn`;
 * its member reads `dss_app_value`, takes the address of an element of
 * `dss_app_table` and of the function `dss_app_twice` — all defined HERE. Each
 * check returns its own code when the member's view disagrees with the
 * program's: 1 the value, 2 the element's address, 3 the function's address.
 * 17 + 25 = 42 only if every reference reached its definition. */
int dss_app_value = 17;
int dss_app_table[4] = {1, 2, 25, 4};
int dss_app_twice(int x) { return 2 * x; }

int  dss_lib_reads_app(void);
int *dss_lib_row(int i);
int (*dss_lib_callback(void))(int);

int main(void) {
    if (dss_lib_reads_app() != 17) return 1;
    if (dss_lib_row(2) != &dss_app_table[2]) return 2;
    if (dss_lib_callback() != dss_app_twice) return 3;
    return dss_app_value + *dss_lib_row(2) + dss_lib_callback()(0);
}
