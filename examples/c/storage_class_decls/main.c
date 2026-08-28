// FC4 c1 stage 2b — storage-class specifiers + qualifier placement:
//
//   * `register int r = 20;`  — C storage-class HINT: parses (the
//     localDeclSpecifiers prefix) and is semantically inert; r is an
//     ordinary local. (Note: ISO C forbids `&register`-object — this
//     program never takes r's address.)
//   * `auto int a2 = 20;`     — C89/C99 `auto` WITH a type specifier
//     (the default storage class, also inert). C23 `auto x = 1;` type
//     INFERENCE is deliberately a parse error (pinned residue:
//     D-CSUBSET-C23-AUTO-INFERENCE).
//   * `const int * restrict rp = &v;` — `restrict` on a POINTER layer
//     (the only grammar-legal position, C 6.7.3: it qualifies the
//     pointer, not the pointee); semantically inert today. The const
//     qualifies the pointee — *rp is a clean LOAD.
//
// Exit arithmetic: r + a2 + *rp = 20 + 20 + 2 = 42. A dropped
// initializer on either specifier-led decl shifts the exit by 20
// (!= 0 mod 256); a broken *rp load shifts by 2.
int main() {
    register int r = 20;
    auto int a2 = 20;
    int v = 2;
    const int * restrict rp = &v;
    return r + a2 + *rp;
}
