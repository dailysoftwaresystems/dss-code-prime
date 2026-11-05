/* P69 fold 3c, lane p69/xa: two relocation types the reference compilers write for ordinary C that no DSS document
 * declared until this fold (D-LK-RELOCATION-TYPES-REFERENCE-COMPILERS-EMIT-FOR-C-UNDECLARED). One source, two objects:
 *
 *   reference_relocation_types_aarch64_elf_gcc_O2.o
 *       aarch64-linux-gnu-gcc 13.3.0 -O2 -c (WSL Ubuntu 24.04): `tail` ends in `b callee`, R_AARCH64_JUMP26 -- the
 *       branch WITHOUT link of a tail call, which gcc writes at -O2 for every call in tail position.
 *   reference_relocation_types_x86_64_elf_gcc_O2_nopic.o
 *       gcc 13.3.0 -O2 -fno-pic -c (WSL Ubuntu 24.04): `pick` reads `table(,%rdi,4)`, a sign-extended disp32 against
 *       `table`, R_X86_64_32S -- what every -fno-pic image's indexed access to a global array is.
 *
 * Built with `-ffile-prefix-map` unneeded: neither object records a path (no debug information). The test links each
 * beside a DSS-compiled `main` that defines `callee` and reads `pick`; both programs exit 42.
 */
int table[8] = {11, 22, 33, 44, 55, 66, 77, 88};

int callee(int);

int tail(int x) { return callee(x + 1); }

int pick(int i) { return table[i]; }
