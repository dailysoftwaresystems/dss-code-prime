// D-MIR-SYNTH-PASSES-UNVERIFIED-ON-SINGLE-CU-PATH — the ORDER pin, and the only
// half of that closure no behavioural test can observe.
//
// ── WHAT IS PINNED ──────────────────────────────────────────────────────────
// The post-synthesis MIR verify is worth exactly as much as its POSITION. Both
// driver seams run `synthesizeSehFunclets` LAST; a verify one statement earlier
// covers every synthesis pass except the one that rewrites the most — the SEH
// parent policy relayouts every guarded region's blocks and appends one funclet
// function per region. That is where this row's residue lived: the single-CU
// seam had a verify placed deliberately IN FRONT of the SEH pass, and the merge
// seam had none at all after `optimizeModule` (which itself runs before it).
//
// ── ⚠ WHY THIS CANNOT BE A BEHAVIOURAL TEST ─────────────────────────────────
// Once `synthesizeSehFunclets` re-derives its own `StructCfMarker`s — which it
// now does, like its siblings — a CORRECT compiler emits no diagnostic whether
// the verify runs after the pass, before it, or not at all. The placement only
// becomes observable on a module that is already broken, i.e. on the NEXT
// defect, which by definition has not been written yet. So the property that
// must not regress is structural: at each seam the verify call FOLLOWS the SEH
// synthesis call. That is a fact about the source text, and this guard reads the
// source text. (`SynthSehFunclets.RelayoutLeavesStructCfMarkersCanonical` in
// tests/mir is the behavioural half of the PASS's duty, and
// `program/test_synth_verify_failure_path` is the behavioural half of what the
// verify itself does when a module is broken; the three together are the
// closure.)
//
// ── HOW IT AVOIDS BEING VACUOUS ─────────────────────────────────────────────
// Both sites are matched on the C++ CALL form `!<name>(` — never on the bare
// identifier, which appears throughout both drivers in prose. And every match
// runs over a COMMENT- AND LITERAL-BLANKED copy of the file (`blankNonCode`), so
// an occurrence that survives only inside a `//` line, inside a `/* … */` block
// or inside a string literal is NOT a call. Each name must then appear EXACTLY
// ONCE per file, so a deleted call reds on the count, a COMMENTED-OUT call reds
// on the count, a duplicated one reds on the count, and a call moved back in
// front of the SEH pass reds on the order. Those are the four ways this row
// reopens, and all four fail toward RED. An unreadable or empty file is a RED
// too, never a silent pass — a guard whose subject it cannot read has measured
// nothing.
//
// ⚠ THE FOURTH WAY IS HERE BECAUSE THE GUARD SHIPPED VACUOUS AGAINST IT.
// The first spelling of this guard matched the raw file text and asserted, in
// this very section, that its THREE ways were the ways. ✔MEASURED by cycle P63's
// independent review: comment the single-CU call out as
// `// if (!verifySynthesizedModule(cuMir.mir, …))` — the driver then does not
// verify behind the SEH pass at all — and this guard reported `2/2 passed`. A
// `//` prefix does not disturb a substring match, and commenting a call out is
// the commonest way a call is removed by hand, so the one removal the guard
// could not see was the likeliest one. A leading-`//` blacklist was deliberately
// NOT the fix: it leaves the indented form, the trailing-comment form and the
// `/* … */` form alive. The count of ways is restated above because a claim that
// enumerates its own coverage must be widened by the same change that widens the
// coverage — otherwise the next reader trusts the old number.
// `BlankNonCodeSeesOnlyRealCalls` pins the blanking rule itself on a synthetic
// input, so the mechanism is a measured fact and not an argument.
//
// ── ⚠ THE ROW'S PRESCRIBED RED-ON-DISABLE LEVER WAS DECLINED, AND WHY ───────
// [[D-MIR-SYNTH-PASSES-UNVERIFIED-ON-SINGLE-CU-PATH]] prescribed its own
// red-on-disable arm: hand-build a wrong-arity call in the SEH pass behind a
// test-only lever, assert the verifier names it, remove the verify and require
// the test to go green. That lever was NOT built, and the omission is a decision
// rather than an oversight, recorded here because the next reader diffing the
// row against the delivered pins would otherwise find a silent mismatch. It is
// an ADD-direction mutant — it injects a synthetic defect — and an ADD-direction
// mutant stays GREEN in exactly the case that matters, when the real pass loses
// the property on its own; it also asks for fault-injection scaffolding inside
// production source, for which this repository has no precedent and which would
// ship a lever a user's build can reach. What replaced it: this source-order
// guard for the PLACEMENT half, and `program/test_synth_verify_failure_path` for
// the BEHAVIOUR half — that one calls `verifySynthesizedModule` directly with a
// hand-broken module, adding nothing to `src/`, and requires the tier diagnostic
// and the specific broken invariant to be reported together.
//
// HOST-INDEPENDENT: nothing is compiled and nothing is run.

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] std::string readSource(std::string_view relative) {
    auto const root = dss::test::findRepoRoot();
    EXPECT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();
    if (!root.has_value()) return {};
    std::filesystem::path const p = *root / relative;
    std::ifstream in(p, std::ios::binary);
    EXPECT_TRUE(in.good())
        << "cannot read " << p.string()
        << " — this guard's subject IS the source tree, so an unreadable file "
           "is a red, never a skip";
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

[[nodiscard]] bool isIdentifierByte(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z') || c == '_';
}

// A copy of `text` in which every byte that is NOT C++ code — the interior of a
// `//` line comment, of a `/* … */` block comment, of a string literal and of a
// character literal — is replaced by a space.
//
// ★ SAME LENGTH, deliberately: every offset computed on the result still indexes
// the ORIGINAL file, so the order comparison stays a statement about the real
// source and a future diagnostic can quote a real position. Newlines survive
// blanking for the same reason.
//
// ⚠ A `'` opens a character literal only when the byte before it is not an
// identifier byte. Without that clause C++14's digit separator — `1'000'000u`,
// which `src/program/program.cpp` really does contain — would open a literal
// that never closes and blank the whole tail of the file.
//
// Raw string literals (`R"(…)"`) are NOT modelled. Neither subject contains one
// (✔MEASURED, cycle P63: zero `R"` in either driver), and the failure direction
// if one ever appears is RED — an unterminated scan state is reported below by
// name rather than being absorbed.
[[nodiscard]] std::string blankNonCode(std::string const& text) {
    enum class Scan { Code, LineComment, BlockComment, StringLiteral, CharLiteral };
    std::string out   = text;
    Scan        state = Scan::Code;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char const c    = text[i];
        char const next = (i + 1 < text.size()) ? text[i + 1] : '\0';
        switch (state) {
        case Scan::Code:
            if (c == '/' && next == '/') {
                state = Scan::LineComment;
                out[i] = ' ';
                out[i + 1] = ' ';
                ++i;
            } else if (c == '/' && next == '*') {
                state = Scan::BlockComment;
                out[i] = ' ';
                out[i + 1] = ' ';
                ++i;
            } else if (c == '"') {
                state  = Scan::StringLiteral;
                out[i] = ' ';
            } else if (c == '\'' && !(i > 0 && isIdentifierByte(text[i - 1]))) {
                state  = Scan::CharLiteral;
                out[i] = ' ';
            }
            break;
        case Scan::LineComment:
            if (c == '\n') state = Scan::Code;
            else out[i] = ' ';
            break;
        case Scan::BlockComment:
            if (c == '*' && next == '/') {
                out[i]     = ' ';
                out[i + 1] = ' ';
                ++i;
                state = Scan::Code;
            } else if (c != '\n') {
                out[i] = ' ';
            }
            break;
        case Scan::StringLiteral:
        case Scan::CharLiteral: {
            char const closer =
                (state == Scan::StringLiteral) ? '"' : '\'';
            if (c == '\\') {
                out[i] = ' ';
                if (i + 1 < text.size()) {
                    out[i + 1] = ' ';
                    ++i;
                }
            } else if (c == closer) {
                out[i] = ' ';
                state  = Scan::Code;
            } else if (c != '\n') {
                out[i] = ' ';
            }
            break;
        }
        }
    }
    EXPECT_TRUE(state == Scan::Code || state == Scan::LineComment)
        << "the comment/literal scan ended inside a comment or a literal — this "
           "guard mis-parsed its subject, so every verdict it reaches below is "
           "about a file it did not read correctly. It fails toward RED (the "
           "mis-parsed tail is blanked, so a call inside it reads as absent), "
           "but fix the SCAN, never the pin";
    return out;
}

// Every offset in `code` — already blanked by `blankNonCode` — at which the C++
// call form `!name(` appears. Separated from the assertions so the matching rule
// has exactly one definition and can be measured directly by the self-test.
[[nodiscard]] std::vector<std::size_t> callFormOffsets(std::string const& code,
                                                       std::string_view   name) {
    std::string const        needle = "!" + std::string(name) + "(";
    std::vector<std::size_t> hits;
    for (std::size_t at = code.find(needle); at != std::string::npos;
         at = code.find(needle, at + 1)) {
        hits.push_back(at);
    }
    return hits;
}

// Offset of the ONE call of `name` in `code`. Reports (and returns npos) unless
// there is exactly one.
[[nodiscard]] std::size_t soleCallOffset(std::string const& code,
                                         std::string_view   name,
                                         std::string_view   file) {
    std::vector<std::size_t> const hits = callFormOffsets(code, name);
    EXPECT_FALSE(hits.empty())
        << file << " no longer calls " << name
        << " in CODE — the post-synthesis verify seam was deleted. A mention "
           "left behind in a comment or a string literal does not count: this "
           "guard blanks both before matching, precisely because commenting the "
           "call out is how it is usually removed by hand";
    EXPECT_LE(hits.size(), 1u)
        << file << " calls " << name
        << " more than once — this guard reasons about ONE call site per file, "
           "so a second one has to be declared here deliberately rather than "
           "discovered by a reader wondering why the pin went quiet";
    if (hits.size() != 1) return std::string::npos;
    return hits.front();
}

void expectVerifyFollowsSehSynthesis(std::string_view relative) {
    std::string const text = readSource(relative);
    ASSERT_FALSE(text.empty()) << relative << " read back empty";
    std::string const code = blankNonCode(text);
    ASSERT_EQ(code.size(), text.size())
        << relative
        << ": the blanking pass must preserve length, or every offset below "
           "stops indexing the real file";
    ASSERT_NE(code, text)
        << relative
        << ": the blanking pass changed nothing, which for a driver this "
           "heavily commented means it did not run — and a no-op blanking is "
           "exactly the vacuity this guard was reopened to close";
    std::size_t const seh =
        soleCallOffset(code, "synthesizeSehFunclets", relative);
    std::size_t const verify =
        soleCallOffset(code, "verifySynthesizedModule", relative);
    ASSERT_NE(seh, std::string::npos);
    ASSERT_NE(verify, std::string::npos);
    EXPECT_GT(verify, seh)
        << relative
        << ": the post-synthesis MIR verify must FOLLOW synthesizeSehFunclets. "
           "In front of it, the synthesis pass that rewrites the most is the one "
           "thing nothing checks — which is the defect this guard's anchor was "
           "opened for.";
}

// The single-CU seam. The verify used to sit here, one statement too early, with
// a caveat saying so; the caveat's stated reason was measured false in P63 and
// the verify moved behind the pass.
TEST(SynthVerifySeamGuard, SingleCuSeamVerifiesAfterSehSynthesis) {
    expectVerifyFollowsSehSynthesis("src/program/compile_pipeline.cpp");
}

// The merge seam. It had no post-synthesis verify at all: `optimizeModule`
// verifies after every pass IT runs, and it runs BEFORE the SEH pass.
TEST(SynthVerifySeamGuard, MergeSeamVerifiesAfterSehSynthesis) {
    expectVerifyFollowsSehSynthesis("src/program/program.cpp");
}

// THE MECHANISM ITSELF, on an input this test owns. The two seam arms above can
// only ever say "the tree looks right today"; they cannot show that the blanking
// rule DISTINGUISHES a call from a comment, because the shipped tree has no
// commented-out call in it. This one does: one real call and four decoys — a
// line comment, a trailing comment on a real statement, a block comment and a
// string literal — and it requires the matcher to see exactly the real one, at
// the real one's offset.
//
// Delete `blankNonCode`'s body (return the input unchanged) and this arm reds
// naming the count — 5 where 1 was required — which is the one red that says
// WHAT broke. (The two seam arms red as well, on their no-op-blanking guard,
// but only after this one has already named the mechanism.)
TEST(SynthVerifySeamGuard, BlankNonCodeSeesOnlyRealCalls) {
    std::string const sample = R"CPP(
void driver() {
    // if (!verifySynthesizedModule(a, b, c)) return;   <- commented out
    /* an older block:
       if (!verifySynthesizedModule(a, b, c)) return;
    */
    char const* why = "reds when !verifySynthesizedModule( is deleted";
    if (!verifySynthesizedModule(theOnlyRealOne)) return;  // !verifySynthesizedModule(
}
)CPP";
    // Located by its unique argument, never by its indentation: the block-comment
    // decoy below is indented too, and matching on leading spaces found THAT one
    // the first time this arm ran.
    std::size_t const real = sample.find("!verifySynthesizedModule(theOnlyRealOne");
    ASSERT_NE(real, std::string::npos) << "the sample lost its one real call";

    std::vector<std::size_t> const raw =
        callFormOffsets(sample, "verifySynthesizedModule");
    ASSERT_EQ(raw.size(), 5u)
        << "the sample must carry one real call and four decoys, or it is not "
           "testing what it claims";

    std::string const code = blankNonCode(sample);
    ASSERT_EQ(code.size(), sample.size());
    std::vector<std::size_t> const hits =
        callFormOffsets(code, "verifySynthesizedModule");
    ASSERT_EQ(hits.size(), 1u)
        << "the blanking pass must reduce one real call and four decoys (line "
           "comment, block comment, string literal, trailing comment) to exactly "
           "one match — it reported " << hits.size();
    EXPECT_EQ(hits.front(), real)
        << "the surviving match must be the REAL call, at its offset in the "
           "ORIGINAL text — a same-length blanking is what makes that true";
}

} // namespace
