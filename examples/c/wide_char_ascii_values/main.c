// C11/C23 6.4.4.4 wide/UTF character constants — end-to-end value witness. Each
// prefixed char is a SCALAR whose value is the decoded code point, typed by its
// prefix: L'x'→wchar_t, u'A'→char16_t, U'A'→char32_t, u8'A'→char8_t. The narrow
// 'x' stays int. A wrong element core or a mis-decoded value flips one guard and
// returns that guard's index instead of 42. The release arm runs the same.

int main(void) {
    if (L'x'  != 120) return 1;   // wchar_t — value is the code point (format-invariant)
    if (u'A'  != 65)  return 2;   // char16_t
    if (U'A'  != 65)  return 3;   // char32_t
    if (u8'A' != 65)  return 4;   // char8_t (ASCII — one UTF-8 unit)
    if ('x'   != 120) return 5;   // narrow int char (UNCHANGED)
    if (sizeof(u'A')  != 2) return 6;   // char16_t width
    if (sizeof(U'A')  != 4) return 7;   // char32_t width
    if (sizeof(u8'A') != 1) return 8;   // char8_t width
    if (sizeof('x')   != 4) return 9;   // narrow char literal is int
    return 42;
}
