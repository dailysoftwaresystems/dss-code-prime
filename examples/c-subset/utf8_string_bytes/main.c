// C11/C23 6.4.5: a `u8"…"` (char8_t) string literal → UTF-8 bytes. For ASCII each
// unit is ONE byte, so read as `unsigned char*`, p[0]+p[1] = 'A'+'B' = 131. Unlike
// u"…"/U"…", u8 keeps the 1-byte element width (Array<U8,3>), but still routes the
// UTF-8 validator (an ill-formed u8"…" fails loud, never emits garbage). The
// release arm runs the same through the shipped optimizer pipeline.

int main(void) {
    unsigned char *p = (unsigned char *)u8"AB";
    return p[0] + p[1];
}
