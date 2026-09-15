// [[D-C-FILE-SCOPE-INFERRED-AUTO-MUST-LEAD-THE-DECLARATION-SPECIFIERS]] (P65)
// — the SIBLING translation unit, and the reason this example has two of them.
//
// A file-scope `auto` declaration is a DEFINITION with EXTERNAL LINKAGE, and
// writing a specifier in front of `auto` must not change that. ✔MEASURED
// 2026-09-08, each reference probed SEPARATELY: `nm` shows `D g_ext` for a plain
// `auto g_ext = 11;` on gcc 13.3.0 (`-std=c2x`) and clang 18.1.3 (`-std=c23`),
// and it shows the SAME letter when the declaration is written
// `[[maybe_unused]] auto g_ext = 11;` — a leading standard attribute, which both
// references accept here. main.c reads this object through an ordinary
// `extern int g_ext;`, so an inference that came out INTERNAL (or that dropped
// the object entirely) fails the LINK rather than returning a wrong number.
[[maybe_unused]] auto g_ext = 11;
