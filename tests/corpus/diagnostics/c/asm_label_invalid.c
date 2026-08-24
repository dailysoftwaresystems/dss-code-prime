// TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME): the asm-label forms that must
// FAIL LOUD.
//
// SUPERSEDES `asm_label_on_declaration.c`, which pinned the whole construct as a
// parse error and said so explicitly: "Admitting asm labels on declarations is a
// SEPARATE feature with its own anchor — if that lands, this golden changes
// deliberately." That feature landed in TF-C88, so the VALID shape moved to the
// runnable example `asm_label` and this file keeps corpus coverage on the three
// shapes that stay rejected. What must STILL never happen is the TF-C80 abort
// (uncaught std::out_of_range from the diagnostic renderer) on any of them.
//
//   1. an EMPTY label — decodes to zero bytes. The most dangerous of the three:
//      an empty name reaches `nameOf` as the "module-private" signal, which drops
//      the symbol-table row and lets the writer substitute a synthetic `sym_<id>`.
//   2. TWO labels on one declarator — which assembler name was meant is unknowable.
//   3. a label on a TYPEDEF declarator — a type alias has no symbol to rename;
//      `typedefDeclaratorList` takes BARE declarators, so this is a parse error.
int empty_label(void) __asm("");
int two_labels(void) __asm("a") __asm("b");
typedef int aliased_t __asm("nope");
int main(void) { return 42; }
