/* TF-C52 (D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT) end-to-end witness.
 *
 * The ADDRESS of an undefined extern FUNCTION taken as a LIVE value (not a
 * direct call), then called INDIRECTLY through that value. On the arm64
 * RELOCATABLE object this address materialization now uses the GOT
 * (R_AARCH64_ADR_GOT_PAGE + R_AARCH64_LD64_GOT_LO12_NC) so a foreign
 * default-PIE link accepts it -- witnessed STRUCTURALLY by test_elf_writer's
 * GotExternAddrValueEmitsAdrGotPageAndLd64GotLo12OnAarch64 pin + the foreign
 * `aarch64-linux-gnu-gcc` default-PIE + qemu probe. THIS example proves the
 * whole source -> binary -> run chain: the taken address is the REAL `abs`,
 * so calling through it computes abs correctly across every shipped target
 * (x86_64 uses PC-relative rel32; the DSS-linked arm64 exec binds it its own
 * way -- the corpus proves the construct RUNS on each).
 *
 * NON-FOLDING (so the `release` optimizer arm genuinely exercises the address
 * materialization, not a masked no-op): the `volatile` function-pointer store
 * forbids the optimizer from devirtualizing the indirect call back to a direct
 * `abs` (which would delete the materialization) AND from DCE-ing it; `argc` (a
 * runtime value) feeds abs so ConstFold/Mem2Reg cannot precompute the result.
 * RED-ON-DISABLE: a wrong materialized address makes the indirect call land
 * elsewhere -> the result is not 42. gcc/clang -std=c17 agree (exit 42). */
extern int abs(int);

int main(int argc, char **argv) {
    (void)argv;
    int (*fp)(int) = abs;             /* materialize &abs as a live VALUE */
    int (*volatile hold)(int) = fp;   /* volatile store: no devirtualization / DCE */
    int (*g)(int) = hold;             /* reload the materialized address */
    int r = g(1 - argc - 42);         /* argc==1 -> g(-42) -> abs(-42) == 42 (runtime arg) */
    return r == 42 ? 42 : 1;
}
