// D-CSUBSET-STATIC-INT-TO-PTR-ABSOLUTE witness (TF-C38, SQLite testfixture-link
// sub-arc): an explicit integer-constant -> pointer cast in a STATIC/global
// initializer — `(void*)0x5`. C permits an integer-constant address in a
// static-storage pointer initializer (6.6 / 6.3.2.3); its value IS the integer,
// an ABSOLUTE address with NO symbol and NO relocation (gcc/clang emit the same
// bytes). The real trigger is tcl.h's
//   #define TCL_CHANNEL_VERSION_5 ((Tcl_ChannelTypeVersion)0x5)
// used inside a `static Tcl_ChannelType {…}` initializer — the LAST error
// blocking the SQLite testfixture MAIN TU (tclsqlite-ex.c).
//
// Before this cycle const-eval refused the cast-to-pointer (invariant: "pointer
// targets remain non-foldable"), so the member fell to `runtimeInit` and tripped
// the aggregate bitfield-rvalue fail-loud guard (H_UnsupportedLoweringForKind).
// `tryClassifyIntToPtrConst` now folds the pointer Cast's integer operand into a
// plain uint64 pointer leaf (the encoder writes 8 raw LE bytes, no reloc) — the
// sibling of the c67/c80 null-pointer and c68/c80 SQLITE_INT_TO_PTR classifiers.
//
// This one exit code witnesses every reached shape:
//   * p        — a TOP-LEVEL scalar int->ptr           (the classifyGlobals cascade)
//   * X.b      — an AGGREGATE MEMBER int->ptr           (the member loop)
//   * X.a      — a SYMBOL-ADDRESS (string) member BESIDE the int->ptr leaf: the
//                mix proves the int arm does NOT cannibalize the reloc arm — the
//                string is a real link-time reloc, dereferenced at runtime below.
//   * arr[0/1] — an ARRAY of int->ptr                   (the array arm)
//   * q        — a BLOCK-SCOPE static int->ptr: a static-duration local lowers to
//                a module global, so it reaches the SAME classifyGlobals path.
//
// RED-ON-DISABLE: remove either wiring of tryClassifyIntToPtrConst and each
// int->ptr initializer above falls to runtimeInit -> the aggregate fail-loud ->
// this TU no longer COMPILES (never reaches exit 18).

struct Two { char* a; void* b; };

static void*      p   = (void*)0x5;                 /* scalar int->ptr            */
static struct Two X   = { "hi", (void*)0x7 };       /* string reloc + int->ptr mix */
static void*      arr[] = { (void*)0x1, (void*)0x2 }; /* array of int->ptr         */

int main(void) {
    static void* q = (void*)0x3;                    /* block-scope static int->ptr */
    if (X.a[0] != 'h') return 99;                   /* the string reloc leaf is LIVE */
    return (int)((long)p + (long)X.b + (long)arr[0] + (long)arr[1] + (long)q);
    /* 5 + 7 + 1 + 2 + 3 = 18 */
}
