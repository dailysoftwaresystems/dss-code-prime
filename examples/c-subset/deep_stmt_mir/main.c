/* D-PARSE-DEEP-NEST-RECURSION-MEMORY (plan 24 Stage 4b) end-to-end witness for
 * the now-iterative HIR->MIR STATEMENT lowerer. The control-flow statement arms in
 * src/mir/lowering/hir_to_mir.cpp (Block child-list, IfStmt then/else, While/DoWhile/
 * For bodies, LabelStmt, SwitchStmt arms) are multi-phase frames on an explicit
 * StmtFrame WORK-STACK DRIVER (lowerStmt): a control-flow statement mints its blocks
 * + emits its branches at the right phases while its sub-statements RE-ENTER the
 * driver, so a deeply-nested statement nest lowers HIR->MIR with FLAT O(1) host-stack
 * cost per level. This program is a -level nest { if (a) { ... { g = 42; } ... } }
 * combining the Block child-list arm and the IfStmt then-arm (~2-3 lowerStmt frames
 * per level), exactly the axis the work-stack flattening removes.
 *
 * a is read ONCE from an un-inlined t() returning 1, so a is a runtime value the
 * optimizer cannot fold -> the full nested-diamond CFG survives lowering (the deep
 * flatten witness); every if(a) is runtime-true, so control descends the ENTIRE
 * then-spine to the innermost g = 42; -> returns 42. The result lives in a GLOBAL g
 * (memory), DELIBERATELY not a local: a local assigned at the bottom + merged up
 * through every level is mem2reg-promoted into a depth-deep SSA phi web (~N live
 * values) that exhausts the register file under the release pipeline (x86
 * L_VirtualRegInPostRegalloc scratch exhaustion; arm64 spills past the unscaled imm9
 * frame-store slot, the separate backend gap: the scaled LDR/STR imm12 frame-store form is unimplemented, fail-loud, a residual of the closed D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12 large-frame family).
 * A global stays in memory -> no phi web -> the deep nest is pure control flow with
 * ~O(1) register pressure, so this stays a STATEMENT-FLATTENING witness rather than a
 * register-pressure / large-frame backend stress test.
 *
 * MISCOMPILE-SENSITIVE: g starts 0 and only the innermost statement sets it to 42. A
 * statement-frame phase bug (wrong CondBr edge, dropped/duplicated diamond, wrong
 * sub-statement block, mis-ordered Store/Br) would skip the deep g = 42; -> return 0,
 * not 42. OUTPUT-IDENTITY vs the prior recursive lowerer is enforced by the full ctest
 * suite (byte-identical MIR control-flow goldens + the strict pin
 * IterativeDeepIfNestLowersFlatAndByteIdentical, red-on-disable to a swapped
 * successor); this example adds the deep-flatten + real-codegen RUN witness. The
 * optimizedPipelines arm proves the lowered MIR survives the full release pipeline;
 * the baseline arm is the real-codegen witness. arm64 runs under qemu; macho on the
 * macos-latest leg. */
int g = 0;
int t(void) { return 1; }

int main(void) {
    int a = t();
    { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { g = 42; } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } }
    return g;
}
