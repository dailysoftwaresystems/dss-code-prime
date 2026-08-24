// TF-C79 (D-CSUBSET-INLINE-FUNCTION-SPECIFIER): every C99 6.7.4 `inline` FORM
// that DOES produce a definition in its own translation unit, in one program.
//
// The three that stay in THIS TU:
//   · `static inline`      — internal linkage, 6.7.4p6 (any internal-linkage
//                            function may be inline); always emitted.
//   · `static __inline`     \  the GNU spellings. ONE token kind carries all
//   · `static __inline__`   /  three — MEASURED with clang, `__inline int p(int)
//                            {…}` and `inline int p(int){…}` produce the exact
//                            same symbol state, so they are synonyms and not a
//                            dialect fork.
//   · `extern inline`      — 6.7.4p7: an inline definition WITH extern DOES
//                            provide the external definition; emitted Global.
//   · an `inline` PROTOTYPE beside a plain definition — 6.7.4p7's quantifier
//                            is over ALL file-scope declarations, so the one
//                            plain declaration restores the external
//                            definition. MEASURED: clang emits `T _p` here.
//
// ★ THE EXIT CODE GATES ON THE VALUES, not on "it compiled". Each helper adds a
// DISTINCT amount and they are chained, so a body that was dropped, emitted
// twice, or wired to the wrong callee moves the exit code. The arithmetic:
// 0 →add_two→ 2 →add_three→ 5 →add_five→ 10 →times_two→ 20 →minus_one→ 19,
// then 19 + 23 = 42.
//
// ★ The block-scope `inline` prototype is deliberately present. TF-C77 rejected
// admitting a specifier at file scope but not inside a body — one specifier must
// not mean two things depending on where it is written — so `localDeclSpecifier`
// carries `InlineKeyword` too. C99 6.7.1p5 permits no storage-class specifier
// other than `extern` on a block-scope function declaration, so the bare form is
// the legal one to pin here (a block-scope `static inline int f(int);` is a
// constraint violation and is NOT what this line asserts).
//
// VERIFIED clang-clean (`-fsyntax-only -Wall -Wextra -isysroot $(xcrun
// --show-sdk-path)`, zero errors, zero warnings) and the clang-linked binary
// independently exits 42, so the expected exit code is ground truth from a real
// toolchain rather than merely what DSS happens to produce.

static inline int add_two(int x) { return x + 2; }
static __inline int add_three(int x) { return x + 3; }
static __inline__ int add_five(int x) { return x + 5; }

extern inline int times_two(int x) { return x * 2; }

inline int minus_one(int x);
int minus_one(int x) { return x - 1; }

int main(void) {
    inline int declared_in_block(int);

    int v = add_two(0);
    v = add_three(v);
    v = add_five(v);
    v = times_two(v);
    v = minus_one(v);
    return v + 23;
}

int declared_in_block(int x) { return x; }
