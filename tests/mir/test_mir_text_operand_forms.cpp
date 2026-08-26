// D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS — the three
// `.dssir` forms whose WRITER rendered an operand/payload tail that the READER
// had no arm for, plus the class-level guard that makes a fourth one loud.
//
// ★★★ WHY THESE ARE THEIR OWN FILE AND NOT ROWS IN `test_mir_text.cpp`.
// The pre-fix behaviour of the `indirectbr` arm was a PROCESS ABORT inside
// `MirBuilder::addInst` (a terminator reached through the non-terminator API),
// not a failed expectation. A round trip that aborts takes every sibling
// assertion in the same binary down with it, so the forms that abort live where
// a crash names them rather than the file that happened to run first.
//
// ⚠ EACH TEST HERE ASSERTS THE ROUND TRIP IS BYTE-IDENTICAL, which is the
// contract `mir_text.hpp` states — NOT merely that the parse reported no error.
// A reader that drops a payload and re-emits the default is a SUCCESSFUL parse;
// only the second emit can see the loss.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_text.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

struct RoundTrip {
    std::string firstEmit;
    std::string secondEmit;
    bool        parseOk = false;
};

RoundTrip roundTrip(Mir const& mir, TypeInterner const& interner,
                    std::vector<std::string> const& names) {
    DiagnosticReporter r1, r2, r3;
    MirTextContext ctx{&interner, &names};
    std::string first = emitMir(mir, ctx, r1);
    auto parsed = parseMir(first, CompilationUnitId{1}, r2);
    MirTextContext ctx2{&parsed->interner, &parsed->symbolNames};
    std::string second = emitMir(parsed->mir, ctx2, r3);
    return {std::move(first), std::move(second), parsed->ok};
}

} // namespace

// ── (a) `indirectbr` — the SUSPECT the row named, executed ────────────────────
//
// The writer renders ` %v<addr> { %b1, %b2 }` — an operand and the whole
// address-taken successor list, no parens. The reader had no `IndirectBr` arm at
// all, so the generic `default:` read ZERO operands and called
// `MirBuilder::addInst`, which refuses a TERMINATOR through that entry point and
// ABORTS the process.
//
// ⚠ THE ROW INFERRED THE WRONG GUARD. It predicted the ARITY refusal
// (`takes [1, 1] operands but got 0`) from `opcodeInfo(IndirectBr)`. The
// terminator refusal in `addInst` is tested FIRST and fires first, so the abort
// text is the terminator one. Both abort; the row's mechanism was wrong and its
// verdict was right, which is exactly why a suspect gets executed.
TEST(MirTextOperandForms, IndirectBrRoundTripsThroughTheReader) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const vptr  = ti.pointer(ti.primitive(TypeKind::Void));
    TypeId const fnSig = ti.fnSig(std::span<TypeId const>{}, i32, CallConv::CcSysV);

    MirBuilder b;
    b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const t1    = b.createBlock(StructCfMarker::Linear);
    MirBlockId const t2    = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(entry);
    MirInstId const addr = b.addBlockAddress(t1, vptr);
    std::array<MirBlockId, 2> succs{t1, t2};
    b.addIndirectBr(addr, succs);
    b.beginBlock(t1);
    b.addReturn(b.addConst(i32Lit(1), i32));
    b.beginBlock(t2);
    b.addReturn(b.addConst(i32Lit(2), i32));
    Mir m = std::move(b).finish();

    std::vector<std::string> names{"", "cg"};
    RoundTrip const rt = roundTrip(m, ti, names);
    EXPECT_NE(rt.firstEmit.find("indirectbr"), std::string::npos)
        << "the writer no longer renders the mnemonic this pin is about:\n"
        << rt.firstEmit;
    EXPECT_TRUE(rt.parseOk) << rt.firstEmit;
    EXPECT_EQ(rt.firstEmit, rt.secondEmit);
}

// ── (a′) `indirectbr` REACHED AS A MNEMONIC — the isolated case ──────────────
//
// ★★★ THIS ARM EXISTS BECAUSE THE TEST ABOVE CANNOT DECIDE THE QUESTION, and
// that is a measurement rather than a style preference. ✔MEASURED: in text the
// WRITER produces, `blockaddress` always precedes the `indirectbr` that consumes
// it — a computed goto has to materialize the address first — so the reader
// desyncs on `blockaddress`'s unconsumed ` %bN` tail and the recovery
// re-tokenizes the `indirectbr` line into `%`, `v1`, `{` … The mnemonic is never
// looked up, `MirBuilder::addInst` is never called with it, and the parse ends
// as a refusal with an empty module.
//
// So the `indirectbr` arm can only be reached from text where nothing before it
// has desynced — hand-written `.dssir`, or writer output once (b) is fixed. This
// input reaches it directly.
TEST(MirTextOperandForms, HandWrittenIndirectBrIsReadBack) {
    std::string const text =
        "dssir 1\n"
        "symbols {\n"
        "  %1 \"cg\"\n"
        "}\n"
        "module {\n"
        "  function %1 : fn() -> i32 {\n"
        "    block %b1 [entry] {\n"
        "      %v1 = const : i32 (lit int 0 : i32)\n"
        "      indirectbr %v1 { %b2 }\n"
        "    }\n"
        "    block %b2 {\n"
        "      %v3 = const : i32 (lit int 5 : i32)\n"
        "      return %v3\n"
        "    }\n"
        "  }\n"
        "}\n";
    DiagnosticReporter r;
    auto parsed = parseMir(text, CompilationUnitId{1}, r);
    ASSERT_TRUE(parsed->ok);
    DiagnosticReporter r2;
    MirTextContext ctx{&parsed->interner, &parsed->symbolNames};
    std::string const reemit = emitMir(parsed->mir, ctx, r2);
    EXPECT_EQ(text, reemit);
}

// ── (b) `blockaddress` — the payload IS the target block ─────────────────────
//
// The writer renders ` %b<payload>`; the reader's generic arm read no payload at
// all and built `blockaddress` with payload 0 — a DIFFERENT block, silently —
// and then choked on the leftover `%b3` while parsing the NEXT instruction.
// Loud eventually, wrong first.
//
// ★ The byte-comparison is what sees the wrong-first half: a payload-0
// `blockaddress` re-emits as `%b0`, so `firstEmit != secondEmit` even on a run
// where the trailing tokens happened to recover.
TEST(MirTextOperandForms, BlockAddressCarriesItsTargetBlockThroughText) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const vptr  = ti.pointer(ti.primitive(TypeKind::Void));
    TypeId const fnSig = ti.fnSig(std::span<TypeId const>{}, i32, CallConv::CcSysV);

    MirBuilder b;
    b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tgt   = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(entry);
    MirInstId const addr = b.addBlockAddress(tgt, vptr);
    std::array<MirBlockId, 1> succs{tgt};
    b.addIndirectBr(addr, succs);
    b.beginBlock(tgt);
    b.addReturn(b.addConst(i32Lit(3), i32));
    Mir m = std::move(b).finish();

    std::vector<std::string> names{"", "cg"};
    RoundTrip const rt = roundTrip(m, ti, names);
    EXPECT_NE(rt.firstEmit.find("blockaddress"), std::string::npos) << rt.firstEmit;
    EXPECT_TRUE(rt.parseOk) << rt.firstEmit;
    EXPECT_EQ(rt.firstEmit, rt.secondEmit);
    // The target is not block 0 — a reader that drops the payload re-emits `%b0`
    // and this states the fact directly rather than only through the byte compare.
    EXPECT_EQ(rt.secondEmit.find("blockaddress : ptr<void> %b0"), std::string::npos)
        << rt.secondEmit;
}

// D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED: `blockaddress_export` carries
// its SYMBOL in the payload and reaches its block THROUGH its operand, so both must
// survive the text. It rides the paired GENERIC arms on both sides rather than
// getting explicit ones — which is a claim about this format, not an assumption, so
// it is measured here. The class guard this file exists for (a writer arm that
// renders a field its reader arm drops) fires as a byte mismatch OR as the
// unconsumed-tail refusal; both are covered by the two assertions below.
TEST(MirTextOperandForms, BlockAddressExportCarriesItsSymbolAndOperandThroughText) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const vptr  = ti.pointer(ti.primitive(TypeKind::Void));
    TypeId const fnSig = ti.fnSig(std::span<TypeId const>{}, i32, CallConv::CcSysV);

    MirBuilder b;
    b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tgt   = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(entry);
    MirInstId const addr = b.addBlockAddress(tgt, vptr);
    // SymbolId 9 — deliberately NOT 1 (the function's own) and NOT 0 (the invalid
    // sentinel the writer suppresses), so a dropped payload re-emits a DIFFERENT
    // string instead of coincidentally the same one.
    b.addBlockAddressExport(addr, SymbolId{9});
    std::array<MirBlockId, 1> succs{tgt};
    b.addIndirectBr(addr, succs);
    b.beginBlock(tgt);
    b.addReturn(b.addConst(i32Lit(3), i32));
    Mir m = std::move(b).finish();

    std::vector<std::string> names{"", "cg"};
    RoundTrip const rt = roundTrip(m, ti, names);
    EXPECT_NE(rt.firstEmit.find("blockaddress_export"), std::string::npos)
        << rt.firstEmit;
    EXPECT_TRUE(rt.parseOk) << rt.firstEmit;
    EXPECT_EQ(rt.firstEmit, rt.secondEmit);
    // The symbol is stated directly, not only through the byte compare: a reader
    // that dropped the payload would re-emit the instruction with NO payload tail
    // at all, and `9` would vanish from the text.
    EXPECT_NE(rt.secondEmit.find("payload 9"), std::string::npos) << rt.secondEmit;
}

// ── (c) `byvaluestackarg` — a PACKED payload, spelled in two fields ──────────
//
// The writer renders ` size <bytes> exhaust <class>`; the reader's generic arm
// looked only for the literal keyword `payload`, so both fields were dropped
// (size 0, exhaust none) and the words `size 16 exhaust gpr` were then
// re-tokenized as the next instruction — where `size` is not a mnemonic.
TEST(MirTextOperandForms, ByValueStackArgCarriesSizeAndExhaustClass) {
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const vptr  = ti.pointer(ti.primitive(TypeKind::Void));
    TypeId const fnSig = ti.fnSig(std::span<TypeId const>{}, i32, CallConv::CcSysV);

    MirBuilder b;
    b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const home = b.addConst(i32Lit(0), i32);
    std::array<MirInstId, 1> carrier{home};
    MirInstId const bvsa = b.addInst(
        MirOpcode::ByValueStackArg, carrier, vptr,
        /*payload=*/std::uint32_t{24}
            | (static_cast<std::uint32_t>(kByValueStackArgExhaustGpr)
               << kByValueStackArgExhaustShift));
    (void)bvsa;
    b.addReturn(home);
    Mir m = std::move(b).finish();

    std::vector<std::string> names{"", "bv"};
    RoundTrip const rt = roundTrip(m, ti, names);
    EXPECT_NE(rt.firstEmit.find("byvaluestackarg"), std::string::npos) << rt.firstEmit;
    EXPECT_TRUE(rt.parseOk) << rt.firstEmit;
    EXPECT_EQ(rt.firstEmit, rt.secondEmit);
    // State the two fields directly: a reader that drops the payload re-emits
    // `size 0 exhaust none`, which the byte compare catches but does not NAME.
    EXPECT_NE(rt.secondEmit.find("size 24 exhaust gpr"), std::string::npos)
        << rt.secondEmit;
}

// ── the same class in the TYPE grammar, found while fixing the above ────────
//
// `appendType` and `parseType` are the other write-then-read pair in this file,
// and the same asymmetry was in it: the writer rendered spellings the reader had
// no keyword for. These were not in the row; they were found by reading the two
// switches against each other, which is what the row's own fix required anyway.

TEST(MirTextOperandForms, ComplexTypeRoundTripsThroughTheTypeGrammar) {
    // C99 `_Complex` (D-CSUBSET-COMPLEX). The writer's own comment states that a
    // complex slot IS a `Ptr<complex<elem>>` in MIR, so this spelling is on the
    // path of every module that survives `_Complex` lowering — and `parseType`
    // had no `complex` keyword, so all of them emitted text that came back
    // `unknown type 'complex'`.
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const f64     = ti.primitive(TypeKind::F64);
    TypeId const cplx    = ti.complex(f64);
    TypeId const slot    = ti.pointer(cplx);
    TypeId const voidTy  = ti.primitive(TypeKind::Void);
    TypeId const fnSig   = ti.fnSig(std::span<TypeId const>{}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    (void)b.addInst(MirOpcode::Alloca, {}, slot, 0);
    b.addReturn();
    Mir m = std::move(b).finish();

    std::vector<std::string> names{"", "f"};
    RoundTrip const rt = roundTrip(m, ti, names);
    EXPECT_NE(rt.firstEmit.find("ptr<complex<f64>>"), std::string::npos) << rt.firstEmit;
    EXPECT_TRUE(rt.parseOk) << rt.firstEmit;
    EXPECT_EQ(rt.firstEmit, rt.secondEmit);
}

TEST(MirTextOperandForms, BitIntTypeHasASpellingInBothDirections) {
    // C23 `_BitInt(N)` (D-CSUBSET-BITINT). `kMirTextPrimTable` carries no row for
    // `BitInt` — deliberately, it is not a bare keyword — and `appendType` had no
    // arm for it either, so the writer fell into `default:`, found `primName`
    // empty and emitted `?`: a MIR module holding a bit-precise type could be
    // dumped and never read back. The spelling is the `.dsshir` tier's, which is
    // the same rule this file already applies to the shared `bitint` LITERAL.
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const bi37   = ti.bitInt(37, /*isSigned=*/true);
    TypeId const bu9    = ti.bitInt(9, /*isSigned=*/false);
    TypeId const voidTy = ti.primitive(TypeKind::Void);
    TypeId const fnSig  = ti.fnSig(std::span<TypeId const>{}, voidTy, CallConv::CcSysV);

    MirBuilder b;
    b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    (void)b.addInst(MirOpcode::Alloca, {}, ti.pointer(bi37), 0);
    (void)b.addInst(MirOpcode::Alloca, {}, ti.pointer(bu9), 0);
    b.addReturn();
    Mir m = std::move(b).finish();

    std::vector<std::string> names{"", "f"};
    RoundTrip const rt = roundTrip(m, ti, names);
    EXPECT_NE(rt.firstEmit.find("ptr<_BitInt(37)>"), std::string::npos) << rt.firstEmit;
    EXPECT_NE(rt.firstEmit.find("ptr<unsigned _BitInt(9)>"), std::string::npos) << rt.firstEmit;
    EXPECT_TRUE(rt.parseOk) << rt.firstEmit;
    EXPECT_EQ(rt.firstEmit, rt.secondEmit);
}

TEST(MirTextOperandForms, TheUndecodableTypeMarkerIsRefusedByNameAndWarnsOnce) {
    // Emitting with no `TypeInterner` renders every type as `?`. The header used
    // to promise that such a dump was still "re-parseable"; it never was, because
    // `?` names no type and guessing one would silently retype the module.
    //
    // ★ TWO FACTS IN ONE ARM, and they belong together: the emitter says this at
    // WARNING (it is a mode the caller chose, not a defect in the module) while
    // every "this value has no spelling" site says Error — the split that used to
    // be made by a DEFAULT ARGUMENT, and made differently in the two text tiers.
    TypeInterner ti{CompilationUnitId{1}};
    TypeId const i32   = ti.primitive(TypeKind::I32);
    TypeId const fnSig = ti.fnSig(std::span<TypeId const>{}, i32, CallConv::CcSysV);
    MirBuilder b;
    b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    b.addReturn(b.addConst(i32Lit(1), i32));
    Mir m = std::move(b).finish();

    DiagnosticReporter emitRep;
    MirTextContext ctx{};   // no interner
    std::string const text = emitMir(m, ctx, emitRep);
    EXPECT_NE(text.find('?'), std::string::npos) << text;
    std::size_t warnings = 0;
    for (auto const& d : emitRep.all()) {
        EXPECT_NE(d.severity, DiagnosticSeverity::Error)
            << "a caller-chosen type-less dump is not an Error: " << d.actual;
        if (d.severity == DiagnosticSeverity::Warning) ++warnings;
    }
    EXPECT_EQ(warnings, 1u) << "reported once, not per type";

    DiagnosticReporter parseRep;
    auto parsed = parseMir(text, CompilationUnitId{2}, parseRep);
    EXPECT_FALSE(parsed->ok) << "'?' must not be read as a type";
    bool named = false;
    for (auto const& d : parseRep.all()) {
        if (d.actual.find("emitter's mark") != std::string::npos) named = true;
    }
    EXPECT_TRUE(named)
        << "'?' is this compiler's own output coming back, not an author's typo, "
           "and the two need different sentences";
}

// ── the mnemonic refusal names its accepted set ─────────────────────────────
//
// D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET, the `.dssir` twin of `hir_text`'s
// `parseOp` arm: `unknown opcode 'foo'` named nothing at all. The set is
// projected off the same `opcodeInfo` walk `opcodeFromMnemonic` performs.
//
// ⚠ NO FEED-BACK ARM HERE, AND THE REASON IS SPECIFIC RATHER THAN LAZINESS.
// Every other vocabulary pin in this tree feeds each advertised spelling back
// through the reader; for mnemonics that is not merely awkward, it is UNSAFE —
// each opcode has its own operand shape, and a bare mnemonic reaches
// `MirBuilder::addInst` with zero operands, which ABORTS THE PROCESS on the
// arity guard. So this arm asserts membership on spellings that exist ONLY in
// `opcodeInfo`, which a retyped literal list could not have supplied.
TEST(MirTextOperandForms, UnknownOpcodeRefusalNamesTheAcceptedSet) {
    DiagnosticReporter r;
    auto parsed = parseMir(
        "dssir 1\nsymbols { %1 \"f\" }\nmodule {\n"
        "  function %1 : fn() -> void {\n    block %b1 [entry] {\n"
        "      nosuchopcode\n      return\n    }\n  }\n}\n",
        CompilationUnitId{1}, r);
    EXPECT_FALSE(parsed->ok);
    std::string msg;
    for (auto const& d : r.all()) {
        if (d.actual.find("unknown opcode") != std::string::npos) msg = d.actual;
    }
    ASSERT_FALSE(msg.empty()) << "the mnemonic arm must refuse by name";
    for (std::string_view want : {"'add'", "'indirectbr'", "'blockaddress'",
                                  "'byvaluestackarg'", "'seh_filter_return'"}) {
        EXPECT_NE(msg.find(want), std::string::npos)
            << "the refusal must name " << want << ":\n" << msg;
    }
}

// ── an invalid TYPE must not reach a builder that aborts ────────────────────
//
// ★★★ FOUND WHILE PINNING THE `?` REFUSAL ABOVE, AND IT IS REACHABLE FROM AN
// ORDINARY TYPO. `parseType` refuses an unknown type name and returns
// `InvalidType`; `parseGlobal` and `parseFunction` then handed that straight to
// `MirBuilder::addGlobal` / `addFunction`, both of which ABORT THE PROCESS on an
// invalid type. ✔MEASURED 2026-08-23, verbatim:
//   `dss::MirBuilder fatal: addGlobal: type TypeId must be valid`
//   `dss::MirBuilder fatal: addFunction: signature TypeId must be valid (FnSig)`
// both with exit 0xC0000409, from `: bogus`. The `errors_` short-circuit in
// `finalize()` would have discarded the module — but `finalize()` never runs,
// because the abort happens several steps earlier
// (D-MIR-TEXT-INVALID-TYPE-REACHES-A-BUILDER-THAT-ABORTS).
TEST(MirTextOperandForms, AnUnknownTypeNameIsRefusedRatherThanAborting) {
    auto probe = [](std::string const& text) {
        DiagnosticReporter r;
        auto parsed = parseMir(text, CompilationUnitId{1}, r);
        bool named = false;
        for (auto const& d : r.all()) {
            if (d.actual.find("unknown type 'bogus'") != std::string::npos) named = true;
        }
        EXPECT_FALSE(parsed->ok);
        EXPECT_TRUE(named) << "the refusal must name the offending spelling";
    };
    probe("dssir 1\nsymbols { %1 \"f\" %2 \"g\" }\nmodule {\n"
          "  global %2 : bogus = zero\n"
          "  function %1 : fn() -> void {\n    block %b1 [entry] {\n"
          "      return\n    }\n  }\n}\n");
    probe("dssir 1\nsymbols { %1 \"f\" }\nmodule {\n"
          "  function %1 : bogus {\n    block %b1 [entry] {\n"
          "      return\n    }\n  }\n}\n");
}

TEST(MirTextOperandForms, ARefusedFunctionHeaderDoesNotCascadeOverItsBody) {
    // The recovery half, stated separately because a brace-skip and a bare
    // `return` are both non-aborting and only one of them keeps the diagnostic
    // readable: without the skip, every `block %bN {` line of the orphaned body
    // is re-offered to the MODULE loop and refused in turn.
    DiagnosticReporter r;
    auto parsed = parseMir(
        "dssir 1\nsymbols { %1 \"f\" }\nmodule {\n"
        "  function %1 : bogus {\n"
        "    block %b1 [entry] {\n      return\n    }\n"
        "    block %b2 {\n      return\n    }\n"
        "  }\n}\n",
        CompilationUnitId{1}, r);
    EXPECT_FALSE(parsed->ok);
    std::size_t blockComplaints = 0;
    for (auto const& d : r.all()) {
        if (d.actual.find("block") != std::string::npos) ++blockComplaints;
    }
    EXPECT_EQ(blockComplaints, 0u)
        << "the body of a refused function must not produce diagnostics of its own";
}

// ── the CLASS guard: an unconsumed operand tail is refused, by name ──────────
//
// Every arm above was one instance of ONE defect: the writer rendered a tail the
// reader's arm did not consume, and the reader carried on. Pinning the three
// instances leaves the class open — the next payload-carrying opcode reopens it.
// `parseInstruction` now refuses any instruction whose line still holds tokens
// after its arm has run, so a future writer/reader asymmetry is LOUD at the
// instruction that caused it instead of at whatever the leftovers re-tokenize
// into.
//
// ⚠ THE INPUT HERE IS HAND-WRITTEN, DELIBERATELY. Driving this through the
// writer would require a writer/reader pair that already disagrees — i.e. the
// very defect being guarded against. A hand-written tail reproduces the SHAPE
// (a well-formed instruction plus an unconsumed remainder) without needing one.
TEST(MirTextOperandForms, UnconsumedOperandTailIsRefusedByName) {
    std::string const text =
        "dssir 1\n"
        "symbols {\n"
        "  %1 \"f\"\n"
        "}\n"
        "module {\n"
        "  function %1 : fn() -> i32 {\n"
        "    block %b1 [entry] {\n"
        "      %v1 = const : i32 (lit int 7 : i32) trailing 99\n"
        "      return %v1\n"
        "    }\n"
        "  }\n"
        "}\n";
    DiagnosticReporter r;
    auto parsed = parseMir(text, CompilationUnitId{1}, r);
    EXPECT_FALSE(parsed->ok);
    bool sawTailRefusal = false;
    for (auto const& d : r.all()) {
        if (d.actual.find("unparsed") != std::string::npos
            && d.actual.find("trailing") != std::string::npos) {
            sawTailRefusal = true;
        }
    }
    EXPECT_TRUE(sawTailRefusal)
        << "the refusal must NAME the leftover text; otherwise an author reads a "
           "diagnostic about the NEXT line";
}
