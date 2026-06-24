/* D-PARSE-DEEP-NEST-RECURSION-MEMORY (plan 24 Stage 4-cfg) end-to-end witness
 * for the now-iterative HIR->MIR control-flow VALUE lowerer. The Ternary value
 * arm in src/mir/lowering/hir_to_mir.cpp is now a multi-phase frame on the
 * shared {Value,Address} explicit WORK-STACK DRIVER (it interleaves
 * createBlock/addCondBr/addBr/beginBlock/addPhi with its cond/then/else
 * sub-lowerings across phases), so a deeply right-nested `?:` chain lowers
 * HIR->MIR with FLAT O(1) host-stack cost per nesting level instead of one
 * host-recursion frame per level.
 *
 * This program is a 150-level right-nested ternary `c0 ? t0 : c1 ? t1 : ... :
 * pick(42)`. Right-associativity makes each ELSE arm the next ternary, so the
 * 150-deep nesting (and the recursion it would otherwise cost) grows down the
 * ELSE spine -- exactly the axis the work-stack flattening removes. Every
 * condition is `pick(0)` and every THEN arm is `pick(99)`: `pick` is an
 * un-inlined identity, so the optimizer cannot fold the conditions away and the
 * full diamond/phi CFG survives to MIR. All conditions are runtime-false, so
 * control walks the ENTIRE else spine to the innermost else `pick(42)` -> the
 * program returns 42.
 *
 * MISCOMPILE-SENSITIVE: a Ternary phase bug that swapped a join phi's then/else
 * incomings (or switched to the wrong insertion block) would yield a THEN value
 * `pick(99)` -> exit 99, not 42; a dropped/duplicated diamond would corrupt the
 * walk. A regression that UNWRAPPED the Ternary arm back to host recursion would
 * recurse 150 frames deep during HIR->MIR lowering (the lowering runs on the 64
 * MiB worker stack, core/substrate/large_stack_call, and 150 nested diamonds is
 * already well past the host main-stack crash point for the heavy recursive
 * lowering frame), so an accidental main-stack regression would crash (rc-127)
 * here instead of exiting 42.
 *
 * OUTPUT-IDENTITY against the prior recursive lowerer is enforced by the full
 * ctest suite (the byte-identical MIR block/phi goldens in
 * tests/mir/test_mir_lowering_c_subset.cpp, plus the SF-4 strict differential
 * pins IterativeDeepTernaryChainLowersFlatAndByteIdentical /
 * IterativeDeepLogicalChainLowersFlatAndByteIdentical which pin the exact deep
 * block-id + phi-predecessor sequence on the test's main stack); this example
 * adds the deep-flatten + real-codegen RUN witness for the MIR Ternary arm. The
 * optimizedPipelines arm proves the lowered MIR survives the full release
 * pipeline (the baseline arm is the real-codegen witness). arm64 runs under
 * qemu; macho on the macos-latest leg. */
int pick(int x) { return x; }

int main(void) {
    return pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(0) ? pick(99) : pick(42);
}
