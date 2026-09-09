// [[D-CSUBSET-AUTO-FILE-SCOPE]] (P65, C23 6.7.9) — the SIBLING translation unit,
// and the reason this example has two of them.
//
// A file-scope `auto` declaration is a DEFINITION with EXTERNAL LINKAGE, not a
// file-local convenience. ✔MEASURED 2026-09-08: `nm` shows `D g_ext` on gcc
// 13.3.0 (-std=c2x) and on clang 18.1.3 (-std=c23), and a sibling TU declaring
// `extern int g_ext;` links against it and reads the value back — which is
// exactly what main.c does here. If DSS inferred the type but gave the object
// INTERNAL linkage (the block-scope row's `staticStorage` reading, which routes
// a declaration to a hidden module-global), this example would fail to LINK
// rather than return a wrong number.
auto g_ext = 11;
