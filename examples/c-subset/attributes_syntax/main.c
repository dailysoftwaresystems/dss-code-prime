// FC4 c1 stage 2b — attribute syntax surfaces (the value here is that
// these forms PARSE + compile + RUN; pre-FC4 every one was a parse
// error):
//
//   * C23 [[...]] standard attributes (`[[deprecated]]`,
//     `[[nodiscard("why")]]`) — parsed and semantically IGNORED
//     (stdAttr rides linkageSpecifierIgnoredRules; D14).
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
