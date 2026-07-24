/* TF-C50 (D-LK-ARM64-ELF-RELOC-EXTERN-DISPATCH + D-ML7-2.9) end-to-end witness.
 *
 * A DIRECT call to an undefined extern function. In an arm64 RELOCATABLE object
 * this lowers to a plain `BL` + R_AARCH64_CALL26 (the foreign linker inserts the
 * veneer) with the dead callee-address `adrp`+`add` lea SUPPRESSED -- witnessed
 * STRUCTURALLY by test_elf_writer's ObjectExternCallEmitsUndefImportNameAndCall26
 * RelocOnAarch64 pin + the foreign `aarch64-linux-gnu-gcc` default-PIE + qemu
 * probe. THIS runnable example proves the direct extern call RUNS end-to-end on
 * each shipped target: abs(argc-43) with argc==1 -> abs(-42) == 42.
 *
 * NON-FOLDING: `argc` (a runtime value) feeds abs so ConstFold cannot precompute
 * the result, and the direct extern call is a genuine BL/CALL at runtime -- the
 * `release` arm exercises the real call, not a folded literal. RED-ON-DISABLE: a
 * mis-materialized callee makes the call land elsewhere -> the result is not 42.
 * gcc/clang -std=c17 agree (exit 42). */
extern int abs(int);

int main(int argc, char **argv) {
    (void)argv;
    return abs(argc - 43) == 42 ? 42 : 1;   /* argc==1 -> abs(-42) == 42 */
}
