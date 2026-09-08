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

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
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

// A scratch directory unique to this binary, under the system temp root. It is
// NOT the repository root: a bare output filename would land in whatever
// directory the runner happened to start in, which on this project has twice
// meant probe artifacts committed into a source tree.
[[nodiscard]] fs::path scratchDir() {
    fs::path const d = fs::temp_directory_path() / "dss_emit_hir_mode_test";
    fs::create_directories(d);
    return d;
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

TEST(EmitHirMode, StructDefinitionTravelsWithEveryUseOfIt) {
    auto const r = emitTo("structs",
                          "struct Point { int x; int y; };\n"
                          "int sum_fields(struct Point *p) { return p->x + p->y; }\n");
    ASSERT_EQ(r.rc, 0) << r.err;

    // The FIELD TYPES are in the text, not an id pointing at an interner this
    // process happens to still hold. `struct "Point"` alone would be an opaque
    // name — the `{i32, i32}` is the claim.
    EXPECT_NE(r.artifact.find("struct \"Point\" {i32, i32}"), std::string::npos)
        << r.artifact;
    // And it travels THROUGH the pointer, at the use site, not only at the
    // declaration: a reader that met `ptr<struct "Point">` with no body could
    // not state a property about `p->x`.
    EXPECT_NE(r.artifact.find("ptr<struct \"Point\" {i32, i32}>"), std::string::npos)
        << r.artifact;

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
    // ★★★ v3 CLOSED THE FORMAT'S ONE SELF-CONTAINMENT HOLE. Until v3 a TU
    // containing `struct Node { struct Node *next; }` — the ordinary linked list,
    // and with it every tree, intrusive container and parent pointer in real C —
    // exited non-zero with no file. The type graph is CYCLIC and the grammar had
    // no back-reference form, so the writer emitted the poison `?` and an Error.
    //
    // `rec <H>` is that form, and this arm is the END-TO-END half: not the codec
    // in isolation but the shipped CLI over real C source, through tokenize,
    // preprocess, parse, semantic analysis and HIR lowering. All three shapes the
    // consumer actually has are here — self-reference, a MUTUALLY recursive pair,
    // and a self-reference reached through a TYPEDEF (where the name at the use
    // site is not the name on the definition).
    struct Arm { char const* name; char const* src; char const* expect; };
    std::array<Arm, 3> const arms{{
        {"self", "struct Node { int v; struct Node *next; };\n"
                 "int head_value(struct Node *n) { return n->v; }\n",
                 "struct \"Node\" rec 1 {i32, ptr<rec 1>}"},
        {"mutual", "struct B;\n"
                   "struct A { int x; struct B *b; };\n"
                   "struct B { int y; struct A *a; };\n"
                   "int ax(struct A *p) { return p->x; }\n"
                   "int by(struct B *p) { return p->y; }\n",
                   "struct \"A\" rec 1 {i32, ptr<struct \"B\" rec 2 {i32, ptr<rec 1>}>}"},
        {"typedef", "typedef struct TNode TNode;\n"
                    "struct TNode { int v; TNode *left; TNode *right; };\n"
                    "int tv(TNode *t) { return t->v; }\n",
                    "struct \"TNode\" rec 1 {i32, ptr<rec 1>, ptr<rec 1>}"},
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

    // CONTROL: an ACYCLIC composite carries NO handle, so the marker is a
    // property of the type graph and not of "being a struct" — without this,
    // "the handle appeared" is equally consistent with the writer stamping one
    // on everything.
    auto const control = emitTo("cyclic_control",
                                "struct Leaf { int v; int *p; };\n"
                                "int leaf_value(struct Leaf *n) { return n->v; }\n");
    EXPECT_EQ(control.rc, 0) << control.err;
    EXPECT_NE(control.artifact.find("struct \"Leaf\" {i32, ptr<i32>}"), std::string::npos)
        << control.artifact;
    EXPECT_EQ(control.artifact.find(" rec "), std::string::npos) << control.artifact;
    EXPECT_TRUE(reparse(control.artifact).ok);
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
    EXPECT_TRUE(r.artifact.starts_with("dsshir 3\n")) << r.artifact.substr(0, 64);
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
        {"no producer line", "dsshir 3\nmodule \"toy\" {\n}\n"},
        {"an unquoted producer", "dsshir 3\nproducer 7\nmodule \"toy\" {\n}\n"},
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
    auto good = parseHir(std::string{"dsshir 3\nproducer \"ctl\"\nmodule \"toy\" {\n}\n"},
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
    EXPECT_TRUE(out.str().starts_with("dsshir 3\n")) << out.str().substr(0, 64);
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
    EXPECT_NE(text.find("dsshir-format-version 3\n"), std::string::npos) << text;

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
