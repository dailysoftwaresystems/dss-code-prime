// ★★★ THE STANDING GUARD FOR THE CONTENT-ADDRESSED CONFIG MEMO
// (D-CONFIG-A-SCHEMA-DOCUMENT-IS-REBUILT-ONCE-PER-LOAD-INSIDE-ONE-PROCESS).
//
// THE PROPERTY UNDER TEST: loading the same document bytes twice inside one
// process costs one build, and loading DIFFERENT bytes — including bytes in a
// document the host merely REFERENCES — never reuses the first build.
//
// ★★ WHY THE STAKES ARE A SILENT MISCOMPILE RATHER THAN A SLOW BUILD. A
// `GrammarSchema` is the compiler's whole front-end decision surface: which
// lexemes exist, which shapes parse, which alternative wins. A memo that
// served a schema built from SUPERSEDED bytes would compile the user's source
// against a grammar that is not the one on disk, produce a plausible artifact,
// and turn nothing red. That is why the arms below are differentials over ONE
// variable each, and why the two failure directions are tested separately: an
// over-eager memo (serving a stale schema) and an under-eager one (never
// hitting, so the mechanism is a no-op wearing a green suit).
//
// ★★ RED-ON-DISABLE IS BY CONSTRUCTION, NOT BY ARRANGEMENT. `MemoHits*` goes
// red the moment the memo stops hitting, and `...Invalidates...` goes red the
// moment it hits when it must not. Neither depends on a flag, a build option, a
// mutated shipped file or a spawned subprocess: both arms run in this process,
// on documents this file builds, on the very same run.
//
// ⚠ WHAT THIS FILE DELIBERATELY DOES NOT DO: it never edits a shipped config.
// The referenced-document arm COPIES the one fragment it needs into a scratch
// tree and points `$DSS_CONFIG_ROOT` at the copy, so a concurrent suite reading
// the live tree cannot see anything this test does.

#include "core/types/config_document_memo.hpp"
#include "core/types/grammar_schema.hpp"

#include "repo_root.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace dss;
using dss::detail::ConfigDocumentMemo;

namespace {

// ── The synthetic subject ───────────────────────────────────────────────────
//
// A COMPLETE, self-contained language whose every spelling is alien to every
// shipped one, so nothing here can pass by coinciding with `c`. The single
// varying knob is `marker`, which changes the document's BYTES and — because it
// is a real keyword spelling — also changes an OBSERVABLE property of the
// schema. Both halves matter: a test that varied only bytes could not tell a
// working memo from one that returns the wrong schema whose difference happens
// to be invisible.
[[nodiscard]] std::string makeDoc(std::string_view marker) {
    std::string doc = R"({
  "dssSchemaVersion": 4,
  "language": { "name": "MemoProbe", "version": "0.0.1" },
  "tokens": {
    " ": [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "!": [{ "kind": "BangEnd" }]
  },
  "keywords": [
    { "word": ")";
    doc += marker;
    doc += R"(", "kind": "MarkerWord" }
  ],
  "shapes": {
    "root":      { "sequence": [{ "repeat": "probeStmt" }] },
    "probeStmt": { "sequence": ["MarkerWord", "Identifier", "BangEnd"] }
  }
})";
    return doc;
}

[[nodiscard]] std::string firstError(std::vector<ConfigDiagnostic> const& d) {
    return d.empty() ? std::string{"<no diagnostics>"} : d.front().message;
}

// Load, failing the CALLING test with the loader's own words rather than a
// bare null if the document does not load.
[[nodiscard]] std::shared_ptr<GrammarSchema> loadOk(std::string const& doc,
                                                    std::string_view label) {
    auto result = GrammarSchema::loadFromText(doc, label);
    if (!result.has_value()) {
        ADD_FAILURE() << "probe document failed to load under label '" << label
                      << "': " << firstError(result.error());
        return nullptr;
    }
    return *result;
}

// ── The observable projection ───────────────────────────────────────────────
//
// ★★ THIS IS THE ARM THAT MAKES THE MEMO SOUND, and it is a claim about the
// LOADER, not about the memo: returning a previously built instance is correct
// only if rebuilding from the same bytes would have produced an
// indistinguishable one. So the projection is taken over the surface the
// PARSER actually consults — every rule's FIRST / FOLLOW / nullability /
// predictive prefix, and every position's slot kind, expected set, alt-branch
// enumeration, nullable tail and speculation attributes — rather than over a
// summary count, which could agree while the tables underneath disagreed.
//
// ⚠ It walks the interner rather than a hand-listed set of rule names, so a
// document that grows a rule is covered without this file being edited.
[[nodiscard]] std::string project(GrammarSchema const& s) {
    std::string out;
    auto const num = [&out](auto v) {
        out += std::to_string(static_cast<long long>(v));
        out += ',';
    };
    out += "name=";
    out += s.name();
    out += ";version=";
    out += s.version();
    out += ";digest=";
    out += s.contentDigest();
    out += ";maxLexeme=";
    num(s.rules().size());
    num(s.schemaTokens().size());

    for (std::uint32_t v = 0; v < s.rules().size() + 1; ++v) {
        const RuleId rule{v};
        out += "\nR";
        num(v);
        out += s.isNullable(rule) ? "n" : "-";
        out += s.isExprRule(rule) ? "e" : "-";
        out += s.commitAfterPrefix(rule) ? "c" : "-";
        num(s.exprAtom(rule).v);
        num(s.exprMinPrecedence(rule));
        num(s.typeNameCommitRule(rule).v);
        num(static_cast<int>(s.typeNameCommitPolarity(rule)));
        out += "F";
        for (auto const t : s.firstSetOf(rule)) num(t.v);
        out += "L";
        for (auto const t : s.followSetOf(rule)) num(t.v);
        out += "P";
        for (std::size_t i = 0; i < s.predictivePrefixLen(rule); ++i) {
            out += '(';
            for (auto const t : s.predictivePrefixAt(rule, i)) num(t.v);
            out += ')';
        }
        // Positions are reachable only through cursors, so the walk enters the
        // rule and follows every branch edge the schema exposes.
        auto cur = s.enterRule(rule);
        for (std::uint32_t pos = 0; pos < 4096 && cur.valid(); ++pos) {
            out += "\n  p";
            num(cur.posId());
            num(static_cast<int>(s.slotKind(cur)));
            num(s.slotRuleRef(cur).v);
            num(s.lookahead(cur));
            out += s.isAtEndOfRule(cur) ? "E" : "-";
            out += s.nullableTail(cur) ? "T" : "-";
            out += s.isSpeculativeAlt(cur) ? "S" : "-";
            out += s.canEndSource(cur) ? "$" : "-";
            out += "X";
            for (auto const t : s.expectedSet(cur)) num(t.v);
            out += "A";
            for (auto const r : s.altRuleBranches(cur)) num(r.v);
            out += "N";
            num(s.nullableBranch(cur).posId());
            // Advance through whichever token this position admits, so the walk
            // covers the body rather than stopping at the entry.
            SchemaCursor next{};
            for (auto const t : s.expectedSet(cur)) {
                next = s.advance(cur, t);
                if (next.valid()) break;
            }
            if (!next.valid()) next = s.leaveRule(cur);
            cur = next;
        }
    }
    return out;
}

// ── The referenced-document fixture ─────────────────────────────────────────
//
// A host that reaches the SHIPPED `asm` grammar through `languageReferences`,
// but resolves it out of a scratch `src/dss-config/sources/` tree holding a
// COPY of that fragment. The copy is what the invalidation arm edits.
constexpr std::string_view kAsmHostHead = R"({
  "dssSchemaVersion": 4,
  "language": { "name": "MemoAsmHost", "version": "0.0.1" },
  "languageReferences": {
    "asm": {
      "entry": "asmStmt",
      "bindRules": { "operandExpr": "hostValue", "templateText": "hostPayload" },
      "bindTokens": {
        "asmKeyword": "EmitWord", "volatileQualifier": "LoudWord",
        "inlineQualifier": "TightWord", "gotoQualifier": "LeapWord",
        "sectionSeparator": "PipeMark", "sectionSeparatorFused": "PipePipeMark",
        "operandSeparator": "AmpMark", "symbolName": "Identifier",
        "argsOpen": "AngleOpen", "argsClose": "AngleClose",
        "symbolicNameOpen": "BraceOpen", "symbolicNameClose": "BraceClose",
        "statementEnd": "BangEnd"
      }
    }
  },
  "tokens": {
    " ":  [{ "kind": "Whitespace", "flags": ["EmptySpace"] }],
    "||": [{ "kind": "PipePipeMark", "priority": 20 }],
    "|":  [{ "kind": "PipeMark",     "priority": 10 }],
    "&":  [{ "kind": "AmpMark" }],
    "<<": [{ "kind": "AngleOpen" }],
    ">>": [{ "kind": "AngleClose" }],
    "{":  [{ "kind": "BraceOpen" }],
    "}":  [{ "kind": "BraceClose" }],
    "!":  [{ "kind": "BangEnd" }],
    "`":  [{ "kind": "TickText" }]
  },
  "keywords": [
    { "word": "EMIT",  "kind": "EmitWord" },
    { "word": "LOUD",  "kind": "LoudWord" },
    { "word": "TIGHT", "kind": "TightWord" },
    { "word": "LEAP",  "kind": "LeapWord" }
  ],
  "shapes": {
    "root":          { "sequence": [{ "repeat": "hostStmt" }] },
    "hostStmt":      { "alt": ["asmStmt", "hostPlainStmt"] },
    "hostPlainStmt": { "sequence": ["hostValue", "BangEnd"] },
    "hostValue":     { "sequence": ["Identifier"] },
    "hostPayload":   { "sequence": ["TickText"] }
  }
})";

[[nodiscard]] std::string readWhole(std::filesystem::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

void writeWhole(std::filesystem::path const& p, std::string_view bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. THE MEMO HITS — the mechanism is not a no-op.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ConfigDocumentMemo, SameBytesSameLabelReturnTheSameInstance) {
    ConfigDocumentMemo<GrammarSchema>::clear();
    const std::string doc = makeDoc("EMIT");

    auto const first = loadOk(doc, "<memo-probe-hit>");
    ASSERT_NE(first, nullptr);
    auto const second = loadOk(doc, "<memo-probe-hit>");
    ASSERT_NE(second, nullptr);

    // ★ POINTER identity, not value equality: the whole point is that the
    // second load did not run the builder at all. A value-equality assertion
    // would pass just as happily against a memo that never hits, which is the
    // failure this arm exists to catch.
    EXPECT_EQ(first.get(), second.get())
        << "the second load of identical bytes under an identical label "
           "rebuilt the schema — the content-addressed memo did not hit, so "
           "every repeat load in this process is paying a full rebuild";
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. A CHANGED DOCUMENT IS A DIFFERENT SCHEMA — the invalidation half.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ConfigDocumentMemo, ChangedBytesYieldADifferentSchema) {
    ConfigDocumentMemo<GrammarSchema>::clear();
    const std::string before = makeDoc("EMIT");
    const std::string after  = makeDoc("EMIT2");
    ASSERT_NE(before, after);

    auto const a = loadOk(before, "<memo-probe-invalidate>");
    ASSERT_NE(a, nullptr);
    auto const b = loadOk(after, "<memo-probe-invalidate>");
    ASSERT_NE(b, nullptr);

    EXPECT_NE(a.get(), b.get()) << "edited bytes addressed the OLD entry";
    EXPECT_NE(a->contentDigest(), b->contentDigest());

    // ⚠ Pointer inequality alone would be satisfied by a memo that never
    // stores. The OBSERVABLE difference is what proves the second schema is
    // the one the NEW bytes describe: the keyword's spelling moved, so the old
    // spelling must no longer resolve and the new one must.
    EXPECT_FALSE(a->lookupLexeme("EMIT").empty());
    EXPECT_TRUE(a->lookupLexeme("EMIT2").empty());
    EXPECT_TRUE(b->lookupLexeme("EMIT").empty());
    EXPECT_FALSE(b->lookupLexeme("EMIT2").empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. THE LABEL IS PART OF THE KEY.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ConfigDocumentMemo, SameBytesUnderADifferentLabelDoNotShare) {
    ConfigDocumentMemo<GrammarSchema>::clear();
    const std::string doc = makeDoc("EMIT");

    auto const viaA = loadOk(doc, "<memo-probe-label-a>");
    auto const viaB = loadOk(doc, "<memo-probe-label-b>");
    ASSERT_NE(viaA, nullptr);
    ASSERT_NE(viaB, nullptr);

    // A loader writes its `sourceLabel` into the diagnostics and the per-shape
    // origin map it carries, so two labels are two legitimately different
    // results. Sharing them would make the memo observable, which is exactly
    // what a transparent one must never be.
    EXPECT_NE(viaA.get(), viaB.get());
    EXPECT_EQ(viaA->contentDigest(), viaB->contentDigest());
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. A REBUILD FROM THE SAME BYTES IS INDISTINGUISHABLE — the soundness proof.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ConfigDocumentMemo, ARebuiltSchemaIsIndistinguishableFromTheMemoizedOne) {
    const std::string doc = makeDoc("EMIT");

    ConfigDocumentMemo<GrammarSchema>::clear();
    auto const cold = loadOk(doc, "<memo-probe-equivalence>");
    ASSERT_NE(cold, nullptr);
    const std::string coldProjection = project(*cold);

    // A SECOND cold build of the identical bytes. Clearing the memo first is
    // what makes this a rebuild rather than a second look at the same object.
    ConfigDocumentMemo<GrammarSchema>::clear();
    auto const rebuilt = loadOk(doc, "<memo-probe-equivalence>");
    ASSERT_NE(rebuilt, nullptr);
    ASSERT_NE(cold.get(), rebuilt.get())
        << "clear() did not take effect, so this arm compared one object with "
           "itself and proved nothing";

    EXPECT_EQ(coldProjection, project(*rebuilt))
        << "two builds of identical bytes disagree on the parser's own "
           "decision surface — the memo's soundness rests on this equality, so "
           "a difference here is a defect in the LOADER, not in the memo";
    EXPECT_FALSE(coldProjection.empty());

    // And the warm path returns that same, already-proven object.
    auto const warm = loadOk(doc, "<memo-probe-equivalence>");
    ASSERT_NE(warm, nullptr);
    EXPECT_EQ(rebuilt.get(), warm.get());
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. CONCURRENCY — many threads, one document, no torn entry.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ConfigDocumentMemo, ConcurrentLoadsOfOneDocumentAgree) {
    ConfigDocumentMemo<GrammarSchema>::clear();
    const std::string doc = makeDoc("EMIT");

    constexpr int kThreads = 16;
    std::vector<std::shared_ptr<GrammarSchema>> results(kThreads);
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&, i] {
            while (!go.load(std::memory_order_acquire)) { /* line them up */ }
            auto r = GrammarSchema::loadFromText(doc, "<memo-probe-threads>");
            if (r.has_value()) results[static_cast<std::size_t>(i)] = *r;
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();

    // ★ A racing pair may BOTH build — that is allowed and costs only work.
    // What is NOT allowed is a null, a half-built schema, or two schemas that
    // disagree, so those are what the arm checks.
    const std::string expected = project(*loadOk(doc, "<memo-probe-threads>"));
    for (int i = 0; i < kThreads; ++i) {
        auto const& r = results[static_cast<std::size_t>(i)];
        ASSERT_NE(r, nullptr) << "thread " << i << " got no schema";
        EXPECT_EQ(project(*r), expected) << "thread " << i
            << " observed a schema that differs from the settled one";
    }

    // Once the race is over the memo has settled on ONE entry, and every
    // subsequent load must reach it.
    auto const afterA = loadOk(doc, "<memo-probe-threads>");
    auto const afterB = loadOk(doc, "<memo-probe-threads>");
    ASSERT_NE(afterA, nullptr);
    EXPECT_EQ(afterA.get(), afterB.get());
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. THE HALF A HOST DIGEST CANNOT SEE — an edited REFERENCED document.
// ─────────────────────────────────────────────────────────────────────────────
//
// ★★★ THE ARM THIS WHOLE FILE EXISTS FOR. The host's bytes never change here,
// so its SHA-256 never changes, so a memo keyed on the host digest alone would
// hit — and hand back a grammar built from the SUPERSEDED fragment. Nothing
// downstream could tell.
TEST(ConfigDocumentMemo, AnEditedReferencedDocumentInvalidatesTheHost) {
    auto const shippedConfig = dss::test::findConfigRoot();
    ASSERT_TRUE(shippedConfig.has_value()) << dss::test::repoRootDiagnostic();
    auto const shippedAsm = *shippedConfig / "sources" / "asm.lang.json";
    ASSERT_TRUE(std::filesystem::exists(shippedAsm))
        << "the referenced fragment this arm copies is missing: " << shippedAsm;

    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp, "config-memo"};
    auto const sources = scratch.path() / "src" / "dss-config" / "sources";
    std::error_code ec;
    std::filesystem::create_directories(sources, ec);
    ASSERT_FALSE(ec) << "could not build the scratch config tree: " << ec.message();

    auto const fragment = sources / "asm.lang.json";
    const std::string original = readWhole(shippedAsm);
    ASSERT_FALSE(original.empty());
    writeWhole(fragment, original);

    // ⚠ The override, not the cwd walk: this arm must resolve the COPY, and a
    // walk that found the live tree would make it silently vacuous.
    dss::test_support::ScopedEnv const rootOverride{"DSS_CONFIG_ROOT",
                                            scratch.path().string()};

    ConfigDocumentMemo<GrammarSchema>::clear();
    const std::string host = std::string{kAsmHostHead};

    auto const before = loadOk(host, "<memo-probe-reference>");
    ASSERT_NE(before, nullptr);

    // The ledger is what the invalidation rides on, so it is asserted directly
    // rather than inferred from the behaviour it produces.
    ASSERT_EQ(before->referencedDocuments().size(), 1u)
        << "the host merged a document but recorded no dependency — the memo "
           "has nothing to re-check and would serve a stale fragment";
    EXPECT_EQ(std::filesystem::path{before->referencedDocuments()[0].path},
              fragment);
    EXPECT_EQ(before->referencedDocuments()[0].digest.size(), 64u);

    // THE CONTROL, and without it this arm cannot tell invalidation from a
    // memo that simply never hits: with the fragment UNTOUCHED, the host must
    // still hit.
    auto const control = loadOk(host, "<memo-probe-reference>");
    ASSERT_NE(control, nullptr);
    ASSERT_EQ(before.get(), control.get())
        << "the host did not hit even with nothing edited, so the invalidation "
           "observed below would prove nothing";

    // ── the one variable: the FRAGMENT's bytes, never the host's ──
    std::string edited = original;
    edited.insert(edited.find_last_of('}'), ",\n  \"$memoProbeMarker\": \"1\"\n");
    ASSERT_NE(edited, original);
    writeWhole(fragment, edited);

    auto const after = loadOk(host, "<memo-probe-reference>");
    ASSERT_NE(after, nullptr);
    EXPECT_NE(before.get(), after.get())
        << "the host's own bytes did not change, so its digest did not change, "
           "and the memo served a schema built from the SUPERSEDED fragment — "
           "a silent miscompile, not a stale cache entry";
    EXPECT_NE(before->referencedDocuments()[0].digest,
              after->referencedDocuments()[0].digest)
        << "the rebuilt schema recorded the OLD fragment digest";

    // And the host document itself really was byte-identical across the two
    // loads, so the difference above can only have come from the fragment.
    EXPECT_EQ(before->contentDigest(), after->contentDigest());
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. AN UNREADABLE DEPENDENCY IS A MISS, NEVER A HIT.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ConfigDocumentMemo, ADependencyThatCannotBeReadIsRefused) {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp, "config-memo-gone"};
    auto const missing = scratch.path() / "not-here.lang.json";
    ASSERT_FALSE(std::filesystem::exists(missing));

    EXPECT_FALSE(dss::detail::digestConfigDocumentFile(missing.string()).has_value())
        << "an unreadable dependency digested to SOMETHING, so a lookup would "
           "compare that value and could match";

    auto const present = scratch.path() / "present.lang.json";
    writeWhole(present, "abc");
    auto const digest = dss::detail::digestConfigDocumentFile(present.string());
    ASSERT_TRUE(digest.has_value());
    // SHA-256("abc"), FIPS 180-4 — a literal oracle, not our own output.
    EXPECT_EQ(*digest,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}
