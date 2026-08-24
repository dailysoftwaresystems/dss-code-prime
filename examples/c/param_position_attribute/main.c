// D-CSUBSET-ATTRIBUTE-PARAM-POSITION (P32 lane A) witness: an attribute on a
// FUNCTION PARAMETER, in BOTH positions and BOTH spellings.
//
// Before this the whole surface was a parse error — `declHeadForParam` had no
// attribute slot at all, so `void f([[maybe_unused]] int p)` died at `P0009` and
// `int f(int p __attribute__((__unused__)))` at `P0001 expected 'ParenClose'`.
// The second of those is not an exotic form: it is the idiom C code uses to
// silence an unused parameter in a DEFINITION, which is why the leading position
// alone would have been half a fix.
//
// ★ VERIFIED against BOTH references, `-std=c2x -Wall -Wextra`: gcc 13.3.0 and
// clang 19.1.7 compile this file with ZERO errors and ZERO warnings, and both
// binaries exit 42. The `-Wextra` part is the interesting half — it is what makes
// `-Wunused-parameter` live, so an attribute that did NOT suppress the warning
// would show up here as reference noise rather than as silence.
//
// ★★ WHAT THIS FILE CANNOT WITNESS, and where that half lives. A corpus example
// must build, so it can only show the ACCEPTING half — and a GRAMMAR-ONLY fix
// passes every line here while the attributes reach no reader and are dropped in
// silence. The pin that separates the two is in
// `tests/analysis/semantic/test_attribute_clause_name_token_class.cpp`: a
// `deprecated` attribute written on a PARAMETER must produce a real
// warn-on-use diagnostic at the parameter's USE site (both references warn there
// too), which is only possible if the run reached `scanAttributeSemantics`.
//
// RED-ON-DISABLE for THIS file: drop `{"optional": "paramDeclSpecifiers"}` from
// the `param` shape in `src/dss-config/sources/c.lang.json` → the first function
// fails P0009 and the program no longer compiles (the runner reports a compile
// failure, not exit 42). Dropping the trailing `paramTrailingAttrRun` instead
// reddens the third one with P0001.
//
// Front-end feature (grammar + declaration lowering), target/format-agnostic:
// x86_64 (PE + ELF) and arm64 (ELF under qemu, Mach-O macos leg).

// (1) The C23 spelling, leading position.
static int only_second(int a, [[maybe_unused]] int b) { return a; }

// (2) The GNU spelling, leading position.
static int gnu_unused(int a, __attribute__((__unused__)) int b) { return a; }

// (3) The GNU spelling AFTER the parameter's declarator — the C idiom. One
//     attribute must not mean two things depending on which side of the
//     declarator it sits, which is the rule the struct-member and
//     init-declarator runs already follow.
static int after_decl(int a, int b __attribute__((__unused__))) { return a; }

// (4) A PROTOTYPE, where the parameter has no name for the attribute to be about
//     — the position must still parse, because a header declares before it
//     defines and the two must agree.
int decl_only([[maybe_unused]] int p);
int decl_only(int p) { return p; }

int main(void) {
    return only_second(20, 1)   // 20
         + gnu_unused(12, 1)    // + 12 = 32
         + after_decl(6, 1)     // +  6 = 38
         + decl_only(4);        // +  4 = 42
}
