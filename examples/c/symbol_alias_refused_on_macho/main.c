/* A SECOND NAME OF A DEFINITION IS REFUSED BY NAME ON MACH-O.
 *
 * Every Apple clang refuses `alias("target")` ("aliases are not supported on darwin"), so the
 * Mach-O documents say why no definition gets a second name there, and the attribute is an
 * error naming it -- never dropped, which would leave `dss_second` a name nobody defines.
 * The same unit builds and runs on ELF and PE: examples/c/symbol_alias_second_name.
 */
int dss_target(void) { return 42; }
__attribute__((alias("dss_target"))) int dss_second(void);

int main(void) { return dss_second(); }
