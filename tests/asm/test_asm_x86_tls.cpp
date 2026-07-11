// x86_64 TLS encoder + schema pins — TLS C1 (D-CSUBSET-THREAD-LOCAL).
//
// Pins the two NEW x86 encoding capabilities the local-exec access
// sequence rides, byte-exact:
//   * `tlsbase` — `mov r64, seg:[disp32]` via the payload-byte segment
//     prefix (template `payloadBytePrefix`) + the absolute-SIB literal
//     disp32 slot (`absdisp32.mem`): EXACTLY
//     <seg> 48 8B 04 25 <disp32 LE>. The segment byte + displacement
//     are BOTH per-format config threaded through the LIR inst
//     (payload / MemOffset operand) — the pins verify a config change
//     flips ONLY the config-owned bytes.
//   * the 2-op `lea reg, [base + tpoff32(sym)]` variant — the
//     symbol-bearing memory displacement (`memreloc.disp32`): the
//     4-byte RELOC PLACEHOLDER sits at the MEMORY-DISPLACEMENT
//     position (right after ModR/M [+ SIB]), NOT the trailing Disp32
//     position, and records Relocation{tls-tpoff32, sym} at exactly
//     that byte offset.
// Plus the schema-tier pins: the target `tls` identity block + the
// `tls`-flagged relocation row + the format `tlsAccess` block round-
// trip on the SHIPPED configs (and stay ABSENT on the legs whose TLS
// cycles have not landed — absence IS the fail-loud gate), and the
// loader/validate() fail-louds for every malformed combination.

#include "asm/asm.hpp"
#include "asm_test_support.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"
#include "lir/lir.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using dss::test_support::asm_::countDiagnostics;

namespace {

// Build a one-function LIR whose single block holds the instructions
// `emit` adds plus a trailing `ret` (the block must be terminated).
template <typename Emit>
[[nodiscard]] Lir buildSingleFnLirWithRet(TargetSchema const& schema,
                                          Emit emit) {
    LirBuilder b{schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    emit(b);
    auto const retOp = schema.opcodeByMnemonic("ret");
    if (!retOp.has_value()) {
        ADD_FAILURE() << "test fixture: x86_64 schema missing 'ret'";
        std::abort();
    }
    (void)b.addReturn(*retOp, {});
    return std::move(b).finish();
}

// Assemble + return the single function (bytes + relocations).
struct AssembledFn {
    std::vector<std::uint8_t> bytes;
    std::vector<Relocation>   relocs;
};

[[nodiscard]] AssembledFn
assembleFirstFn(Lir const& lir, TargetSchema const& schema,
                DiagnosticReporter& rep) {
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto result = assemble(lir, schema, lirToMir, rep);
    AssembledFn out;
    EXPECT_EQ(result.functions.size(), 1u);
    if (result.functions.empty()) return out;
    out.bytes  = result.functions[0].bytes;
    out.relocs = result.functions[0].relocations;
    return out;
}

// Physical GPR by schema register name.
[[nodiscard]] LirReg physGpr(TargetSchema const& schema,
                             std::string_view name) {
    auto const ord = schema.registerByName(name);
    EXPECT_TRUE(ord.has_value()) << "register not found: " << name;
    return LirReg{static_cast<std::uint32_t>(ord.value_or(0)),
                  /*isPhysical=*/1,
                  /*cls=*/static_cast<std::uint8_t>(LirRegClass::GPR)};
}

// Emit ONE `tlsbase <dst>` with the given payload (segment byte) and
// tp-slot displacement, then assemble.
[[nodiscard]] AssembledFn
assembleTlsBase(TargetSchema const& schema, std::string_view dstName,
                std::uint32_t payload, std::int32_t disp,
                DiagnosticReporter& rep) {
    auto const tlsbaseOp = schema.opcodeByMnemonic("tlsbase");
    EXPECT_TRUE(tlsbaseOp.has_value());
    LirReg const dst = physGpr(schema, dstName);
    Lir lir = buildSingleFnLirWithRet(schema, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeMemOffset(disp) };
        (void)b.addInst(*tlsbaseOp, dst, ops, payload);
    });
    return assembleFirstFn(lir, schema, rep);
}

} // namespace

// ── `tlsbase` byte pins ──────────────────────────────────────────────

TEST(X86Tls, TlsBaseRaxEmitsSegPrefix_64_48_8B_04_25_Disp0) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    DiagnosticReporter rep;
    auto const fn = assembleTlsBase(**schema, "rax", /*payload=*/0x64,
                                    /*disp=*/0, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    // mov rax, fs:[0] = 64 (fs override, FROM PAYLOAD) + REX.W 48 +
    // 8B + ModRM(mod=00 reg=rax rm=100) 04 + SIB(index=none base=101)
    // 25 + disp32 0; then ret C3.
    ASSERT_EQ(fn.bytes.size(), 10u);
    EXPECT_EQ(fn.bytes[0], 0x64);
    EXPECT_EQ(fn.bytes[1], 0x48);
    EXPECT_EQ(fn.bytes[2], 0x8B);
    EXPECT_EQ(fn.bytes[3], 0x04);
    EXPECT_EQ(fn.bytes[4], 0x25);
    EXPECT_EQ(fn.bytes[5], 0x00);
    EXPECT_EQ(fn.bytes[6], 0x00);
    EXPECT_EQ(fn.bytes[7], 0x00);
    EXPECT_EQ(fn.bytes[8], 0x00);
    EXPECT_EQ(fn.bytes[9], 0xC3);
    // The tp read is config-resolved at LINK time never — no reloc.
    EXPECT_TRUE(fn.relocs.empty());
}

TEST(X86Tls, TlsBaseGsPayloadFlipsOnlyByteZero) {
    // The segment byte is PER-FORMAT CONFIG riding the payload — 0x65
    // (gs, the PE value) must flip ONLY byte 0 (the same target row
    // serves ELF and PE with config-only differences).
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    DiagnosticReporter repFs, repGs;
    auto const fs = assembleTlsBase(**schema, "rax", 0x64, 0, repFs);
    auto const gs = assembleTlsBase(**schema, "rax", 0x65, 0, repGs);
    EXPECT_EQ(repFs.errorCount(), 0u);
    EXPECT_EQ(repGs.errorCount(), 0u);
    ASSERT_EQ(fs.bytes.size(), gs.bytes.size());
    ASSERT_FALSE(gs.bytes.empty());
    EXPECT_EQ(gs.bytes[0], 0x65);
    for (std::size_t i = 1; i < fs.bytes.size(); ++i) {
        EXPECT_EQ(fs.bytes[i], gs.bytes[i])
            << "byte " << i << " must not depend on the segment payload";
    }
}

TEST(X86Tls, TlsBaseNonZeroDisplacementEmitsLiteralDispLE) {
    // The tp-slot displacement is config too (PE's TEB slot is
    // gs:[0x58]) — the MemOffset operand's value must land as the
    // literal LE disp32, byte-exact.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    DiagnosticReporter rep;
    auto const fn = assembleTlsBase(**schema, "rax", 0x65, 0x58, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(fn.bytes.size(), 10u);
    EXPECT_EQ(fn.bytes[0], 0x65);
    EXPECT_EQ(fn.bytes[5], 0x58);
    EXPECT_EQ(fn.bytes[6], 0x00);
    EXPECT_EQ(fn.bytes[7], 0x00);
    EXPECT_EQ(fn.bytes[8], 0x00);
}

TEST(X86Tls, TlsBaseR8DerivesRexR) {
    // The destination rides ModR/M.reg, so r8's high bit drives REX.R:
    // REX = 0x48 | R(0x04) = 0x4C (NOT 0x49 = REX.W|B — the rm field
    // is the SIB-follows marker, not a register).
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    DiagnosticReporter rep;
    auto const fn = assembleTlsBase(**schema, "r8", 0x64, 0, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(fn.bytes.size(), 5u);
    EXPECT_EQ(fn.bytes[0], 0x64);
    EXPECT_EQ(fn.bytes[1], 0x4C);
    EXPECT_EQ(fn.bytes[2], 0x8B);
    EXPECT_EQ(fn.bytes[3], 0x04);   // ModRM: mod=00 reg=r8.lo3(000) rm=100
    EXPECT_EQ(fn.bytes[4], 0x25);
}

TEST(X86Tls, TlsBaseZeroPayloadFailsLoud) {
    // payloadBytePrefix with payload low-byte 0: a zero prefix byte is
    // never a valid segment override — a lowering that forgot to set
    // the payload must fail LOUD, not emit a 0x00 byte that decodes
    // as `add [rax], al`.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    DiagnosticReporter rep;
    (void)assembleTlsBase(**schema, "rax", /*payload=*/0, /*disp=*/0, rep);
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_GE(countDiagnostics(rep, DiagnosticCode::A_NoMatchingEncodingVariant),
              1u);
}

// ── the TLS `lea` (memreloc.disp32) byte pins ────────────────────────

TEST(X86Tls, TlsLeaRaxRaxEmitsTpoffRelocAtMemDispPosition) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const leaOp = (*schema)->opcodeByMnemonic("lea");
    ASSERT_TRUE(leaOp.has_value());
    LirReg const rax = physGpr(**schema, "rax");
    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rax),
            LirOperand::makeSymbolRef(99),
        };
        (void)b.addInst(*leaOp, rax, ops);
    });
    DiagnosticReporter rep;
    auto const fn = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    // lea rax, [rax + tpoff32(sym)] = REX.W 48 + 8D + ModRM(mod=10
    // reg=rax rm=rax) 80 + 4 reloc placeholder bytes; then ret C3.
    ASSERT_EQ(fn.bytes.size(), 8u);
    EXPECT_EQ(fn.bytes[0], 0x48);
    EXPECT_EQ(fn.bytes[1], 0x8D);
    EXPECT_EQ(fn.bytes[2], 0x80);
    EXPECT_EQ(fn.bytes[3], 0x00);
    EXPECT_EQ(fn.bytes[4], 0x00);
    EXPECT_EQ(fn.bytes[5], 0x00);
    EXPECT_EQ(fn.bytes[6], 0x00);
    EXPECT_EQ(fn.bytes[7], 0xC3);
    // The relocation sits at the MEMORY-DISPLACEMENT position — byte 3
    // (after REX + opcode + ModRM), NOT a trailing step-8 slot.
    ASSERT_EQ(fn.relocs.size(), 1u);
    EXPECT_EQ(fn.relocs[0].offset, 3u);
    EXPECT_EQ(fn.relocs[0].target, SymbolId{99});
    auto const* tpoff = (*schema)->relocationByName("tls-tpoff32");
    ASSERT_NE(tpoff, nullptr);
    EXPECT_EQ(fn.relocs[0].kind, tpoff->kind);
    EXPECT_EQ(fn.relocs[0].addend, 0);
}

TEST(X86Tls, TlsLeaRcxR12TakesSibPathWithRexB) {
    // r12's lo3 == 100 forces the SIB byte (the x86-64 rsp/r12 rule):
    // REX = 0x48|B = 0x49; ModRM = mod=10 reg=rcx(001) rm=100 = 0x8C;
    // SIB = scale=0 index=none(100) base=r12.lo3(100) = 0x24; the
    // reloc placeholder then sits at byte 4.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const leaOp = (*schema)->opcodeByMnemonic("lea");
    ASSERT_TRUE(leaOp.has_value());
    LirReg const rcx = physGpr(**schema, "rcx");
    LirReg const r12 = physGpr(**schema, "r12");
    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(r12),
            LirOperand::makeSymbolRef(7),
        };
        (void)b.addInst(*leaOp, rcx, ops);
    });
    DiagnosticReporter rep;
    auto const fn = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(fn.bytes.size(), 9u);
    EXPECT_EQ(fn.bytes[0], 0x49);
    EXPECT_EQ(fn.bytes[1], 0x8D);
    EXPECT_EQ(fn.bytes[2], 0x8C);
    EXPECT_EQ(fn.bytes[3], 0x24);
    EXPECT_EQ(fn.bytes[4], 0x00);
    EXPECT_EQ(fn.bytes[8], 0xC3);
    ASSERT_EQ(fn.relocs.size(), 1u);
    EXPECT_EQ(fn.relocs[0].offset, 4u);
    EXPECT_EQ(fn.relocs[0].target, SymbolId{7});
}

// ── shipped-schema pins ──────────────────────────────────────────────

TEST(X86Tls, ShippedX64DeclaresTlsIdentityVariant2AndTpoffRelocRow) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    // The tls identity block: Variant II, no TCB header.
    auto const& tls = (*schema)->tlsIdentity();
    ASSERT_TRUE(tls.has_value())
        << "x86_64 must declare its static-TLS layout convention";
    EXPECT_EQ(tls->variant, TlsVariant::Variant2);
    EXPECT_EQ(tls->tcbHeaderBytes, 0u);
    // The tls-tpoff32 relocation row: Linear, 4 bytes, absolute,
    // tls-flagged. The pre-existing rows stay tls=false.
    auto const* tpoff = (*schema)->relocationByName("tls-tpoff32");
    ASSERT_NE(tpoff, nullptr);
    EXPECT_TRUE(tpoff->tls);
    EXPECT_EQ(tpoff->formulaKind, RelocFormulaKind::Linear);
    EXPECT_EQ(tpoff->widthBytes, 4u);
    EXPECT_FALSE(tpoff->pcRelative);
    EXPECT_EQ(tpoff->addendBias, 0);
    auto const* rel32 = (*schema)->relocationByName("rel32");
    ASSERT_NE(rel32, nullptr);
    EXPECT_FALSE(rel32->tls);
}

TEST(X86Tls, ShippedArm64DeclaresTlsRowsSinceC2) {
    // TLS C2 LANDED (this pin's C1-era predecessor asserted absence):
    // arm64 now declares its Variant-I identity + the tlsbase MRS
    // opcode + the tprel reloc pair. The x86-NAMED tls-tpoff32 kind
    // stays arm64-absent — each target names its own tpoff kinds
    // (the detailed arm64 pins live in test_asm_arm64_tls.cpp).
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    EXPECT_TRUE((*schema)->tlsIdentity().has_value());
    EXPECT_TRUE((*schema)->opcodeByMnemonic("tlsbase").has_value());
    EXPECT_NE((*schema)->relocationByName("tls-tprel-hi12"), nullptr);
    EXPECT_NE((*schema)->relocationByName("tls-tprel-lo12"), nullptr);
    EXPECT_EQ((*schema)->relocationByName("tls-tpoff32"), nullptr);
}

TEST(X86Tls, ShippedFormatsTlsAccessPresenceMatchesLandedLegs) {
    // ELF-Linux x86_64 exec (C1): local-exec {fs=0x64, disp 0}.
    // arm64-ELF (C2): local-exec {0, 0} — the MRS shape consumes
    // neither x86 template value. pe64 (C3): pe-indexed {gs=0x65,
    // disp 0x58, __dss_tls_index}. Mach-O remains ABSENT until C4 —
    // absence IS the K_FormatLacksThreadLocalSupport gate for it.
    auto elf = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(elf.has_value());
    auto const ta = (*elf)->tlsAccess();
    ASSERT_TRUE(ta.has_value());
    EXPECT_EQ(ta->model, TlsAccessModel::LocalExec);
    EXPECT_EQ(ta->segmentPrefixByte, 0x64u);
    EXPECT_EQ(ta->baseDisplacement, 0u);

    auto pe = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(pe.has_value());
    auto const taPe = (*pe)->tlsAccess();
    ASSERT_TRUE(taPe.has_value())
        << "pe64 declares tlsAccess since TLS C3";
    EXPECT_EQ(taPe->model, TlsAccessModel::PeIndexed);
    EXPECT_EQ(taPe->segmentPrefixByte, 0x65u);       // gs
    EXPECT_EQ(taPe->baseDisplacement, 0x58u);        // TEB TLS array
    EXPECT_EQ(taPe->tlsIndexSlotName, "__dss_tls_index");

    auto arm = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(arm.has_value());
    auto const taArm = (*arm)->tlsAccess();
    ASSERT_TRUE(taArm.has_value())
        << "arm64-ELF declares tlsAccess since TLS C2";
    EXPECT_EQ(taArm->model, TlsAccessModel::LocalExec);
    EXPECT_EQ(taArm->segmentPrefixByte, 0u);
    EXPECT_EQ(taArm->baseDisplacement, 0u);
}

// ── loader / validate() fail-louds ───────────────────────────────────

namespace {

// Minimal target-JSON skeleton with one parameterizable opcode block.
[[nodiscard]] std::string synthTargetJson(std::string_view opcodesTail) {
    std::string s = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "relocations": [
            { "name": "tls-tpoff32", "kind": 4, "formula": "linear",
              "pcRelative": false, "addendBias": 0, "widthBytes": 4,
              "tls": true }
        ],
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
    )";
    s += opcodesTail;
    s += "]}";
    return s;
}

} // namespace

TEST(X86TlsSchema, MemRelocDisp32WithoutRelocationKindRejectedAtLoad) {
    // memreloc.disp32 is symbol-bearing — a wire without relocationKind
    // must be rejected at load (the generic symbol-slot pairing rule).
    auto const json = synthTargetJson(R"(
        { "mnemonic": "lea", "result": "value",
          "minOperands": 2, "maxOperands": 2,
          "encoding": {
            "format": "x86-variable",
            "variants": [
              { "guard": { "operandKinds": ["reg", "symbol"] },
                "template": { "rexW": true, "opcode": [141] },
                "resultSlot": "modrm.reg",
                "wires": [
                  { "index": 0, "slotKind": "modrm.rm.mem" },
                  { "index": 1, "slotKind": "memreloc.disp32" }
                ]
              }
            ]
          } }
    )");
    EXPECT_FALSE(TargetSchema::loadFromText(json, "synth.target.json").has_value())
        << "memreloc.disp32 without relocationKind must be rejected";
}

TEST(X86TlsSchema, MemRelocDisp32PlusLiteralDisp32MemRejectedAtLoad) {
    // A memory operand has exactly ONE displacement field — a variant
    // wiring BOTH the literal and the relocated form would double-emit
    // (instruction-stream corruption). validate() rejects at load.
    auto const json = synthTargetJson(R"(
        { "mnemonic": "lea", "result": "value",
          "minOperands": 3, "maxOperands": 3,
          "encoding": {
            "format": "x86-variable",
            "variants": [
              { "guard": { "operandKinds": ["reg", "memoffset", "symbol"] },
                "template": { "rexW": true, "opcode": [141] },
                "resultSlot": "modrm.reg",
                "wires": [
                  { "index": 0, "slotKind": "modrm.rm.mem" },
                  { "index": 1, "slotKind": "disp32.mem" },
                  { "index": 2, "slotKind": "memreloc.disp32",
                    "relocationKind": "tls-tpoff32" }
                ]
              }
            ]
          } }
    )");
    EXPECT_FALSE(TargetSchema::loadFromText(json, "synth.target.json").has_value())
        << "a literal AND a relocated displacement in one variant must "
           "be rejected";
}

TEST(X86TlsSchema, MemRelocDisp32WithoutMemBaseWireRejectedAtLoad) {
    // The relocated displacement is the disp32 OF a [base + disp32]
    // memory operand — without a modrm.rm.mem base wire there is no
    // memory operand to displace.
    auto const json = synthTargetJson(R"(
        { "mnemonic": "lea", "result": "value",
          "minOperands": 1, "maxOperands": 1,
          "encoding": {
            "format": "x86-variable",
            "variants": [
              { "guard": { "operandKinds": ["symbol"] },
                "template": { "rexW": true, "opcode": [141] },
                "resultSlot": "modrm.reg",
                "wires": [
                  { "index": 0, "slotKind": "memreloc.disp32",
                    "relocationKind": "tls-tpoff32" }
                ]
              }
            ]
          } }
    )");
    EXPECT_FALSE(TargetSchema::loadFromText(json, "synth.target.json").has_value())
        << "memreloc.disp32 without a modrm.rm.mem base wire must be "
           "rejected";
}

TEST(X86TlsSchema, AbsoluteDisp32MemCoWiredWithBaseRejectedAtLoad) {
    // The absolute-SIB form owns the WHOLE memory operand (no base
    // register) — co-wiring a modrm.rm.mem base is contradictory.
    auto const json = synthTargetJson(R"(
        { "mnemonic": "tlsbase", "result": "value",
          "minOperands": 2, "maxOperands": 2,
          "encoding": {
            "format": "x86-variable",
            "variants": [
              { "guard": { "operandKinds": ["reg", "memoffset"] },
                "template": { "rexW": true, "opcode": [139] },
                "resultSlot": "modrm.reg",
                "wires": [
                  { "index": 0, "slotKind": "modrm.rm.mem" },
                  { "index": 1, "slotKind": "absdisp32.mem" }
                ]
              }
            ]
          } }
    )");
    EXPECT_FALSE(TargetSchema::loadFromText(json, "synth.target.json").has_value())
        << "absdisp32.mem co-wired with a base register must be rejected";
}

TEST(X86TlsSchema, PayloadBytePrefixOnFixed32VariantRejectedAtLoad) {
    // A fixed-word ISA has no prefix bytes — payloadBytePrefix on a
    // fixed32 variant is dead data at best (the mandatoryPrefix rule's
    // mirror).
    auto const json = synthTargetJson(R"(
        { "mnemonic": "bogus", "result": "value",
          "minOperands": 1, "maxOperands": 1,
          "encoding": {
            "format": "fixed32",
            "variants": [
              { "guard": { "operandKinds": ["reg"] },
                "template": { "payloadBytePrefix": true,
                              "fixedWord": 3573751840 },
                "resultSlot": "rd",
                "wires": [ { "index": 0, "slotKind": "rn" } ]
              }
            ]
          } }
    )");
    EXPECT_FALSE(TargetSchema::loadFromText(json, "synth.target.json").has_value())
        << "payloadBytePrefix on a fixed32 variant must be rejected";
}

TEST(X86TlsSchema, UnknownTlsVariantNameRejectedAtLoad) {
    // The two variants produce OPPOSITE-SIGNED tpoffs — an unknown
    // spelling must never silently pick one.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "tls": { "variant": "variant3", "tcbHeaderBytes": 0 },
        "opcodes": [ { "mnemonic": "invalid", "result": "none" } ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "synth.target.json").has_value())
        << "unknown tls variant name must be rejected at load";
}

TEST(X86TlsSchema, TlsBlockMissingVariantRejectedAtLoad) {
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "tls": { "tcbHeaderBytes": 16 },
        "opcodes": [ { "mnemonic": "invalid", "result": "none" } ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "synth.target.json").has_value())
        << "a tls block without the required 'variant' must be rejected";
}

namespace {

// Load the SHIPPED elf64-x86_64-linux-exec format JSON, apply an
// in-memory mutation to its `tlsAccess` block, and re-load from text.
// Base = an always-valid shipped file (no parallel broken JSON to
// rot); the mutation is the ONLY delta, so a load failure pins the
// tlsAccess rule itself, not an unrelated skeleton defect. (The
// mutate_target_schema.hpp discipline, format-side.)
[[nodiscard]] LoadResult<std::shared_ptr<ObjectFormatSchema>>
loadShippedElfExecWithTlsAccessMutation(
    std::function<void(nlohmann::json&)> const& mutate) {
    auto pathR = findShippedConfig(
        ShippedConfigLocator{"elf64-x86_64-linux-exec", "object-formats",
                             ".format.json", "object format",
                             DiagnosticCode::C_InvalidFormatName});
    if (!pathR.has_value()) {
        return std::unexpected(std::move(pathR).error());
    }
    std::ifstream in{*pathR};
    std::ostringstream buf;
    buf << in.rdbuf();
    nlohmann::json doc = nlohmann::json::parse(buf.str());
    mutate(doc);
    return ObjectFormatSchema::loadFromText(doc.dump(),
                                            "mutated-elf-exec.format.json");
}

} // namespace

TEST(X86TlsSchema, FormatTlsAccessUnknownModelRejectedAtLoad) {
    auto r = loadShippedElfExecWithTlsAccessMutation([](nlohmann::json& doc) {
        doc.at("tlsAccess").at("model") = "global-dynamic";
    });
    EXPECT_FALSE(r.has_value())
        << "an unknown tlsAccess model must be rejected at load — a typo "
           "must never silently degrade to 'no TLS support'";
}

TEST(X86TlsSchema, FormatTlsAccessMissingModelRejectedAtLoad) {
    auto r = loadShippedElfExecWithTlsAccessMutation([](nlohmann::json& doc) {
        doc.at("tlsAccess").erase("model");
    });
    EXPECT_FALSE(r.has_value())
        << "a tlsAccess block without the required 'model' must be rejected";
}

TEST(X86TlsSchema, FormatTlsAccessSegmentPrefixOutOfRangeRejectedAtLoad) {
    auto r = loadShippedElfExecWithTlsAccessMutation([](nlohmann::json& doc) {
        doc.at("tlsAccess").at("segmentPrefixByte") = 256;
    });
    EXPECT_FALSE(r.has_value())
        << "segmentPrefixByte outside [0,255] must be rejected";
}

TEST(X86TlsSchema, FormatTlsAccessIdentityMutationStillLoads) {
    // Control for the three negatives above: the SAME load path with a
    // no-op mutation must succeed and round-trip the shipped values —
    // proving the negatives fail BECAUSE of their mutation, not
    // because of the re-serialize/load harness.
    auto r = loadShippedElfExecWithTlsAccessMutation([](nlohmann::json&) {});
    ASSERT_TRUE(r.has_value());
    auto const ta = (*r)->tlsAccess();
    ASSERT_TRUE(ta.has_value());
    EXPECT_EQ(ta->model, TlsAccessModel::LocalExec);
    EXPECT_EQ(ta->segmentPrefixByte, 0x64u);
    EXPECT_EQ(ta->baseDisplacement, 0u);
}
