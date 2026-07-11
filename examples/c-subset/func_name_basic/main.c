// FC17.5 (D-CSUBSET-FUNC-PREDEFINED-IDENTIFIER, C99 6.4.2.2): the predefined
// identifier `__func__` (+ the GNU `__FUNCTION__` alias), END TO END:
//
//   * `fn = __func__`         — the read FOLDS to a string-literal constant
//                               and array->pointer DECAYS (fn[0] == 'm');
//   * `len_of(__func__)`      — the CALL-ARG decay route + a full content walk
//                               (strlen-style loop == 4 proves every byte AND
//                               the implicit NUL terminator landed in rodata);
//   * `__func__ == __func__`  — the F2 IDENTITY requirement (C99 declares ONE
//                               static array per function): both folded reads
//                               materialize the SAME memoized rodata global,
//                               so the pointers compare EQUAL. RED-on-disable
//                               for the byte-content memo — without it two
//                               distinct globals compare unequal;
//   * `__FUNCTION__[0]`       — the alias spelling binds identically (config
//                               vocabulary, not a hardcoded name);
//   * `helper()`              — PER-FUNCTION content: inside helper the same
//                               spelling folds to "helper", not "main";
//   * `sizeof __func__`       — the semantic stamp is Array<char, 5> ("main"
//                               + NUL), so sizeof == 5 (an array, NOT a
//                               decayed pointer — 8 would break the exit).
//
// exit = first(1)*10 + len(4) + ident(1) + alias(1) + helper(1) + sizeof(5)
//        + 20 = 42, data-model independent.
// RED-on-revert: without the feature `__func__` is an undeclared identifier.

int len_of(const char *s) {
    int n = 0;
    while (s[n] != 0) { n = n + 1; }
    return n;
}

int helper(void) {
    return __func__[0] == 'h' ? 1 : 0;   // "helper" — per-function content
}

int main(void) {
    const char *fn = __func__;                    // decay at init
    int first = fn[0] == 'm' ? 1 : 0;             // 1 — "main"
    int len   = len_of(__func__);                 // 4 — call-arg decay + NUL
    int ident = (__func__ == __func__) ? 1 : 0;   // 1 — the F2 identity pin
    int alias = __FUNCTION__[0] == 'm' ? 1 : 0;   // 1 — the GNU alias
    int help  = helper();                         // 1 — per-function content
    int size  = (int)sizeof __func__;             // 5 — Array<char,5>, no decay
    return first * 10 + len + ident + alias + help + size + 20;   // 42
}
