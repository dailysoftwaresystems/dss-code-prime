// The `.dssir` reader RETURNS on every input (P68 round 8, lane `ht`, part 1b).
//
// `parseMir`'s contract is a refusal — `ok == false` and a named diagnostic —
// for any text it cannot read, and `mirsum::decodeModuleBody` promises its own
// callers "nullopt, never a partially-filled module" on top of that. An ABORT
// honours neither. `MirBuilder` aborts on every precondition it has, and is
// right to: from compiler code a violation is a programming error. A reader of
// TEXT meets the same violations as ordinary malformed input, so it must ask
// each question first — from the builder's own rule — and refuse by name.
//
// ✔MEASURED before the fix, each shape parsed in an isolated child, WSL Release:
// 17 shapes killed the process (three of them introduced by this lane's own
// instruction-flag list), three truncations HUNG, three shapes were read CLEAN
// although no module can mean them, and one bad block produced 13 diagnostics.
// The row for the global/function arms (the invalid-type-reaches-a-builder row,
// P28) had closed two arms; the instruction grammar was never covered.
//
// Three pins:
//   1. THE TABLE — every measured shape returns, `ok == false`, with its OWN
//      refusal by name and nothing else (no cascade).
//   2. THE SWEEP — by construction, so the NEXT entrance cannot hide: every
//      truncation of a golden module and every single-token deletion of it is
//      parsed in-process, and each must RETURN (any verdict). Before each parse
//      the shape is written to stderr and flushed, so an abort — or a hang that
//      ends in ctest's TIMEOUT — names the shape that did it.
//   3. THE CASCADE — one bad block costs exactly its own diagnostic, and the
//      blocks after it are READ (a second defect in one is reported too).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"   // kTlsIndexReservedSymbolIdValue, isWriterReservedSymbolIdValue
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "mir/mir_text.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace dss;

namespace {

struct Parsed {
    bool ok = true;
    std::vector<std::string> diagnostics;
};

Parsed parse(std::string const& text) {
    DiagnosticReporter r;
    auto const res = parseMir(text, CompilationUnitId{7}, r);
    Parsed p;
    p.ok = res->ok;
    for (auto const& d : r.all()) p.diagnostics.push_back(d.actual);
    return p;
}

std::string listed(std::vector<std::string> const& v) {
    std::string s;
    for (auto const& x : v) s += "\n    " + x;
    return s;
}

// One printable line for stderr: newlines shown as `\n`, clipped.
std::string oneLine(std::string_view s, std::size_t max) {
    std::string out;
    for (char const c : s.substr(0, std::min(s.size(), max))) {
        if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// Written BEFORE a parse and flushed: if the parse aborts or hangs, this is the
// last line of the entry's output, and it names the shape.
void announce(char const* kind, std::size_t index, std::string_view detail) {
    std::fprintf(stderr, "[reader-never-aborts] %s #%zu: %s\n", kind, index,
                 std::string{detail}.c_str());
    std::fflush(stderr);
}

std::string oneFunction(std::string const& blocks) {
    return "dssir 3\n"
           "symbols {\n  %1 \"f\"\n  %2 \"g\"\n}\n"
           "module {\n"
           "  function %1 : fn() -> i32 {\n" + blocks +
           "  }\n"
           "}\n";
}

std::string entryBlock(std::string const& body) {
    return oneFunction("    block %b1 [entry] {\n" + body + "    }\n");
}

std::string twoFunctions(std::string const& first, std::string const& second) {
    return "dssir 3\n"
           "symbols {\n  %1 \"f\"\n  %2 \"g\"\n}\n"
           "module {\n"
           "  function %1 : fn() -> i32 {\n" + first +
           "  }\n"
           "  function %2 : fn() -> i32 {\n" + second +
           "  }\n"
           "}\n";
}

std::string const kOneBlockF =
    "    block %b1 [entry] {\n"
    "      %v1 = const : i32 (lit int 1 : i32)\n"
    "      return %v1\n"
    "    }\n";

// Each shape carries ONE defect, and is written so that nothing after it
// depends on what the defect refused — the count then says "its own refusal and
// nothing else".
struct Shape {
    char const* name;
    std::string text;
    char const* refusal;     // a substring of the one refusal the shape must get
    std::size_t diagnostics; // how many it gets in all
};

std::vector<Shape> measuredShapes() {
    return {
        // ── the builder's result-type rule (`opcodeInfo(op).result`) ──
        {"E1 a value opcode with no ': type'",
         entryBlock("      %v1 = alloca ptr<i32>\n"
                    "      %v2 = const : i32 (lit int 2 : i32)\n      return %v2\n"),
         "'alloca' produces a value, so its type is written ': <type>'", 1},
        {"E1b const with no ': type'",
         entryBlock("      %v1 = const (lit int 1 : i32)\n"
                    "      %v2 = const : i32 (lit int 2 : i32)\n      return %v2\n"),
         "'const' produces a value, so its type is written ': <type>'", 1},
        {"E2 a type on a no-value opcode",
         entryBlock("      %v1 = alloca : ptr<i32>\n      %v2 = const : i32 (lit int 1 : i32)\n"
                    "      store : i32 (%v2, %v1)\n      return %v2\n"),
         "'store' produces no value, so it takes no ': <type>'", 1},
        {"E17 a type on a terminator",
         entryBlock("      %v1 = const : i32 (lit int 1 : i32)\n      return : i32 %v1\n"),
         "'return' produces no value, so it takes no ': <type>'", 1},
        // ── the builder's operand bounds (`opcodeInfo(op)` min/max) ──
        {"E3a too few operands",
         entryBlock("      %v1 = const : i32 (lit int 1 : i32)\n      %v2 = add : i32 (%v1)\n"
                    "      return %v1\n"),
         "'add' takes 2 to 2 operand(s) and this line lists 1", 1},
        {"E3b no operands",
         entryBlock("      %v1 = load : i32\n"
                    "      %v2 = const : i32 (lit int 2 : i32)\n      return %v2\n"),
         "'load' takes 1 to 1 operand(s) and this line lists 0", 1},
        // ── the slot-0 sentinel and the dedicated builders' own bounds ──
        {"E4 the sentinel's mnemonic",
         entryBlock("      %v1 = invalid : i32\n"
                    "      %v2 = const : i32 (lit int 2 : i32)\n      return %v2\n"),
         "unknown opcode 'invalid'", 1},
        {"E5 an arg ordinal past its field",
         entryBlock("      %v1 = arg : i32 (70000)\n"
                    "      %v2 = const : i32 (lit int 2 : i32)\n      return %v2\n"),
         "arg ordinal 70000 / position 70000 does not fit the argument payload", 1},
        {"E6 globaladdr of the invalid symbol",
         entryBlock("      %v1 = globaladdr : ptr<i32> (%0)\n"
                    "      %v2 = const : i32 (lit int 2 : i32)\n      return %v2\n"),
         "globaladdr names symbol %0, the invalid sentinel", 1},
        {"EG a global of the invalid symbol",
         "dssir 3\nmodule {\n  global %0 : i32 = zero\n}\n",
         "a global names symbol %0, the invalid sentinel", 1},
        // ── the block rules (`openBlockHasTerminator`, `isBlockUnopened`) ──
        {"E7 an instruction after the terminator",
         entryBlock("      %v1 = const : i32 (lit int 1 : i32)\n      return %v1\n"
                    "      %v2 = const : i32 (lit int 2 : i32)\n"),
         "block %b1 continues after its terminator", 1},
        {"E11 a repeated block header",
         oneFunction("    block %b1 [entry] {\n      br %b2\n    }\n"
                     "    block %b2 {\n      %v1 = const : i32 (lit int 1 : i32)\n"
                     "      return %v1\n    }\n"
                     "    block %b2 {\n      %v2 = const : i32 (lit int 2 : i32)\n"
                     "      return %v2\n    }\n"),
         "block %b2 is declared twice", 1},
        {"E16 a function with no blocks", oneFunction(""), "function %1 has no blocks", 1},
        // A header whose `{` is missing: the header's own refusal, and ONE for
        // the stray body the function loop then meets — never one per token.
        {"E20 a block header without its '{'",
         oneFunction("    block %b1 [entry] {\n      br %b2\n    }\n"
                     "    block %b2\n      %v1 = const : i32 (lit int 1 : i32)\n"
                     "      return %v1\n"),
         "expected 'block' inside function, got 'v1' — skipped to the next block header", 2},
        // ── a module-wide block id naming another function (`isBlockOfOpenFunction`) ──
        {"E19 a branch into another function",
         twoFunctions(kOneBlockF, "    block %b2 [entry] {\n      br %b1\n    }\n"),
         "%b1 is a block of another function", 1},
        {"E19b a block address of another function's block",
         twoFunctions(kOneBlockF, "    block %b2 [entry] {\n"
                                  "      %v2 = blockaddress : ptr<i8> %b1\n"
                                  "      %v3 = const : i32 (lit int 3 : i32)\n"
                                  "      return %v3\n    }\n"),
         "%b1 is a block of another function", 1},
        {"E19c a header reusing another function's block id",
         twoFunctions(kOneBlockF, "    block %b1 [entry] {\n"
                                  "      %v2 = const : i32 (lit int 2 : i32)\n"
                                  "      return %v2\n    }\n"),
         "block %b1 is a block of another function", 1},
        // ── a refused function followed by another (`closeFunction_`) ──
        {"E18b an undeclared branch target, then a second function",
         twoFunctions("    block %b1 [entry] {\n      br %b7\n    }\n",
                      "    block %b3 [entry] {\n      %v2 = const : i32 (lit int 2 : i32)\n"
                      "      return %v2\n    }\n"),
         "branch target %b7 was never declared", 1},
        // ── this lane's own flag list ──
        {"E8a an unknown flag",
         entryBlock("      %v1 = alloca [voaltile] : ptr<i32>\n      %v2 = load : i32 (%v1)\n"
                    "      return %v2\n"),
         "unknown instruction flag 'voaltile' on 'alloca'", 1},
        {"E8b an unterminated flag list",
         entryBlock("      %v1 = alloca [synthetic : ptr<i32>\n"
                    "      %v2 = const : i32 (lit int 2 : i32)\n      return %v2\n"),
         "'alloca''s instruction-flag list is not closed on its line", 1},
        {"E8c a non-name in the flag list",
         entryBlock("      %v1 = alloca [7] : ptr<i32>\n"
                    "      %v2 = const : i32 (lit int 2 : i32)\n      return %v2\n"),
         "expected a flag name, ',' or ']', got '7'", 1},
        {"E13 a flag on a terminator",
         entryBlock("      %v1 = const : i32 (lit int 1 : i32)\n      return [volatile] %v1\n"),
         "'return' is a terminator and carries no instruction flags", 1},
        // ── read clean although no module can mean them ──
        {"E14 a value defined twice",
         entryBlock("      %v1 = alloca : ptr<i32>\n      %v1 = alloca : ptr<i32>\n"
                    "      %v2 = load : i32 (%v1)\n      return %v2\n"),
         "value %v1 is defined twice", 1},
        // ── the cascade's own shape, and the neighbours that already returned ──
        {"E9 a condbr on an unknown value",
         oneFunction("    block %b1 [entry] {\n      condbr %v9 %b2 %b2\n    }\n"
                     "    block %b2 {\n      %v1 = const : i32 (lit int 1 : i32)\n"
                     "      return %v1\n    }\n"),
         "unknown value handle '%v9'", 1},
        {"E10 a phi with an unknown incoming value",
         oneFunction("    block %b1 [entry] {\n      br %b2\n    }\n"
                     "    block %b2 {\n      %v1 = phi : i32 [(%v9, %b1)]\n      return %v1\n    }\n"),
         "unknown value handle '%v9'", 1},
        {"E12 a non-power-of-two alignment",
         entryBlock("      %v1 = alloca : ptr<i32> align=3\n"
                    "      %v2 = const : i32 (lit int 2 : i32)\n      return %v2\n"),
         "alloca alignment 3 is not a power of two", 1},
    };
}

// The three lists that HUNG on truncated text. A truncation leaves every
// enclosing construct open too, and each of those is reported in its own words,
// so these pin the list's refusal and the total rather than "exactly one".
struct Truncation {
    char const* name;
    std::string text;
    char const* refusal;
};

std::vector<Truncation> measuredTruncations() {
    std::string const head = "dssir 3\nsymbols {\n  %1 \"f\"\n}\nmodule {\n"
                             "  function %1 : fn() -> i32 {\n";
    return {
        {"H1 cut inside an operand list",
         head + "    block %b1 [entry] {\n      %v1 = const : i32 (lit int 1 : i32)\n"
                "      %v2 = add : i32 (%v1,",
         "'add''s operand list is not closed on its line — the input ends inside it"},
        {"H2 cut inside a phi's incoming list",
         head + "    block %b1 [entry] {\n      br %b2\n    }\n"
                "    block %b2 {\n      %v1 = phi : i32 [(%v9, %b1)",
         "'phi''s incoming list is not closed on its line — the input ends inside it"},
        {"H3 cut inside a switch's case list",
         head + "    block %b1 [entry] {\n      %v1 = const : i32 (lit int 1 : i32)\n"
                "      switch %v1 {",
         "'switch''s case list is not closed on its line — the input ends inside it"},
    };
}

MirLiteralValue intLit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// A valid module that reaches EVERY arm of the instruction grammar — each
// dedicated reader arm, the generic arm with a payload and an alignment, every
// instruction flag — plus a global with attributes, a function attribute, every
// terminator and an SEH region. The sweep cuts and deletes its tokens.
std::string goldenModule() {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32  = in.primitive(TypeKind::I32);
    TypeId const i1   = in.primitive(TypeKind::Bool);
    TypeId const pI32 = in.pointer(i32);
    TypeId const pI8  = in.pointer(in.primitive(TypeKind::I8));
    TypeId const gSig = in.fnSig({}, i32, CallConv::CcSysV);
    std::array<TypeId, 1> const params{i32};
    TypeId const fSig = in.fnSig(params, i32, CallConv::CcSysV);
    TypeId const hSig = in.fnSig({}, in.primitive(TypeKind::Void), CallConv::CcSysV);

    MirBuilder b;
    (void)b.addGlobal(i32, SymbolId{4}, b.literalPoolAdd(intLit(7)), MirFuncId{},
                      SymbolBinding::Global, SymbolVisibility::Default, /*isConst=*/true,
                      MirThreadStorage::Shared, /*alignmentBytes=*/16);

    // f(a): every non-terminator arm, then every terminator.
    (void)b.addFunction(fSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const sw    = b.createBlock(StructCfMarker::Linear);
    MirBlockId const other = b.createBlock(StructCfMarker::Linear);
    MirBlockId const join  = b.createBlock(StructCfMarker::Linear);
    MirBlockId const exit  = b.createBlock(StructCfMarker::Linear);
    MirBlockId const dead  = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(entry);
    MirInstId const a    = b.addArg(0, i32, MirInstFlags::Synthetic);
    MirInstId const c5   = b.addConst(intLit(5), i32);
    MirInstId const c6   = b.addConst(intLit(6), i32);
    MirInstId const ga   = b.addGlobalAddr(SymbolId{2}, in.pointer(gSig), MirInstFlags::Volatile);
    MirInstId const slot = b.addInst(MirOpcode::Alloca, {}, pI32, /*bytes=*/8,
                                     MirInstFlags::Volatile, /*payload2=*/64);
    std::array<MirInstId, 2> const storeOps{c5, slot};
    b.addInst(MirOpcode::Store, storeOps, InvalidType, 0, MirInstFlags::AtomicInitExempt);
    std::array<MirInstId, 1> const loadOps{slot};
    MirInstId const ld  = b.addInst(MirOpcode::Load, loadOps, i32, 0, MirInstFlags::Volatile);
    MirInstId const ald = b.addInst(MirOpcode::AtomicLoad, loadOps, i32, /*order=*/5,
                                    MirInstFlags::None, /*payload2=*/8);
    std::array<MirInstId, 1> const callOps{ga};
    MirInstId const call = b.addInst(MirOpcode::Call, callOps, i32, 0, MirInstFlags::ReturnsTwice);
    std::array<MirInstId, 2> const addOps{ld, ald};
    MirInstId const sum = b.addInst(MirOpcode::Add, addOps, i32, 0,
                                    MirInstFlags::Synthetic | MirInstFlags::Volatile);
    std::array<MirInstId, 2> const cmpOps{a, c5};
    MirInstId const cmp = b.addInst(MirOpcode::ICmpEq, cmpOps, i1);
    b.addCondBr(cmp, sw, other);
    b.beginBlock(sw);
    std::array<std::pair<MirInstId, MirBlockId>, 2> const cases{
        std::pair<MirInstId, MirBlockId>{c5, join}, std::pair<MirInstId, MirBlockId>{c6, dead}};
    b.addSwitch(a, cases, other);
    b.beginBlock(other);
    b.addBr(join);
    b.beginBlock(join);
    std::array<MirPhiIncoming, 2> const incomings{MirPhiIncoming{sum, sw},
                                                  MirPhiIncoming{call, other}};
    MirInstId const phi = b.addPhi(i32, incomings);
    MirInstId const ba  = b.addBlockAddress(exit, pI8);
    std::array<MirBlockId, 1> const targets{exit};
    b.addIndirectBr(ba, targets);
    b.beginBlock(exit);
    b.addReturn(phi);
    b.beginBlock(dead);
    b.addUnreachable();

    // g(): a function attribute.
    (void)b.addFunction(gSig, SymbolId{2}, SymbolBinding::Global, SymbolVisibility::Default,
                        /*noInline=*/true);
    MirBlockId const gEntry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(gEntry);
    b.addReturn(b.addConst(intLit(1), i32));

    // h(): an SEH region — begin, end, filter return.
    (void)b.addFunction(hSig, SymbolId{3});
    MirBlockId const hEntry  = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const guarded = b.createBlock(StructCfMarker::Linear);
    MirBlockId const filter  = b.createBlock(StructCfMarker::Linear);
    MirBlockId const handler = b.createBlock(StructCfMarker::Linear);
    MirBlockId const hJoin   = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(hEntry);
    b.addSehTryBegin(guarded, filter, /*region=*/1);
    b.beginBlock(guarded);
    b.addInst(MirOpcode::SehTryEnd, {}, InvalidType, /*region=*/1);
    b.addBr(hJoin);
    b.beginBlock(filter);
    b.addSehFilterReturn(b.addConst(intLit(1), i32), handler, /*region=*/1);
    b.beginBlock(handler);
    b.addBr(hJoin);
    b.beginBlock(hJoin);
    b.addReturn();

    Mir m = std::move(b).finish();
    rederiveStructCfMarkers(m);   // the verifier checks stored == derived
    std::vector<std::string> const names{"", "f", "g", "h", "gv"};
    MirTextContext const ctx{&in, &names};
    DiagnosticReporter r;
    return emitMir(m, ctx, r);
}

// The token boundaries of the golden text, close enough to the reader's lexer
// for the purpose: a maximal run of identifier characters, a quoted string, the
// two-character `->`, or any other single character. Deleting one of these is a
// "single-token deletion"; where this splits a reader token in two (a signed
// literal, say), the result is still malformed input the reader must survive.
std::vector<std::pair<std::size_t, std::size_t>> tokenSpans(std::string const& s) {
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    std::size_t i = 0;
    auto const word = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    };
    while (i < s.size()) {
        char const c = s[i];
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') { ++i; continue; }
        std::size_t j = i + 1;
        if (word(c)) {
            while (j < s.size() && word(s[j])) ++j;
        } else if (c == '"') {
            while (j < s.size() && s[j] != '"') j += (s[j] == '\\' && j + 1 < s.size()) ? 2 : 1;
            if (j < s.size()) ++j;
        } else if (c == '-' && j < s.size() && s[j] == '>') {
            ++j;
        }
        spans.emplace_back(i, j);
        i = j;
    }
    return spans;
}

} // namespace

// ── 1. THE TABLE ────────────────────────────────────────────────────────────
TEST(MirTextReaderNeverAborts, EveryMeasuredShapeIsRefusedByNameAndNothingElse) {
    std::size_t index = 0;
    for (Shape const& shape : measuredShapes()) {
        SCOPED_TRACE(shape.name);
        announce("table", index++, shape.name);
        Parsed const p = parse(shape.text);
        EXPECT_FALSE(p.ok);
        bool named = false;
        for (auto const& d : p.diagnostics) {
            if (d.find(shape.refusal) != std::string::npos) named = true;
        }
        EXPECT_TRUE(named) << "expected a refusal containing \"" << shape.refusal
                           << "\"; got:" << listed(p.diagnostics);
        EXPECT_EQ(p.diagnostics.size(), shape.diagnostics)
            << "its own refusal and nothing else; got:" << listed(p.diagnostics);
    }
}

// The sentinel's mnemonic is gone from the ACCEPTED SET too — the lookup and the
// list it renders are one walk.
TEST(MirTextReaderNeverAborts, TheSentinelIsNotAdvertisedAsAnOpcode) {
    Parsed const p = parse(entryBlock("      nosuchopcode\n      return\n"));
    std::string msg;
    for (auto const& d : p.diagnostics) {
        if (d.find("unknown opcode 'nosuchopcode'") != std::string::npos) msg = d;
    }
    ASSERT_FALSE(msg.empty()) << listed(p.diagnostics);
    EXPECT_NE(msg.find("'add'"), std::string::npos) << msg;
    EXPECT_EQ(msg.find("'invalid'"), std::string::npos) << msg;
}

TEST(MirTextReaderNeverAborts, ATruncatedListIsRefusedInsteadOfLoopingForever) {
    std::size_t index = 0;
    for (Truncation const& cut : measuredTruncations()) {
        SCOPED_TRACE(cut.name);
        announce("truncated list", index++, cut.name);
        Parsed const p = parse(cut.text);
        EXPECT_FALSE(p.ok);
        bool named = false;
        for (auto const& d : p.diagnostics) {
            if (d.find(cut.refusal) != std::string::npos) named = true;
        }
        EXPECT_TRUE(named) << "expected \"" << cut.refusal << "\"; got:" << listed(p.diagnostics);
    }
}

// Every mnemonic the reader accepts, written bare — no type, no operands — must
// come back as a refusal, never reach a builder precondition. (Each opcode has
// its own operand shape, so a bare mnemonic is malformed for nearly all of them;
// it used to ABORT on the operand-count guard.)
TEST(MirTextReaderNeverAborts, EveryAcceptedMnemonicWrittenBareReturns) {
    std::size_t index = 0;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(MirOpcode::Count_); ++i) {
        auto const op = static_cast<MirOpcode>(i);
        std::string const mnemonic{opcodeInfo(op).mnemonic};
        if (op == MirOpcode::Invalid) continue;   // no spelling (the table pins it)
        for (bool const withSlot : {true, false}) {
            std::string const line = (withSlot ? "      %v1 = " : "      ") + mnemonic + "\n";
            announce("bare mnemonic", index++, oneLine(line, 60));
            Parsed const p = parse(entryBlock(line + "      unreachable\n"));
            (void)p;   // any verdict: the property is that the parse RETURNED
        }
    }
    EXPECT_GT(index, 100u) << "the opcode walk visited too few mnemonics to mean anything";
}

// ── THE OWNER'S RULE THE READER ASKS ────────────────────────────────────────
//
// `isBlockOfOpenFunction` is the one owner of "a block of the open function":
// `beginBlock`, every terminator's successor list and `addBlockAddress` enforce
// it by aborting, and the reader asks it first. It answers — never aborts — for
// any id, including one past the arena and one of another function.
TEST(MirTextReaderNeverAborts, TheBuilderAnswersWhetherABlockIsOfTheOpenFunction) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const sig = in.fnSig({}, in.primitive(TypeKind::Void), CallConv::CcSysV);
    MirBuilder b;
    EXPECT_FALSE(b.isBlockOfOpenFunction(MirBlockId{1, 0})) << "no function is open";
    (void)b.addFunction(sig, SymbolId{1});
    MirBlockId const f1 = b.createBlock(StructCfMarker::EntryBlock);
    EXPECT_TRUE(b.isBlockOfOpenFunction(f1));
    b.beginBlock(f1);
    b.addReturn();
    (void)b.addFunction(sig, SymbolId{2});
    MirBlockId const g1 = b.createBlock(StructCfMarker::EntryBlock);
    EXPECT_TRUE(b.isBlockOfOpenFunction(g1));
    EXPECT_FALSE(b.isBlockOfOpenFunction(f1)) << "a block of the PREVIOUS function";
    EXPECT_FALSE(b.isBlockOfOpenFunction(MirBlockId{}));
    EXPECT_FALSE(b.isBlockOfOpenFunction(MirBlockId{g1.v + 100, g1.arenaTag})) << "past the arena";
    b.beginBlock(g1);
    b.addReturn();
}

// ✔MEASURED P68 (lane `ht`) on x86_64 and arm64: a block address naming another
// function's block passed the builder AND the verifier, then `mir_to_lir`
// refused it — it maps blocks per function. The rule now lives in the builder,
// the owner of MIR validity, and fires there: from compiler code it is a
// programming error, so it ABORTS, naming the rule.
TEST(MirTextReaderNeverAborts, TheBuilderAbortsOnABlockAddressOfAnotherFunctionsBlock) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            TypeInterner in{CompilationUnitId{1}};
            TypeId const sig = in.fnSig({}, in.primitive(TypeKind::Void), CallConv::CcSysV);
            MirBuilder b;
            (void)b.addFunction(sig, SymbolId{1});
            MirBlockId const f1 = b.createBlock(StructCfMarker::EntryBlock);
            b.beginBlock(f1);
            b.addReturn();
            (void)b.addFunction(sig, SymbolId{2});
            MirBlockId const g1 = b.createBlock(StructCfMarker::EntryBlock);
            b.beginBlock(g1);
            (void)b.addBlockAddress(f1, in.pointer(in.primitive(TypeKind::I8)));
        },
        // `.+`, not a class: gtest's SIMPLE regex (Windows) has no `[0-9]`, and the
        // POSIX one (Linux, macOS) has no `\d` — `.` and `+` mean the same in both.
        "addBlockAddress: target MirBlockId=.+ is not a block of the open function");
}

// ── 2. THE SWEEP ────────────────────────────────────────────────────────────
TEST(MirTextReaderNeverAborts, TheGoldenModuleReadsCleanAndRoundTrips) {
    std::string const golden = goldenModule();
    DiagnosticReporter r;
    auto const res = parseMir(golden, CompilationUnitId{7}, r);
    std::vector<std::string> diags;
    for (auto const& d : r.all()) diags.push_back(d.actual);
    ASSERT_TRUE(res->ok) << "the golden must be VALID, or the sweep measures noise:" << listed(diags)
                         << "\n" << golden;
    MirTextContext const ctx{&res->interner, nullptr, &res->symbolNames};
    DiagnosticReporter r2;
    EXPECT_EQ(emitMir(res->mir, ctx, r2), golden);
}

TEST(MirTextReaderNeverAborts, EveryTruncationAndEverySingleTokenDeletionOfTheGoldenReturns) {
    std::string const golden = goldenModule();
    auto const spans = tokenSpans(golden);
    // A vacuous sweep passes for the wrong reason: pin that the golden still holds
    // every construct the sweep is meant to cut through — by construct, not by
    // size, so a shorter spelling of the same module does not red it and a golden
    // that lost a construct (✔ROD: the writer's flag list disabled) does.
    for (std::string_view const construct :
         {"global %", "[const, align=16]", "[noinline]", "arg [synthetic]", "[synthetic, volatile]",
          "align=64", "atomic_load", "call [returns_twice]", "icmp.eq", "condbr ", "switch ",
          " = phi ", "blockaddress ", "indirectbr ", "unreachable", "seh_try_begin ",
          "seh_try_end ", "seh_filter_return "}) {
        ASSERT_NE(golden.find(construct), std::string::npos)
            << "the golden no longer holds '" << construct
            << "', so the sweep no longer cuts through it:\n" << golden;
    }
    ASSERT_GT(spans.size(), 400u);
    std::size_t index = 0;
    std::size_t readClean = 0;
    // Every truncation — at EVERY character, which includes every token boundary
    // and the cuts inside a token as well.
    for (std::size_t cut = 0; cut < golden.size(); ++cut) {
        std::string const text = golden.substr(0, cut);
        std::size_t const from = cut > 60 ? cut - 60 : 0;
        announce("truncation", index++,
                 std::format("at byte {} of {}, ending '{}'", cut, golden.size(),
                             oneLine(std::string_view{golden}.substr(from, cut - from), 60)));
        if (parse(text).ok) ++readClean;
    }
    // Every single-token deletion.
    for (std::size_t t = 0; t < spans.size(); ++t) {
        auto const [start, end] = spans[t];
        std::string const text = golden.substr(0, start) + golden.substr(end);
        std::size_t const from = start > 30 ? start - 30 : 0;
        announce("deletion", index++,
                 std::format("token {} '{}' at byte {}, context '{}'", t,
                             oneLine(std::string_view{golden}.substr(start, end - start), 40),
                             start,
                             oneLine(std::string_view{golden}.substr(from, end + 30 - from), 100)));
        if (parse(text).ok) ++readClean;
    }
    // Reaching this line IS the property. The count is reported, not pinned:
    // a deletion may leave a valid module (`[volatile]` → `[]`), and which ones
    // do is not what this entry is about.
    std::fprintf(stderr, "[reader-never-aborts] %zu shapes returned, %zu of them read clean\n",
                 index, readClean);
    EXPECT_EQ(index, golden.size() + spans.size());
}

// ── 3. THE CASCADE ──────────────────────────────────────────────────────────
TEST(MirTextReaderNeverAborts, OneBadBlockCostsItsOwnDiagnosticAndLaterBlocksAreRead) {
    std::string const bad =
        "    block %b1 [entry] {\n      br %b2\n    }\n"
        "    block %b2 {\n      %v1 = const : i32 (lit int 1 : i32)\n"
        "      %v2 = bogus : i32 (%v1)\n      br %b3\n    }\n";
    // A clean later block adds nothing.
    {
        Parsed const p = parse(oneFunction(
            bad + "    block %b3 {\n      %v3 = const : i32 (lit int 3 : i32)\n"
                  "      return %v3\n    }\n"));
        EXPECT_FALSE(p.ok);
        ASSERT_EQ(p.diagnostics.size(), 1u) << listed(p.diagnostics);
        EXPECT_NE(p.diagnostics[0].find("unknown opcode 'bogus'"), std::string::npos)
            << p.diagnostics[0];
    }
    // A later block with its OWN defect is read, and that defect is reported:
    // the block was not skipped, and nothing between the two is invented.
    {
        Parsed const p = parse(oneFunction(
            bad + "    block %b3 {\n      %v3 = const : i32 (lit int 3 : i32)\n"
                  "      %v4 = add : i32 (%v3)\n      return %v3\n    }\n"));
        EXPECT_FALSE(p.ok);
        ASSERT_EQ(p.diagnostics.size(), 2u) << listed(p.diagnostics);
        EXPECT_NE(p.diagnostics[0].find("unknown opcode 'bogus'"), std::string::npos)
            << listed(p.diagnostics);
        EXPECT_NE(p.diagnostics[1].find("'add' takes 2 to 2 operand(s) and this line lists 1"),
                  std::string::npos) << listed(p.diagnostics);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PART 1c — THE SYMBOL TABLE IS SIZED BY THE TEXT, AND A NUMBER IN IT IS READ
// AS WRITTEN (P68 round 8, lane `ht`).
//
// ✔MEASURED before the fix (WSL Release): the reader sized its table by the
// largest slot the text named — `resize(v + 1)` — so `symbols { %100000000 "x" }`
// returned a 3.2 GB table from 40 bytes, `%4000000000` threw `std::bad_alloc`
// out of `parseMir`, and `%4294967295` wrapped `v + 1` to 0 and wrote past the
// empty table (SIGSEGV). The slots are raw CU SymbolIds and LEGITIMATELY SPARSE
// (804 of 804 example modules; the LIR twin legitimately holds 0xFFFFFF01), so no
// bound replaces the resize: the table holds one entry per declaration.
// ═══════════════════════════════════════════════════════════════════════════
namespace {

std::string preambleOnly(std::string const& symbols) {
    return "dssir 3\nsymbols {\n" + symbols + "}\nmodule {\n}\n";
}

struct ParsedTable {
    bool                                           ok = true;
    std::vector<std::string>                       diagnostics;
    std::unordered_map<std::uint32_t, std::string> names;
};

ParsedTable parseTable(std::string const& text) {
    DiagnosticReporter r;
    auto const res = parseMir(text, CompilationUnitId{7}, r);
    ParsedTable t;
    t.ok    = res->ok;
    t.names = res->symbolNames;
    for (auto const& d : r.all()) t.diagnostics.push_back(d.actual);
    return t;
}

bool anyHas(std::vector<std::string> const& v, std::string_view needle) {
    for (auto const& x : v) if (x.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

TEST(MirTextSymbolTable, EveryMeasuredShapeReadsAsTheOneEntryItDeclares) {
    for (std::uint32_t const slot : {100000000u, 4000000000u, 4294967295u}) {
        ParsedTable const t = parseTable(preambleOnly(std::format("  %{} \"x\"\n", slot)));
        EXPECT_TRUE(t.ok) << "%" << slot << listed(t.diagnostics);
        ASSERT_EQ(t.names.size(), 1u) << "%" << slot << " sized the table by its number";
        ASSERT_TRUE(t.names.contains(slot)) << "%" << slot << " landed on another slot";
        EXPECT_EQ(t.names.at(slot), "x");
    }
}

TEST(MirTextSymbolTable, TheSentinelARepeatAndA33BitSlotAreRefusedByName) {
    ParsedTable const zero = parseTable(preambleOnly("  %0 \"x\"\n"));
    EXPECT_FALSE(zero.ok);
    EXPECT_TRUE(zero.names.empty());
    EXPECT_TRUE(anyHas(zero.diagnostics,
                       "symbol slot %0 is the invalid-symbol sentinel and names no symbol"))
        << listed(zero.diagnostics);

    ParsedTable const twice = parseTable(preambleOnly("  %5 \"a\"\n  %5 \"b\"\n"));
    EXPECT_FALSE(twice.ok);
    EXPECT_TRUE(anyHas(twice.diagnostics, "symbol slot %5 is declared twice"))
        << listed(twice.diagnostics);
    ASSERT_EQ(twice.names.size(), 1u);
    EXPECT_EQ(twice.names.at(5), "a") << "the repeat overwrote the first declaration";

    // 2^32 + 1 is not slot 1.
    ParsedTable const wide = parseTable(preambleOnly("  %4294967297 \"x\"\n"));
    EXPECT_FALSE(wide.ok);
    EXPECT_TRUE(wide.names.empty()) << "a 33-bit slot was cut to a 32-bit one";
    EXPECT_TRUE(anyHas(wide.diagnostics,
                       "% handle value '4294967297' does not fit its 32-bit field"))
        << listed(wide.diagnostics);
    EXPECT_EQ(wide.diagnostics.size(), 1u) << "the refusal cascaded:" << listed(wide.diagnostics);
}

// The class-letter form read its digits with a hand-rolled 32-bit loop, so
// `%v4294967297` WRAPPED to `%v1` — another value.
TEST(MirTextSymbolTable, AClassLetterHandlePast32BitsIsRefusedNotWrapped) {
    Parsed const p = parse(entryBlock(
        "      %v1 = const : i32 (lit int 1 : i32)\n"
        "      %v4294967297 = const : i32 (lit int 2 : i32)\n"
        "      return %v1\n"));
    EXPECT_FALSE(p.ok);
    EXPECT_TRUE(anyHas(p.diagnostics, "% handle value '4294967297' does not fit its 32-bit field"))
        << listed(p.diagnostics);
}

// The control: the TOP of the id space is a legitimate slot, and a function
// holding it reads, re-emits under the same id, and reads again.
TEST(MirTextSymbolTable, AFunctionAtTheTopSymbolIdReadsAndRoundTrips) {
    std::string const text =
        "dssir 3\nsymbols {\n  %4294967295 \"top\"\n}\nmodule {\n"
        "  function %4294967295 : fn() -> i32 {\n" + kOneBlockF + "  }\n}\n";
    DiagnosticReporter r1;
    auto const first = parseMir(text, CompilationUnitId{7}, r1);
    ASSERT_TRUE(first->ok) << text;
    ASSERT_EQ(first->symbolNames.size(), 1u);
    ASSERT_EQ(first->mir.moduleFuncCount(), 1u);
    EXPECT_EQ(first->mir.funcSymbol(first->mir.funcAt(0)).v, 4294967295u);
    MirTextContext ctx{&first->interner, nullptr, &first->symbolNames};
    DiagnosticReporter w;
    std::string const again = emitMir(first->mir, ctx, w);
    EXPECT_NE(again.find("%4294967295 \"top\""), std::string::npos) << again;
    DiagnosticReporter r2;
    auto const second = parseMir(again, CompilationUnitId{7}, r2);
    ASSERT_TRUE(second->ok) << again;
    EXPECT_EQ(second->mir.funcSymbol(second->mir.funcAt(0)).v, 4294967295u);
    EXPECT_EQ(second->symbolNames.at(4294967295u), "top");
}

// ═══════════════════════════════════════════════════════════════════════════
// THE LOWERING'S OWN SYMBOLS, MINTED PAST THE MODULE'S HIGH-WATER MARK
// (P68 round 8, lane `ht`, part 1c).
//
// `mir_to_lir` mints block, jump-table and sign-mask symbols past the highest
// function / global / extern SymbolId. The seed was `maxV + 1u`, written twice,
// and a module read from TEXT can hold the TOP id (the sparse table above makes
// that a legitimate text): the seed WRAPPED to 0 — the invalid sentinel — and the
// next mint was 1, an id the module may own. On the way it could also hand out
// 0xFFFFFF01, the writer-reserved PE `_tls_index` singleton. Both minters now draw
// through ONE `mintPastHighWater`, which steps over the reserved id by the owner's
// predicate and refuses exhaustion ONCE, by name (`L_SymbolIdSpaceExhausted`).
// Each module here is emitted to `.dssir` and READ BACK before it is lowered — the
// entrance that can hold such an id.
// ═══════════════════════════════════════════════════════════════════════════
namespace {

constexpr std::uint32_t kTopSymbolId = 0xFFFF'FFFFu;

// A THROW, not an abort: GoogleTest reports a throw as the failure of the ONE test
// that asked, where an abort would take every sibling in this binary with it.
std::shared_ptr<TargetSchema> shippedX86() {
    auto target = TargetSchema::loadShipped("x86_64");
    if (!target) throw std::runtime_error("TargetSchema::loadShipped(x86_64) failed");
    return *target;
}

MirLiteralValue i32Literal(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// One function `sym` that takes the addresses of TWO of its blocks (two mints)
// and branches through them.
Mir blockAddressModule(TypeInterner& ti, std::uint32_t sym) {
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const vptr  = ti.pointer(ti.primitive(TypeKind::Void));
    TypeId const fnSig = ti.fnSig(std::span<TypeId const>{}, i32, CallConv::CcSysV);
    MirBuilder b;
    b.addFunction(fnSig, SymbolId{sym});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const t1    = b.createBlock(StructCfMarker::Linear);
    MirBlockId const t2    = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(entry);
    MirInstId const a1 = b.addBlockAddress(t1, vptr);
    (void)b.addBlockAddress(t2, vptr);
    std::array<MirBlockId, 2> succs{t1, t2};
    b.addIndirectBr(a1, succs);
    b.beginBlock(t1);
    b.addReturn(b.addConst(i32Literal(1), i32));
    b.beginBlock(t2);
    b.addReturn(b.addConst(i32Literal(2), i32));
    return std::move(b).finish();
}

// One function `sym` whose switch is dense enough for a jump table (10 cases,
// 0..9 — past the lowering's 8-case floor, span == count).
Mir denseSwitchModule(TypeInterner& ti, std::uint32_t sym) {
    constexpr std::size_t kCases = 10;
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const voidT = ti.primitive(TypeKind::Void);
    std::array<TypeId, 1> params{i32};
    TypeId const fnSig = ti.fnSig(params, voidT, CallConv::CcSysV);
    MirBuilder b;
    b.addFunction(fnSig, SymbolId{sym});
    MirBlockId const entry = b.createBlock(StructCfMarker::SwitchHead);
    std::array<MirBlockId, kCases> caseBlocks{};
    for (auto& cb : caseBlocks) cb = b.createBlock(StructCfMarker::SwitchCase);
    MirBlockId const dflt = b.createBlock(StructCfMarker::SwitchCase);
    b.beginBlock(entry);
    MirInstId const disc = b.addArg(0, i32);
    std::array<std::pair<MirInstId, MirBlockId>, kCases> cases{};
    for (std::size_t i = 0; i < kCases; ++i) {
        cases[i] = {b.addConst(i32Literal(static_cast<std::int64_t>(i)), i32), caseBlocks[i]};
    }
    b.addSwitch(disc, cases, dflt);
    for (auto const cb : caseBlocks) { b.beginBlock(cb); b.addReturn(); }
    b.beginBlock(dflt);
    b.addReturn();
    Mir m = std::move(b).finish();
    rederiveStructCfMarkers(m);   // the reader's verifier checks stored == derived
    return m;
}

// Emit to `.dssir` naming only `sym`, and read it back.
std::unique_ptr<MirParseResult> throughText(Mir const& m, TypeInterner const& ti,
                                            std::uint32_t sym) {
    std::unordered_map<std::uint32_t, std::string> const names{{sym, "f"}};
    MirTextContext ctx{&ti, nullptr, &names};
    DiagnosticReporter w;
    std::string const text = emitMir(m, ctx, w);
    DiagnosticReporter r;
    auto parsed = parseMir(text, CompilationUnitId{7}, r);
    std::vector<std::string> diags;
    for (auto const& d : r.all()) diags.push_back(d.actual);
    EXPECT_TRUE(parsed->ok) << text << listed(diags);
    return parsed;
}

struct Lowered {
    MirToLirResult           result;
    std::vector<std::string> exhausted;   // every L_SymbolIdSpaceExhausted, as text
    std::vector<std::string> errors;      // every Error, as `code: text`
};

Lowered lowerRead(MirParseResult const& parsed, DiagnosticReporter::Config cfg = {}) {
    auto const sch = shippedX86();
    DiagnosticReporter rep{cfg};
    Lowered out{lowerToLir(parsed.mir, *sch, parsed.interner, rep), {}, {}};
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_SymbolIdSpaceExhausted) {
            EXPECT_EQ(d.severity, DiagnosticSeverity::Error);
            out.exhausted.push_back(d.actual);
        }
        if (d.severity == DiagnosticSeverity::Error) {
            out.errors.push_back(std::format("{}: {}", diagnosticCodeName(d.code), d.actual));
        }
    }
    return out;
}

// Every SymbolId a SymbolRef operand of the lowered module names.
std::vector<std::uint32_t> symbolRefs(Lir const& lir) {
    std::vector<std::uint32_t> out;
    for (std::uint32_t fi = 0; fi < lir.moduleFuncCount(); ++fi) {
        LirFuncId const f = lir.funcAt(fi);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(f); ++bi) {
            LirBlockId const blk = lir.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(blk); ++ii) {
                for (LirOperand const& op : lir.instOperands(lir.blockInstAt(blk, ii))) {
                    if (op.kind == LirOperandKind::SymbolRef) out.push_back(op.symbolV);
                }
            }
        }
    }
    return out;
}

} // namespace

// A block address in a function at the top id: nothing is left to mint.
// Refused ONCE (two block addresses, one refusal), by name, and no minted id
// lands on 1 — the id the old wrap handed out second.
TEST(LoweringMintsPastTheHighWater, ABlockAddressAtTheTopIdIsRefusedOnceByName) {
    TypeInterner ti{CompilationUnitId{1}};
    auto const parsed = throughText(blockAddressModule(ti, kTopSymbolId), ti, kTopSymbolId);
    ASSERT_EQ(parsed->mir.funcSymbol(parsed->mir.funcAt(0)).v, kTopSymbolId);
    Lowered const low = lowerRead(*parsed);
    ASSERT_EQ(low.exhausted.size(), 1u) << "wanted ONE refusal:" << listed(low.errors);
    EXPECT_NE(low.exhausted[0].find("cannot mint a block symbol"), std::string::npos)
        << low.exhausted[0];
    EXPECT_NE(low.exhausted[0].find("4294967295"), std::string::npos) << low.exhausted[0];
    for (std::uint32_t const v : symbolRefs(low.result.lir)) {
        EXPECT_TRUE(v == 0 || v == kTopSymbolId)
            << "a block symbol was minted as " << v << " — past the top, i.e. wrapped";
    }
}

TEST(LoweringMintsPastTheHighWater, ADenseSwitchAtTheTopIdIsRefusedOnceByName) {
    TypeInterner ti{CompilationUnitId{1}};
    auto const parsed = throughText(denseSwitchModule(ti, kTopSymbolId), ti, kTopSymbolId);
    ASSERT_EQ(parsed->mir.funcSymbol(parsed->mir.funcAt(0)).v, kTopSymbolId);
    Lowered const low = lowerRead(*parsed);
    ASSERT_EQ(low.exhausted.size(), 1u) << "wanted ONE refusal:" << listed(low.errors);
    EXPECT_NE(low.exhausted[0].find("cannot mint a jump-table or sign-mask symbol"),
              std::string::npos) << low.exhausted[0];
    for (JumpTableDescriptor const& jt : low.result.jumpTableDescriptors) {
        EXPECT_FALSE(jt.tableSymbol.valid() && jt.tableSymbol.v != kTopSymbolId)
            << "the table symbol was minted as " << jt.tableSymbol.v;
        for (auto const& [blk, sym] : jt.blockSymbols) {
            EXPECT_FALSE(sym.valid()) << "block " << blk << "'s symbol was minted as " << sym.v;
        }
    }
}

// One below the reserved id: the next id the minter would draw IS the reserved
// `_tls_index` singleton. It is stepped over — the table symbol is 0xFFFFFF02 —
// never aliased, on both paths.
TEST(LoweringMintsPastTheHighWater, TheWriterReservedIdIsSteppedOverNotAliased) {
    static_assert(isWriterReservedSymbolIdValue(kTlsIndexReservedSymbolIdValue));
    constexpr std::uint32_t kBelow = kTlsIndexReservedSymbolIdValue - 1u;
    constexpr std::uint32_t kPast  = kTlsIndexReservedSymbolIdValue + 1u;
    {
        TypeInterner ti{CompilationUnitId{1}};
        auto const parsed = throughText(denseSwitchModule(ti, kBelow), ti, kBelow);
        Lowered const low = lowerRead(*parsed);
        EXPECT_TRUE(low.errors.empty()) << listed(low.errors);
        ASSERT_EQ(low.result.jumpTableDescriptors.size(), 1u);
        EXPECT_EQ(low.result.jumpTableDescriptors[0].tableSymbol.v, kPast)
            << "the first mint past 0xFFFFFF00 was not the id after the reserved one";
        for (auto const& [blk, sym] : low.result.jumpTableDescriptors[0].blockSymbols) {
            EXPECT_GT(sym.v, kTlsIndexReservedSymbolIdValue)
                << "block " << blk << "'s symbol " << sym.v << " is not past the reserved id";
        }
    }
    {
        TypeInterner ti{CompilationUnitId{1}};
        auto const parsed = throughText(blockAddressModule(ti, kBelow), ti, kBelow);
        Lowered const low = lowerRead(*parsed);
        EXPECT_TRUE(low.errors.empty()) << listed(low.errors);
        std::vector<std::uint32_t> const refs = symbolRefs(low.result.lir);
        EXPECT_NE(std::find(refs.begin(), refs.end(), kPast), refs.end())
            << "no block address was bound to the id after the reserved one";
        EXPECT_EQ(std::find(refs.begin(), refs.end(), kTlsIndexReservedSymbolIdValue), refs.end())
            << "a block address was aliased onto the writer-reserved _tls_index id";
    }
}

// ★ THE REFUSAL IS UNSUPPRESSABLE: a user's `--suppress` or a demotion cannot
// turn it off, because without it the module the error gate withholds would
// ship with a block address aliased to an id the module owns. This is the
// behaviour the `kUnsuppressableCodes` row buys, pinned by what it does.
TEST(LoweringMintsPastTheHighWater, TheExhaustionRefusalSurvivesSuppressionAndDemotion) {
    TypeInterner ti{CompilationUnitId{1}};
    auto const parsed = throughText(blockAddressModule(ti, kTopSymbolId), ti, kTopSymbolId);
    DiagnosticReporter::Config cfg;
    cfg.policy.suppress.insert(DiagnosticCode::L_SymbolIdSpaceExhausted);
    cfg.policy.overrides[DiagnosticCode::L_SymbolIdSpaceExhausted] = DiagnosticSeverity::Warning;
    Lowered const low = lowerRead(*parsed, cfg);
    ASSERT_EQ(low.exhausted.size(), 1u)
        << "a suppression or a demotion silenced the exhaustion refusal:" << listed(low.errors);
    EXPECT_TRUE(std::any_of(low.errors.begin(), low.errors.end(), [](std::string const& e) {
        return e.starts_with("L_SymbolIdSpaceExhausted: ");
    })) << "the refusal was demoted below Error:" << listed(low.errors);
}

// ═══════════════════════════════════════════════════════════════════════════
// PART 1c-b — THE MIR LITERAL TWINS (P68 round 8, lane `ht`): the SAME spellings
// the HIR reader was held to in 1c-a, held to the SAME rules. The MIR `bitint` arm
// `reserve`d its declared limb count before reading one limb (`1000000000000`
// asked for 8 TB) and then looped that many times past the end of the text; it
// narrowed a 64-bit width to 32 unchecked and bounded neither the width nor the
// signedness. `wfloat` read every width but 128 as F80, and its writer spelled a
// kind with no width as 80. The `_BitInt(N)` TYPE width was interned as it stood.
// ═══════════════════════════════════════════════════════════════════════════
namespace {

std::string literalGlobal(std::string const& type, std::string const& value) {
    return "dssir 3\nsymbols {\n  %1 \"g\"\n}\nmodule {\n  global %1 : " + type +
           " = lit " + value + "\n}\n";
}

std::string zeroGlobal(std::string const& type) {
    return "dssir 3\nsymbols {\n  %1 \"g\"\n}\nmodule {\n  global %1 : " + type +
           " = zero\n}\n";
}

} // namespace

TEST(MirTextLiteralTwins, ABitIntLiteralIsReadAsTheTextHoldsIt) {
    Parsed const undeclared =
        parse(literalGlobal("_BitInt(64)", "bitint 64 0 1000000000000 5 : bitint"));
    EXPECT_FALSE(undeclared.ok);
    EXPECT_TRUE(anyHas(undeclared.diagnostics,
                       "bitint literal declares 1000000000000 limbs and the text holds 1"))
        << listed(undeclared.diagnostics);
    for (std::string const w : {std::string{"0"}, std::to_string(std::uint64_t{kBitIntMaxWidth} + 1)}) {
        Parsed const p = parse(literalGlobal("_BitInt(64)", "bitint " + w + " 0 1 5 : bitint"));
        EXPECT_FALSE(p.ok);
        EXPECT_TRUE(anyHas(p.diagnostics, "bitint literal width " + w + " is not a _BitInt width"))
            << "width " << w << listed(p.diagnostics);
    }
    Parsed const wide = parse(literalGlobal("_BitInt(64)", "bitint 4294967297 0 1 5 : bitint"));
    EXPECT_FALSE(wide.ok);
    EXPECT_TRUE(anyHas(wide.diagnostics, "bitint width value '4294967297' does not fit its 32-bit field"))
        << listed(wide.diagnostics);
    Parsed const sign = parse(literalGlobal("_BitInt(8)", "bitint 8 2 1 5 : bitint"));
    EXPECT_FALSE(sign.ok);
    EXPECT_TRUE(anyHas(sign.diagnostics,
                       "bitint literal signedness 2 is not 0 (unsigned) or 1 (signed)"))
        << listed(sign.diagnostics);
    for (std::string_view const s : {"0", "1"}) {
        Parsed const ok = parse(literalGlobal("_BitInt(8)", std::format("bitint 8 {} 1 5 : bitint", s)));
        EXPECT_TRUE(ok.ok) << "signedness " << s << listed(ok.diagnostics);
    }
}

TEST(MirTextLiteralTwins, TheBitIntTypeWidthIsOneTheModelDefines) {
    std::string const past = std::to_string(std::uint64_t{kBitIntMaxWidth} + 1);
    for (std::string const& w : {std::string{"0"}, std::string{"-1"}, past}) {
        Parsed const p = parse(zeroGlobal("_BitInt(" + w + ")"));
        EXPECT_TRUE(anyHas(p.diagnostics, "_BitInt width " + w + " is not a _BitInt width"))
            << "_BitInt(" << w << ")" << listed(p.diagnostics);
    }
    DiagnosticReporter r;
    auto const res = parseMir("dssir 3\nsymbols {\n  %1 \"g\"\n}\nmodule {\n  global %1 : _BitInt(" +
                                  std::to_string(kBitIntMaxWidth) + ") = zero\n}\n",
                              CompilationUnitId{7}, r);
    std::vector<std::string> diags;
    for (auto const& d : r.all()) diags.push_back(d.actual);
    EXPECT_TRUE(res->ok) << "the model's widest width was refused" << listed(diags);
}

TEST(MirTextLiteralTwins, AWideFloatWidthIsOneTheModelDefines) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const f80  = ti.primitive(TypeKind::F80);
    TypeId const f128 = ti.primitive(TypeKind::F128);
    WideFloatValue const v80  = WideFloatValue::fromDouble(1.5, TypeKind::F80);
    WideFloatValue const v128 = WideFloatValue::fromDouble(-0.1, TypeKind::F128);
    MirBuilder b;
    MirLiteralValue l80;  l80.value = v80;   l80.core = TypeKind::F80;
    MirLiteralValue l128; l128.value = v128; l128.core = TypeKind::F128;
    b.addGlobal(f80, SymbolId{1}, b.literalPoolAdd(std::move(l80)), MirFuncId{},
                SymbolBinding::Global, SymbolVisibility::Default, false, MirThreadStorage::Shared);
    b.addGlobal(f128, SymbolId{2}, b.literalPoolAdd(std::move(l128)), MirFuncId{},
                SymbolBinding::Global, SymbolVisibility::Default, false, MirThreadStorage::Shared);
    Mir const m = std::move(b).finish();
    std::unordered_map<std::uint32_t, std::string> const names{{1, "a"}, {2, "b"}};
    MirTextContext ctx{&ti, nullptr, &names};
    DiagnosticReporter w;
    std::string const text = emitMir(m, ctx, w);
    EXPECT_NE(text.find("wfloat 80 "), std::string::npos) << text;
    EXPECT_NE(text.find("wfloat 128 "), std::string::npos) << text;
    DiagnosticReporter r;
    auto const back = parseMir(text, CompilationUnitId{7}, r);
    ASSERT_TRUE(back->ok) << text;
    auto const* b80 = std::get_if<WideFloatValue>(
        &back->mir.literalValue(back->mir.globalInitLiteralIndex(back->mir.globalAt(0))).value);
    auto const* b128 = std::get_if<WideFloatValue>(
        &back->mir.literalValue(back->mir.globalInitLiteralIndex(back->mir.globalAt(1))).value);
    ASSERT_NE(b80, nullptr);
    ASSERT_NE(b128, nullptr);
    EXPECT_EQ(*b80, v80);
    EXPECT_EQ(*b128, v128);

    std::string bad = text;
    std::size_t const at = bad.find("wfloat 80 ");
    ASSERT_NE(at, std::string::npos);
    bad.replace(at, std::string_view{"wfloat 80 "}.size(), "wfloat 64 ");
    Parsed const p = parse(bad);
    EXPECT_FALSE(p.ok);
    EXPECT_TRUE(anyHas(p.diagnostics, "wfloat width 64 is not a wide-float format this model "
                                      "defines — accepted: 80, 128"))
        << listed(p.diagnostics);
}

// The writer asks the one width table too: the only `WideFloatValue` kind it has
// no row for (the inert F64 a default-constructed value carries) is refused,
// never spelled 80.
TEST(MirTextLiteralTwins, AWideFloatKindWithNoWidthIsRefusedByTheWriter) {
    ASSERT_FALSE(WideFloatValue::formatBitWidth(WideFloatValue{}.kind()).has_value());
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const f80 = ti.primitive(TypeKind::F80);
    MirBuilder b;
    MirLiteralValue lit; lit.value = WideFloatValue{}; lit.core = TypeKind::F80;
    b.addGlobal(f80, SymbolId{1}, b.literalPoolAdd(std::move(lit)), MirFuncId{},
                SymbolBinding::Global, SymbolVisibility::Default, false, MirThreadStorage::Shared);
    Mir const m = std::move(b).finish();
    std::unordered_map<std::uint32_t, std::string> const names{{1, "a"}};
    MirTextContext ctx{&ti, nullptr, &names};
    DiagnosticReporter w;
    std::string const text = emitMir(m, ctx, w);
    std::vector<std::string> diags;
    for (auto const& d : w.all()) diags.push_back(d.actual);
    EXPECT_TRUE(anyHas(diags, "has no `wfloat` width — accepted: 80, 128")) << listed(diags);
    EXPECT_EQ(text.find("wfloat 80"), std::string::npos) << "the kind was spelled 80:\n" << text;
}

// A `_BitInt` initializer is WRITTEN at all: the literal-core table had no row for
// `BitInt`, so the writer refused every one (`?`) and the reader refused the `?`.
TEST(MirTextLiteralTwins, ABitIntInitializerIsWrittenAndRoundTrips) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const bt = ti.bitInt(70, /*isSigned=*/true);
    BitIntValue const value(std::vector<std::uint64_t>{12345u, 0u}, 70u, true);
    MirBuilder b;
    MirLiteralValue lit;
    lit.value = value;
    lit.core  = TypeKind::BitInt;
    b.addGlobal(bt, SymbolId{1}, b.literalPoolAdd(std::move(lit)), MirFuncId{},
                SymbolBinding::Global, SymbolVisibility::Default, false, MirThreadStorage::Shared);
    Mir const m = std::move(b).finish();
    std::unordered_map<std::uint32_t, std::string> const names{{1, "big"}};
    MirTextContext ctx{&ti, nullptr, &names};
    DiagnosticReporter w;
    std::string const text = emitMir(m, ctx, w);
    std::vector<std::string> werr;
    for (auto const& d : w.all()) werr.push_back(d.actual);
    EXPECT_TRUE(werr.empty()) << "the writer refused a _BitInt initializer:" << listed(werr);
    EXPECT_NE(text.find(": bitint"), std::string::npos) << text;
    DiagnosticReporter r;
    auto const back = parseMir(text, CompilationUnitId{7}, r);
    std::vector<std::string> rerr;
    for (auto const& d : r.all()) rerr.push_back(d.actual);
    ASSERT_TRUE(back->ok) << text << listed(rerr);
    auto const* got = std::get_if<BitIntValue>(
        &back->mir.literalValue(back->mir.globalInitLiteralIndex(back->mir.globalAt(0))).value);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(*got, value);
    MirTextContext ctx2{&back->interner, nullptr, &back->symbolNames};
    DiagnosticReporter w2;
    EXPECT_EQ(emitMir(back->mir, ctx2, w2), text);
}
