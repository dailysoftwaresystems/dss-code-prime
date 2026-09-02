// D-CSUBSET-GNU-UNKNOWN-NAME-GATE-ASYMMETRY — the THIRD tier, the file-scope one.
//
// An unmodelled GNU attribute NAME is ignorable vocabulary, not a constraint
// violation. gcc 13.3.0, clang 18.1.3 and mingw-w64 gcc 13.2.0 — probed
// SEPARATELY — all compile every line below and exit 0, each emitting
// `'<name>' attribute directive ignored [-Wattributes]`. DSS used to exit 1 with
// `error[H000C] '<name>' is not a recognized linkage specifier`, so it REFUSED
// programs all three references build and run.
//
// ★★ THE VERDICT IS THE DECLARATION ROW'S OWN, AND THE ENGINE WAS DISOBEYING IT.
// `topLevelDecl` declares `unknownStrictAttributeIsError: false` — the
// C23-ignorable posture — and the HIR linkage tier hardcoded an Error without ever
// reading the key. The semantic tier has honoured it since TF-C73, which is
// exactly the asymmetry: two tiers, one config-driven, one not. They now read ONE
// key and can only move together.
//
// ⚠ WARNED, NEVER SILENTLY DROPPED. Each name below still produces
// `warning[H_UnknownLinkageSpecifier]`; the attribute simply has no effect, which
// is what every reference does. A build that wants the old refusal keeps it with
// `--warnings-as-errors`.
//
// ⚠ AND THE PROGRAM MUST STILL RUN CORRECTLY — an ignored attribute must not
// disturb the declaration it decorates. Every decorated entity below is used, and
// the exit code is the witness: an attribute that silently ate a `static`, a
// storage class or an initializer would change it.
//
// ⚠⚠ AND THE REFUSAL WAS HIDING SOMETHING, WHICH IS WHY THAT IS RECORDED HERE.
// The blanket file-scope error also refused attributes the REFERENCES IMPLEMENT,
// not merely ones nobody models: the repo's own reference-conformance corpus
// carried `a_attribute_constructor_gnu` and `a_attribute_alias_gnu` as
// `@acknowledged-gap` rows, and both now compile. `alias` fails LOUD downstream
// (`error[K_SymbolUndefined]` at link — measured), so nothing silently ships.
// `__attribute__((constructor))` does NOT: it is accepted, WARNED and ignored, so
// the function never runs — ✔MEASURED, the same source exits 0 under DSS and 42
// under mingw-w64 gcc 13.2.0. That is an ANNOUNCED divergence rather than a silent
// one, and it is the correct FRONT-END verdict (DSS says exactly which name it
// does not model); implementing the attribute is a separate, larger piece of work
// tracked by [[D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN]].
//
// RED-ON-DISABLE: restore the hardcoded `Error` severity at `linkageFrom`'s
// unknown-name arm and this example fails to compile at all.

__attribute__((frobnicate)) int decorated_global = 11;

__attribute__((wibble)) static int decorated_static(void) { return 13; }

__attribute__((frobnicate)) __attribute__((wibble)) int twice_decorated = 18;

int main(void) {
    /* 11 + 13 + 18 == 42 — only if all three declarations kept their meaning. */
    return decorated_global + decorated_static() + twice_decorated;
}
