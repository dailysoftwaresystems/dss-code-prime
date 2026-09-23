// `.dssir` instruction fields — every field a `MirInst` carries survives a text
// round trip (P68 round 8, lane `ht`, part 1b).
//
// ✔MEASURED before this file: the MIR text writer printed NO instruction flag and
// NO secondary payload. A `Volatile` load and a `ReturnsTwice` call were written
// `%v2 = load : i32 (%v1)` / `%v4 = call : i32 (%v3)` and read back with flags 0;
// an alloca carrying alignment 3 in `payload2` was written without it and read
// back VALID — both with `ok == true` and no diagnostic. By reading, the same
// generic arm dropped EVERY non-zero `payload2`: an over-aligned local's
// alignment, and an atomic access's provable alignment.
// The pre-existing round-trip pin could not see any of it: it asserts
// `firstEmit == secondEmit`, and a TOTALLY LOSSY round trip satisfies that. These
// cases compare the PARSED MODULE's fields, field by field.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "mir/mir_text.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// ★ THE NEXT FIELD IS CAUGHT BY CONSTRUCTION. The comparison below names every
// `MirInst` field; a field added to the POD changes its size, and this assertion
// refuses to compile until the comparison — and the text format — cover it.
static_assert(sizeof(detail::MirInst) == 28,
              "MirInst gained or lost a field: extend `expectSameInstructions` "
              "(and the .dssir writer + reader) to cover it, then update this size");

MirLiteralValue intLit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

std::size_t countReaderRefusals(DiagnosticReporter const& r) {
    std::size_t n = 0;
    for (auto const& d : r.all()) {
        if (d.code == DiagnosticCode::I_TextMalformed
            || d.code == DiagnosticCode::I_TextUnknownName
            || d.code == DiagnosticCode::I_TextVersionMismatch) {
            ++n;
        }
    }
    return n;
}

std::vector<MirInstId> layoutOrder(Mir const& m) {
    std::vector<MirInstId> out;
    for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi) {
        MirFuncId const fn = m.funcAt(fi);
        for (std::uint32_t bi = 0; bi < m.funcBlockCount(fn); ++bi) {
            MirBlockId const b = m.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < m.blockInstCount(b); ++ii) {
                out.push_back(m.blockInstAt(b, ii));
            }
        }
    }
    return out;
}

// Every `MirInst` field, instruction by instruction in layout order. The operand
// ids and the type are compared through the re-emission (byte identity), which
// renders both; this compares the scalars the text could otherwise drop.
void expectSameInstructions(Mir const& before, Mir const& after) {
    std::vector<MirInstId> const a = layoutOrder(before);
    std::vector<MirInstId> const b = layoutOrder(after);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::string const what{opcodeInfo(before.instOpcode(a[i])).mnemonic};
        SCOPED_TRACE(what);
        EXPECT_EQ(after.instOpcode(b[i]), before.instOpcode(a[i]));
        EXPECT_EQ(static_cast<unsigned>(after.instFlags(b[i])),
                  static_cast<unsigned>(before.instFlags(a[i]))) << "flags";
        EXPECT_EQ(after.instPayload(b[i]), before.instPayload(a[i])) << "payload";
        EXPECT_EQ(after.instPayload2(b[i]), before.instPayload2(a[i])) << "payload2";
        // A Phi's operand range addresses the PHI pool — its incomings.
        if (before.instOpcode(a[i]) == MirOpcode::Phi) {
            EXPECT_EQ(after.phiIncomings(b[i]).size(), before.phiIncomings(a[i]).size())
                << "phi incoming count";
        } else {
            EXPECT_EQ(after.instOperands(b[i]).size(), before.instOperands(a[i]).size())
                << "operand count";
        }
        EXPECT_EQ(after.instType(b[i]).valid(), before.instType(a[i]).valid()) << "has a type";
    }
}

struct RoundTrip {
    std::string first;
    std::string second;
    std::unique_ptr<MirParseResult> parsed;
    std::size_t readerRefusals = 0;
    std::vector<std::string> diagnostics;
};

RoundTrip roundTrip(Mir const& m, TypeInterner const& in,
                    std::vector<std::string> const& names) {
    RoundTrip rt;
    MirTextContext ctx{&in, &names};
    DiagnosticReporter r1;
    rt.first = emitMir(m, ctx, r1);
    DiagnosticReporter r2;
    rt.parsed = parseMir(rt.first, CompilationUnitId{2}, r2);
    rt.readerRefusals = countReaderRefusals(r2);
    for (auto const& d : r2.all()) rt.diagnostics.push_back(d.actual);
    MirTextContext ctx2{&rt.parsed->interner, nullptr, &rt.parsed->symbolNames};
    DiagnosticReporter r3;
    rt.second = emitMir(rt.parsed->mir, ctx2, r3);
    return rt;
}

std::string joined(std::vector<std::string> const& v) {
    std::string s;
    for (auto const& x : v) s += "\n  " + x;
    return s;
}

} // namespace

// ★★ THE PROPERTY: every field of every instruction survives, per reader ARM —
// the generic arm (alloca, load, store, call, atomic load, add), and each
// dedicated one (const, arg, globaladdr, intrinsic, phi, blockaddress,
// byvaluestackarg, seh_try_end) — with every `MirInstFlags` bit set somewhere,
// a combination of two, a non-default `payload` and a non-default `payload2`.
TEST(MirTextInstructionFields, EveryFieldOfEveryInstructionSurvivesARoundTrip) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32  = in.primitive(TypeKind::I32);
    TypeId const pI32 = in.pointer(i32);
    TypeId const voidPtr = in.pointer(in.primitive(TypeKind::Void));
    TypeId const calleeSig = in.fnSig({}, i32, CallConv::CcSysV);
    std::array<TypeId, 1> const params{i32};
    TypeId const fnSig = in.fnSig(params, i32, CallConv::CcSysV);

    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tail  = b.createBlock(StructCfMarker::Linear);
    b.beginBlock(entry);
    MirInstId const arg = b.addArg(0, i32, MirInstFlags::Synthetic);
    MirInstId const c5  = b.addConst(intLit(5), i32, MirInstFlags::Synthetic);
    MirInstId const ga  = b.addGlobalAddr(SymbolId{2}, in.pointer(calleeSig),
                                          MirInstFlags::Volatile);
    MirInstId const slot = b.addInst(MirOpcode::Alloca, {}, pI32, /*bytes=*/8,
                                     MirInstFlags::Volatile, /*payload2=*/64);
    MirInstId const plain = b.addInst(MirOpcode::Alloca, {}, pI32);
    std::array<MirInstId, 2> const storeOps{c5, slot};
    b.addInst(MirOpcode::Store, storeOps, InvalidType, 0, MirInstFlags::AtomicInitExempt);
    std::array<MirInstId, 1> const loadOps{slot};
    MirInstId const ld = b.addInst(MirOpcode::Load, loadOps, i32, 0, MirInstFlags::Volatile);
    MirInstId const ald = b.addInst(MirOpcode::AtomicLoad, loadOps, i32, /*order=*/5,
                                    MirInstFlags::None, /*payload2=*/8);
    std::array<MirInstId, 1> const callOps{ga};
    MirInstId const call = b.addInst(MirOpcode::Call, callOps, i32, 0,
                                     MirInstFlags::ReturnsTwice);
    std::array<MirInstId, 1> const intrOps{c5};
    (void)b.addInst(MirOpcode::IntrinsicCall, intrOps, i32, /*intrinsic=*/3,
                    MirInstFlags::Synthetic);
    std::array<MirInstId, 2> const addOps{ld, ald};
    MirInstId const sum = b.addInst(MirOpcode::Add, addOps, i32, 0,
                                    MirInstFlags::Synthetic | MirInstFlags::Volatile);
    std::array<MirInstId, 1> const byvOps{plain};
    (void)b.addInst(MirOpcode::ByValueStackArg, byvOps, i32,
                    16u | (static_cast<std::uint32_t>(kByValueStackArgExhaustNone)
                           << kByValueStackArgExhaustShift),
                    MirInstFlags::Synthetic);
    (void)b.addInst(MirOpcode::SehTryEnd, {}, InvalidType, /*region=*/1,
                    MirInstFlags::Synthetic);
    (void)b.addBlockAddress(tail, voidPtr, MirInstFlags::Synthetic);
    (void)arg; (void)call;
    b.addBr(tail);
    b.beginBlock(tail);
    std::array<MirPhiIncoming, 1> const incomings{MirPhiIncoming{sum, entry}};
    MirInstId const phi = b.addPhi(i32, incomings, MirInstFlags::Synthetic);
    b.addReturn(phi);
    Mir const m = std::move(b).finish();

    std::vector<std::string> names{"", "f", "g"};
    RoundTrip const rt = roundTrip(m, in, names);
    ASSERT_EQ(rt.readerRefusals, 0u)
        << "the reader refused text its own writer produced:\n" << rt.first
        << joined(rt.diagnostics);
    expectSameInstructions(m, rt.parsed->mir);
    EXPECT_EQ(rt.first, rt.second) << "the re-emission differs";
    // The spellings, pinned where they appear.
    EXPECT_NE(rt.first.find("load [volatile] : i32"), std::string::npos) << rt.first;
    EXPECT_NE(rt.first.find("call [returns_twice] : i32"), std::string::npos) << rt.first;
    EXPECT_NE(rt.first.find("store [atomic_init_exempt]"), std::string::npos) << rt.first;
    EXPECT_NE(rt.first.find("add [synthetic, volatile] : i32"), std::string::npos) << rt.first;
    EXPECT_NE(rt.first.find("alloca [volatile] : ptr<i32> payload 8 align=64"),
              std::string::npos) << rt.first;
    EXPECT_NE(rt.first.find("align=8"), std::string::npos) << rt.first;
}

// A FLAGLESS, default-aligned instruction is spelled exactly as before the fix —
// byte-identical to the text every existing `.dssir` expectation holds.
TEST(MirTextInstructionFields, AFlaglessDefaultAlignedModuleIsSpelledAsBefore) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const fnSig = in.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder b;
    (void)b.addFunction(fnSig, SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    MirInstId const slot = b.addInst(MirOpcode::Alloca, {}, in.pointer(i32));
    std::array<MirInstId, 1> const loadOps{slot};
    b.addReturn(b.addInst(MirOpcode::Load, loadOps, i32));
    Mir const m = std::move(b).finish();
    std::vector<std::string> names{"", "f"};
    MirTextContext ctx{&in, &names};
    DiagnosticReporter r;
    EXPECT_EQ(emitMir(m, ctx, r),
              "dssir 2\n"
              "symbols {\n"
              "  %1 \"f\"\n"
              "}\n"
              "module {\n"
              "  function %1 : fn() -> i32 {\n"
              "    block %b1 [entry] {\n"
              "      %v1 = alloca : ptr<i32>\n"
              "      %v2 = load : i32 (%v1)\n"
              "      return %v2\n"
              "    }\n"
              "  }\n"
              "}\n");
}

// A non-power-of-two alignment is WRITTEN (the module holds it) and REFUSED on
// the read by the rule and the message the global attribute uses — a text reader
// validates its input the same way everywhere; the verifier's own rule stays
// pinned on the builder path (`MirVerifierVerdict.*`).
TEST(MirTextInstructionFields, ANonPowerOfTwoAlignmentReadBackIsRefusedByTheRule) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    MirBuilder b;
    (void)b.addFunction(in.fnSig({}, in.primitive(TypeKind::Void), CallConv::CcSysV),
                        SymbolId{1});
    MirBlockId const entry = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(entry);
    b.addInst(MirOpcode::Alloca, {}, in.pointer(i32), 0, MirInstFlags::None,
              /*payload2=*/3);
    b.addReturn();
    Mir const m = std::move(b).finish();
    std::vector<std::string> names{"", "f"};
    MirTextContext ctx{&in, &names};
    DiagnosticReporter r1;
    std::string const text = emitMir(m, ctx, r1);
    ASSERT_NE(text.find("alloca : ptr<i32> align=3"), std::string::npos)
        << "the writer must spell what the module holds:\n" << text;
    DiagnosticReporter r2;
    auto const res = parseMir(text, CompilationUnitId{2}, r2);
    EXPECT_FALSE(res->ok);
    bool named = false;
    for (auto const& d : r2.all()) {
        if (d.code == DiagnosticCode::I_TextMalformed
            && d.actual.find("alloca alignment 3 is not a power of two in [1, ")
                   != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named) << "the refusal must name the rule";
}

// A flag spelling the reader does not know is refused BY NAME, with the accepted
// set — never read as "no flags".
TEST(MirTextInstructionFields, AnUnknownFlagIsRefusedByNameWithTheAcceptedSet) {
    std::string const text =
        "dssir 2\n"
        "symbols {\n  %1 \"f\"\n}\n"
        "module {\n"
        "  function %1 : fn() -> i32 {\n"
        "    block %b1 [entry] {\n"
        "      %v1 = alloca [voaltile] : ptr<i32>\n"
        "      %v2 = load : i32 (%v1)\n"
        "      return %v2\n"
        "    }\n"
        "  }\n"
        "}\n";
    DiagnosticReporter r;
    auto const res = parseMir(text, CompilationUnitId{1}, r);
    EXPECT_FALSE(res->ok);
    bool named = false;
    for (auto const& d : r.all()) {
        if (d.actual.find("unknown instruction flag 'voaltile' on 'alloca' — accepted: ")
                != std::string::npos
            && d.actual.find("volatile") != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named) << "the refusal must name the flag and the accepted set";
}
