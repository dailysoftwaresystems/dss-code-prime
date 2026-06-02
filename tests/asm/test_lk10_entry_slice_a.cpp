// D-LK10-ENTRY Slice A: new LIR opcodes for the runnable-binary spine
// (plan 14 §2.13). Round-trip tests that construct synthetic Lir via
// LirBuilder using the new opcodes + `assemble()` to bytes, pinning
// the wire-level byte sequences. Discipline established at silent-
// failure FN-2 (audit on `d642655`): substrate vocabulary additions
// MUST have a real round-trip caller — encoder-table-row unit tests
// alone would leave a dead-substrate landing (the project's already-
// caught anti-pattern at `9945457` audit).
//
// Slice A opcodes pinned here:
//   * `unreachable` — existing terminator opcode, encoding added at
//     Slice A so the trampoline emitter can append it after noreturn
//     call sites. Bytes: `0F 0B` (x86_64 UD2).
//   * `syscall` — new opcode, encoding `0F 05` (x86_64 SYSCALL).
//     Zero-operand. Non-terminator.
//   * `call_indirect_via_extern` — new opcode, encoding `FF 15 disp32`
//     (x86_64 call qword ptr [RIP+disp32]). 1 SymbolRef operand;
//     produces a REL32 relocation on the disp32 patch site.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "diagnostic_count.hpp"  // dss::test_support::countCode
#include "lir/lir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace dss;

TEST(LK10EntrySliceA, UnreachableOpEncodesToUd2) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const unreachableOp =
        (*schema)->opcodeByMnemonic("unreachable");
    ASSERT_TRUE(unreachableOp.has_value())
        << "unreachable opcode must be declared in x86_64.target.json";

    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    (void)b.addUnreachable(*unreachableOp);
    Lir lir = std::move(b).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    auto result = assemble(lir, **schema, lirToMir, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "unreachable opcode must encode without errors — Slice A "
           "added an encoding block so the trampoline emitter can "
           "append it after noreturn call sites";
    ASSERT_EQ(result.functions.size(), 1u);
    auto const& bytes = result.functions[0].bytes;
    ASSERT_EQ(bytes.size(), 2u)
        << "x86_64 UD2 is `0F 0B` — exactly 2 bytes";
    EXPECT_EQ(bytes[0], 0x0F);
    EXPECT_EQ(bytes[1], 0x0B);
}

TEST(LK10EntrySliceA, SyscallOpEncodesToFixedTwoBytes) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const syscallOp = (*schema)->opcodeByMnemonic("syscall");
    auto const unreachableOp =
        (*schema)->opcodeByMnemonic("unreachable");
    ASSERT_TRUE(syscallOp.has_value())
        << "syscall opcode must be declared in x86_64.target.json "
           "(Slice A addition)";
    ASSERT_TRUE(unreachableOp.has_value());

    // Body: syscall ; unreachable. The unreachable terminates the
    // block (LirBuilder requires a terminator; syscall is non-
    // terminator, so we append unreachable as the verifier hint
    // the trampoline emitter uses).
    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    (void)b.addInst(*syscallOp, LirReg{}, {});
    (void)b.addUnreachable(*unreachableOp);
    Lir lir = std::move(b).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    auto result = assemble(lir, **schema, lirToMir, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(result.functions.size(), 1u);
    auto const& bytes = result.functions[0].bytes;
    // syscall (0F 05) + unreachable (0F 0B) = 4 bytes.
    ASSERT_EQ(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x0F);
    EXPECT_EQ(bytes[1], 0x05) << "syscall low opcode byte must be 0x05";
    EXPECT_EQ(bytes[2], 0x0F);
    EXPECT_EQ(bytes[3], 0x0B);
}

TEST(LK10EntrySliceA, CallIndirectViaExternEmitsRel32Reloc) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const callIndOp =
        (*schema)->opcodeByMnemonic("call_indirect_via_extern");
    auto const unreachableOp =
        (*schema)->opcodeByMnemonic("unreachable");
    ASSERT_TRUE(callIndOp.has_value())
        << "call_indirect_via_extern opcode must be declared in "
           "x86_64.target.json (Slice A addition)";
    ASSERT_TRUE(unreachableOp.has_value());

    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    // Operand 0 = SymbolRef to extern import (e.g. kernel32!ExitProcess
    // when the trampoline emitter wires this up). The target SymbolId
    // is the synthetic ExternImport's id; the linker resolves the
    // disp32 to the IAT slot's VA at link time.
    LirOperand const ops[] = { LirOperand::makeSymbolRef(99) };
    (void)b.addInst(*callIndOp, LirReg{}, ops);
    (void)b.addUnreachable(*unreachableOp);
    Lir lir = std::move(b).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    auto result = assemble(lir, **schema, lirToMir, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(result.functions.size(), 1u);
    auto const& bytes  = result.functions[0].bytes;
    auto const& relocs = result.functions[0].relocations;
    // call_indirect_via_extern: FF 15 disp32 (6 bytes) + unreachable
    // 0F 0B (2 bytes) = 8 bytes total.
    ASSERT_EQ(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0xFF)
        << "call_indirect_via_extern first opcode byte must be 0xFF";
    EXPECT_EQ(bytes[1], 0x15)
        << "ModR/M byte 0x15 = mod=00 reg=/2 rm=101 → [RIP+disp32]";
    // disp32 placeholder zero-filled — linker patches at link time.
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0x00);
    EXPECT_EQ(bytes[4], 0x00);
    EXPECT_EQ(bytes[5], 0x00);
    // Trailing unreachable.
    EXPECT_EQ(bytes[6], 0x0F);
    EXPECT_EQ(bytes[7], 0x0B);

    // Exactly one relocation at disp32 patch site (offset 2 past
    // FF 15) targeting the extern SymbolId{99}, kind = rel32.
    ASSERT_EQ(relocs.size(), 1u);
    EXPECT_EQ(relocs[0].offset, 2u)
        << "reloc patch site is at byte 2 (just after FF 15)";
    EXPECT_EQ(relocs[0].target.v, 99u);
    EXPECT_EQ(relocs[0].addend, 0)
        << "addend must be 0 — `rel32` row's addendBias=-4 already "
           "encodes the displacement bias; a non-zero addend here "
           "would double-count it and silently skew the IAT-slot VA. "
           "test-analyzer dim-2 H1 / 2-agent convergence at 756c5ea "
           "audit (FOLD-NOW pin against D-AS4-4 producer regression).";
    auto const rel32Info =
        (*schema)->relocationByName("rel32");
    ASSERT_NE(rel32Info, nullptr);
    EXPECT_EQ(relocs[0].kind, rel32Info->kind);
}

TEST(LK10EntrySliceA, FullTrampolineShapeSyscallArm) {
    // Round-trip pin for the full Stage-1 syscall trampoline body
    // (the 5-op sequence per plan §2.13). This is the LIR
    // construction the trampoline emitter will produce (without yet
    // wiring through `entry_trampoline.{hpp,cpp}` — that lands in
    // Slice C); the test pins the substrate's end-to-end consumption
    // of the new opcodes via LirBuilder + assemble().
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const callOp    = (*schema)->opcodeByMnemonic("call");
    auto const movOp     = (*schema)->opcodeByMnemonic("mov");
    auto const syscallOp = (*schema)->opcodeByMnemonic("syscall");
    auto const unreachOp = (*schema)->opcodeByMnemonic("unreachable");
    ASSERT_TRUE(callOp.has_value());
    ASSERT_TRUE(movOp.has_value());
    ASSERT_TRUE(syscallOp.has_value());
    ASSERT_TRUE(unreachOp.has_value());

    auto const raxOrd = (*schema)->registerByName("rax");
    auto const rdiOrd = (*schema)->registerByName("rdi");
    ASSERT_TRUE(raxOrd.has_value());
    ASSERT_TRUE(rdiOrd.has_value());

    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{42});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    // Physical regs use `makePhysicalReg(ordinal, regClass)` — bare
    // `LirReg{ordinal}` defaults to a virtual reg whose id collides
    // with the physical ordinal namespace.
    auto const raxPhys = makePhysicalReg(*raxOrd, LirRegClass::GPR);
    auto const rdiPhys = makePhysicalReg(*rdiOrd, LirRegClass::GPR);
    // 1. call user_entry (SymbolRef = 88, the user function)
    LirOperand const callOps[] = { LirOperand::makeSymbolRef(88) };
    (void)b.addInst(*callOp, InvalidLirReg, callOps);
    // 2. mov rdi, rax (status from return reg to syscall arg reg)
    LirOperand const movRegOps[] = { LirOperand::makeReg(raxPhys) };
    (void)b.addInst(*movOp, rdiPhys, movRegOps);
    // 3. mov rax, 231 (exit_group syscall number)
    LirOperand const movImmOps[] = { LirOperand::makeImmInt32(231) };
    (void)b.addInst(*movOp, raxPhys, movImmOps);
    // 4. syscall
    (void)b.addInst(*syscallOp, InvalidLirReg, {});
    // 5. unreachable
    (void)b.addUnreachable(*unreachOp);
    Lir lir = std::move(b).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    auto result = assemble(lir, **schema, lirToMir, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "full syscall trampoline must assemble cleanly via the "
           "Slice A opcodes — proves Slice A is wired through "
           "assemble() not just encoder-row tests (silent-failure "
           "FN-2 discipline)";
    ASSERT_EQ(result.functions.size(), 1u);
    auto const& bytes  = result.functions[0].bytes;
    auto const& relocs = result.functions[0].relocations;
    // Exactly one relocation on the call (REL32 → SymbolId{88}).
    ASSERT_EQ(relocs.size(), 1u);
    EXPECT_EQ(relocs[0].target.v, 88u);
    EXPECT_EQ(relocs[0].addend, 0)
        << "addend must be 0 — see CallIndirectViaExternEmitsRel32Reloc";
    // Pin TOTAL byte count + middle byte sequence (3-agent
    // convergence: test-analyzer H2 + dim-2 H2 + code-architect Q6
    // FOLD-NOW at 756c5ea audit). A regression in the middle ops'
    // encodings (REX.W byte, mov-reg variant selection, imm32
    // bytes) would silently leave the suffix-only pin passing.
    //   call rel32        : E8 00 00 00 00          = 5 bytes
    //   mov rdi, rax      : 48 8B F8                = 3 bytes (REX.W 8B /r, ModR/M mod=11 reg=rdi=111 rm=rax=000 = F8)
    //   mov rax, 231      : 48 C7 C0 E7 00 00 00    = 7 bytes (REX.W C7 /0 imm32, ModR/M mod=11 reg=/0 rm=rax=000 = C0)
    //   syscall           : 0F 05                   = 2 bytes
    //   unreachable (ud2) : 0F 0B                   = 2 bytes
    //                                          total = 19 bytes
    // The mov-reg-reg variant uses opcode 0x8B (MOV r64, r/m64) with
    // resultSlot=modrm.reg and operand0 wired to modrm.rm — see the
    // `mov` opcode's "reg" variant in x86_64.target.json.
    ASSERT_EQ(bytes.size(), 19u);
    EXPECT_EQ(bytes[5], 0x48);
    EXPECT_EQ(bytes[6], 0x8B);
    EXPECT_EQ(bytes[7], 0xF8);
    EXPECT_EQ(bytes[8],  0x48);
    EXPECT_EQ(bytes[9],  0xC7);
    EXPECT_EQ(bytes[10], 0xC0);
    EXPECT_EQ(bytes[11], 0xE7);  // 231 LE byte 0
    EXPECT_EQ(bytes[12], 0x00);
    EXPECT_EQ(bytes[13], 0x00);
    EXPECT_EQ(bytes[14], 0x00);
    EXPECT_EQ(bytes[15], 0x0F);
    EXPECT_EQ(bytes[16], 0x05);
    EXPECT_EQ(bytes[17], 0x0F);
    EXPECT_EQ(bytes[18], 0x0B);
}

TEST(LK10EntrySliceA, FullTrampolineShapeByNameImportArm) {
    // Round-trip pin for the Windows ByNameImport trampoline (4-op
    // sequence; shadow-space sub-rsp omitted for this test — it's a
    // separate concern handled by Slice C reading callingConventions[
    // ms_x64].shadowSpaceBytes). Pins call + call_indirect_via_extern
    // + unreachable working together end-to-end.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const callOp     = (*schema)->opcodeByMnemonic("call");
    auto const movOp      = (*schema)->opcodeByMnemonic("mov");
    auto const callIndOp  =
        (*schema)->opcodeByMnemonic("call_indirect_via_extern");
    auto const unreachOp  = (*schema)->opcodeByMnemonic("unreachable");
    ASSERT_TRUE(callOp.has_value());
    ASSERT_TRUE(movOp.has_value());
    ASSERT_TRUE(callIndOp.has_value());
    ASSERT_TRUE(unreachOp.has_value());

    auto const raxOrd = (*schema)->registerByName("rax");
    auto const rcxOrd = (*schema)->registerByName("rcx");
    ASSERT_TRUE(raxOrd.has_value());
    ASSERT_TRUE(rcxOrd.has_value());

    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{42});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    auto const raxPhys = makePhysicalReg(*raxOrd, LirRegClass::GPR);
    auto const rcxPhys = makePhysicalReg(*rcxOrd, LirRegClass::GPR);
    LirOperand const callOps[] = { LirOperand::makeSymbolRef(88) };
    (void)b.addInst(*callOp, InvalidLirReg, callOps);
    LirOperand const movRegOps[] = { LirOperand::makeReg(raxPhys) };
    (void)b.addInst(*movOp, rcxPhys, movRegOps);
    LirOperand const exitOps[] = { LirOperand::makeSymbolRef(99) };
    (void)b.addInst(*callIndOp, InvalidLirReg, exitOps);
    (void)b.addUnreachable(*unreachOp);
    Lir lir = std::move(b).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    auto result = assemble(lir, **schema, lirToMir, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(result.functions.size(), 1u);
    auto const& bytes  = result.functions[0].bytes;
    auto const& relocs = result.functions[0].relocations;
    // Two REL32 relocs: one on the direct call (target 88, user_entry),
    // one on the indirect call (target 99, ExitProcess IAT slot).
    ASSERT_EQ(relocs.size(), 2u);
    EXPECT_EQ(relocs[0].target.v, 88u);
    EXPECT_EQ(relocs[1].target.v, 99u);
    EXPECT_EQ(relocs[0].addend, 0);
    EXPECT_EQ(relocs[1].addend, 0);
    // Reloc emission order is well-defined: direct call (offset 1)
    // precedes indirect call (offset 5 + 3 + 2 = 10) — dim-2 M2 pin
    // catches a regression that reorders the relocs vector.
    EXPECT_LT(relocs[0].offset, relocs[1].offset);
    // Pin TOTAL byte count:
    //   call rel32        : E8 00 00 00 00          = 5 bytes
    //   mov rcx, rax      : 48 8B C8                = 3 bytes (REX.W 8B /r, ModR/M mod=11 reg=rcx=001 rm=rax=000 = C8)
    //   FF 15 disp32      : FF 15 00 00 00 00       = 6 bytes
    //   unreachable (ud2) : 0F 0B                   = 2 bytes
    //                                          total = 16 bytes
    ASSERT_EQ(bytes.size(), 16u);
    EXPECT_EQ(bytes[5], 0x48);
    EXPECT_EQ(bytes[6], 0x8B);
    EXPECT_EQ(bytes[7], 0xC8);
    EXPECT_EQ(bytes[8],  0xFF);
    EXPECT_EQ(bytes[9],  0x15);
    EXPECT_EQ(bytes[14], 0x0F);
    EXPECT_EQ(bytes[15], 0x0B);
}

// dim-2 M1 fold (negative-guard contract pins on the new Slice A
// opcodes). The encoder's variant-guard match (walker_util.hpp:108)
// is length+kind exact; a regression to a length-only or
// prefix-style match would silently accept malformed operands.
// Three loud-fail pins on the new opcodes specifically.

TEST(LK10EntrySliceA, SyscallWithStrayOperandFailsLoud) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const syscallOp = (*schema)->opcodeByMnemonic("syscall");
    auto const unreachOp = (*schema)->opcodeByMnemonic("unreachable");
    ASSERT_TRUE(syscallOp.has_value());
    ASSERT_TRUE(unreachOp.has_value());

    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    // A stray imm operand on a zero-operand opcode does NOT match
    // the empty-list variant guard. ff7b76d 3rd-order audit:
    // silent-failure verified `LirBuilder::addInst` (lir.cpp:210-227)
    // checks only `opcode != 0` and `opcodeInfo(opcode) != nullptr`
    // — it does NOT enforce min/max-operands (see
    // `x86_64.target.json:8` $reservedFields: "minOperands/
    // maxOperands fields are NOT yet fully enforced"). The encoder's
    // variant-guard match in `walker_util.hpp::operandsMatchGuard`
    // is the SOLE gate today. Anchor D-LIR-BUILDER-OPERAND-COUNT-GATE
    // — if a future LirBuilder operand-count gate is added, the test
    // would start to fail at addInst (different code path); that
    // signals an intentional substrate change the test author
    // updates for.
    LirOperand const strayOps[] = { LirOperand::makeImmInt32(42) };
    (void)b.addInst(*syscallOp, InvalidLirReg, strayOps);
    (void)b.addUnreachable(*unreachOp);
    Lir lir = std::move(b).finish();
    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    (void)assemble(lir, **schema, lirToMir, rep);
    // silent-failure FOLD-NOW (3rd-order audit): pin the SPECIFIC
    // diagnostic code, not just `errorCount > 0` — a regression
    // firing the wrong diagnostic (e.g. an unrelated infra error)
    // would silently satisfy a permissive `EXPECT_GT(errorCount, 0)`.
    EXPECT_GT(::dss::test_support::countCode(
                  rep, DiagnosticCode::A_NoMatchingEncodingVariant), 0u)
        << "syscall with stray operand must fire "
           "A_NoMatchingEncodingVariant from the encoder's variant "
           "guard — the empty-list guard cannot match a 1-operand inst.";
}

TEST(LK10EntrySliceA, CallIndirectViaExternWithRegOperandFailsLoud) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const callIndOp =
        (*schema)->opcodeByMnemonic("call_indirect_via_extern");
    auto const unreachOp = (*schema)->opcodeByMnemonic("unreachable");
    ASSERT_TRUE(callIndOp.has_value());
    ASSERT_TRUE(unreachOp.has_value());
    auto const raxOrd = (*schema)->registerByName("rax");
    ASSERT_TRUE(raxOrd.has_value());

    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    // Reg operand instead of SymbolRef — must NOT match the
    // {operandKinds: ["symbol"]} variant guard.
    LirOperand const wrongOps[] = {
        LirOperand::makeReg(makePhysicalReg(*raxOrd, LirRegClass::GPR))
    };
    (void)b.addInst(*callIndOp, InvalidLirReg, wrongOps);
    (void)b.addUnreachable(*unreachOp);
    Lir lir = std::move(b).finish();
    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    (void)assemble(lir, **schema, lirToMir, rep);
    // Pin specific diagnostic code (silent-failure ff7b76d 3rd-order
    // audit FOLD-NOW) — the encoder's variant-guard mismatch fires
    // A_NoMatchingEncodingVariant. A regression that fires a
    // different code would silently pass a loose errorCount > 0.
    EXPECT_GT(::dss::test_support::countCode(
                  rep, DiagnosticCode::A_NoMatchingEncodingVariant), 0u)
        << "call_indirect_via_extern with Reg operand must fire "
           "A_NoMatchingEncodingVariant — the variant guard "
           "{operandKinds: [\"symbol\"]} requires SymbolRef.";
}
