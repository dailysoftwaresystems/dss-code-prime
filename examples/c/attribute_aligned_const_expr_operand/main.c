// ★★ [[D-CSUBSET-ATTRIBUTE-ARG-CONSTANT-EXPRESSION]] — a KEYWORD-LED constant
// expression as the operand of `__attribute__((aligned(...)))`, carried through the
// real backend to an exit code.
//
// THE WITNESS IS REAL. The shipped Xcode SDK's
// `MacOSX.sdk/usr/include/libkern/OSAtomicDeprecated.h` writes
// `typedef int64_t __attribute__((__aligned__((sizeof(int64_t))))) …`, and the same
// shape again with `_Alignof(...)` and with a typedef-name inside the `sizeof`.
// `sizeof` and `_Alignof` are KEYWORDS, and the attribute-argument atom admitted only
// Identifier / IntLiteral / FloatLiteral / string / nested-parens — so no alternative
// matched them and the header died at the parser. ✔MEASURED through the shipped CLI
// before the fix: `error[P_NoAlternativeMatched] expected 'Identifier', 'IntLiteral',
// 'FloatLiteral', 'StringStart', 'ParenOpen', … — got 'sizeof'`, with a `got ')'`
// cascade behind it, while the same construct with an integer literal built and ran.
//
// ★ WHY THE EXIT CODE IS THE ASSERTION, AND WHY EVERY ROW DISCRIMINATES. The unit
// pins in tests/analysis/semantic/test_attribute_arg_const_expr.cpp stop at the
// stored alignment; this file carries the same answers through layout, codegen and
// the linker to a number. Each truth row is built so a DROPPED or MIS-FOLDED operand
// changes the total rather than leaving a clean compile behind:
//   * `struct pair` is 16 bytes with alignment 8, so `sizeof` (16) and `_Alignof` (8)
//     are DIFFERENT numbers — a row reading the wrong one fails instead of passing by
//     coincidence.
//   * every member row places the decorated member after a `char`, so the member's
//     OFFSET (and therefore the aggregate's size) moves when the request lands. With
//     the attribute dropped, m1 would be 16/8 rather than 32/16 and m2/m3/m4 would be
//     2/1 rather than 16/8 and 32/16.
//   * the numbers are TARGET-INDEPENDENT on purpose: `long long` is 8 bytes and
//     8-aligned on x86_64-linux, x86_64-windows (LLP64), aarch64-linux and
//     arm64-darwin alike. `long` is deliberately NOT used — it is 8 under LP64 and 4
//     under LLP64, so it would make the windows arm pass through the very defect this
//     file guards.
//
// ★ THE TYPEDEF WITNESS CARRIES NO POINTS, DELIBERATELY, AND THAT IS NOT AN OVERSIGHT.
// `osint64_t` below is the SDK line verbatim in shape, and its only claim here is that
// the file COMPILES — a typedef interns to the same TypeId as its aliasee, so DSS
// cannot represent an over-aligned alias and a satisfied request (8 on a type that
// already aligns to 8) is a proven no-op with no observable runtime consequence. The
// discriminating half of that arm is a REFUSAL and cannot live in a file that must
// build; it is pinned as
// AttributeArgConstExpr.TypedefRequestStricterThanTheAliasFailsLoud, whose red proves
// the operand folded to 16 rather than vanishing. Read the two together.
//
// ★ REFERENCES, each probed SEPARATELY, over what WORKS rather than what is merely
// accepted: gcc 13.3.0 (WSL, `-std=gnu17` and `-std=c2x`), clang 18.1.3 (WSL,
// `-std=gnu17` and `-std=c23`) and mingw-w64 gcc 13.2.0 all BUILD AND RUN the three
// witness shapes to 42 at `-Wall -Wextra` with zero warnings. MSVC 19.51 casts NO vote
// on `__attribute__` — its CONTROL, the identical construct with an integer literal,
// is refused by the same C2143 — so its vote was taken in its own spelling, where it
// REFUSES: `__declspec(align(sizeof(T)))` is `error C2059: syntax error: 'sizeof'`
// while the `__declspec(align(8))` control is rc 0. Three working references to one
// refusal, so the disjunction requires DSS to accept.
//
// RED-ON-DISABLE: remove `"sizeofExpr"` / `"alignofExpr"` from `attrArgAtom`'s alt
// in c.lang.json and this file does not COMPILE at all (P_NoAlternativeMatched on
// `sizeof`). Put `attrArgAtom` back as `attrArgItem`'s first element (i.e. take the
// `attrArgConstExpr` expression level away) and the keyword-led rows still build
// while every OPERATOR row below dies at the parser. Remove `attributeArgExprRule`
// from `semantics.attributeSemantics` instead and it still parses, but the operand
// descent walks past the expression into its type reference and every row fails
// loud — all three stop this file exiting 42.

// 16 bytes, alignment 8 — the two numbers this file discriminates with.
struct pair { long long a; long long b; };

// A file-scope object of that type, for the `sizeof <expression>` operand form.
static struct pair g_pair;

// (1) THE COMPOSITE SINK. `aligned` on a struct DEFINITION raises the aggregate's own
// alignment, through a different consumer than the declaration-level one below —
// both locate the operand with the same descent, so a wrong stop breaks both.
struct box { char c; } __attribute__((__aligned__(sizeof(struct pair))));

// (2) THE DECLARATION-LEVEL SINK, `sizeof(type-name)` operand, DOUBLE PARENS — the
// exact punctuation the SDK header writes.
struct m1 { char pad; long long v __attribute__((__aligned__((sizeof(struct pair))))); };

// (3) `_Alignof(type-name)` operand. Expected 8, NOT 16 — a row that read `sizeof` by
// mistake would fail here rather than pass.
struct m2 { char pad; char v __attribute__((__aligned__(_Alignof(struct pair)))); };

// (4) The GNU `__alignof__` spelling — the one a header that must still compile as C89
// writes, and therefore the one an SDK actually contains. An alias of the same keyword
// kind, so it must reach the same fold with no second rule.
struct m3 { char pad; char v __attribute__((__aligned__(__alignof__(struct pair)))); };

// (5) `sizeof <expression>` — the VALUE form of the operator, a different branch of
// the sizeof rule. Admitting only the type-name branch would leave this a parse error
// while the row looked closed.
struct m4 { char pad; char v __attribute__((__aligned__(sizeof g_pair))); };

// THE SDK TYPEDEF WITNESS, verbatim in shape. Points: none (see the note above); the
// claim is that it compiles.
typedef long long __attribute__((__aligned__((sizeof(long long))))) osint64_t;
static osint64_t g_alias;

// ── (6..) THE OPERATOR FORMS, and PRECEDENCE is the reason they are here ──────
//
// An attribute argument is a CONSTANT-EXPRESSION, not two keyword-led primaries.
// ✔MEASURED, each reference probed SEPARATELY with an `aligned(16)` CONTROL beside
// it, BUILD **and** RUN with `_Alignof` and `__builtin_offsetof` asserted: gcc
// 13.3.0 (`-std=gnu17`, `-std=c2x`), clang 18.1.3 (`-std=gnu17`, `-std=c23`) and
// mingw-w64 gcc 13.2.0 all build AND run every row below to 42 with zero warnings
// at `-Wall -Wextra`. Only MSVC refuses, and it refuses in its own spelling.
//
// ★ THESE ROWS CARRY NO POINTS AND FAIL WITH THEIR OWN EXIT CODE INSTEAD, so a
// wrong FOLD is distinguishable from a wrong ROW. `o1` is the axis that matters
// most: `2 + 2 * 3` is 8, and a left-to-right operator chain — the cheap way to
// admit operators — folds it to 12. A wrong alignment is a wrong LAYOUT that
// compiles clean, so this row is the file's guard against the silent miscompile.
struct o1 { char pad; char v __attribute__((__aligned__(2 + 2 * 3))); };
struct o2 { char pad; char v __attribute__((__aligned__(sizeof(struct pair) * 2))); };
struct o3 { char pad; char v __attribute__((__aligned__(2 * _Alignof(struct pair)))); };
struct o4 { char pad; char v __attribute__((__aligned__(1 << 4))); };
struct o5 { char pad; char v __attribute__((__aligned__(1 ? 8 : 32))); };

int main(void) {
    int score = 0;

    // The operator rows, each with its own exit code so a failure names itself.
    if (sizeof(struct o1) != 16u || _Alignof(struct o1) !=  8u) return 5;
    if (sizeof(struct o2) != 64u || _Alignof(struct o2) != 32u) return 6;
    if (sizeof(struct o3) != 32u || _Alignof(struct o3) != 16u) return 7;
    if (sizeof(struct o4) != 32u || _Alignof(struct o4) != 16u) return 8;
    if (sizeof(struct o5) != 16u || _Alignof(struct o5) !=  8u) return 9;

    if (sizeof(struct box) == 16u && _Alignof(struct box) == 16u) score += 12;
    if (sizeof(struct m1)  == 32u && _Alignof(struct m1)  == 16u) score += 10;
    if (sizeof(struct m2)  == 16u && _Alignof(struct m2)  ==  8u) score +=  8;
    if (sizeof(struct m3)  == 16u && _Alignof(struct m3)  ==  8u) score +=  7;
    if (sizeof(struct m4)  == 32u && _Alignof(struct m4)  == 16u) score +=  5;

    // The alias must still measure as its aliasee — the request was a no-op, and an
    // engine that "honored" it by inventing an over-aligned alias would say 16 here.
    if (sizeof(osint64_t) != 8u || _Alignof(osint64_t) != 8u) return 3;
    g_alias = (osint64_t)sizeof(struct box);
    if (g_alias != 16) return 4;
    g_pair.a = 1;

    return score;
}
