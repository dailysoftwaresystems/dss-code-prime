# Plan 24 — Iterative Expression+Statement Traversal (close D-PARSE-DEEP-NEST-RECURSION-MEMORY)

**Status:** 🟦 GO — staged build (branch feature/0-0-2-p14, 2026-06-23). Plan-lock v1 = REWORK →
design v2 folds every finding (§4 ledger). User signed off the corrected six-family scope (4th
confirmation) + authorized "as many cycles as needed"; backup branch
`feature/0-0-2-p14-before-rework` exists for comparison. **Stage 1 (semantic `subtreeType`) ✅
DONE**; **Stage 2 (semantic `pass1`/`pass2`) ✅ DONE** (output-identity 376 ctest +
deep nested-block scope pin) — Stage 3 (`cst_to_hir` expr) next.

## §0 — Decision provenance & HONEST (corrected) scope

User chose **"Full iterative rewrite"** 3× with full disclosure: NOT a crash fix (all passes
already run on a 64 MiB `callOnLargeStack` worker), NO production consumer (valid C nests ≤4),
highest-risk multi-cycle surgery, pursued as a **robustness goal** ("robust before complete").

**★ Plan-lock v1 correction (re-disclose at sign-off): the surface is SIX recursion families,
not four passes** — the design must reflect this and the cycle/risk estimate rises accordingly:
1. Parser `parseExpressionAt` (expr) — speculation stays recursive (bounded 8).
2. CST→HIR `lowerExpr` (expr) **+ its re-entry into statement lowering** (comma → `ExprStmt`,
   `classifyLvalue` prep stmts).
3. CST→HIR statement lowering (the stmt tier `lowerExpr` re-enters; blocks/if/while nest).
4. HIR→MIR `lowerExpr` (expr) **+ `lowerStmt`** (`hir_to_mir.cpp:4237`; SeqExpr/comma →
   `lowerStmt` → nested stmt bodies → `lowerExpr`). Statement nesting is the DEEPER axis.
5. Semantic `pass1`/`pass2` (whole-tree walk) + `subtreeType` (expr typer).
6. Const-eval — CST `evalImpl` (`cst_const_eval.cpp:69`) + HIR `evaluateConstant`
   (`const_eval.cpp`); on the `int a[((((1)))) ]` / global-init deep paths.

**★ BC-1 (fail-loud, NON-NEGOTIABLE): the cap is NEVER removed.** Stage-final raises
`maxExpressionDepth` to a high explicit ceiling (e.g. 1,000,000) and keeps `P_ExpressionTooDeep`
as the positioned backstop. The `large_stack_call` worker is RETAINED for every recursion NOT
yet proven flat (esp. type-node/declarator recursion — C-2, OUT of scope). The cap-lift is gated
on a **proven-flat checklist** covering all six families; dropping the large-stack reliance is
scoped to ONLY the proven-flat (expression+statement+const-eval) path.

## §1 — Shared technique (plan-lock: SOUND — unchanged)

Each recursive `R(node)→result` becomes a driver loop over an explicit `std::vector<Frame>`
work-stack; each `Frame` is a POD `{node, uint8_t phase, <per-level locals>, <child-result slots>}`.
A phase either pushes a child frame + advances its own phase, or finishes and delivers its result
into the parent frame's phase-keyed slot. Interleaved side effects (CFG block emission, scope
push, HIR/ExprStmt emission) become phase transitions. **Mutual recursions share ONE work-stack
with a frame-KIND tag** (parser {Climb,Primary,AtomDrive}; cst_to_hir {Expr,Lvalue,Stmt};
hir_to_mir {Value,Address,Stmt}). **THE gate = OUTPUT-IDENTITY:** byte-identical CST/HIR/MIR/Type/
diagnostics/arena-order vs the recursive version, enforced by the full 374-test suite green after
every stage + the deep differential pins below (§3).

## §2 — The hard parts (plan-lock-corrected)

**Parser (SF-1 restated):** NOT "suspend/resume a loop." `parseUntilFrameDepth` is stateless
(`while frames.size()>targetDepth { stepOnce() }`, parser.cpp:1146) — all real state is already
heap. Hoist `parseExpressionAt`+`parsePrimary`+`parseUntilFrameDepth` into ONE unified driver with
frame kinds {Climb,Primary,AtomDrive}; an AtomDrive frame's "resume" is just "re-test the depth
predicate → `stepOnce` again or pop." **SF-1 correctness item: the watchdog tuple
(`lastCursor/lastTokPos/lastDepth`, currently one `Impl`-level tuple, parser.cpp:1390) must become
a per-AtomDrive-frame snapshot** so an inner climb can't desync the outer drive's stall detector.

**Parser speculation (SF-2 — THE top silent-desync risk):** speculation stays recursive (bounded
8), but `trySpeculativeBranch`→`walkExpression`→`parseExpressionAt`(iterative) means a live
`SpeculationProbe` WRAPS the iterative climb. The probe dtor (parser.cpp:1035-1066) restores
`frames`/walker/builder/tokens/sketch but knows nothing of the NEW climb work-stack → a rolled-back
probe leaves orphan climb frames → desync. **FIX: `SpeculationProbe` snapshots a `climbDepthBefore_`
at construction and pops the climb work-stack back to it in the dtor, mirroring the `frames`
restore at 1048-1053.** Pin: a speculative cast-vs-paren probe that rolls back AT depth, asserting
the climb stack is empty after.

**HIR→MIR CFG arms (MF-1):** Ternary (1071)/LogicalAnd-Or (1106) are 3-4 phase diamonds recording
predecessor block IDs BETWEEN child lowerings (frame holds `thenBB/elseBB/joinBB/lhsPred/...`).
SeqExpr (1394) re-enters `lowerStmt` → the work-stack frame-kind set MUST include {Stmt}; statement
nesting is a first-class stage (4b), not "a stmt-index cursor."

**CST→HIR (MF-2):** `lowerExpr↔classifyLvalue` mutual recursion + `classifyLvalue` synthesizes prep
stmts; expr lowering is reached FROM statement lowering. Frame-kind set {Expr,Lvalue,Stmt}.
`lowerFlatExpr` (SQL) shares the `combineBinary` helper but has its own driver (C-1) → its own
frame kind.

## §3 — Staging (re-organized for six families; gating; risk)

| # | Stage | Risk | Per-stage gate (all + full 374 ctest green = OUTPUT-IDENTITY) |
|---|---|---|---|
| 1 | semantic `subtreeType` ✅ **DONE** | low | OUTPUT-IDENTITY 374→375 ctest green + corpus `deep_subtree_type_dim` (200-deep parens around a `long long` in an array-dim `sizeof`, typed at Pass-1.5 on the UNSTAMPED CST → 8 → exit 42, value-sensitive). **Validation deviation (honest):** the planned isolated small-stack unit pin (SF-4) was infeasible — BOTH `subtreeType` AND `EngineState` are anonymous-namespace (no test-callable seam without API pollution). Used a deep-CORRECTNESS corpus pin instead (big stack); the small-stack FLAT property is validated end-to-end at Stage 7. Used deep PARENS (Wrapper arm, ~150ms) not deep BINARY (parser-bound: 18s at depth 60 — the parser super-linearity this arc fixes at Stage 5, so a deep-binary pin is impractical until then). |
| 2 | semantic `pass1`/`pass2` ✅ **DONE** | low-med | OUTPUT-IDENTITY 375→376 ctest green + corpus `deep_stmt_scope_walk` (150 nested block scopes; pass1 builds a 150-deep scope chain, pass2 walks it + the innermost `return base` resolves through all 150 scopes → exit 42, value-sensitive). Conversion: extracted per-node helpers (`pass1Node` returns the child scope `here`; `pass2Post` is the post-child handling, where a `return;` ends that node's post-work not the whole walk) + explicit-stack drivers — `pass1` pre-order (pop→work→push children), `pass2` two-phase post-order (phase 0 resolve scope/loopDepth+push children, phase 1 `pass2Post`); children pushed in reverse for source-order LIFO; realloc-safe (copy frame fields before any push_back). Deep BLOCKS (parse O(N), no expr speculation) not deep binary. |
| 3 | cst_to_hir `lowerExpr` ({Expr} arms) | med | **INCREMENT 1 ✅ BUILT (2026-06-23, delegated agent → my gate + cross-platform validate).** `lowerExpr` is now an explicit heap work-stack DRIVER: an `enter` lambda that ONLY pushes (never recurses) + `std::vector<ExprFrame>` (Kind {PassThrough,Binary,Unary} + phase machine) + a shared `result` slot, using the Stage-1/2 realloc-safe idiom (copy frame fields + advance phase BEFORE each enter; combine copies out before pop). FLATTENED arms: paren/wrapper descent (PassThrough), PLAIN binary operands (Binary frame builds **RHS-then-LHS** to match the recursive `combineBinary(node,e,lowerExpr(lhs),lowerExpr(rhs))` arg-eval order), unary operand (Unary). DELEGATED unchanged (re-enter the driver for their operands, so deep operands inside shallow complex arms still flatten): Comma/Assign (lowerBinary), ternary, postfix (Call/Index/Member/PostInc), cast, sizeof, flatExpr, classifyLvalue, operand leaf/identifier terminals. Extractions (byte-identical slices): `lowerOperandTerminal` (paren→nullopt+descendInto), `combineUnaryOp`, `plainBinaryEntry`/`unaryEntry` (nullptr→delegate). **OUTPUT-IDENTITY VALIDATED on BOTH MSVC (right-to-left arg eval) AND MinGW-GCC (left-to-right): 377/377 each** → proves NO committed golden captures the lhs/rhs literal-pool/arena order, so the fixed RHS-first is cross-platform-safe (the old recursive form's platform-dependent order being green on all legs is the proof). Corpus `deep_expr_lower` (200 nested parens + unary + plain `40+2`, exit 42, full-release arm). `P_ExpressionTooDeep` cap UNCHANGED. **INCREMENT 2 ✅ BUILT (2026-06-23, delegated agent → my gate; 378/378 MSVC + MinGW-GCC).** Flattened 5 more arms onto the driver (new ExprFrame kinds Cast/Postfix/Ternary/Comma): cast, postfix Member (`a.b`/`a->b`), postfix Index (`a[i]` + index-promote), ternary (`coerceCondition` as a phase-1 transition between cond and the arms), comma (`ExprStmt(lhs)` as a phase-1 transition between lhs and rhs). Extractions: castPrologue/combineCast, combineMember/combineIndex, ternaryOperands/combineTernary, combineComma + classifiers postfixFlattenPlan/commaBinary; `lowerOperandTerminal` gained a castNode out-param; dead `lowerOperand` wrapper REMOVED. None of these arms depend on the host's arg-eval order (single-child or sequential statements) → cross-platform-safe by construction (MinGW left-to-right confirms). Corpus `deep_ternary_chain` (150 right-nested ternaries, exit 42, full-release arm). **INCREMENT 3 ✅ BUILT (2026-06-23, delegated agent → my gate; 379/379 MSVC + MinGW-GCC).** Postfix CALL flattened (new ExprFrame Kind::Call + a `callCtxs` LIFO stack LOCAL to lowerExpr holding the accumulating callee/args/paramTypes; the frame stores only a STABLE INDEX `aux` — indices survive `work`/`callCtxs` realloc where references would dangle; `pumpCallArgs` re-addresses `callCtxs[idx]` fresh each access since a scalar arg that is itself a call grows the vector). Callee first, then args LEFT-TO-RIGHT with per-arg coerce (a sequential loop → platform-independent, MinGW confirms); brace-init args lower inline (`lowerExprOrBraceInit`); the M2 stable-paramTypes copy is preserved. Dead `lowerCast` recursive entry REMOVED (castPrologue/combineCast retained). Corpus `deep_call_chain` (150 nested `id()` calls, exit 42). **RESIDUAL — the {Lvalue} surgery (deferred): Assign + PostInc/PostDec route through `classifyLvalue` (interleaved prep-stmt synthesis + `lowerExpr(core)`); flattening is HIGH-risk-for-output-identity + LOW-payoff (only deep `a=b=c=…` / `x++ ++…` CHAINS recurse; their operands already flatten via the driver). Left DELEGATING. → resolve at Stage 7: either add a {Lvalue} frame OR lift the cap to a high-but-bounded value the 64 MB worker covers (a deep assign/postinc chain then FAILS LOUD at the cap, never overflows).** Stage 3 expr lowering now flattens 9 of 12 arm-classes: paren, binary, unary, cast, member, index, ternary, comma, Call. |
| 3b | cst_to_hir statement tier ({Stmt}) ✅ **DONE** (2026-06-24, delegated agent → my gate; 380/380 MSVC + MinGW-GCC) | med | `lowerStmt` is now an explicit work-stack DRIVER (`enterStmt` only-pushes + `std::vector<StmtFrame>` Kind{PassThrough,Block,If,While,For,Label} + phase machine + `stmtResult` slot + a `blockCtxs` LIFO accumulator for the unbounded block `stmts` list, referenced by stable INDEX `aux` — the inc-3 `callCtxs` mechanism). FLATTENED: transparent wrapper, Block, If/If-else, While/DoWhile, For, Label — each enters its child STATEMENTS in source order (then-before-else; list left-to-right), with conditions/for-clauses lowered INLINE in phase 0 via the now-flat `lowerExpr`/`lowerForClause` at the same position (cond/init/update arena ids precede the body's, as before). Shared prologue/epilogue helpers (`blockChildNodes`/`ifPrologue`/`whilePrologue`/`forPrologue`/`labelPrologue`) = ONE source of truth for the frames + the retained (now-dead, like `lowerOperand` was) recursive handlers. DELEGATED: terminals (VarDecl/ExprStmt/Return/Break/Continue/Goto/IndirectGoto/Skip/Extension — no nested child stmt) + SWITCH (intricate label-peeling `lowerSwitch` stays recursive but RE-ENTERS the driver per `lowerStmt(body)`, so deep stmts inside arms still flatten). Emission-order platform-independent (sequential children) — MinGW left-to-right confirms. Corpus `deep_block_nest` (200 nested `{}` blocks → exit 42, full-release arm); adversarial 180-deep if/else + 150-deep for ran flat. Cap UNCHANGED. RESIDUAL: Switch arm-grouping recursion (deferred — rare deep-switch; arms flatten via re-entry). |
| 4 | hir_to_mir `lowerExpr` straight-line value arms ({Value}) ✅ **DONE** (2026-06-24, delegated agent → my gate; 381/381 MSVC + MinGW-GCC) | **high** | `src/mir/lowering/hir_to_mir.cpp` `lowerExpr` (the value giant-switch) is now an explicit work-stack DRIVER for the STRAIGHT-LINE arms: UnaryOp, BinaryOp, Deref, Cast (array→ptr decay excluded → delegates, it mints rodata). **KEY DISTINCTION FROM cst_to_hir: the BinaryOp frame builds LHS-then-RHS LEFT-TO-RIGHT** — the recursive hir_to_mir form is two SEQUENTIAL statements (`lhs=lowerExpr(kids[0]); rhs=lowerExpr(kids[1])`), NOT function-call args, so it is platform-independent (cst_to_hir's Binary frame was RHS-first to match MSVC right-to-left ARG eval; getting this backwards here is a miscompile, pinned red-on-reorder). Shared epilogues combineUnary/combineBinaryOp/combineDeref/combineCast (the recursive `lowerExprNode` body is now dead-via-driver). DELEGATED (own children re-enter the driver, so deep sub-exprs inside them still flatten): CFG arms (Ternary/&&/||/SeqExpr — emit blocks+phi, HIGH block-ID/phi-pred risk), Call/IntrinsicCall (~400-line by-value-struct ABI + variadic), by-ADDRESS arms (MemberAccess/Index rvalue-read via lowerLvalueAddress, AddressOf, Va*, ConstructAggregate). OUTPUT-IDENTITY is STRICTER for MIR (vreg#/op-order/block-ID/phi-pred all pinned by `test_mir_lowering_c_subset` goldens; monotonic ids ⇒ emission order). New SF-4 strict pins `IterativeDeepBinaryChain` + `IterativeDeepUnaryChain` (4000-deep; RED-on-disable: revert→rc-127 host overflow, RHS-first→operand-order assert fails — BOTH demonstrated). Corpus `deep_mir_expr` (120 negations → exit 42, full-release arm). Cap untouched. **4-ADDRESS ✅ DONE (2026-06-24, delegated agent → my gate; 382/382 MSVC + MinGW-GCC):** the value driver became a UNIFIED {Value,Address} driver `runExprDriver(node, rootWantAddr)` (`lowerExpr`/`lowerLvalueAddress` are thin wrappers; one `request(n, wantAddr)` classifier). Flattened MemberAccess address (MemberAddr frame) + Index address (IndexAddr frame) — the deep `a.b.c.d` / `a[i][j][k]` axes — handling the {Value}↔{Address} mutual recursion via ONE shared work-stack (an Index address needs its subscript as a VALUE + its base as a storage-ADDRESS / pointer-VALUE; emission = base-deepest-first GEP backbone then offsets/strides, all sequential statements → platform-independent, MinGW confirms). Strict pin `IterativeDeepIndexAddressChain` (1500-deep storage-Index chain; red-on-disable: revert → recursive overflow ~900) + corpus `deep_lvalue_chain` (150-deep `char[1]…[1]`, exit 42, full-release arm). **4-CFG ✅ DONE (2026-06-24, delegated agent → my gate; 383/383 MSVC + MinGW):** all FOUR CFG value arms flattened byte-identically — Ternary (4-phase frame: cond → mint then/else/join IN THAT ORDER → condbr → then → else → phi[then-pred, else-pred]), LogicalAnd/Or (3-phase: lhs → capture lhsPred → mint rhsBB/joinBB → polarity-correct condbr → rhs → phi[lhs, rhs]), SeqExpr (side-effect stmts stay on `lowerStmt`'s own local driver, only the RESULT TAIL re-enters the expr driver). Block-creation ORDER + phi-PREDECESSOR order replicate the recursive oracle EXACTLY (existing CFG goldens UNCHANGED). The pre-existing shallow goldens only assert `inc.size()==2` (blind to ORDER) — so the NEW 1000-deep strict pins `IterativeDeepTernaryChain` + `IterativeDeepLogicalChain` (assert 3N+1 block ids + each phi's exact predecessor ids in incoming order, wrapped in runOnLargeStack) are load-bearing; red-on-disable DEMONSTRATED for BOTH corruption classes (phi-incoming swap AND createBlock-order swap → pins red, reverted). Corpus `deep_ternary_mir` (150-deep right-nested `?:`, runtime `pick()` un-foldable, exit 42, phi-swap→99 miscompile-sensitive, both harnesses). **RESIDUAL: Call/IntrinsicCall (~400-line by-value-struct ABI + variadic) + ConstructAggregate/VaArg/Deref-as-lvalue still DELEGATING (children re-enter the driver, so deep sub-exprs inside them flatten).** NOTE: an ORTHOGONAL recursion family surfaced — `computeLayout` recurses once per array DIMENSION (~2300 host-stack ceiling); that is plan-24 family C-2 (type-node recursion), out of THIS arc's expression/statement scope — anchor a separate residual if a deep array-dim consumer ever needs it. |
| 4b | hir_to_mir `lowerStmt` ({Stmt}) | **high** | synthetic deep stmt nest → flat MIR on a small stack |
| 5 | parser `parseExpressionAt` unified driver + SF-1 + SF-2 | **high** | deep parse on a small stack; CST token-stream/tree-shape differential pin for mixed-assoc (`a-b-c` left, `a=b=c` right, `a?b:c?d:e`); speculation-rollback-at-depth pin |
| 6 | const-eval `evalImpl` (CST) + `evaluateConstant` (HIR) | med | deep const-expr in array-dim + deep global-init folded on a small stack |
| 7 | lift cap (256→1e6, KEEP `P_ExpressionTooDeep`) + scope-down large-stack reliance to the proven-flat path + e2e example | med | `examples/c-subset/deeply_nested_expression_iterative` (depth ~5000, currently fails the cap) compiles+runs to a known exit on the DEFAULT stack; RED-on-disable: re-impose 256 → `P_ExpressionTooDeep` |

**SF-3/SF-4/SF-5 (the oracle is too shallow without these):** the 374 corpus exercises the hard
arms only at depth 1-4. Each stage 3-5 MUST add a **byte-identical differential pin** capturing the
exact structure a phase bug corrupts (MIR: phi-predecessor IDs + block order; HIR: arena + ExprStmt
order; CST: token stream + tree shape), produced by **synthetically-built deep trees** (SF-4 — the
parser can't emit beyond the cap until Stage 5, so stages 1-4 build deep inputs via the public
`TreeBuilder`/HIR/MIR builders, not by parsing). Each stage 1-6 adds a **temporary test-only
beyond-cap override** (SF-5) raising ONLY that pass's reachable depth past 256 on a small stack +
asserting no overflow AND output-identity vs the big-stack recursive result at depth ~1000 — the
only way to validate the small-stack property per-stage before the global cap lift.

**Staging invariants:** no stage lifts the global cap until Stage 7. Each stage is OUTPUT-IDENTICAL
(the 374 suite + differential pins are the oracle) and commits independently green + independently
pr-reviewed (bounded blast radius per commit). Agnostic throughout (host-stack mechanics, BC-2 PASS).

## §4 — Plan-lock v1 findings ledger (all addressed in v2)

- **MF-1** (MIR expr re-enters `lowerStmt`) → Stage 4b + {Stmt} frame kind + statement-nest pins. ✅
- **MF-2** (CST→HIR same stmt coupling) → Stage 3b + {Stmt} frame kind. ✅
- **MF-3** (two const-eval recursions on the cap-lift path) → Stage 6 (first-class) + BC-1 gating. ✅
- **SF-1** (parser CRUX is a unified 3-kind driver, not suspend/resume; per-frame watchdog) → §2. ✅
- **SF-2** (SpeculationProbe must restore the climb work-stack) → §2 + Stage-5 rollback pin. ✅
- **SF-3** (oracle too shallow) + **SF-4** (synthetic deep trees) + **SF-5** (per-stage beyond-cap
  override) → §3 per-stage gates. ✅
- **BC-1** (never remove the cap; keep large-stack for unproven recursions) → §0 + Stage 7. ✅
- **BC-3** (re-disclose true six-family scope) → §0; surfaced to user before Stage 1. ✅
- **C-1** (`lowerFlatExpr` own driver) → Stage 3 own frame kind. **C-2** (type-node/declarator
  recursion OUT of scope; large-stack retained) → §0. **C-3** (binder sketch / oracle reparse
  byte-identity) → Stage-5 sketch-output pin.
