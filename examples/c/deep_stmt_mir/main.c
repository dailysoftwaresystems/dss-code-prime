/* D-PARSE-DEEP-NEST-RECURSION-MEMORY (plan 24 Stage 4b) end-to-end witness for
 * the now-iterative HIR->MIR STATEMENT lowerer. The control-flow statement arms in
 * src/mir/lowering/hir_to_mir.cpp (Block child-list, IfStmt then/else, While/DoWhile/
 * For bodies, LabelStmt, SwitchStmt arms) are multi-phase frames on an explicit
 * StmtFrame WORK-STACK DRIVER (lowerStmt): a control-flow statement mints its blocks
 * + emits its branches at the right phases while its sub-statements RE-ENTER the
 * driver, so a deeply-nested statement nest lowers HIR->MIR with FLAT O(1) host-stack
 * cost per level. This program is a 250-level nest { if (a) { ... { g = 42; } ... } }
 * combining the Block child-list arm and the IfStmt then-arm (~2-3 lowerStmt frames
 * per level), exactly the axis the work-stack flattening removes.
 *
 * a is read ONCE from t(), which returns 1 and is DECLARED noinline, so a is a runtime
 * value no pipeline can fold -> the full nested-diamond CFG survives lowering (the deep
 * flatten witness) AND every optimized arm; every if(a) is runtime-true, so control
 * descends the ENTIRE then-spine to the innermost g = 42; -> returns 42. The noinline
 * is load-bearing: without it the shipped release pipeline inlines t(), folds all 250
 * conditions and deletes the nest (MEASURED 2026-09-19, pe64: the release image was
 * 3584 bytes against 8704 with noinline), so the release arm would have witnessed
 * nothing about the nest while this comment said it could not be folded.
 *
 * The result lives in a GLOBAL g (memory), so the nest stays pure control flow with
 * ~O(1) register pressure and this remains a focused STATEMENT-FLATTENING witness. The
 * same nest with g as a LOCAL -- which Mem2Reg promotes into an SSA phi chain as deep
 * as the nest -- is its own example, deep_stmt_mir_local_phi_web. That shape was once
 * refused (x86 L_VirtualRegInPostRegalloc scratch exhaustion; arm64 frame stores past
 * the unscaled imm9 slot); MEASURED 2026-09-18/19 it compiles and runs to 42 on
 * x86_64 and arm64 ELF and on pe64, at debug and release. Keeping g global here is a
 * choice of focus, not a workaround.
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
__attribute__((noinline)) int t(void) { return 1; }

int main(void) {
    int a = t();
    { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { if (a) { g = 42; } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } } }
    return g;
}
