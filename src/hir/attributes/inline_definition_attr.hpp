#pragma once

// D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED (C99/C11/C23 6.7.4p7):
// the per-FUNCTION-DEFINITION mark that says "this body is an INLINE DEFINITION —
// it provides NO external definition for the function, and must never be emitted".
// Attached via `HirAttribute<InlineDefinitionAttr>` to the NATIVE FUNCTION node
// CST→HIR lowered from a file-scope definition every one of whose declarations
// spelled `inline` (or a GNU `__inline` / `__inline__` synonym) WITHOUT `extern`.
//
// ★★★ WHAT CHANGED AND WHY, BECAUSE THE PREVIOUS READING WAS HALF RIGHT.
// TF-C79 originally DISCARDED such a body outright and left only an
// `ExternFunction` behind. That is the correct EMISSION decision and it is
// conformant — ✔MEASURED 2026-08-15 on gcc 13.3.0, clang 18.1.3 and clang 19,
// over `inline` and `__inline__`, `-std` in {default, c99, gnu17}: at `-O0` all
// three FAIL TO LINK on a called inline definition with no external definition
// anywhere, and `nm` shows `U <name>` — no local body, no weak symbol, nothing.
// But 6.7.4p7 also says the inline definition "provides an ALTERNATIVE to an
// external definition, which a translator may use to implement ANY CALL to the
// function in the same translation unit", and at `-O1`/`-O2`/`-O3`/`-Os` all
// three oracles DO exactly that: they inline the body, the call disappears, and
// the program links. Discarding the body at CST→HIR made that second half
// unreachable — the optimizer's inliner is perfectly capable of the splice (its
// §2.9 legality gate admits a small, non-weak, non-recursive Global callee, and
// it already clones `InlineAsm`), it simply never had a body to see.
//
// ⇒ THE BODY IS NOW LOWERED, AND THIS MARK IS WHAT KEEPS IT FROM BEING EMITTED.
// The `ExternFunction` declaration is still emitted beside it, unchanged, so
// every downstream link decision (a sibling CU's external definition binding,
// a shipped-shim claim, or a loud `K_SymbolUndefined`) is reached by exactly the
// path it was reached by before. The body is an ADDITION visible only to the
// optimizer, and the optimizer's epilogue removes it again.
//
// ★★ HOW THE FACT SURVIVES TO THAT EPILOGUE — AND WHY THERE IS NO `MirFunc`
// FLAG. A `MirFuncAttribute` side-table would be silently LOST at the first
// rebuild (every optimizer pass rebuilds the module into a fresh arena with
// fresh ids), and a lost mark emits the body — a link failure with no
// diagnostic. The surviving carrier is a property the module already has: an
// inline definition is THE ONLY function whose SymbolId is ALSO declared by an
// `ExternImport`. HIR→MIR fails loud on that pair for every other producer
// (`hir_to_mir.cpp`, "Each SymbolId must belong to either a function OR an
// extern, never both"), so the pair is unambiguous, and SymbolIds are preserved
// verbatim by every rebuild. This attribute's job is therefore narrow and
// entirely at the HIR→MIR seam: it tells that check WHICH pair is the sanctioned
// one, so the invariant keeps catching real bugs everywhere else.
//
// ★ ONLY A `Global` BINDING IS EVER MARKED. 6.7.4p7 constrains functions with
// EXTERNAL linkage. A `static inline` has internal linkage (6.7.4p6 admits any
// internal-linkage function as inline) and MUST still be emitted, as must a
// `weak` one — MEASURED, clang emits `t _p` for `static inline`. Those never
// reach this map and are lowered as ordinary definitions.
//
// The side-table stays sparse: only inline definitions are recorded, and a node
// with no entry is an ordinary definition that is emitted normally.

namespace dss {

struct InlineDefinitionAttr {
    bool isInlineDefinition = false;
};

} // namespace dss
