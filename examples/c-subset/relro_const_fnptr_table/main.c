/* c145 D-LK-RELRO-CONST-DATA-RELOCATABLE runtime witness.
 *
 * A `const` global whose STATIC INITIALIZER is a link-time symbol address — a
 * const function-pointer TABLE (`const op ctab[] = { add10, add20 };` — the
 * sqlite VFS-method-table / `aSyscall[]` shape) and a const scalar pointer
 * (`int *const gpp = &gbase;`). Each slot carries a load-time RELOCATION (the
 * loader writes the resolved target VA into it), yet the object is `const`, so
 * it routes to the new `RelRoConst` DataSectionKind: "relocated-read-only".
 *   - In a relocatable object it lands in `.data.rel.ro` with a `.rela.data.rel.ro`
 *     R_X86_64_64 / R_AARCH64_ABS64 entry the FINAL linker resolves (this is what
 *     let the real sqlite3.c amalgamation compile to a foreign-linkable sqlite3.o).
 *   - In an executable image it is merged into the writable-at-load data segment
 *     and the pointer slots are relocated in place (ELF .data / PE .rdata base-
 *     reloc / Mach-O __DATA dyld-rebase), then the CALL-THROUGH / DEREF reads the
 *     resolved addresses.
 *
 * Exit arithmetic: ctab[0](5)=add10(5)=15 ; ctab[1](5)=add20(5)=25 ; *gpp=gbase=2
 *                  -> 15 + 25 + 2 = 42. A mis-relocated table slot (a garbage fn
 *                  pointer) SIGSEGVs on the indirect call; a mis-relocated *gpp
 *                  reads garbage -> the exit shifts off 42.
 *
 * The pe arm RUNS on the windows leg (a native runtime witness); elf x86_64/arm64
 * and macho arm64 run on the linux/qemu/darwin legs. The release arm witnesses
 * the optimizer over the const-table load + indirect-call + relro-pointer-deref
 * shapes (folding the table to its known targets is a legal, semantics-preserving
 * outcome; the baseline arm exercises the un-folded relro data access). The
 * byte-level `.data.rel.ro` / `.rela.data.rel.ro` emission is pinned separately +
 * red-on-disable in tests/link/test_elf_writer.cpp
 * (RelRoConstDataItemEmitsDataRelRoSectionAndRelaDataRelRo). */
static int add10(int v) { return v + 10; }
static int add20(int v) { return v + 20; }

typedef int (*op)(int);

const op ctab[2] = { add10, add20 };   /* const fn-ptr table -> RelRoConst  */
static int gbase = 2;
int *const gpp = &gbase;               /* const scalar pointer -> RelRoConst */

int main(void) {
    return ctab[0](5) + ctab[1](5) + *gpp;   /* 15 + 25 + 2 = 42 */
}
