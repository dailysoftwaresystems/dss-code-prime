// [[D-C-FILE-SCOPE-INFERRED-AUTO-MUST-LEAD-THE-DECLARATION-SPECIFIERS]] (P65,
// C 6.7p2 + C23 6.7.9) — the SPECIFIER-LED half of file-scope type inference.
//
// The sibling example `auto_file_scope_inference` writes every specifier AFTER
// `auto`, because that is all DSS accepted when it landed. This one writes them
// BEFORE, which is the spelling a human actually types and the only spelling
// `alignas` has on clang. C 6.7p2 makes the declaration specifiers an UNORDERED
// SET, so the two orders are the same declaration; ✔MEASURED 2026-09-08, each
// reference probed SEPARATELY on its own translation unit:
//
//   static auto g = 42;        gcc 13.3.0 -std=c2x rc=0 `nm` d g   clang 18.1.3 -std=c23 rc=0
//   auto static g = 42;        gcc rc=0 `nm` d g                   clang rc=0
//   const auto k = 42;         gcc rc=0 `nm` R k                   clang rc=0 `nm` R k
//   auto const k = 42;         gcc rc=0 `nm` R k                   clang rc=0 `nm` R k
//   thread_local auto t = 5;   gcc rc=0 `nm` D t                   clang rc=0 `nm` D t
//   alignas(16) auto a = 3;    gcc rc=0                            clang rc=0
//   auto alignas(16) a = 3;    gcc rc=0                            clang rc=1  ← the trailing
//                                                                     order is the LESS
//                                                                     portable one
//
// ★★★ THE EXAMPLE IS NOT A PARSE TEST, AND THE ORDER IS EXACTLY WHERE A PARSE
// TEST WOULD MISS. The risk a grammar change of this shape carries is that the
// declaration parses into the RIGHT rule and the specifier written on the WRONG
// SIDE of `auto` is then never read — a `static` that stops meaning internal
// linkage, or a `const` that stops binding. Both are silent. So every arm below
// gates on a RUNTIME consequence of a specifier that is written BEFORE `auto`:
//
//   * `auto g_ext = 11;` in def.c is read here through a plain `extern int
//     g_ext;`, so an inference that produced INTERNAL linkage fails the LINK
//     rather than the comparison.
//   * `static auto s_int = 20;` must keep INTERNAL linkage and hold its value.
//   * `const auto k_const = 7;` must bind the QUALIFIER through the specifier
//     prefix from the LEADING position; the release arm must still produce 7.
//   * `const auto d_dbl = 2.0;` must infer DOUBLE with a leading `const`:
//     `d_dbl + d_dbl` is taken in floating point and truncated to 4.
//   * `static auto p_str = "ok";` must DECAY to a pointer (C23 6.7.9) and be
//     indexable at runtime.
//
// exit = 11 + 20 + 7 + 4 + 0 = 42, the same total the `auto`-led sibling
// produces from the same five declarations — which is the point: the two orders
// are ONE declaration written two ways.
//
// ⚠ WHAT MADE THE LEADING ORDER COST SOMETHING, AND WHY IT NO LONGER DOES.
// Admitting a specifier BEFORE `auto` puts {static, const, constexpr,
// thread_local, inline, alignas, __attribute__, [[} into the inference rule's
// FIRST, so each of those leads gains a SECOND top-level candidate and the
// parser must decide between them. It used to decide by PROBING the other
// candidate under a token budget, and a file-scope construct is not a bounded
// token class — it contains function definitions. The parser now DESCENDS into
// an alt's final candidate instead of probing it, which is the same rule its
// all-fail path already replayed, so the budget never binds on the reading the
// parser was going to take anyway (`finalCandidateDirectDescent_`,
// src/analysis/syntactic/parser.cpp).
//
// RED-ON-DISABLE (REMOVE direction). TWO independent mutants, because the
// capability has two halves and either alone would leave the other unproven:
//   (1) CONFIG, and the mutant is the DOCUMENT — `c.lang.json` is read at RUN
//       time through `$DSS_CONFIG_ROOT`, so NO translation unit recompiles and
//       NO object md5 moves; what moves is the config file's own md5. Delete
//       the LEADING `{"repeat": "topLevelAutoSpecifier"}` from
//       `topLevelAutoSpecifiers` in src/dss-config/sources/c.lang.json — every
//       declaration in this file becomes error[P_NoAlternativeMatched] and the
//       example never links.
//   (2) ENGINE — delete the `finalCandidateDirectDescent_()` call from
//       `abandonAndAdvance_` in src/analysis/syntactic/parser.cpp (source AND
//       object md5 both move). The declarations still parse, but
//       `LargeFileScopeInitializerAfterALeadingSpecifierIsNotProbed` and its
//       siblings go red — see
//       tests/analysis/semantic/test_file_scope_declaration_definedness.cpp.

extern int g_ext;

static auto        s_int   = 20;
const auto         k_const = 7;
const auto         d_dbl   = 2.0;
static auto        p_str   = "ok";

int main(void) {
    int acc = 0;
    acc = acc + g_ext;                 // 11 — EXTERNAL linkage, from def.c
    acc = acc + s_int;                 // 20 — INTERNAL linkage, leading `static`
    acc = acc + k_const;               //  7 — leading `const`, still readable
    acc = acc + (int)(d_dbl + d_dbl);  //  4 — inferred DOUBLE under a leading `const`
    acc = acc + (p_str[0] == 'o' ? 0 : 1);
    return acc;
}
