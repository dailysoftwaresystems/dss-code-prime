// `--emit-hir <path>` — the HIR of a translation unit as a FILE, and the
// consumer contract that goes with it.
//
// WHY THIS SUITE EXISTS. `emitHir` shipped with ZERO product callers: the
// `.dsshir` format round-tripped, verified on load, and had five checked-in
// fixtures, but nothing in the compiler ever wrote one. So every property a
// consumer outside this repository would depend on was, at best, true by
// accident — no test could tell an artifact that is SELF-CONTAINED from one that
// merely round-trips inside a process that still holds the type interner and the
// symbol table it was emitted from.
//
// What is pinned here, and each is a claim a reader holding ONLY the bytes has
// to be able to make:
//   * a TU THAT WOULD NOT LINK still emits, at rc 0 — a single function in
//     isolation, an undefined callee, no `main`. That is the whole reason the
//     flag is a MODE and not a `--compile` modifier;
//   * `rc == 0` ⇔ an artifact is written; a REJECTED source writes NOTHING and
//     exits non-zero;
//   * every referenced TYPE is reconstructible from the text alone — a `struct`
//     travels with its field types, and a `typedef`'d / nested / array-of-struct
//     shape travels with it;
//   * every SOURCE-LEVEL NAME travels — a property is stated about `x`, not `%2`;
//   * the FORMAT VERSION and the PRODUCER REVISION are on the artifact's face,
//     and the producer is the real build stamp rather than a placeholder;
//   * the artifact PARSES BACK and VERIFIES with nothing but its own bytes —
//     asserted through `parseHir`, which runs `HirVerifier` on load;
//   * HIR is TARGET-DEPENDENT, so one target is required and two are refused.
//
// HOST-INDEPENDENT: the front end runs, but nothing is codegen'd, linked, or
// executed, so every arm is valid on every leg.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "hir/hir_text.hpp"
#include "program/cli_args.hpp"
#include "program/program.hpp"
#include "repo_root.hpp"   // the seam guard's subject is the source tree
#include "scratch_dir.hpp" // one scratch directory per PROCESS, claimed atomically

#include <gtest/gtest.h>

#include <algorithm>   // std::min — the opt-in arm's list split
#include <array>
#include <cstdlib>     // std::getenv — the opt-in arm's inputs
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace dss;

namespace fs = std::filesystem;

namespace {

// The one target every leg of this project can RESOLVE (resolution is all this
// suite needs — nothing is assembled or linked). Spelled once so the arms below
// differ only in the thing they are testing.
constexpr std::string_view kTarget = "x86_64:elf64-x86_64-linux";
// The second target exists to make a TARGET-DEPENDENCE claim measurable: its
// data model is LLP64 where the first is LP64, which is the axis that decides
// whether `long`→`int` is an explicit `cast` node or an identity retag.
constexpr std::string_view kTargetLlp64 = "x86_64:pe64-x86_64-windows-exec";

// A scratch directory unique to this PROCESS, under the system temp root,
// claimed atomically by the shared `ScratchDir` helper. It is NOT the
// repository root: a bare output filename would land in whatever directory the
// runner happened to start in, which on this project has twice meant probe
// artifacts committed into a source tree.
//
// It used to be the constant `temp_directory_path()/"dss_emit_hir_mode_test"`,
// and this comment called that "unique to this binary". It was one directory
// per MACHINE, shared by every build tree's copy of this binary, and `emitTo`
// removes its destination before emitting. ✔MEASURED 2026-09-23 (P68 round 8):
// two concurrent instances of the same binary failed 7 of 24 runs across 12
// pairs (an artifact read back EMPTY, or gone, after the other instance's
// `remove`), and every solo run passed. A lane's MSVC gate hit it as a red on
// `EmitHirMode.TypedefUnionEnumAndArrayExtentsAllTravel` while another tree ran
// the same test.
[[nodiscard]] fs::path scratchDir() {
    static dss::test_support::ScratchDir const scratch{
        dss::test_support::Location::Temp, "emit-hir-mode"};
    return scratch.path();
}

[[nodiscard]] fs::path writeSource(std::string_view stem, std::string_view text) {
    fs::path const p = scratchDir() / (std::string{stem} + ".c");
    std::ofstream o{p, std::ios::binary | std::ios::trunc};
    o << text;
    o.close();
    return p;
}

[[nodiscard]] std::string readFile(fs::path const& p) {
    std::ifstream i{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{i},
                       std::istreambuf_iterator<char>{}};
}

// One `--emit-hir` run, in process. Returns the exit code and fills `artifact`
// with what landed on disk (empty when nothing was written).
struct EmitResult {
    int         rc = 0;
    std::string artifact;
    std::string out;
    std::string err;
    fs::path    path;
};

[[nodiscard]] EmitResult emitTo(std::string_view stem, std::string_view source,
                                std::string_view target = kTarget) {
    fs::path const src = writeSource(stem, source);
    fs::path const dst = scratchDir() / (std::string{stem} + ".dsshir");
    std::error_code ec;
    fs::remove(dst, ec);   // never read a previous run's artifact

    EmitResult r;
    r.path = dst;
    std::ostringstream out;
    std::ostringstream err;
    Program p;
    r.rc = p.emitHirText({src.string()}, "c", std::string{target}, dst.string(),
                         out, err);
    r.out = out.str();
    r.err = err.str();
    if (fs::exists(dst)) r.artifact = readFile(dst);
    return r;
}

// Parse an artifact with NOTHING but its own bytes — no interner, no symbol
// table, no side channel. This is the whole self-containment claim, and it is
// why every assertion below reads the artifact back rather than inspecting the
// in-process `Hir` the emitter had.
struct ReparseResult {
    bool        ok = false;
    std::string diagnostics;
};

[[nodiscard]] ReparseResult reparse(std::string const& artifact) {
    DiagnosticReporter r;
    auto parsed = parseHir(artifact, CompilationUnitId{1}, r);
    ReparseResult out;
    out.ok = parsed->ok;
    for (auto const& d : r.all()) {
        out.diagnostics += std::string{diagnosticCodeName(d.code)} + ": " + d.actual + "\n";
    }
    return out;
}

} // namespace

// ── The gate: a TU that does not link, and what rc 0 means ──────────────────

TEST(EmitHirMode, TranslationUnitThatCannotLinkStillEmitsAtExitZero) {
    // No `main`, an undefined callee, and a function nothing calls. Every one of
    // these makes a LINK fail; none of them makes the front end object. If this
    // arm ever needs a linkable input, `--emit-hir` has stopped being a mode.
    auto const r = emitTo("nolink",
                          "extern int nowhere(int);\n"
                          "int lonely(int v) { return nowhere(v) + 1; }\n");
    EXPECT_EQ(r.rc, 0) << r.err;
    EXPECT_FALSE(r.artifact.empty()) << "rc 0 must mean an artifact was written";
    EXPECT_TRUE(r.err.empty()) << r.err;

    auto const back = reparse(r.artifact);
    EXPECT_TRUE(back.ok) << "the artifact did not parse+verify from its own bytes\n"
                         << back.diagnostics << "\n---\n" << r.artifact;
}

TEST(EmitHirMode, RejectedSourceExitsNonZeroAndWritesNothing) {
    // The other half of the contract, and the one that makes rc USABLE: a
    // consumer reads rc != 0 as "DSS rejected this program", so a rejected
    // program must never leave a file behind for them to read anyway.
    auto const r = emitTo("rejected", "int broken(void) { return undeclared_thing; }\n");
    EXPECT_NE(r.rc, 0);
    EXPECT_TRUE(r.artifact.empty())
        << "a rejected source left an artifact on disk:\n" << r.artifact;
    EXPECT_FALSE(fs::exists(r.path));
}

// ── Self-contained TYPES (the reader holds only the text) ───────────────────

TEST(EmitHirMode, AStructIsDefinedOnceAndEveryUseNamesItsHandle) {
    auto const r = emitTo("structs",
                          "struct Point { int x; int y; };\n"
                          "int sum_fields(struct Point *p) { return p->x + p->y; }\n");
    ASSERT_EQ(r.rc, 0) << r.err;

    // The FIELD TYPES are in the text, not an id pointing at an interner this
    // process happens to still hold — and since v5 they are there ONCE, in the
    // artifact's own `types` table. `struct "Point"` alone would be an opaque
    // name; the `{i32, i32}` is the claim.
    EXPECT_NE(r.artifact.find("types {\n  type 1 = struct \"Point\" {i32, i32}\n}\n"),
              std::string::npos) << r.artifact;
    // Every USE names the handle — the declaration, the parameter, and through
    // the pointer — so a reader holding only the bytes resolves `p->x` against
    // the one definition. ✔v4 re-spelled the whole struct at each of these, which
    // is what made the artifact (mentions × reachable graph) on real C.
    EXPECT_NE(r.artifact.find("type_decl %1 : type 1\n"), std::string::npos) << r.artifact;
    EXPECT_NE(r.artifact.find("param %3 : ptr<type 1>\n"), std::string::npos) << r.artifact;
    EXPECT_EQ(r.artifact.find("ptr<struct"), std::string::npos)
        << "a composite was spelled inline at a use site:\n" << r.artifact;
    EXPECT_EQ(r.artifact.find("struct \"Point\""), r.artifact.rfind("struct \"Point\""))
        << "the definition appears more than once:\n" << r.artifact;

    auto const back = reparse(r.artifact);
    EXPECT_TRUE(back.ok) << back.diagnostics << "\n---\n" << r.artifact;
}

TEST(EmitHirMode, TypedefUnionEnumAndArrayExtentsAllTravel) {
    // The remaining shapes the consumer named: a typedef'd struct, a union, an
    // enum, and an ARRAY EXTENT (a count, not a pointer). One source so the arm
    // proves they compose rather than each surviving alone.
    auto const r = emitTo("shapes",
                          "typedef struct Inner { char c; } Inner;\n"
                          "union Bits { int i; float f; };\n"
                          "enum Color { Red, Green };\n"
                          "int probe(Inner a, union Bits b, enum Color c) {\n"
                          "  int grid[4];\n"
                          "  return grid[0] + a.c + b.i + (int)c;\n"
                          "}\n");
    ASSERT_EQ(r.rc, 0) << r.err;
    // ⓘ `char`, not `i8`: C's plain `char` is a THIRD type distinct from both
    // `signed char` and `unsigned char`, and the format keeps that distinction
    // rather than normalising it to a width. A consumer reasoning about
    // signedness needs the difference.
    EXPECT_NE(r.artifact.find("struct \"Inner\" {char}"), std::string::npos) << r.artifact;
    EXPECT_NE(r.artifact.find("union \"Bits\" {i32, f32}"), std::string::npos) << r.artifact;
    // ⚠ `enum "Color"` carries its NAME and (when it diverges from the default)
    // its underlying width — but NOT its enumerator names or values, which are
    // folded to literals at every use. That is a real and documented boundary of
    // the self-containment rule, pinned here so it cannot change silently in
    // either direction. `docs/hir-text-format.md` states it to consumers.
    EXPECT_NE(r.artifact.find("enum \"Color\""), std::string::npos) << r.artifact;
    EXPECT_EQ(r.artifact.find("\"Red\""), std::string::npos)
        << "enumerator names now travel — update the documented boundary:\n" << r.artifact;
    // The EXTENT, spelled: `arr<i32, 4>` and not `ptr<i32>`.
    EXPECT_NE(r.artifact.find("arr<i32, 4>"), std::string::npos) << r.artifact;

    auto const back = reparse(r.artifact);
    EXPECT_TRUE(back.ok) << back.diagnostics << "\n---\n" << r.artifact;
}

TEST(EmitHirMode, CyclicCompositesEmitThroughTheWholeFrontEndAndReadBack) {
    // ★★★ v3 CLOSED THE FORMAT'S ONE SELF-CONTAINMENT HOLE, AND v5 MADE IT
    // ORDINARY. Until v3 a TU containing `struct Node { struct Node *next; }` —
    // the ordinary linked list, and with it every tree, intrusive container and
    // parent pointer in real C — exited non-zero with no file. v3 spelled the
    // cycle with a `rec <H>` back-reference; v5 defines every composite ONCE in
    // the `types` table, so the pointer that closes the cycle is a reference like
    // any other and needs no marker at all.
    //
    // This arm is the END-TO-END half: not the codec in isolation but the shipped
    // CLI over real C source, through tokenize, preprocess, parse, semantic
    // analysis and HIR lowering. All three shapes the consumer actually has are
    // here — self-reference, a MUTUALLY recursive pair, and a self-reference
    // reached through a TYPEDEF (where the name at the use site is not the name on
    // the definition). Each expectation is the artifact's whole `types` section.
    struct Arm { char const* name; char const* src; char const* expect; };
    std::array<Arm, 3> const arms{{
        {"self", "struct Node { int v; struct Node *next; };\n"
                 "int head_value(struct Node *n) { return n->v; }\n",
                 "types {\n  type 1 = struct \"Node\" {i32, ptr<type 1>}\n}\n"},
        {"mutual", "struct B;\n"
                   "struct A { int x; struct B *b; };\n"
                   "struct B { int y; struct A *a; };\n"
                   "int ax(struct A *p) { return p->x; }\n"
                   "int by(struct B *p) { return p->y; }\n",
                   "types {\n  type 1 = struct \"A\" {i32, ptr<type 2>}\n"
                   "  type 2 = struct \"B\" {i32, ptr<type 1>}\n}\n"},
        {"typedef", "typedef struct TNode TNode;\n"
                    "struct TNode { int v; TNode *left; TNode *right; };\n"
                    "int tv(TNode *t) { return t->v; }\n",
                    "types {\n  type 1 = struct \"TNode\" {i32, ptr<type 1>, ptr<type 1>}\n}\n"},
    }};
    for (Arm const& arm : arms) {
        auto const r = emitTo(arm.name, arm.src);
        ASSERT_EQ(r.rc, 0) << arm.name << ": a cyclic composite still refuses\n" << r.err;
        ASSERT_FALSE(r.artifact.empty()) << arm.name;
        EXPECT_EQ(r.artifact.find('?'), std::string::npos)
            << arm.name << ": the poison token is still being written\n" << r.artifact;
        EXPECT_NE(r.artifact.find(arm.expect), std::string::npos)
            << arm.name << ": expected " << arm.expect << "\n" << r.artifact;
        // ⚠ THE HALF THAT MAKES IT WORTH ANYTHING. An artifact that emits but
        // does not read back is not a representation; `reparse` re-parses AND
        // runs HirVerifier on load.
        auto const back = reparse(r.artifact);
        EXPECT_TRUE(back.ok) << arm.name << "\n" << back.diagnostics
                             << "\n---\n" << r.artifact;
    }

    // CONTROL: an ACYCLIC composite is spelled exactly like a cyclic one — one
    // table entry — so cyclicity costs nothing and marks nothing. Without this,
    // "the cyclic arms emit" is equally consistent with the writer special-casing
    // them.
    auto const control = emitTo("cyclic_control",
                                "struct Leaf { int v; int *p; };\n"
                                "int leaf_value(struct Leaf *n) { return n->v; }\n");
    EXPECT_EQ(control.rc, 0) << control.err;
    EXPECT_NE(control.artifact.find("types {\n  type 1 = struct \"Leaf\" {i32, ptr<i32>}\n}\n"),
              std::string::npos) << control.artifact;
    EXPECT_EQ(control.artifact.find(" rec "), std::string::npos) << control.artifact;
    EXPECT_TRUE(reparse(control.artifact).ok);
}

// ── Real C's shape: a composite GRAPH mentioned many times ──────────────────
//
// ★★★ THE CONSUMER-VISIBLE HALF OF THE v5 BOUND, THROUGH THE WHOLE FRONT END. A
// chain of `kGraph` structs (each pointing at the next) is mentioned by
// `kMentions` functions. v4 re-spelled the entire chain at every mention —
// ✔MEASURED on sqlite's amalgamation (P68 r7, lane `cr`): 1.18 MB of it ran 275.9 G
// instructions and died `std::bad_alloc` — where v5 writes each struct's fields
// exactly once. The codec-level COUNT pin lives in `tests/hir/test_hir_text.cpp`
// (`HirTextTypeSpellingCost.*`); this arm asserts the same fact on the artifact a
// consumer reads: one definition per struct, and no field list anywhere else.
TEST(EmitHirMode, ACompositeGraphMentionedManyTimesIsDefinedOnceInTheTable) {
    constexpr int kGraph    = 12;
    constexpr int kMentions = 40;
    std::string src;
    for (int k = 0; k < kGraph; ++k) {
        src += "struct S" + std::to_string(k) + " { int v; ";
        src += (k + 1 < kGraph) ? "struct S" + std::to_string(k + 1) + " *next; };\n"
                                : std::string{"int *last; };\n"};
    }
    for (int m = 0; m < kMentions; ++m) {
        src += "int f" + std::to_string(m) + "(struct S0 *p) { return p->v; }\n";
    }
    auto const r = emitTo("composite_graph", src);
    ASSERT_EQ(r.rc, 0) << r.err;
    std::size_t entries = 0;
    for (std::size_t at = r.artifact.find("\n  type "); at != std::string::npos;
         at = r.artifact.find("\n  type ", at + 1)) {
        ++entries;
    }
    EXPECT_EQ(entries, static_cast<std::size_t>(kGraph)) << r.artifact;
    for (int k = 0; k < kGraph; ++k) {
        std::string const name = "struct \"S" + std::to_string(k) + "\" {";
        EXPECT_EQ(r.artifact.find(name), r.artifact.rfind(name))
            << "S" << k << " is defined more than once";
        EXPECT_NE(r.artifact.find(name), std::string::npos) << "S" << k << " has no definition";
    }
    // No composite is spelled inline anywhere outside the table.
    std::size_t const body = r.artifact.find("\nmodule ");
    ASSERT_NE(body, std::string::npos);
    EXPECT_EQ(r.artifact.find("struct \"", body), std::string::npos)
        << "a composite was spelled inline in the module body";
    EXPECT_EQ(hirArtifactRoundTripFailure(r.artifact), "");
}

// ── Job 2 of P68 round 8: `if (x) return; g();` ─────────────────────────────
//
// ★★★ THE MINIMAL CONSTRUCT sqlite's `test/speedtest1.c` MET, THROUGH THE CLI. v4
// wrote the value-less return as a bare `return`, the next statement's `@loc`
// block followed it, and the reader took that statement for the return's VALUE:
// `--emit-hir` refused its own artifact with rc 2 (`unexpected node kind
// 'ExprStmt' in expression position`). gcc 13.3, clang 18.1.3 and MSVC 14.51 all
// accept the source — it is ISO C, not an extension (✔MEASURED, P68 round 8).
TEST(EmitHirMode, AValueLessReturnFollowedByAStatementEmitsAndReadsBack) {
    auto const r = emitTo("return_then_statement",
                          "void g(void);\n"
                          "void guarded(int x) { if (x) return; g(); }\n");
    ASSERT_EQ(r.rc, 0) << "rc 2 is the refusal sqlite's speedtest1.c met\n" << r.err;
    EXPECT_NE(r.artifact.find("return void\n"), std::string::npos) << r.artifact;
    EXPECT_EQ(hirArtifactRoundTripFailure(r.artifact), "");

    // READ BACK: `g();` is the block's second statement, not the return's value.
    DiagnosticReporter rep;
    auto const parsed = parseHir(r.artifact, CompilationUnitId{1}, rep);
    ASSERT_TRUE(parsed->ok) << r.artifact;
    Hir const& h = parsed->hir;
    auto const decls = h.moduleDecls(h.root());
    ASSERT_FALSE(decls.empty());
    HirNodeId const fn = decls[decls.size() - 1];
    ASSERT_EQ(h.kind(fn), HirKind::Function);
    auto const members = h.children(h.functionBody(fn));
    ASSERT_EQ(members.size(), 2u) << r.artifact;
    EXPECT_EQ(h.kind(members[0]), HirKind::IfStmt);
    EXPECT_EQ(h.kind(members[1]), HirKind::ExprStmt);
    EXPECT_FALSE(h.returnValue(h.ifThen(members[0])).has_value())
        << "the then-arm return came back WITH a value — the v4 misread";
}

// ── A suppress list cannot silence a structural verifier refusal ────────────
//
// ★★ ✔MEASURED P68 (lane `ht`, `dsscp --compile --suppress=<code>`, WSL Release)
// BEFORE these codes joined `kUnsuppressableCodes`: for the six structural
// verifier refusals a C source reaches, suppressing the refusal let the invalid
// tree go on — two green PE executables (`H_SehJumpIntoRegion`,
// `H_SehLabelAddress`, rc 0), two compiler process ABORTS in MIR
// (`H_VlaJumpIntoScope`, `H_VlaComputedGotoInScope`, rc 134) and two refusals
// replaced by an unrelated LIR error (`H_SehBuiltinContext`, `H_SehEarlyExit`);
// `--emit-hir` then blamed its own read-back ("a defect in the compiler, not in
// the source", rc 2). Each arm runs the real front end with the operator's
// suppress list naming its code: the compile must still fail, the refusal must
// still be SHOWN, nothing may be written, and `--emit-hir` must refuse the SOURCE
// (rc 1), not its own artifact. The three codes no C source reaches are pinned in
// `tests/hir/test_hir_text.cpp` (`HirTextVerdict.*`).
TEST(EmitHirMode, ASuppressListCannotSilenceAStructuralVerifierRefusalACSourceReaches) {
    constexpr std::string_view kPe = "x86_64:pe64-x86_64-windows-exec";
    struct Arm {
        char const*      stem;
        DiagnosticCode   code;
        std::string_view target;
        char const*      source;
    };
    std::array<Arm, 6> const arms{{
        {"suppress_seh_builtin", DiagnosticCode::H_SehBuiltinContext, kPe,
         "int f(void) { return (int)_exception_code(); }\n"
         "int main(void) { return f(); }\n"},
        {"suppress_seh_return", DiagnosticCode::H_SehEarlyExit, kPe,
         "int f(int *p) { __try { return *p; } __except (1) { return 42; } }\n"
         "int main(void) { int x = 7; return f(&x); }\n"},
        {"suppress_seh_goto_into", DiagnosticCode::H_SehJumpIntoRegion, kPe,
         "int f(int *p) { int rc = 0; if (*p) goto L;\n"
         "  __try { L: rc = *p; } __except (1) { rc = 42; } return rc; }\n"
         "int main(void) { int x = 7; return f(&x); }\n"},
        {"suppress_seh_label_addr", DiagnosticCode::H_SehLabelAddress, kPe,
         "int f(int *p) { int rc = 0; void *q = &&L; (void)q;\n"
         "  __try { L: rc = *p; } __except (1) { rc = 42; } return rc; }\n"
         "int main(void) { int x = 7; return f(&x); }\n"},
        {"suppress_vla_goto_into", DiagnosticCode::H_VlaJumpIntoScope, kTarget,
         "int main(void) { volatile int vn = 4; int n = vn;\n"
         "  goto L; int a[n]; L: a[0] = 1; return a[0]; }\n"},
        {"suppress_vla_computed_goto", DiagnosticCode::H_VlaComputedGotoInScope, kTarget,
         "int main(void) { volatile int vn = 4; int n = vn; void *p = &&L;\n"
         "  { int a[n]; a[0] = 1; goto *p; } L: return 0; }\n"},
    }};
    for (Arm const& arm : arms) {
        SCOPED_TRACE(std::string{diagnosticCodeName(arm.code)});
        fs::path const src = writeSource(arm.stem, arm.source);
        DiagnosticReporter::Config cfg;
        cfg.policy.suppress.insert(arm.code);

        // (1) THE COMPILE, through the rep-injection overload so the diagnostics
        // the run printed can be read back.
        fs::path const outDir = scratchDir() / (std::string{arm.stem} + "_out");
        std::error_code ec;
        fs::remove_all(outDir, ec);
        DiagnosticReporter rep{cfg};
        Program compiler;
        compiler.setOutputDir(outDir);
        int const rc = compiler.compileFiles({src.string()}, "c",
                                             std::vector<std::string>{std::string{arm.target}},
                                             rep);
        EXPECT_NE(rc, 0) << "the compile went on past a suppressed structural refusal";
        std::size_t shown = 0;
        for (auto const& d : rep.all()) if (d.code == arm.code) ++shown;
        EXPECT_EQ(shown, 1u) << "the suppress list SILENCED the structural refusal";
        EXPECT_TRUE(!fs::exists(outDir) || fs::is_empty(outDir))
            << "an artifact was written for a refused program";

        // (2) `--emit-hir`, with the same list: the SOURCE is refused (rc 1), and
        // nothing is written — not the rc 2 that blames the compiler's read-back.
        fs::path const dst = scratchDir() / (std::string{arm.stem} + ".dsshir");
        fs::remove(dst, ec);
        std::ostringstream out;
        std::ostringstream err;
        Program emitter;
        int const erc = emitter.emitHirText({src.string()}, "c", std::string{arm.target},
                                            dst.string(), out, err, cfg);
        EXPECT_EQ(erc, 1) << err.str();
        EXPECT_FALSE(fs::exists(dst)) << "an artifact was written for a refused program";
    }
}

// ── OPT-IN: sqlite's own `test/speedtest1.c`, wherever the sqlite tree is reachable ──
//
// ctest cannot see an sqlite checkout, so this arm is DISABLED by default and armed
// by hand (the `ArWriter.DISABLED_WriteCoffLibForNativeWitness` precedent):
//   DSS_SQLITE_SPEEDTEST1_C   = <sqlite>/test/speedtest1.c
//   DSS_SQLITE_INCLUDE_DIRS   = ';'-separated include dirs, the one holding the
//                               GENERATED sqlite3.h among them
//   DSS_SQLITE_DEFINES        = optional ','-separated NAME[=VALUE] list (the CLI
//                               recipe's defines, to reproduce a recipe exactly)
//   dss_program_test_emit_hir_mode --gtest_also_run_disabled_tests \
//       --gtest_filter='*SqliteSpeedtest1*'
// Armed without its inputs it FAILS rather than skips, so an armed run cannot pass
// vacuously.
TEST(EmitHirMode, DISABLED_SqliteSpeedtest1EmitsAndReadsBack) {
    char const* const source   = std::getenv("DSS_SQLITE_SPEEDTEST1_C");
    char const* const includes = std::getenv("DSS_SQLITE_INCLUDE_DIRS");
    ASSERT_NE(source, nullptr) << "set DSS_SQLITE_SPEEDTEST1_C to sqlite's test/speedtest1.c";
    ASSERT_NE(includes, nullptr)
        << "set DSS_SQLITE_INCLUDE_DIRS to the ';'-separated include dirs (sqlite3.h's among them)";
    ASSERT_TRUE(fs::exists(source)) << source;
    auto const split = [](std::string_view list, char sep) {
        std::vector<std::string> out;
        std::size_t from = 0;
        while (from <= list.size()) {
            std::size_t const to = std::min(list.find(sep, from), list.size());
            if (to > from) out.emplace_back(list.substr(from, to - from));
            from = to + 1;
        }
        return out;
    };
    std::vector<std::string> const includeDirs = split(includes, ';');
    ASSERT_FALSE(includeDirs.empty());
    std::vector<std::string> defines;
    if (char const* const d = std::getenv("DSS_SQLITE_DEFINES")) defines = split(d, ',');

    fs::path const dst = scratchDir() / "speedtest1.dsshir";
    std::error_code ec;
    fs::remove(dst, ec);
    std::ostringstream out;
    std::ostringstream err;
    Program p;
    p.setIncludeDirs(includeDirs);
    p.setUserDefines(defines);
    int const rc = p.emitHirText({std::string{source}}, "c", std::string{kTarget},
                                 dst.string(), out, err);
    ASSERT_EQ(rc, 0) << "rc 2 was the v4 refusal this arm pins against\n" << err.str();
    std::string const artifact = readFile(dst);
    EXPECT_EQ(hirArtifactRoundTripFailure(artifact), "");
    EXPECT_NE(artifact.find("return void\n"), std::string::npos)
        << "speedtest1.c's `if( … ) return;` sites must spell their absence";
}

// ── Self-contained NAMES ────────────────────────────────────────────────────

TEST(EmitHirMode, SourceLevelNamesTravelInsideTheArtifact) {
    auto const r = emitTo("names",
                          "int scale(int factor) {\n"
                          "  int scaled = factor + factor;\n"
                          "  return scaled;\n"
                          "}\n");
    ASSERT_EQ(r.rc, 0) << r.err;
    // A property is stated about `factor`, never about `%2`. The names live in
    // the file's own `symbols` preamble — not in the `HirTextContext` side table
    // the emitter was handed, which the reader never sees.
    EXPECT_NE(r.artifact.find("symbols {"), std::string::npos) << r.artifact;
    EXPECT_NE(r.artifact.find("\"scale\""), std::string::npos) << r.artifact;
    EXPECT_NE(r.artifact.find("\"factor\""), std::string::npos) << r.artifact;
    EXPECT_NE(r.artifact.find("\"scaled\""), std::string::npos) << r.artifact;

    // ★ AND THEY SURVIVE THE READ. Finding the string in the emitted bytes only
    // proves the writer wrote it; a reader has to get them BACK, which is the
    // property a verdict is reported through.
    DiagnosticReporter rep;
    auto parsed = parseHir(r.artifact, CompilationUnitId{1}, rep);
    ASSERT_TRUE(parsed->ok);
    bool sawFactor = false;
    bool sawScaled = false;
    for (auto const& n : parsed->symbolNames) {
        if (n == "factor") sawFactor = true;
        if (n == "scaled") sawScaled = true;
    }
    EXPECT_TRUE(sawFactor) << "the reader could not recover the parameter's name";
    EXPECT_TRUE(sawScaled) << "the reader could not recover the local's name";
}

// ── Version + producer on the artifact's face ───────────────────────────────

TEST(EmitHirMode, VersionAndRealProducerRevisionHeadTheArtifact) {
    auto const r = emitTo("stamped", "int id(int v) { return v; }\n");
    ASSERT_EQ(r.rc, 0) << r.err;
    // The version is the FIRST line, so a reader can decide whether to continue
    // before it has parsed anything it might misread.
    EXPECT_TRUE(r.artifact.starts_with("dsshir 5\n")) << r.artifact.substr(0, 64);
    EXPECT_NE(r.artifact.find("\nproducer \""), std::string::npos) << r.artifact;
    // ⚠ NOT MERELY "a producer line exists". The emitter accepts an EMPTY
    // producer (hand-built test modules use it), so a routing defect that never
    // supplied the build stamp would leave `producer ""` and every
    // presence-only assertion would still pass. The CLI path must carry a REAL
    // revision.
    EXPECT_EQ(r.artifact.find("producer \"\"\n"), std::string::npos)
        << "the shipped driver emitted an UNATTRIBUTED artifact:\n" << r.artifact;

    DiagnosticReporter rep;
    auto parsed = parseHir(r.artifact, CompilationUnitId{1}, rep);
    ASSERT_TRUE(parsed->ok);
    EXPECT_FALSE(parsed->producer.empty());
    // The stamp always opens with the project version, whatever git could tell
    // it; asserting the SHAPE rather than the value keeps this green across
    // commits without weakening it to "non-empty".
    EXPECT_TRUE(parsed->producer.starts_with("0.")) << parsed->producer;
}

TEST(EmitHirMode, AMalformedOrAbsentVersionIsRefusedLoudlyOnRead) {
    // The safety case, stated by the consumer: *"a verifier that misreads its
    // input can prove a true property of the wrong program."* Each arm is a way
    // the header can be wrong, and every one of them must ERROR — never warn,
    // never recover into a partial module.
    struct Arm {
        std::string_view name;
        std::string_view text;
    };
    Arm const arms[] = {
        {"a future version", "dsshir 99\nproducer \"x\"\nmodule \"toy\" {\n}\n"},
        {"the superseded v1", "dsshir 1\nsymbols {\n}\nmodule \"toy\" {\n}\n"},
        {"no version at all", "module \"toy\" {\n}\n"},
        {"a non-numeric version", "dsshir vNext\nmodule \"toy\" {\n}\n"},
        {"no producer line", "dsshir 5\nmodule \"toy\" {\n}\n"},
        {"an unquoted producer", "dsshir 5\nproducer 7\nmodule \"toy\" {\n}\n"},
    };
    for (auto const& arm : arms) {
        DiagnosticReporter r;
        auto parsed = parseHir(std::string{arm.text}, CompilationUnitId{1}, r);
        EXPECT_FALSE(parsed->ok) << "accepted a malformed header: " << arm.name;
        EXPECT_GT(r.errorCount(), 0u) << "refused silently: " << arm.name;
    }

    // The CONTROL: the same module with a well-formed v2 header parses. Without
    // it, six failures are equally consistent with "this parser refuses
    // everything".
    DiagnosticReporter ok;
    auto good = parseHir(std::string{"dsshir 5\nproducer \"ctl\"\nmodule \"toy\" {\n}\n"},
                         CompilationUnitId{1}, ok);
    EXPECT_TRUE(good->ok);
    EXPECT_EQ(good->producer, "ctl");
}

// ── Spans, and the coordinate system they are in ───────────────────────────

TEST(EmitHirMode, SpansTravelAndTheirBufferIsNamedAndClassified) {
    auto const r = emitTo("spans", "int id(int v) { return v; }\n");
    ASSERT_EQ(r.rc, 0) << r.err;
    EXPECT_NE(r.artifact.find("@loc(buf "), std::string::npos) << r.artifact;
    EXPECT_NE(r.artifact.find("buffers {"), std::string::npos) << r.artifact;
    // ⚠⚠ THE CLASSIFICATION IS THE LOAD-BEARING HALF, not the name. C is
    // preprocessed, so the buffer the spans index is the preprocessor's
    // SYNTHESIZED text — and that buffer is constructed WITH THE MAIN SOURCE'S
    // NAME. Without the marker, `buf N "spans.c"` beside `@loc(buf N, 443..473)`
    // reads as an offset into a real 27-byte file. The marker is what stops a
    // consumer indexing the wrong text and reporting a property "at" it.
    EXPECT_NE(r.artifact.find(" synthesized from "), std::string::npos)
        << "a preprocessed buffer was published without its marker:\n" << r.artifact;

    DiagnosticReporter rep;
    auto parsed = parseHir(r.artifact, CompilationUnitId{1}, rep);
    ASSERT_TRUE(parsed->ok);
    EXPECT_FALSE(parsed->bufferNames.empty());
    bool sawSynth = false;
    for (auto const& b : parsed->bufferNames) {
        if (b.synthesizedMainOrigin != 0) sawSynth = true;
    }
    EXPECT_TRUE(sawSynth) << "the reader could not tell a synthesized buffer apart";
}

// ── HIR is TARGET-DEPENDENT ─────────────────────────────────────────────────

TEST(EmitHirMode, TheSameSourceEmitsDifferentHirForTwoDataModels) {
    // ★ THIS IS WHY `--emit-hir` TAKES A `--target` AND REFUSES TWO. It is not
    // a spelling difference: under LP64 the narrowing initializer lowers to an
    // explicit `cast` NODE, and under LLP64 `long` and `int` are the same
    // representation so there is NO cast node at all. A consumer handed the
    // wrong leg's artifact would be reading a different tree.
    constexpr std::string_view kSource =
        "long identity(long v) { return v; }\n"
        "int narrow(long v) { int n = v; return n; }\n";
    auto const lp64  = emitTo("widths_lp64", kSource, kTarget);
    auto const llp64 = emitTo("widths_llp64", kSource, kTargetLlp64);
    ASSERT_EQ(lp64.rc, 0) << lp64.err;
    ASSERT_EQ(llp64.rc, 0) << llp64.err;

    EXPECT_NE(lp64.artifact.find("i64 \"long\""), std::string::npos) << lp64.artifact;
    EXPECT_EQ(llp64.artifact.find("i64 \"long\""), std::string::npos) << llp64.artifact;
    EXPECT_NE(llp64.artifact.find("i32 \"long\""), std::string::npos) << llp64.artifact;
    // The structural half — a NODE present on one leg and absent on the other.
    EXPECT_NE(lp64.artifact.find("cast [syn] : i32"), std::string::npos) << lp64.artifact;
    EXPECT_EQ(llp64.artifact.find("cast [syn] : i32"), std::string::npos) << llp64.artifact;

    // Both must still be readable on their own terms — a target-specific
    // artifact is still a self-contained one.
    EXPECT_TRUE(reparse(lp64.artifact).ok);
    EXPECT_TRUE(reparse(llp64.artifact).ok);
}

TEST(EmitHirMode, TwoTargetsAreRefusedAtParseTimeRatherThanResolvedSilently) {
    char const* argv[] = {"dsscp",
                          "--emit-hir", "out.dsshir",
                          "--language", "c",
                          "--target", "x86_64:elf64-x86_64-linux",
                          "--target", "x86_64:pe64-x86_64-windows-exec",
                          "foo.c"};
    auto const parsed = parseCliArgs(static_cast<int>(std::size(argv)),
                                     const_cast<char**>(argv));
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().kind, CliArgsError::AmbiguousEmitHirTarget);
    // The message must say WHY, not merely refuse: the operator's next action
    // (run it twice, to two paths) is only obvious once they know HIR moves with
    // the target.
    EXPECT_NE(parsed.error().detail.find("TARGET-DEPENDENT"), std::string::npos)
        << parsed.error().detail;

    // CONTROL: one target parses, and lands the file list and destination where
    // the driver reads them. Without it, the refusal above is equally consistent
    // with "this spelling never parses".
    char const* ok[] = {"dsscp",
                        "--emit-hir", "out.dsshir",
                        "--language", "c",
                        "--target", "x86_64:elf64-x86_64-linux",
                        "foo.c"};
    auto const good = parseCliArgs(static_cast<int>(std::size(ok)),
                                   const_cast<char**>(ok));
    ASSERT_TRUE(good.has_value()) << "control failed to parse";
    EXPECT_EQ(good->emitHirPath.value_or(""), "out.dsshir");
    ASSERT_EQ(good->emitHirFiles.size(), 1u);
    EXPECT_EQ(good->emitHirFiles[0], "foo.c");
    // And it is a MODE: `--compile` alongside it is a conflict, not a pair.
    char const* both[] = {"dsscp", "--emit-hir", "out.dsshir", "--compile", "foo.c",
                          "--target", "x86_64:elf64-x86_64-linux"};
    auto const clash = parseCliArgs(static_cast<int>(std::size(both)),
                                    const_cast<char**>(both));
    ASSERT_FALSE(clash.has_value());
    EXPECT_EQ(clash.error().kind, CliArgsError::DuplicateModeFlag);
}

// ── Determinism ─────────────────────────────────────────────────────────────

TEST(EmitHirMode, TwoRunsOfOneInputProduceByteIdenticalArtifacts) {
    // Same input + same revision ⇒ same bytes. The claim is worth pinning
    // because the emission passes through several unordered containers on the
    // way out (the symbol pre-pass's map, the driver's buffer collection), and
    // a hash-order leak would show up as a verdict that is not reproducible
    // rather than as a failure.
    constexpr std::string_view kSource =
        "struct S { int a; int b; };\n"
        "int f(struct S s, int k) { int t = s.a + k; return t + s.b; }\n";
    auto const first  = emitTo("determinism", kSource);
    auto const second = emitTo("determinism", kSource);
    ASSERT_EQ(first.rc, 0) << first.err;
    ASSERT_EQ(second.rc, 0) << second.err;
    EXPECT_EQ(first.artifact, second.artifact);
}

// ── stdout ──────────────────────────────────────────────────────────────────

TEST(EmitHirMode, DashWritesTheArtifactToTheGivenStreamAndNoFile) {
    fs::path const src = writeSource("stdout_arm", "int id(int v) { return v; }\n");
    std::ostringstream out;
    std::ostringstream err;
    Program p;
    int const rc = p.emitHirText({src.string()}, "c", std::string{kTarget}, "-",
                                 out, err);
    EXPECT_EQ(rc, 0) << err.str();
    EXPECT_TRUE(out.str().starts_with("dsshir 5\n")) << out.str().substr(0, 64);
    EXPECT_TRUE(err.str().empty()) << err.str();
    // A file literally named `-` is the failure this arm exists to catch.
    EXPECT_FALSE(fs::exists(fs::path{"-"}));
}

// ── The node-kind inventory (`--dump-hir-kinds`) ────────────────────────────

TEST(DumpHirKinds, InventoryNamesEveryCoreKindAndIsAttributed) {
    std::string const text = renderHirKindInventory("test-revision");
    EXPECT_TRUE(text.starts_with("dsshir-kinds 1\n")) << text.substr(0, 64);
    EXPECT_NE(text.find("producer \"test-revision\""), std::string::npos) << text;
    // It reports the format version too, because a consumer generating a
    // build-time coverage table needs to know which artifact grammar these kinds
    // belong to.
    EXPECT_NE(text.find("dsshir-format-version 5\n"), std::string::npos) << text;

    // ★ THE COUNT LINE MUST AGREE WITH THE ROWS. A truncated enumeration is the
    // dangerous failure here: a consumer treats this list as EXHAUSTIVE and
    // refuses anything not on it, so a short list makes them refuse valid
    // programs — or, worse, a list that silently grew shorter makes their
    // coverage claim false without any test noticing.
    std::size_t declared = 0;
    {
        auto const at = text.find("core-kind-count ");
        ASSERT_NE(at, std::string::npos) << text;
        declared = static_cast<std::size_t>(
            std::stoul(text.substr(at + std::string_view{"core-kind-count "}.size())));
    }
    std::size_t rows = 0;
    for (std::size_t at = text.find("\nkind \""); at != std::string::npos;
         at = text.find("\nkind \"", at + 1)) {
        ++rows;
    }
    EXPECT_EQ(rows, declared) << "the inventory's count line disagrees with its rows";
    EXPECT_GT(declared, 40u) << "a collapsed enumeration would pass an equality-only check";

    // Spot the three properties a lifter dispatches on, one row of each shape.
    EXPECT_NE(text.find("kind \"Literal\" position expr keyword \"lit\" typed yes"),
              std::string::npos) << text;
    EXPECT_NE(text.find("kind \"Module\" position non-expr"), std::string::npos) << text;
    EXPECT_NE(text.find("kind \"Ref\" position expr keyword \"ref\" typed yes symbol yes"),
              std::string::npos) << text;
    // The OPEN half is named, so a consumer that covered every core kind still
    // knows an extension can arrive.
    EXPECT_NE(text.find("extension-kind-base 256\n"), std::string::npos) << text;
    // The statement vocabulary is published as a set (it is not a function of
    // the kind — `var`/`param` are one kind, two spellings).
    EXPECT_NE(text.find("stmt-keyword \"var\""), std::string::npos) << text;
    EXPECT_NE(text.find("stmt-keyword \"param\""), std::string::npos) << text;
}

TEST(DumpHirKinds, TheFlagIsAModeThatNeedsNeitherLanguageNorTarget) {
    char const* argv[] = {"dsscp", "--dump-hir-kinds"};
    auto const parsed = parseCliArgs(static_cast<int>(std::size(argv)),
                                     const_cast<char**>(argv));
    ASSERT_TRUE(parsed.has_value()) << parsed.error().detail;
    EXPECT_TRUE(parsed->dumpHirKinds);
    EXPECT_TRUE(parsed->targets.empty());
    EXPECT_TRUE(parsed->languageName.empty());

    // And it is exclusive with the other modes, like every mode flag.
    char const* clash[] = {"dsscp", "--dump-hir-kinds", "--compile", "foo.c",
                           "--target", "x86_64:elf64-x86_64-linux"};
    auto const bad = parseCliArgs(static_cast<int>(std::size(clash)),
                                  const_cast<char**>(clash));
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().kind, CliArgsError::DuplicateModeFlag);
}

// ══ THE rc-0 PROMISE IS NOW MEASURED, AND HERE IS THE INSTRUMENT FIRING ══════
//
// ★★★ §2.1 OF THE CONSUMER DOC HAS ALWAYS SOLD rc 0 AS *"the artifact is
// written, complete, AND PARSES BACK"*. Nothing measured the third clause, and
// the clause was FALSE on the day it was written: three shipped writer
// spellings could not be read back by the shipped reader
// (D-HIR-TEXT-NODE-WALK-RECURSES-PER-LEVEL-ON-BOTH-HALVES-AND-THREE-SPELLINGS-DO-NOT-READ-BACK),
// and every one of them exited 0. `hirArtifactRoundTripFailure` is the
// predicate that now stands behind the promise, and `Program::emitHirText`
// refuses with rc 2 rather than writing an artifact that fails it.
//
// ⚠ THE ARMS BELOW ARE THE PROOF THAT THE PREDICATE CAN FAIL. A round-trip
// check never shown to refuse anything is indistinguishable from one that
// returns "clean" unconditionally — which is exactly why the check takes an
// ARTIFACT rather than living inside the mode: a test can hand it a synthetic
// one carrying an unreadable spelling, and no compiler defect is needed to
// watch it fire.

TEST(HirArtifactRoundTrip, ControlARealArtifactFromTheShippedModeRoundTripsCleanly) {
    // THE CONTROL, NAMED AND FIRST. Every refusal arm below is only meaningful
    // if the predicate accepts what the shipped writer actually produces — a
    // predicate that refused everything would satisfy all three of them.
    auto const r = emitTo("roundtrip_control",
                          "struct P { int x; int y; };\n"
                          "int sum(struct P *p) { return p->x + p->y; }\n"
                          "int main(void) { struct P p = {1, 2}; return sum(&p); }\n");
    ASSERT_EQ(r.rc, 0) << r.err;
    ASSERT_FALSE(r.artifact.empty());
    EXPECT_EQ(hirArtifactRoundTripFailure(r.artifact), "")
        << "the shipped mode wrote an artifact its own reader cannot take back";
}

TEST(HirArtifactRoundTrip, AnArtifactCarryingATokenTheGrammarHasNoRuleForIsRefused) {
    // The `goto *<expr>` class: the writer spelled a byte the reader's lexer
    // had no token for at all, and four shipped artifacts were refused by name.
    // Reproduced synthetically, because that particular hole is now closed.
    std::string const bad =
        "dsshir 5\n"
        "producer \"planted\"\n"
        "module \"C\" {\n"
        "  \x01\x02 not a production this grammar has\n"
        "}\n";
    std::string const why = hirArtifactRoundTripFailure(bad);
    ASSERT_FALSE(why.empty()) << "an unreadable artifact was accepted";
    EXPECT_NE(why.find("did NOT parse back"), std::string::npos) << why;
}

TEST(HirArtifactRoundTrip,
     AnArtifactThatParsesWithAnEmptyReporterIsStillRefusedWhenItReEmitsDifferently) {
    // ★★★ THE ARM THAT JUSTIFIES THE WHOLE DESIGN, AND THE ONE THE CHECK THAT
    // ALREADY EXISTED WOULD HAVE PASSED. The historical
    // `lit float 18446744073709551616` parsed CLEAN — `HirParseResult::ok`
    // true, reporter EMPTY — and handed back 0.0. Its shape is *a literal whose
    // spelling carries a value the reader cannot preserve*, and that shape is
    // reproducible with no compiler defect at all: a decimal with more
    // significant digits than a double can hold parses to exactly 1.0, and this
    // writer prints 1.0 as `1`.
    //
    // So this arm plants that shape and asserts BOTH halves:
    //   (a) the parse is SILENT — `ok` true, reporter empty — which is
    //       precisely what a "does it parse?" consumer calls success;
    //   (b) the round-trip predicate refuses it anyway, on the BYTES.
    // (a) is not decoration. Without it this arm would merely say "a bad
    // artifact is refused", where the claim actually being made is the sharper
    // one: byte identity sees a defect a clean parse cannot.
    auto const r = emitTo("roundtrip_float_value",
                          "double one(void) { return 1.0; }\n");
    ASSERT_EQ(r.rc, 0) << r.err;
    std::string const needle = "lit float 1 ";
    ASSERT_NE(r.artifact.find(needle), std::string::npos)
        << "premise broken: this arm needs the writer to print 1.0 as `1`, so "
           "that a longer spelling of the same value is a BYTE difference:\n"
        << r.artifact;
    std::string planted = r.artifact;
    planted.replace(planted.find(needle), needle.size(),
                    "lit float 1.00000000000000000000001 ");

    // (a) THE READER IS PERFECTLY HAPPY WITH IT.
    DiagnosticReporter rep;
    auto const parsed = parseHir(planted, CompilationUnitId{1}, rep);
    ASSERT_TRUE(parsed->ok)
        << "premise broken: this arm needs a spelling that parses CLEANLY";
    EXPECT_TRUE(rep.all().empty())
        << "premise broken: the parse reported something, so the weaker "
           "'does it parse?' check would have caught this and the byte compare "
           "would not be carrying the claim this arm makes";

    // (b) AND THE BYTE COMPARE CATCHES IT ANYWAY.
    std::string const why = hirArtifactRoundTripFailure(planted);
    ASSERT_FALSE(why.empty())
        << "a value the reader could not preserve was accepted — this is "
           "exactly the silent half of the defect class this check exists for";
    EXPECT_NE(why.find("re-emits"), std::string::npos) << why;
}

TEST(HirArtifactRoundTrip, TheModeAsksThePredicateAndRefusesWithItsOwnExitCode) {
    // ⚠⚠ A SEAM GUARD, AND IT IS HERE BECAUSE THE SEAM IS OTHERWISE
    // UNOBSERVABLE. `emitHirText` reaches its refusal path only for a program
    // whose HIR this build cannot serialize — and while the codec is correct
    // there is no such program, so deleting the call would leave every
    // behavioural arm in this repository GREEN. That is the argument
    // `test_synth_verify_seam_guard.cpp` makes for the synthesis verifier, and
    // this is the same answer: pin the CALL structurally.
    auto const root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();
    fs::path const p = *root / "src" / "program" / "program.cpp";
    std::ifstream in{p, std::ios::binary};
    ASSERT_TRUE(in.good())
        << "cannot read " << p.string()
        << " — this guard's subject IS the source tree, so an unreadable file "
           "is a red, never a skip";
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string const code = buf.str();

    // The mode's body, delimited by its own signature and the next definition.
    std::size_t const from = code.find("int Program::emitHirText(");
    ASSERT_NE(from, std::string::npos)
        << "Program::emitHirText is gone or renamed; this guard needs updating "
           "rather than deleting";
    std::size_t const to = code.find("\nint Program::", from + 1);
    ASSERT_NE(to, std::string::npos);
    std::string const body = code.substr(from, to - from);

    // The CALL — spelled with its open paren, which the prose inside that body
    // deliberately does not use, so a comment cannot satisfy this guard.
    EXPECT_NE(body.find("hirArtifactRoundTripFailure("), std::string::npos)
        << "`--emit-hir` no longer asks whether its own artifact reads back. "
           "docs/hir-text-format.md 2.1 still promises a consumer that rc 0 "
           "means it does, and nothing else in this suite can notice, because "
           "a correct codec produces no artifact that would fail.";
    EXPECT_NE(body.find("return 2;"), std::string::npos)
        << "the round-trip refusal no longer has its own exit code, so neither "
           "a consumer nor this repo's corpus gate can tell a rejected SOURCE "
           "from an artifact this compiler could not read back";
}
