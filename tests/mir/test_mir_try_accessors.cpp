// [[D-MIR-ACCESSORS-ABORT-ON-WRONG-OPCODE]] — the NON-FATAL twins of `Mir`'s
// typed payload readers, and the two guards that keep them what they are.
//
// ★★★ THE DEFECT THIS FILE IS ABOUT IS NOT THE ABORT. `Mir::argIndex` and its
// siblings call `std::abort()` on a wrong-opcode read, and that is CORRECT: an
// accessor that answered with a garbage index would be the silent-wrong-value
// failure this project refuses. Nothing here weakens it — `src/` still gets the
// aborting form, and `MirTryAccessors.AreTestOnlyAndAbsentFromSrc` below fails if that ever stops
// being true. What the abort costs is confined to TEST code, where killing the
// process costs every SIBLING test in the binary its verdict: no `[  FAILED  ]`
// line, no case name, exactly the unattributable signature
// `no_abort_in_tests_guard` exists to prevent.
//
// ★★ AND THE COST IS NOT HYPOTHETICAL — IT IS THE SHAPE THE NATURAL ASSERTION
// TAKES. Measured across `tests/` when this landed, TWELVE sites read a typed
// payload whose only protection was a NON-FATAL `EXPECT_EQ` on the opcode of the
// SAME instruction, one line above:
//
//     EXPECT_EQ(m.instOpcode(arg), MirOpcode::Arg);   // records, falls THROUGH
//     EXPECT_EQ(m.argIndex(arg), 0u);                 // aborts
//
// `EXPECT_*` does not stop the test, so on precisely the regression that pair was
// written to report, control reaches the abort and the run dies. The guard is
// decorative. Writing it safely instead costs a hand-rolled opcode test per
// assertion — which is why `test_mir_merge.cpp`'s SINGLE-SLOT IDENTITY PROBES and
// `test_mir_inline_asm_immediate.cpp`'s `constValue` each grew their own private
// copy of one — and the defensive form a hurried author actually writes,
// `if (wrong opcode) return;`, trades the crash for a VACUOUS PASS. That is the
// disincentive the row recorded: it points away from operand-chain pins, which
// are the assertions this project most wants.
//
// ⚠ THE ROW PRESCRIBED THREE TWINS AND NAMED `instPayload` AS ONE OF THEM. Both
// halves were refuted here. `instPayload` does NOT abort on a wrong opcode — it
// is `instArena_.at(id).payload`, a RAW reader with no opcode to be wrong about,
// so it has no twin and needs none. And the aborting set is not three but
// THIRTEEN: nine typed payload readers through `payloadForOpcode_`, the two asm
// readers with their own two-arm test, and the two operand-pool readers.
// `EveryAbortingPayloadAccessorHasANonFatalTwin` below is why the count cannot
// silently go stale again.

#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"          // TargetRegClass
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace dss;

namespace {

constexpr TypeId kFnSig{1};
constexpr TypeId kI32{2};

MirLiteralValue intLit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// One module carrying an instance of every opcode the twins discriminate on, so
// each assertion can name a RIGHT-opcode subject and a WRONG-opcode subject that
// are siblings in the same frozen module. `phi` is reached through a real CFG
// because `addPhi` requires one.
struct Fixture {
    MirBuilder b;
    MirInstId  arg{}, konst{}, globalAddr{}, blockAddr{}, phi{};

    Fixture() {
        b.addFunction(kFnSig, SymbolId{1});
        MirBlockId const entry  = b.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const header = b.createBlock(StructCfMarker::LoopHeader);
        MirBlockId const exit   = b.createBlock(StructCfMarker::LoopExit);

        b.beginBlock(entry);
        arg        = b.addArg(0, kI32);
        konst      = b.addConst(intLit(5), kI32);
        globalAddr = b.addGlobalAddr(SymbolId{9}, kI32);
        blockAddr  = b.addBlockAddress(exit, kI32);
        b.addBr(header);

        b.beginBlock(header);
        phi = b.addPhi(kI32);
        b.addPhiIncoming(phi, MirPhiIncoming{konst, entry});
        b.addBr(exit);

        b.beginBlock(exit);
        b.addReturn(phi);
    }

    Mir finish() { return std::move(b).finish(); }
};

} // namespace

// ── 1. THE TWINS AGREE WITH THEIR SIBLINGS ON THE RIGHT OPCODE ──────────────
//
// ⚠ THIS ARM IS WHAT STOPS THE FILE PASSING VACUOUSLY. A twin hard-wired to
// `return std::nullopt` would satisfy every wrong-opcode assertion below and
// nothing else in the suite could tell. Each value is compared against the
// ABORTING sibling's answer rather than against a literal, so the pin is
// "the twin reports what the compiler reads", which is the property that
// matters and the one a divergent re-decode of the packed payload would break.
TEST(MirTryAccessors, RightOpcodeReturnsExactlyTheAbortingFormsValue) {
    Fixture   f;
    Mir const m = f.finish();

    EXPECT_EQ(m.tryArgIndex(f.arg), std::optional{m.argIndex(f.arg)});
    EXPECT_EQ(m.tryArgPosition(f.arg), std::optional{m.argPosition(f.arg)});
    EXPECT_EQ(m.tryConstLiteralIndex(f.konst),
              std::optional{m.constLiteralIndex(f.konst)});
    EXPECT_EQ(m.tryGlobalAddrSymbol(f.globalAddr),
              std::optional{m.globalAddrSymbol(f.globalAddr)});
    EXPECT_EQ(m.tryBlockAddressTarget(f.blockAddr),
              std::optional{m.blockAddressTarget(f.blockAddr)});

    // And they carry the REAL values, not merely equal ones — a fixture whose
    // Arg ordinal happened to be whatever both sides read would make the
    // agreement above unfalsifiable.
    EXPECT_EQ(m.tryArgIndex(f.arg), 0u);
    ASSERT_TRUE(m.tryConstLiteralIndex(f.konst).has_value());
    EXPECT_EQ(std::get<std::int64_t>(
                  m.literalValue(*m.tryConstLiteralIndex(f.konst)).value),
              5);
    EXPECT_EQ(m.tryGlobalAddrSymbol(f.globalAddr), std::optional{SymbolId{9}});

    // The operand-pool pair: each answers for its own pool and nullopt for the
    // other's, which is the whole content of the `usesPhiPool` split.
    ASSERT_TRUE(m.tryPhiIncomings(f.phi).has_value());
    EXPECT_EQ(m.tryPhiIncomings(f.phi)->size(), 1u);
    ASSERT_TRUE(m.tryInstOperands(f.konst).has_value());
    EXPECT_EQ(m.tryInstOperands(f.konst)->size(), m.instOperands(f.konst).size());
}

// ── 2. THE WRONG OPCODE YIELDS nullopt WHERE THE SIBLING KILLS THE BINARY ────
//
// ★★★ HOW WE KNOW THE ARM PINNED HERE IS THE ONE THAT USED TO BE FATAL, rather
// than a case the aborting form would also have survived: the DEATH TESTS below
// drive the SAME accessor over the SAME wrong-opcode instruction of the SAME
// fixture and assert it dies. A nullopt assertion here paired with a proven
// abort there is the before/after of one identical call, not two adjacent
// claims about different situations.
TEST(MirTryAccessors, WrongOpcodeReturnsNulloptInsteadOfAborting) {
    Fixture   f;
    Mir const m = f.finish();

    // `konst` is a Const — every one of these is the wrong question to ask it.
    // Compared against a DISENGAGED optional of the same type rather than against
    // `std::nullopt`, so a failure prints the value that came back instead of
    // falling into gtest's raw-byte fallback for `std::nullopt_t`.
    EXPECT_EQ(m.tryArgIndex(f.konst), std::optional<std::uint32_t>{});
    EXPECT_EQ(m.tryArgPosition(f.konst), std::optional<std::uint32_t>{});
    EXPECT_EQ(m.tryGlobalAddrSymbol(f.konst), std::optional<SymbolId>{});
    EXPECT_EQ(m.tryBlockAddressTarget(f.konst), std::optional<MirBlockId>{});
    EXPECT_EQ(m.tryBlockAddressExportSymbol(f.konst), std::optional<SymbolId>{});
    EXPECT_EQ(m.tryIntrinsicId(f.konst), std::optional<std::uint32_t>{});
    EXPECT_EQ(m.tryReturnPieceOrdinal(f.konst), std::optional<std::uint32_t>{});
    EXPECT_EQ(m.tryReturnPieceRegClass(f.konst), std::optional<TargetRegClass>{});
    EXPECT_EQ(m.tryAsmDescriptorIndex(f.konst), std::optional<std::uint32_t>{});
    EXPECT_EQ(m.tryAsmDescriptor(f.konst), nullptr);
    EXPECT_EQ(m.tryConstLiteralIndex(f.arg), std::optional<std::uint32_t>{});

    // The pool split, both directions. `has_value()` rather than an optional
    // comparison: `std::span` has no `operator==`, so two engaged optionals of
    // spans are not comparable at all.
    EXPECT_FALSE(m.tryPhiIncomings(f.konst).has_value());
    EXPECT_FALSE(m.tryInstOperands(f.phi).has_value());

    // ★ AND THE WHOLE POINT: this test is still running. Thirteen wrong-opcode
    // reads happened above; with the aborting form, the FIRST of them would have
    // ended the process here and taken every other case in this binary with it.
}

TEST(MirTryAccessorsDeathTest, TheAbortingSiblingsStillDieOnTheSameReads) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Fixture   f;
    Mir const m = f.finish();

    // Each of these is the exact call its `try*` twin answered with nullopt in
    // the test above. Product code keeps this behaviour — that is the row's
    // standing constraint, and this is where it is pinned.
    EXPECT_DEATH({ (void)m.argIndex(f.konst); }, "argIndex");
    EXPECT_DEATH({ (void)m.argPosition(f.konst); }, "argPosition");
    EXPECT_DEATH({ (void)m.globalAddrSymbol(f.konst); }, "globalAddrSymbol");
    EXPECT_DEATH({ (void)m.blockAddressTarget(f.konst); }, "blockAddressTarget");
    EXPECT_DEATH({ (void)m.intrinsicId(f.konst); }, "intrinsicId");
    EXPECT_DEATH({ (void)m.returnPieceOrdinal(f.konst); }, "returnPieceOrdinal");
    EXPECT_DEATH({ (void)m.asmDescriptorIndex(f.konst); }, "asmDescriptorIndex");
    EXPECT_DEATH({ (void)m.constLiteralIndex(f.arg); }, "constLiteralIndex");
    EXPECT_DEATH({ (void)m.phiIncomings(f.konst); }, "not a Phi");
    EXPECT_DEATH({ (void)m.instOperands(f.phi); }, "use phiIncomings");
}

// ── 3. THE GUARDS ───────────────────────────────────────────────────────────

namespace {

// The twins, spelled once. `EveryAbortingPayloadAccessorHasANonFatalTwin` proves
// this list has not fallen behind `mir.cpp`, so it is a fact rather than a
// hopeful copy.
std::vector<std::string> const& tryAccessorNames() {
    static std::vector<std::string> const kNames{
        "tryArgIndex",           "tryArgPosition",
        "tryConstLiteralIndex",  "tryGlobalAddrSymbol",
        "tryBlockAddressTarget", "tryBlockAddressExportSymbol",
        "tryIntrinsicId",        "tryReturnPieceOrdinal",
        "tryReturnPieceRegClass","tryAsmDescriptorIndex",
        "tryAsmDescriptor",      "tryInstOperands",
        "tryPhiIncomings",
    };
    return kNames;
}

[[nodiscard]] std::string readFile(std::filesystem::path const& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Which twin names occur in `text`. Factored out so the tree scan and the
// self-test arm below run the IDENTICAL matcher — a guard whose ability to fail
// is only asserted through a flag nobody passes has proved nothing about itself
// (D-GATE-TWO-GUARDS-SELF-TEST-BEHIND-A-FLAG-NOBODY-PASSES).
[[nodiscard]] std::set<std::string> twinNamesIn(std::string_view text) {
    std::set<std::string> hits;
    for (auto const& name : tryAccessorNames()) {
        if (text.find(name) != std::string_view::npos) hits.insert(name);
    }
    return hits;
}

} // namespace

// ★★★ THE CONSTRAINT THE ROW MADE NON-NEGOTIABLE, ENFORCED RATHER THAN ASSUMED.
// The twins exist so TESTS can pin an operand chain without killing the binary.
// If `src/` starts calling them, the compiler acquires a way to read a payload
// off the wrong opcode and carry on — the silent-wrong-value failure the aborting
// form exists to make impossible. Naming and a comment are not enforcement, and
// this project has measured what un-enforced conventions are worth.
//
// ⚠ NO ESCAPE HATCH, DELIBERATELY. An exemption that every future call site can
// claim refuses nothing — a guard whose escape all its candidates trigger is
// disarmed rather than lenient. The only files exempt here are the two that
// DECLARE and DEFINE the twins, which cannot mention them any less.
TEST(MirTryAccessors, AreTestOnlyAndAbsentFromSrc) {
    namespace fs = std::filesystem;
    fs::path const src = dss::test::repoRoot() / "src";
    ASSERT_TRUE(fs::is_directory(src)) << "no src/ under " << src.string();

    fs::path const declHome = src / "mir" / "mir.hpp";
    fs::path const defHome  = src / "mir" / "mir.cpp";

    std::size_t scanned = 0;
    std::vector<std::string> offences;
    for (auto const& e : fs::recursive_directory_iterator(src)) {
        if (!e.is_regular_file()) continue;
        fs::path const& p   = e.path();
        std::string const ext = p.extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".cc") continue;
        if (fs::equivalent(p, declHome) || fs::equivalent(p, defHome)) continue;
        ++scanned;
        for (auto const& hit : twinNamesIn(readFile(p))) {
            offences.push_back(p.lexically_relative(src).generic_string() + ": " + hit);
        }
    }

    // A scan that reached nothing is green for the wrong reason.
    EXPECT_GT(scanned, 100u) << "the src/ walk found implausibly few sources — "
                                "the guard would pass without checking anything";

    for (auto const& o : offences) {
        ADD_FAILURE() << "src/ must not call a `try*` MIR accessor: " << o
                      << "\n  The aborting form is the product-code contract: a "
                         "wrong-opcode read is a CALLER BUG and must stay fatal. "
                         "The `try*` twins exist for tests only "
                         "[[D-MIR-ACCESSORS-ABORT-ON-WRONG-OPCODE]].";
    }

    // ★ THE SELF-TEST, IN THE SAME RUN AS THE CHECK. Proves the matcher above can
    // actually fire, so a green tree arm means "found nothing" rather than
    // "looked for nothing".
    EXPECT_EQ(twinNamesIn("auto v = mir.tryArgIndex(id);"),
              (std::set<std::string>{"tryArgIndex"}));
    EXPECT_TRUE(twinNamesIn("auto v = mir.argIndex(id);").empty())
        << "the matcher fires on the ABORTING name — it would refuse all of src/";
}

// ★★★ THE RATCHET THAT SURVIVES THE NEXT ACCESSOR. Every typed payload reader
// registers its own name as a string literal in its `payloadForOpcode_` call, so
// the aborting set is DERIVABLE from `mir.cpp` rather than transcribed. Adding a
// tenth reader without its twin reds here — which is the drift this row's own
// history demonstrates, having recorded three accessors when there were nine and
// having named one (`instPayload`) that never aborted at all.
TEST(MirTryAccessors, EveryAbortingPayloadAccessorHasANonFatalTwin) {
    namespace fs = std::filesystem;
    fs::path const mirCpp = dss::test::repoRoot() / "src" / "mir" / "mir.cpp";
    fs::path const mirHpp = dss::test::repoRoot() / "src" / "mir" / "mir.hpp";
    std::string const cpp = readFile(mirCpp);
    std::string const hpp = readFile(mirHpp);
    ASSERT_FALSE(cpp.empty()) << "could not read " << mirCpp.string();
    ASSERT_FALSE(hpp.empty()) << "could not read " << mirHpp.string();

    // An identifier-shaped string literal that also names a `Mir::` member IS an
    // accessor-name argument. The fatal FORMAT strings carry spaces and colons,
    // so they cannot match; `MirBuilder::` members cannot match either, because
    // the probe demands the literal `Mir::` prefix.
    auto abortingAccessorsIn = [](std::string const& text) {
        std::set<std::string> names;
        std::regex const ident{"\"([A-Za-z_][A-Za-z0-9_]*)\""};
        for (auto it = std::sregex_iterator(text.begin(), text.end(), ident);
             it != std::sregex_iterator(); ++it) {
            std::string const n = (*it)[1].str();
            if (text.find("Mir::" + n + "(") != std::string::npos) names.insert(n);
        }
        return names;
    };

    std::set<std::string> const aborting = abortingAccessorsIn(cpp);
    EXPECT_GE(aborting.size(), 9u)
        << "the derivation found fewer typed payload accessors than mir.cpp had "
           "when this landed — the parse, not the tree, is what changed";

    for (auto const& name : aborting) {
        std::string twin = "try";
        twin += static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        twin += name.substr(1);
        EXPECT_NE(hpp.find(twin + "("), std::string::npos)
            << "`Mir::" << name << "` aborts on a wrong opcode but has no `"
            << twin << "` twin in mir.hpp. Add one (it tests the opcode and "
               "DELEGATES — never re-decode the payload) and list it in "
               "tryAccessorNames() so the src/ guard covers it too. "
               "[[D-MIR-ACCESSORS-ABORT-ON-WRONG-OPCODE]]";
        EXPECT_NE(std::find(tryAccessorNames().begin(), tryAccessorNames().end(), twin),
                  tryAccessorNames().end())
            << twin << " is missing from tryAccessorNames(), so the src/ guard "
                       "would not refuse it";
    }

    // ★ THE SELF-TEST: the derivation finds a planted accessor, so a green run
    // above means the tree is complete rather than the parse being blind.
    std::set<std::string> const planted = abortingAccessorsIn(
        "std::uint32_t Mir::widgetOrdinal(MirInstId id) const {\n"
        "    return payloadForOpcode_(instArena_.at(id), MirOpcode::Widget, id,\n"
        "                             \"widgetOrdinal\");\n}\n");
    EXPECT_EQ(planted, (std::set<std::string>{"widgetOrdinal"}));
}

// ── 4. THE RATCHET FOR TEST CODE ─────────────────────────────────────────────
// [[D-MIR-ACCESSORS-ABORT-ON-WRONG-OPCODE]] — the TEST-side half of the row
// above, and the only anchor this section cites. ⚠ Deliberately not a NEW id:
// `anchor_registry_guard` refuses an anchor cited under `tests/` with no row in
// `.plans/`, and the lane that wrote this section is forbidden `.plans/**` — so
// minting one here would have shipped a guaranteed-red gate. The class is the
// same defect this file already documents, seen from the authoring end.
//
// ★★★ THE GUARDS ABOVE STOP `src/` MISUSING THE TWINS. NOTHING STOPPED A TEST
// AUTHOR WRITING THE ORIGINAL DEFECT AGAIN. The twins made the safe spelling
// available; availability is a convention, and this project has measured twice
// that a convention has no teeth AT THE MOMENT OF THE DECISION — the dangling-
// `find` class was "fixed by adopting a convention" twice before a deleted
// overload finally made it a compile error. So the shape is REFUSED here.
//
// ⚠⚠ WHAT THIS REFUSES IS DELIBERATELY NARROW, AND THE NARROWNESS IS THE
// DESIGN. "A payload read whose opcode is not established in a way that returns
// before the read" is not decidable from text: the establishment is routinely a
// `continue` in an enclosing loop, or an opcode FILTER inside a helper three
// frames up (`phisInFuncBySymbol`, `forEachPhi`, `soleAsm`). ✔MEASURED across
// tests/opt + tests/lir when this landed: 59 of the 68 Mir payload reads there
// are established that way and are CORRECT. A matcher broad enough to see the
// naked ones would refuse those 59 too, and a guard nobody can keep green gets
// disabled. So this refuses ONE shape, exactly, and refuses it loudly:
//
//     EXPECT_EQ(m.instOpcode(x), MirOpcode::Const);   // records, falls THROUGH
//     ... m.constLiteralIndex(x) ...                  // ABORTS
//
// a NON-FATAL opcode assertion on subject `x`, and within three statements
// (either direction) a fatal payload read on the SAME subject text.
//
// ★★ THE ESCAPE IS THE REMEDY, WHICH IS WHY IT DISARMS NOTHING. An author can
// silence this by writing `ASSERT_EQ` instead of `EXPECT_EQ` — and that IS the
// fix, because `ASSERT_*` returns before the read. It is also not claimable
// everywhere: `ASSERT_*` expands to `return;` and will not COMPILE in a
// value-returning helper, which is precisely where the naked reads lived. There
// is NO comment marker, NO allow-list and NO per-file exemption
// [[D-GATE-TARGET-SCHEMA-PRODUCER-GUARD-CARRIED-AN-INERT-EXEMPTION]].
//
// ⚠ KNOWN FALSE NEGATIVES, stated rather than discovered later: a read with no
// opcode assertion anywhere near it; an assertion on a DIFFERENT SPELLING of the
// same instruction (`ops[0]` vs a copy of it); a read more than three statements
// away; and `instOperands`, excluded below with its own reason.

namespace {

// One statement of a translation unit, as this guard sees it.
struct Statement {
    std::size_t line = 0;
    std::string code;
};

// ★ COMMENTS ARE DROPPED AND LITERAL CONTENT IS EMPTIED (the quotes survive), so
// no keyword, accessor name or subject can ever be matched out of PROSE. That is
// not tidiness — it is a defect this matcher already had: the stop-word `return`
// was matching inside the message string `"...must reach main's Return"` and
// silently hid one of the two real offences in the tree. A matcher that fails
// toward CLEAN is worse than none.
[[nodiscard]] std::vector<Statement> statementsOf(std::string_view text) {
    std::vector<Statement> out;
    std::string            buf;
    std::size_t            line = 1, start = 1;
    auto flush = [&] {
        std::string collapsed;
        bool space = false;
        for (char c : buf) {
            if (std::isspace(static_cast<unsigned char>(c))) { space = !collapsed.empty(); continue; }
            if (space) { collapsed.push_back(' '); space = false; }
            collapsed.push_back(c);
        }
        if (!collapsed.empty()) out.push_back(Statement{start, collapsed});
        buf.clear();
    };
    for (std::size_t i = 0; i < text.size();) {
        char const c = text[i];
        if (c == '\n')                                    { ++line; buf.push_back(c); ++i; continue; }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) {
                if (text[i] == '\n') ++line;
                ++i;
            }
            i += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            buf.push_back(c);
            for (++i; i < text.size();) {
                if (text[i] == '\\')  { i += 2; continue; }
                if (text[i] == c)     { buf.push_back(c); ++i; break; }
                if (text[i] == '\n')  ++line;
                ++i;
            }
            continue;
        }
        if (c == ';' || c == '{' || c == '}') { flush(); ++i; start = line; continue; }
        buf.push_back(c);
        ++i;
    }
    flush();
    return out;
}

// The text between the `(` at `openParen` and its matching `)`.
[[nodiscard]] std::optional<std::string> balancedArg(std::string const& s,
                                                     std::size_t openParen) {
    int depth = 0;
    for (std::size_t i = openParen; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')' && --depth == 0) return s.substr(openParen + 1, i - openParen - 1);
    }
    return std::nullopt;
}

// ★★★ DERIVED FROM `tryAccessorNames()`, NOT TRANSCRIBED. That list is itself
// proved complete against `mir.cpp` by
// `EveryAbortingPayloadAccessorHasANonFatalTwin`, so a fourteenth aborting reader
// joins this refusal the moment its twin is declared — the drift this row's own
// history demonstrates cannot repeat here.
//
// ⚠ `instOperands` IS EXCLUDED, and this is the one judgement call in the guard.
// It is the ONLY one of the thirteen whose abort fires on a SINGLE opcode (a Phi)
// rather than on the complement of one, so `EXPECT_EQ(op, Add); instOperands(x)`
// is safe for every opcode but one; and it is the ONLY name shared with a
// DIFFERENT class — `Lir::instOperands` (src/lir/lir.hpp) has no opcode test at
// all — which a textual matcher cannot tell apart without types. ✔MEASURED:
// including it produced 41 offences across tests/**, and every one was a false
// positive.
[[nodiscard]] std::vector<std::string> const& fatalReaderNames() {
    static std::vector<std::string> const kNames = [] {
        std::vector<std::string> names;
        for (auto const& twin : tryAccessorNames()) {
            std::string n = twin.substr(3);  // strip "try"
            n[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(n[0])));
            if (n == "instOperands") continue;
            names.push_back(n);
        }
        return names;
    }();
    return kNames;
}

struct Offence {
    std::string file, reader, subject;
    std::size_t line = 0;
};

struct ScanResult {
    std::vector<Offence> offences;
    std::size_t          candidates = 0;  // non-fatal opcode assertions REACHED
};

// A statement that transfers control or asserts fatally ENDS the window: the
// subject cannot reach the read past it. Conservative by construction — it can
// only shrink the refusal, never widen it.
[[nodiscard]] bool endsTheWindow(std::string const& s) {
    static std::regex const stop{
        R"(\b(continue|return|break|if|for|while|switch)\b|ASSERT_)"};
    return std::regex_search(s, stop);
}

[[nodiscard]] ScanResult expectThenFatalIn(std::string_view text,
                                           std::string const& file = {}) {
    ScanResult                   out;
    std::vector<Statement> const st = statementsOf(text);
    constexpr std::size_t        kWindow = 3;

    for (std::size_t k = 0; k < st.size(); ++k) {
        std::string const& s = st[k].code;
        if (s.find("EXPECT_") == std::string::npos) continue;
        std::size_t const at = s.find("instOpcode(");
        if (at == std::string::npos) continue;
        auto subject = balancedArg(s, at + std::string_view{"instOpcode"}.size());
        if (!subject.has_value() || subject->empty()) continue;
        ++out.candidates;

        for (int dir : {1, -1}) {
            for (std::size_t w = 1; w <= kWindow; ++w) {
                std::size_t const idx = static_cast<std::size_t>(
                    static_cast<std::ptrdiff_t>(k) + dir * static_cast<std::ptrdiff_t>(w));
                if (idx >= st.size()) break;  // wraps on underflow — same guard
                std::string const& n = st[idx].code;
                if (endsTheWindow(n)) break;
                for (auto const& reader : fatalReaderNames()) {
                    std::string twin = "try";
                    twin += static_cast<char>(std::toupper(static_cast<unsigned char>(reader[0])));
                    twin += reader.substr(1);
                    if (n.find("." + reader + "(" + *subject + ")") != std::string::npos
                        && n.find(twin) == std::string::npos) {
                        out.offences.push_back(Offence{file, reader, *subject, st[idx].line});
                        break;
                    }
                }
            }
        }
    }
    return out;
}

} // namespace

TEST(MirTryAccessors, NoTestReadsAPayloadGuardedOnlyByANonFatalOpcodeExpect) {
    namespace fs = std::filesystem;
    fs::path const tests = dss::test::repoRoot() / "tests";
    ASSERT_TRUE(fs::is_directory(tests)) << "no tests/ under " << tests.string();

    std::size_t          scanned = 0, candidates = 0;
    std::vector<Offence> offences;
    for (auto const& e : fs::recursive_directory_iterator(tests)) {
        if (!e.is_regular_file()) continue;
        fs::path const&   p   = e.path();
        std::string const ext = p.extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".cc") continue;
        ++scanned;
        std::string const text = readFile(p);
        // ★ A SOUND PRE-FILTER, NOT A SAMPLE. The candidate test needs the
        // identifier `instOpcode` in the file's CODE, and stripping comments and
        // whitespace can only remove characters BETWEEN tokens — it can never
        // assemble that identifier out of pieces. So a file whose RAW text lacks
        // it cannot produce a candidate, and skipping it loses nothing. It is
        // matched WITHOUT the `(` on purpose: `instOpcode/*c*/(x)` collapses to
        // `instOpcode(x)`, so a paren-inclusive filter WOULD have a false
        // negative. `scanned` still counts every file the walk reached, so the
        // anti-vacuity floor below measures the WALK, not the filter.
        if (text.find("instOpcode") == std::string::npos) continue;
        ScanResult r = expectThenFatalIn(text, p.lexically_relative(tests).generic_string());
        candidates += r.candidates;
        offences.insert(offences.end(), r.offences.begin(), r.offences.end());
    }

    for (auto const& o : offences) {
        ADD_FAILURE()
            << o.file << " near line " << o.line << ": `" << o.reader << "(" << o.subject
            << ")` reads a typed payload whose only protection is a NON-FATAL "
               "`EXPECT_*` on the opcode of `" << o.subject << "`.\n"
               "  `EXPECT_*` records the failure and falls THROUGH, so on exactly "
               "the regression that pair reports, control reaches the aborting "
               "reader and the PROCESS DIES — no `[  FAILED  ]` line, no case "
               "name, and every sibling test in this binary loses its verdict.\n"
               "  Fix it either way: call `try" << static_cast<char>(std::toupper(
                   static_cast<unsigned char>(o.reader[0]))) << o.reader.substr(1)
            << "` and assert on the optional, or promote the opcode `EXPECT_*` to "
               "`ASSERT_*` so it RETURNS before the read. "
               "[[D-MIR-ACCESSORS-ABORT-ON-WRONG-OPCODE]]";
    }

    // ── THE TWO ANTI-VACUITY FLOORS ─────────────────────────────────────────
    // A scan that reached no FILES is green for the wrong reason...
    EXPECT_GT(scanned, 300u) << "the tests/ walk found implausibly few sources — "
                                "the guard would pass without checking anything";
    // ...and so is a scan that reached no SUBJECTS. This is the count that the
    // disarmed-guard lesson demands: how many candidates actually REACH the
    // refusal. If a formatting or naming change stopped `EXPECT_*`+`instOpcode(`
    // statements being recognised, the tree arm would go green while refusing
    // nothing at all [[D-GATE-TARGET-SCHEMA-PRODUCER-GUARD-CARRIED-AN-INERT-EXEMPTION]].
    EXPECT_GT(candidates, 100u)
        << "the matcher recognised implausibly few non-fatal opcode assertions "
           "across tests/ — it is no longer reaching its subjects";

    // The derivation must stay anchored to the proven twin list, minus exactly
    // the one documented exclusion.
    EXPECT_EQ(fatalReaderNames().size(), tryAccessorNames().size() - 1)
        << "fatalReaderNames() must be tryAccessorNames() minus `instOperands` "
           "alone — a second silent exclusion is a hole in the refusal";
    EXPECT_EQ(std::find(fatalReaderNames().begin(), fatalReaderNames().end(),
                        "instOperands"),
              fatalReaderNames().end());
    EXPECT_NE(std::find(fatalReaderNames().begin(), fatalReaderNames().end(),
                        "constLiteralIndex"),
              fatalReaderNames().end());
}

// ★ THE SELF-TEST, IN THE SAME RUN AS THE CHECK ABOVE, through the IDENTICAL
// matcher. A guard that only ever reports "found nothing" has proved nothing
// about itself (D-GATE-TWO-GUARDS-SELF-TEST-BEHIND-A-FLAG-NOBODY-PASSES). Each
// negative arm is a SHAPE THE TREE ACTUALLY CONTAINS, so a matcher that started
// refusing them would be caught here rather than by 59 false failures.
TEST(MirTryAccessors, TheNonFatalOpcodeExpectMatcherFiresAndDiscriminates) {
    auto count = [](char const* src) { return expectThenFatalIn(src).offences.size(); };

    // FIRES on the shape, in both orders.
    EXPECT_EQ(count("EXPECT_EQ(m.instOpcode(x), MirOpcode::Const);\n"
                    "EXPECT_EQ(m.constLiteralIndex(x), 3u);\n"), 1u);
    EXPECT_EQ(count("EXPECT_EQ(m.constLiteralIndex(x), 3u);\n"
                    "EXPECT_EQ(m.instOpcode(x), MirOpcode::Const);\n"), 1u);

    // ⚠ THE REGRESSION PIN ON THE MATCHER ITSELF, AND IT COST A REAL MISS. The
    // stop-words are matched in CODE only. While literal content was still part
    // of a statement's text, `\breturn\b` matched inside the READ's own message
    // string — `"...the elided single-return value..."` — so the window ended
    // one statement early and one of the two genuine offences in the tree was
    // silently invisible. A matcher that fails toward CLEAN is worse than none;
    // this arm is that miss, frozen.
    EXPECT_EQ(count("EXPECT_EQ(m.instOpcode(x), MirOpcode::Const);\n"
                    "EXPECT_EQ(m.constLiteralIndex(x), 3u)\n"
                    "    << \"must reach main's Return, not a break or continue\";\n"),
              1u);

    // SILENT on the twin — the fix.
    EXPECT_EQ(count("EXPECT_EQ(m.instOpcode(x), MirOpcode::Const);\n"
                    "EXPECT_EQ(m.tryConstLiteralIndex(x), 3u);\n"), 0u);
    // SILENT on `ASSERT_*` — the other fix; it RETURNS before the read.
    EXPECT_EQ(count("ASSERT_EQ(m.instOpcode(x), MirOpcode::Const);\n"
                    "EXPECT_EQ(m.constLiteralIndex(x), 3u);\n"), 0u);
    // SILENT on the `continue` filter — the shape 59 correct sites in
    // tests/opt + tests/lir are written in.
    EXPECT_EQ(count("EXPECT_EQ(m.instOpcode(x), MirOpcode::Const);\n"
                    "if (m.instOpcode(x) != MirOpcode::Const) continue;\n"
                    "EXPECT_EQ(m.constLiteralIndex(x), 3u);\n"), 0u);
    // SILENT on a DIFFERENT subject — the refusal is subject-keyed, not
    // name-keyed, or it would fire on every adjacent pair in a walk loop.
    EXPECT_EQ(count("EXPECT_EQ(m.instOpcode(x), MirOpcode::Const);\n"
                    "EXPECT_EQ(m.constLiteralIndex(y), 3u);\n"), 0u);
    // SILENT on `instOperands`, the documented exclusion — and this arm is what
    // makes the exclusion VISIBLE rather than an accident of the derivation.
    EXPECT_EQ(count("EXPECT_EQ(l.instOpcode(x), LirOpcode::Mov);\n"
                    "EXPECT_EQ(l.instOperands(x).size(), 2u);\n"), 0u);
    // SILENT when the shape is only PROSE: a comment and a string cannot mint an
    // offence, or every file quoting the defect (this one included) would red.
    EXPECT_EQ(count("// EXPECT_EQ(m.instOpcode(x), MirOpcode::Const);\n"
                    "// EXPECT_EQ(m.constLiteralIndex(x), 3u);\n"), 0u);
    EXPECT_EQ(count("char const* s = \"EXPECT_EQ(m.instOpcode(x), C); \"\n"
                    "               \"EXPECT_EQ(m.constLiteralIndex(x), 3u);\";\n"), 0u);

    // And the candidate counter — the anti-vacuity instrument — actually counts.
    EXPECT_EQ(expectThenFatalIn("EXPECT_EQ(m.instOpcode(x), MirOpcode::Const);\n"
                                "EXPECT_EQ(m.instOpcode(y), MirOpcode::Arg);\n").candidates,
              2u);
}
