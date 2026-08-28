// C23 6.7.10: the 1-argument static_assertion (no message) AND the C23
// `static_assert` spelling. `2+2==4` folds true → the declaration produces
// nothing and the program runs to exit 42. Pins that the message-less form
// compiles (it must NOT fall through to H0009) and that the `static_assert`
// keyword is accepted identically to `_Static_assert`.
static_assert(2 + 2 == 4);

int main(void) { return 42; }
