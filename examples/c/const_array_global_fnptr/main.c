/* D-MIR-ELEMENT-CONST-ARRAY-GLOBAL-CLASSIFICATION runtime witness.
 *
 * A FILE-SCOPE array whose ELEMENTS are const pointers, written in the
 * DIRECT-DECLARATOR spelling `int (*const ops[N])(int)` (NOT the typedef'd
 * `const op tab[N]` that `relro_const_fnptr_table` already covers). C 6.7.3p9
 * so-qualifies the element, so the array is a const object and its load-time-
 * relocated slots belong in relocated-read-only memory (gcc/clang `.data.rel.ro`
 * / PE `.rdata` base-reloc / Mach-O `__DATA` dyld-rebase), NOT writable `.data`.
 *
 * The bug: declaratorObjectIsConst saw the const pointer layer only when it was a
 * DIRECT child of the declarator; here `* const` hides inside the parenthesized
 * group `(*const ops[2])`, so the array mis-classified MUTABLE and the global
 * landed in `.data`. Fixed by descending the group to the true outermost object-
 * forming pointer layer (same recursion the type walk uses). This is the shape of
 * sqlite3.c's `static int (*const sqlite3BuiltinExtensions[])(sqlite3*) = {…};`.
 *
 * Runtime-NEUTRAL (a writable const table still links + runs → the section
 * mis-routing was witnessed TWICE at exit-correct: c145 ELF relro + c148 PE
 * link.exe), so the CLASSIFICATION is red-on-disable at the semantic tier
 * (`ElementConstFnPtrArrayGlobalIsConst`, tests/analysis/semantic) + the asm
 * relocBearingGlobalSection chokepoint; THIS example is the RUNTIME witness that
 * the const table relocates + calls through correctly on every format.
 *
 * Exit arithmetic: ops[0](5)=add10(5)=15 ; ops[i](9)=ops[1](9)=mul3(9)=27
 *                  -> 15 + 27 = 42. A mis-relocated table slot SIGSEGVs on the
 *                  indirect call; a wrong slot shifts the exit off 42. `i` stays a
 *                  RUNTIME index across the un-inlined pick() so the release arm
 *                  keeps a real indexed load off the relro table (no whole-expr
 *                  const-fold), witnessing the optimizer over the const-array
 *                  data access + indirect call. The pe arm RUNS on the windows
 *                  leg; elf x86_64/arm64 + macho arm64 run on linux/qemu/darwin. */
static int add10(int v) { return v + 10; }
static int mul3(int v)  { return v * 3;  }

int pick(int x) { return x; }   /* opaque across the un-inlined call → i runtime */

int (*const ops[2])(int) = { add10, mul3 };   /* array of CONST fn-ptrs -> relro */

int main(void) {
    int i = pick(1);
    return ops[0](5) + ops[i](9);   /* add10(5) + mul3(9) == 15 + 27 == 42 */
}
