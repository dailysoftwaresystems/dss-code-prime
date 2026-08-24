// D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED, the BOUNDARY of the
// C99 6.7.4p7 capability: the inliner may satisfy a CALL to an inline definition,
// and must never satisfy its ADDRESS.
//
// 6.7.4p7 makes the inline definition "an alternative to an external definition,
// which a translator may use to implement any CALL to the function in the same
// translation unit". A call is the whole licence. Taking `&pick` asks for the
// address of THE FUNCTION — which is the address of its external definition, and
// this translation unit provides none — so the reference stands and the link must
// fail loud, at every optimization level.
//
// ★ THIS IS THE ARM THAT WOULD GO SILENTLY WRONG IF THE FIX HAD BEEN BUILT THE
// OTHER WAY. The body IS now lowered into MIR so the optimizer can inline from
// it; had it been given a symbol of its own and emitted, `g` would point at a
// local copy, this program would LINK, and it would link at `-O0` too — a program
// that no reference compiler accepts, quietly accepted. It does not, because the
// body is never emitted: the optimizer's strip epilogue removes it unconditionally
// and only the `ExternFunction` declaration survives to the link tier.
//
// ✔MEASURED on gcc 13.3.0, clang 18.1.3 and clang 19, this exact file:
// `undefined reference to 'pick'` at BOTH `-O0` and `-O2` on all three. DSS
// reports `K_SymbolUndefined` at both `--config=debug` and `--config=release`.
//
// ⚠ THE RELEASE HALF IS MEASURED BUT NOT PINNED HERE, and that is a property of
// the harness rather than a choice: an `expectDiagnostics` example runs only its
// BASELINE arm (`runErrorTarget` declares no optimized arm at all), so this file
// asserts the debug half and the release half rests on the CLI measurement above.
// The two arms of this shape fail the same way for the same reason, which is
// exactly why it is worth saying out loud that only one of them is a test.

inline int pick(void) { return 7; }

int (*g)(void) = pick;

int main(void) { return g(); }
