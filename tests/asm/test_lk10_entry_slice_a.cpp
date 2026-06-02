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
#include "lir/lir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace dss;

namespace {

// Helper: load the shipped x86_64 schema, assert success, run a tiny
// one-function Lir through assemble(), return the result. Tests use
// it to keep the boilerplate out of every TEST.
struct Slice {
    std::shared_ptr<TargetSchema> schema;
    AssembledModule               result;
    DiagnosticReporter            rep;
};

}  // namespace

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
    // Final bytes should end in 0F 05 (syscall) then 0F 0B (unreachable).
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(bytes[bytes.size() - 4], 0x0F);
    EXPECT_EQ(bytes[bytes.size() - 3], 0x05);
    EXPECT_EQ(bytes[bytes.size() - 2], 0x0F);
    EXPECT_EQ(bytes[bytes.size() - 1], 0x0B);
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
    // Final bytes end in unreachable (0F 0B).
    ASSERT_GE(bytes.size(), 2u);
    EXPECT_EQ(bytes[bytes.size() - 2], 0x0F);
    EXPECT_EQ(bytes[bytes.size() - 1], 0x0B);
}
