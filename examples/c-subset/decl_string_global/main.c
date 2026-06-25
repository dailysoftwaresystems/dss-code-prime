/* Cluster-F F5 (decl_string_global): symbol-address GLOBALS — a global initialized
 * to another symbol's LINK-TIME-CONSTANT address (the SQLite string-table blocker):
 *   static int *p = &target;            (&global)
 *   static const char *msg = "hello";   (string literal -> rodata)
 * Each emits a pointer-width slot + an ABSOLUTE-64 relocation that the linker
 * resolves to the target's VA — the new cross-format mechanism (PE .data gate +
 * ELF + Mach-O data-item reloc appliers, abs64 found by formula).
 *
 * exit 143 == *p (42) + msg[1] ('e' = 101). A wrong/unpatched reloc yields a NULL
 * pointer -> SIGSEGV, never a silent wrong value. Red-on-disable: remove the
 * MirSymbolAddrValue assembler arm -> K_NoMatchingObjectFormat (compile fails).
 */
static int target = 42;
static int *p = &target;            /* mutable pointer -> .data, abs64 reloc -> target */
static const char *msg = "hello";   /* mutable pointer -> .data, abs64 reloc -> rodata "hello" */

int main(void) {
    int a = *p;       /* 42 */
    int b = msg[1];   /* 'e' = 101 */
    return a + b;     /* 143 */
}
