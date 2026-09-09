// P65 — `extern` ON A FILE-SCOPE INITIALIZER-INFERRED DECLARATION, the
// SPECIFIER-SET half of [[D-CSUBSET-AUTO-FILE-SCOPE]] (C23 6.7.2p2).
//
// ✔MEASURED before the change, through the shipped DSS CLI: `extern auto g = 1;`
// was `error[P_NoAlternativeMatched]: expected 'Identifier', 'VoidKeyword', … —
// got 'auto'`, and `auto extern g = 1;` was refused IDENTICALLY. That identity
// is what makes this a specifier-SET gap and not an ORDER one: the file-scope
// specifier run was already order-free when both spellings failed, so no amount
// of reordering could have helped — `extern` was simply not a specifier the
// inference row admitted.
//
// ✔THE REFERENCES, each probed SEPARATELY on its own translation unit,
// 2026-09-08:
//   gcc 13.3.0  -std=c2x -c    rc=0  `nm` D g   warns "'g' initialized and
//                                     declared 'extern'";  -pedantic-errors rc=0
//   clang 18.1.3 -std=c23 -c   rc=0  `nm` D g   warns -Wextern-initializer;
//                                     -pedantic-errors rc=0
//   MSVC 19.51.36252           ABSTAINS — it implements no C23 inference at all
//                              (`auto g = 1;` is rc=0 with C4042 "'g': has bad
//                              storage class", i.e. the C89 STORAGE CLASS over
//                              an implicit int, and it refuses `static auto
//                              g = 1;` with C2159 as well), so its C2159 on
//                              `extern auto` is a storage-class collision and
//                              not a vote on this feature.
//   ISO C23     6.7.2p2 "auto may appear with all the others except typedef";
//               6.7.2p4 conditions that on "if the type is to be inferred from
//               an initializer", which is exactly this declaration.
// Two working references and ISO C on the accepting side, with -pedantic-errors
// clean on both — DSS was BELOW the union.
//
// ★★★ THE EXAMPLE IS NOT A PARSE TEST, AND A PARSE-ONLY FIX IS EXACTLY WHAT IT
// IS BUILT TO CATCH. Admitting `ExternKeyword` in the grammar alone would have
// made every declaration here rc 0 while the linkage tier answered
// `warning[H_UnknownLinkageSpecifier]` and DROPPED the keyword — an ACCEPT with
// the specifier discarded, on the way to an object that is silently wrong. So
// every arm gates on a RUNTIME consequence:
//
//   * `g_ext` is DEFINED in def.c by `extern auto g_ext = 11;` and read here
//     through a plain `extern int g_ext;`. A mis-lowering to an IMPORT fails the
//     LINK with an undefined symbol; a mis-lowering of THIS file's `extern int
//     g_ext;` into a definition fails the link with a DUPLICATE. Both directions
//     are covered by the one arm.
//   * `extern auto e_int = 20;` must DEFINE storage here and hold its value —
//     C 6.9.3p1 makes the initializer the definition, `extern` redundant.
//   * `auto extern r_int = 7;` is the REVERSED order of the same set and must
//     mean the same thing; C 6.7p2 makes the declaration specifiers unordered.
//   * `extern const auto k_dbl = 2.0;` must INFER double — `k_dbl + k_dbl` is
//     taken in floating point and truncated to 4, which an int-defaulted type
//     would get wrong — while the leading `const` still binds through a
//     specifier prefix that now also carries `extern`.
//   * `extern auto p_str = "ok";` must DECAY to a pointer (C23 6.7.10p2 infers
//     the type AFTER array-to-pointer conversion) and be indexable at runtime.
//
// exit = 11 + 20 + 7 + 4 + 0 = 42. ✔The same source builds and runs to 42 under
// gcc 13.3.0 and clang 18.1.3, so the expected value is a reference measurement
// and not a DSS self-report.
//
// RED-ON-DISABLE (REMOVE direction). TWO independent mutants, because the change
// has two halves and either alone would leave the other unproven. ⚠ BOTH MUTANTS
// ARE THE CONFIG DOCUMENT, SO **NO OBJECT md5 IS INVOLVED**: `c.lang.json` is
// copied into the build tree's `dss-config-snapshot` at ctest RUN time (never at
// build time — see cmake/DssConfigSnapshot.cmake), so no translation unit
// recompiles and no binary moves. What moves is the config file's own md5, which
// must move and RETURN.
//   (A) GRAMMAR — delete `"ExternKeyword"` from `topLevelAutoSpecifier`'s `alt`
//       in src/dss-config/sources/c.lang.json. Every declaration below goes back
//       to error[P_NoAlternativeMatched] and this example never links.
//   (B) SEMANTICS — delete the `"extern"` entry from
//       `autoInferredTopLevelDecl`'s `linkageSpecifiers`. The declarations still
//       PARSE, which is the point: the keyword becomes an UNKNOWN linkage
//       specifier and is dropped, so this example's definedness arms fail while
//       a parse-shaped pin would stay green.
// Both red sets and the green controls are in
// tests/analysis/semantic/test_specifier_set_extern_auto_and_inline_object.cpp.

extern int g_ext;                       // an IMPORT of def.c's definition

extern auto        e_int = 20;          // extern + inference: a DEFINITION here
auto        extern r_int = 7;           // the reversed order, same declaration
extern const auto  k_dbl = 2.0;         // inferred double under a leading const
extern auto        p_str = "ok";        // decays to a pointer (C23 6.7.10p2)

int main(void) {
    int acc = 0;
    acc = acc + g_ext;                  // 11 — EXTERNAL linkage, defined in def.c
    acc = acc + e_int;                  // 20 — defined HERE by the initializer
    acc = acc + r_int;                  //  7 — `auto extern` means `extern auto`
    acc = acc + (int)(k_dbl + k_dbl);   //  4 — inferred DOUBLE, not int
    acc = acc + (p_str[0] == 'o' ? 0 : 1);
    return acc;
}
