// FC4 c1 stage 2b — attribute syntax surfaces (the value here is that
// these forms PARSE + compile + RUN; pre-FC4 every one was a parse
// error):
//
//   * C23 [[...]] standard attributes (`[[deprecated]]`,
//     `[[nodiscard("why")]]`) — FC17 (D-CSUBSET-ATTRIBUTE-SEMANTICS)
//     made these MEAN: the `f(...)` call below now emits the
//     SUPPRESSIBLE S_DeprecatedSymbolUsed warning (the runner checks
//     exit codes, not warnings, so this example stays green — the
//     warning firing IS the feature working). `nodiscard` does NOT
//     fire here: the call sits in `return` position, which CONSUMES
//     the result (only a bare `f();` expression statement discards).
//     The linkage scan still skips both wholesale (stdAttr rides
//     linkageSpecifierIgnoredRules).
//   * `__attribute__((visibility("hidden")))` on an UNUSED helper —
//     the composite linkage key `visibility:hidden` threads
//     SymbolVisibility::Hidden into MIR, which makes the helper
//     DCE-FOOD under the release pipeline (hidden + uncalled = not
//     externally visible; the unit-tier lever is
//     HiddenVisibilityUnusedFunctionIsDceEliminated). The baseline
//     pipeline keeps it — both arms must exit 42 either way.
//   * `__attribute__((weak))` on a CALLED function — Weak binding
//     survives the sole-definition link (the weak_inline_crosscu
//     precedent).
//
// Exit arithmetic: f(weak_helper(40)) = (40) + 2 = 42.
[[deprecated]] [[nodiscard("why")]] int f(int v) { return v + 2; }

__attribute__((visibility("hidden"))) int dead_helper(int v) { return v * 3; }

__attribute__((weak)) int weak_helper(int v) { return v; }

int main() {
    return f(weak_helper(40));
}
