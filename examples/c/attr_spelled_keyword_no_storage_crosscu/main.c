// Witness for the SEMANTIC-tier twin of the HIR-tier row
// D-C-LINKAGE-SPECIFIER-LOOKUP-IS-POSITION-BLIND-AND-NOT-DUNDER-NORMALIZED.
//
// A declaration-specifier KEYWORD worn as an attribute CLAUSE NAME confers
// NOTHING. `__attribute__((static))` is not `static`; `__attribute__((
// thread_local))` is not `thread_local`. Both references agree unanimously and
// merely warn: gcc 13.3.0 `'static' attribute directive ignored`, clang 18.1.3
// `unknown attribute 'static' ignored` — and both BUILD AND RUN every shape
// below (✔MEASURED 2026-09-01, probed separately).
//
// THE EXAMPLE IS TWO TUs BECAUSE THE WORST CONSEQUENCE IS A LINK-TIME ONE.
// The semantic tier resolved the attribute-position `static` against the
// keyword's own linkage entry and marked `sharedCounter` INTERNAL; once the HIR
// emission tier began reading that mark
// (D-CSUBSET-LINKAGE-INHERITED-INTERNAL-EMITS-GLOBAL) the symbol LEFT the
// object, and this TU's `extern int sharedCounter;` no longer had anything to
// bind to. A single-TU program cannot show that at all — it never asks another
// unit for the symbol. gcc and clang both keep it exported (`nm` reports
// `D sharedCounter`), so a cross-TU read is the reference behaviour.
//
// VALUE-DIVERGENT: provider.c initialises it to 21 and `bumpTwice` adds 2 per
// call through the SHARED object, so 21 + 2 + 2 = 25. If the attribute had
// conferred static storage on `bumpTwice`'s own local the arithmetic would
// differ, and if it had conferred internal linkage the program would not link
// at all.
//
// RED-ON-DISABLE: in `scanSpecifierPrefixStorage`
// (src/analysis/semantic/semantic_analyzer.cpp) drop the attribute-shape skip so
// the walk descends into `attrSpec` again -> `sharedCounter` is minted internal,
// leaves provider.c's object, and this program fails to compile with
// K_SymbolUndefined.

extern int sharedCounter;   // defined in provider.c — must still be EXPORTED
void bumpTwice(void);

int main(void) {
    bumpTwice();            // 21 -> 23
    bumpTwice();            // 23 -> 25
    return sharedCounter;   // 25
}
