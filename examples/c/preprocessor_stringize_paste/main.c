// FC15a (`#`/`##` operators) -- the stringize/token-paste runtime witness, and
// the UNFORGEABLE A2 witness: a `#`/`##` product token's spelling must reach the
// PARSER through a real span in the single synthesized buffer (config A2). If a
// product's span pointed at `#`/`##` (the operator) instead of `add3`/`"hello"`
// (the product), this would FAIL TO COMPILE -- the paste product would not
// resolve to the function `add3`, and the stringize product would not be a valid
// string literal whose length drives the arithmetic.
//
//   * `##` (C 6.10.3.3): `PASTE(add, 3)` pastes `add` ## `3` -> the single token
//     `add3`. The call `add3(12, 12, 12)` only links if the paste product reaches
//     the parser AS THE IDENTIFIER `add3` (a function name), not as `add`/`3`/`##`.
//     add3 returns the sum of its three arguments -> 36. The operands use the RAW
//     (un-pre-expanded) argument, but here `add`/`3` are already bare tokens.
//   * `#` (C 6.10.3.2): `STR(hello)` -> the string literal "hello". sizeof("hello")
//     is 6 (char[5+1]) -- so the stringize product must reach the parser as a
//     valid string literal whose decoded length drives the arithmetic. 36 + 6 = 42.
//
// Fold-resistance: the paste result feeds a real function CALL (live runtime add
// of three args -- not const-folded to one immediate at the unoptimized exit),
// and the string length feeds an arithmetic add. The optimizedPipelines `release`
// arm runs the SHIPPED parser->semantic->codegen->native-run pipeline over the
// SAME source -- without that arm the A2 crux witness is vacuous (the baseline
// arm alone could in principle skip the real string/identifier resolution).
//
//   add3(12,12,12) = 36 ; sizeof("hello") = 6 ; 36 + 6 = 42 -> exit 42.

#define PASTE(a, b) a ## b
#define STR(x)      #x

int add3(int a, int b, int c) {
    return a + b + c;
}

int main(void) {
    int s = PASTE(add, 3)(12, 12, 12);
    return s + (int)sizeof(STR(hello));
}
