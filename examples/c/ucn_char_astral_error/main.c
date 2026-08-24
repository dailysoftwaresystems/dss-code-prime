// C11/C23 6.4.4.4: `u'\U0001F600'` names U+1F600, a supplementary-plane
// code point - but ONE char16_t holds ONE code unit (a surrogate PAIR is two). The
// char path is UNCHANGED by the string surrogate-pair work: it FAILS LOUD with
// H_WideCharValueUnrepresentable (unsuppressable), the UCN analog of the raw
// utf16_char_astral_error. Red-on-disable: drop the U16 range check in
// decodeWideCharCodepoint and this compiles emitting a wrong unit.
int main(void) { return u'\U0001F600'; }
