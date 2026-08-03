// TF-C79 (D-CSUBSET-INLINE-FUNCTION-SPECIFIER): an UNCALLED C99 6.7.4p7 inline
// definition must not be emitted — the shape Apple's SDK headers put in front of
// every macOS translation unit.
//
// `<sys/cdefs.h>` defines `__header_inline` as plain `inline` under the language
// mode DSS compiles in (MEASURED: DSS predefines `__STDC_VERSION__` >= 199901L
// and does NOT define `__GNUC__`, which selects the first arm; the
// `__header_always_inline` alias degrades to the same thing because its
// `always_inline` decoration is `#ifdef __GNUC__`-gated). Headers then define
// whole families of `__header_inline` functions that a given TU never calls.
//
// ★ THE ASSERTION IS NOT "IT LINKED" — an emitted body would ALSO link if its
// contents were self-contained. `never_called` deliberately calls
// `__dss_undescribed_helper`, which is declared and defined NOWHERE. Suppress
// the body (correct) and nothing ever references that symbol, so the program
// links and exits 42. Emit the body (broken) and the reference goes live and the
// link fails loud K_SymbolUndefined naming `__dss_undescribed_helper`. That is a
// red-on-disable that a symbol-table dump could not express as an exit code.
//
// This is precisely why the no-external-definition state is NOT encoded as a
// `weak` binding: weak would EMIT these never-called bodies and drag every
// symbol they mention into the link.
//
// VERIFIED clang-clean (`-fsyntax-only -Wall -Wextra -isysroot $(xcrun
// --show-sdk-path)`) and the clang-linked binary independently exits 42 —
// clang likewise emits nothing for the uncalled inline definition.

extern int __dss_undescribed_helper(int);

inline int never_called(int x) { return __dss_undescribed_helper(x) + 1; }

int main(void) { return 42; }
