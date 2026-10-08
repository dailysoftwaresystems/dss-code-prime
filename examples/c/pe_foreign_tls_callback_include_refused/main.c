/* cl's TLS-CALLBACK idiom in a linked object is refused by name (see expected.json): the
   REFERENCE-built member keeps a callback pointer in `.CRT$XLB` with `/INCLUDE:_tls_used`, and the
   callback sets what this program returns. */
extern int dss_tls_member_seen(void);

int main(void) { return dss_tls_member_seen(); }
