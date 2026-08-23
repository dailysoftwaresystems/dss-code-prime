// THE ARCHIVE MEMBER, and the whole point of it is the EXTERN CALL.
//
// D-LK-MACHO-ISDATA-NO-CALL-SIGNAL. A relocatable object's undefined symbol
// is a NAME and nothing else: Mach-O's nlist_64 carries no STT_FUNC-style
// type hint, so the only thing that can tell an undefined `_puts` from an
// undefined `_environ` is the RELOCATION that reaches it. The reader used to
// answer that from the TARGET row's arithmetic formula, which is branch
// -specific on aarch64 only by accident and carries no role at all on x86_64
// (`S + A - P` is the same arithmetic for a call and for a PC-relative data
// reference). So `macho64-x86_64` had NO working static-library path: any
// member that called any library function was unreadable, while the arm64
// sibling read the identical source cleanly.
//
// ⚠ THE `puts` CALL IS THE TEST. Drop it and this member has no undefined
// symbol at all, the classification is never asked for, and the example goes
// green against a reader that still guesses.
extern int puts(const char *s);

// An inlinable helper with a loop-carried accumulator, so the `release` arm
// has something of its own to optimize and `mustDifferFromBaseline` is not
// asserting x == x. Deliberately NOT `static`: at `--config=release` a file
// -local helper is inlined and DCE'd out of the archive entirely, and this
// example's subject must survive BOTH configurations.
int dss_lib_step(int n) {
    int total = 0;
    for (int i = 0; i < n; ++i) total += 2;
    return total;
}

int dss_lib_answer(void) {
    puts("member");
    return dss_lib_step(21);
}
