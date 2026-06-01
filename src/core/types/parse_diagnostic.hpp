#pragma once

#include "core/export.hpp"
#include "core/types/scope_kind.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dss {

enum class DiagnosticSeverity : std::uint8_t {
    Hint,
    Info,
    Warning,
    Error,
};

[[nodiscard]] DSS_EXPORT std::string_view severityName(DiagnosticSeverity s) noexcept;

// Stable, exhaustively-switchable diagnostic identity. Strings derive from
// this in the formatter via diagnosticCodeText(). Prefixes group the
// originating phase:
//   P_*    parse-time / tree-builder
//   C_*    config-load (GrammarSchema)
//   S_*    semantic (later)
//   H_*    HIR verifier / lowering (plan 09)
//   I_*    IR-gen   (later)
//
// Values are stable across versions — they appear in user-facing output
// and may be referenced from --suppress flags. Never renumber an existing
// entry; append new ones at the end of the relevant range.
enum class DiagnosticCode : std::uint16_t {
    None                          = 0,

    // ── P0xxx — parser / tree-builder ──
    P_UnexpectedToken             = 0x0001,
    P_MissingRequiredChild        = 0x0002,
    P_UnknownToken                = 0x0003,
    P_PrematureEndOfInput         = 0x0004,
    P_InvalidEscapeSequence       = 0x0005,
    P_NumericLiteralOutOfRange    = 0x0006,
    P_DeprecatedSyntax            = 0x0007,
    P_AmbiguousToken              = 0x0008,
    P_NoAlternativeMatched        = 0x0009,
    P_UnclosedScope               = 0x000A,
    P_UnmatchedClose              = 0x000B,
    P_ContextualKeywordResolution = 0x000C,
    P_SchemaCursorDesync          = 0x000D,
    P_IllegalChar                 = 0x000E,
    P_MalformedNumber             = 0x000F,
    P_UnterminatedString          = 0x0010,
    P_UnterminatedComment         = 0x0011,
    P_InvalidEscape               = 0x0012,

    // ── P9xxx — builder internal-invariant violations (release-mode rescues) ──
    P_BuilderInvariant            = 0x9000,
    P_TooManyDiagnostics          = 0x9001,
    P_UnfinishedTree              = 0x9002,
    P_RecoveryStalled             = 0x9003,
    P_MaxSpeculationDepth         = 0x9004,
    P_UncommittedCheckpoint       = 0x9005,
    P_BacktrackFailed             = 0x9006,

    // ── C0xxx — config loader (see plan §5.12) ──
    C_MissingField                = 0xC001,
    C_UnknownShape                = 0xC002,
    C_UnknownToken                = 0xC003,
    C_VersionMismatch             = 0xC005,
    C_CircularShape               = 0xC007,
    C_AmbiguousAlternatives       = 0xC010,
    C_UnclosableScope             = 0xC011,
    C_MalformedJson               = 0xC020,
    C_InvalidLanguageName         = 0xC021,
    C_InvalidPrecedenceTable      = 0xC022,
    C_RedundantScopeRequire       = 0xC023,
    C_ConflictingField            = 0xC024,
    C_UnknownScopeName            = 0xC025,
    C_RedundantField              = 0xC026,
    C_UnknownLexerMode            = 0xC027,
    C_InvalidStringStyle          = 0xC028,
    // A `lexerModes.<m>.defaultToken.kind` was also referenced from a
    // `shapes/*` rule. The builder treats body-default kinds as off-
    // grammar (cursor-skip), so a shape reference would silently never
    // match — surface it at load time instead.
    C_BodyDefaultKindInShape      = 0xC029,
    // Type-extension declarations (SP2; `typeExtensions[]`, schema v3).
    // C_UnknownTypeExtension: a malformed extension entry at load (not an
    //   object / not a valid declaration); ALSO the code a consumer (phase #8
    //   / transpile-map validation) emits when a type references an extension
    //   name that no registry resolved.
    // C_TypeExtensionParamMismatch: an extension parameter has an unknown kind
    //   (not "Integer"/"Type") or a malformed parameter spec.
    C_UnknownTypeExtension        = 0xC02A,
    C_TypeExtensionParamMismatch  = 0xC02B,
    // The `imports` block (schema v4) is malformed: `strategy` missing or not
    // one of "none"/"include-following"/"name-matching", or a required field
    // has the wrong JSON type. (Missing-but-required fields use C_MissingField;
    // unknown rule/token names use C_UnknownShape/C_UnknownToken.)
    C_InvalidImports              = 0xC02C,
    // An `expr` shape was declared without a complete `wrapperRules` block
    // (08.55 cleanup; schema v4). Every `expr`-shape rule must name the three
    // Pratt-walker wrapper rules (binary / unary / postfix); the engine
    // auto-interns and threads those names — it never hardcodes them.
    C_MissingWrapperRules         = 0xC02D,
    // The language declared `IntLiteral`/`FloatLiteral` as multi-char tokens
    // but no `numberStyle` block (08.55 cleanup; schema v4). The tokenizer's
    // scanNumber() drives entirely from the schema's NumberStyle; without it
    // the scanner has no rules to apply. Use ONLY for the "block is required
    // but absent" case at the end of the numberStyle parse — type/shape/
    // range errors inside an existing block use C_InvalidNumberStyle.
    C_MissingNumberStyle          = 0xC02E,
    // The `numberStyle` block is present but malformed: wrong JSON type,
    // out-of-range radix, non-single-char fractionPoint/digitSeparator,
    // unknown emitKind reference, etc. Mirrors the `imports` block's
    // C_InvalidImports discipline (08.55 cleanup; schema v4). Missing
    // required sub-fields use C_MissingField; unknown sub-keys use
    // C_UnknownShape.
    C_InvalidNumberStyle          = 0xC02F,
    // Two or more `wrapperRules` roles (binary/unary/postfix) resolved to the
    // same RuleId — a duplicate-name config error. Distinct from
    // `C_MissingWrapperRules` (missing field) so the operator sees the actual
    // class of failure: the three Pratt-walker frames MUST be distinct or the
    // walker's tree-building corrupts silently.
    C_DuplicateWrapperRules       = 0xC030,
    // The `semantics` block (schema v4) is present but malformed: wrong JSON
    // shape, unknown `kind`/`core`/`constructor`/`nameMatch` enum string, an
    // out-of-range child index, or a typeShape `operandChild` that doesn't
    // point to a valid visible child slot. (Missing required sub-fields stay
    // `C_MissingField`; dangling rule/token names stay `C_UnknownShape`/
    // `C_UnknownToken`.)
    C_InvalidSemantics            = 0xC031,
    // An `artifactProfiles[]` entry (schema v4, plan 06 AP1) names a profile
    // that is not in the loader's registered profile set (cli/gui/lib/
    // staticlib/script/sproc/transpile/shader/hdl) — OR the block is
    // malformed (not an array, or a non-string entry). Mirrors how
    // C_UnknownTypeExtension covers BOTH the malformed-shape and the
    // unknown-name cases for its top-level block. Absent field = valid
    // (empty profile list).
    C_UnknownArtifactProfile      = 0xC032,
    // The `hirLowering` block (schema v4 facet, plan 09 HR8) is present but
    // malformed: not an object, an entry with the wrong JSON type, an
    // out-of-range child index, or a missing required sub-field. Mirrors
    // C_InvalidSemantics. Dangling rule/token names use C_UnknownShape/
    // C_UnknownToken; a missing required field uses C_MissingField. (An
    // unknown HIR kind/op NAME is caught later, at lowering-engine
    // construction — the schema loader in `core` cannot see the `hir`-layer
    // enums — and reported as H_UnsupportedLoweringForKind.)
    C_InvalidHirLowering          = 0xC033,

    // ── S0xxx — semantic analysis (phase #8; see 08.6-semantic-plan §3) ──
    // Emitted by the language-agnostic semantic analyzer
    // (`src/analysis/semantic/`). The 0xE high nibble renders as the letter
    // `S` (see `diagnosticCodePrefix`). Append, never renumber.
    S_UndeclaredIdentifier        = 0xE001,
    S_RedeclaredSymbol            = 0xE002,
    S_TypeMismatch                = 0xE003,
    S_NotCallable                 = 0xE004,
    S_ArgCountMismatch            = 0xE005,
    S_UnknownType                 = 0xE006,
    S_ConstViolation              = 0xE007,
    // A `return` statement whose returned expression type does not assign
    // into the enclosing function's result type — OR a bare `return;` in a
    // non-Void function — OR a `return expr;` in a Void function. Emitted by
    // the config-driven `returnRules` facet.
    S_ReturnTypeMismatch          = 0xE008,
    // A break/continue-style control statement (a `loopControls` rule)
    // appearing outside any loop-context subtree (a `loopRules` rule).
    S_ControlOutsideLoop          = 0xE009,
    // A declared symbol whose minting declaration opted IN to unused-variable
    // warnings (`warnIfUnused: true` on its DeclarationRule) but that has an
    // EMPTY use-set after analysis (never referenced). A WARNING, not an
    // error. Config-driven and per-declaration-kind: a language opts in for
    // local variables but not for parameters (intentionally unused) or
    // globals. Needs no CFG — it reads SE7's `usesBySymbol` reverse index.
    // Scope: "never referenced" only. An assignment LHS is recorded as a use,
    // so a write-only variable (assigned but never read) does NOT warn here;
    // dead-store / write-only detection requires dataflow and stays with the
    // optimizer phase (registry D9).
    S_UnusedVariable              = 0xE00A,
    // An array declarator suffix (`int a[N]`) whose length `N` is absent or is
    // not a compile-time constant integer literal. The engine refuses to guess
    // (e.g. silently decay to a pointer or assume length 0); the declaration's
    // type stays unresolved so downstream phases fail loud rather than
    // miscompile. Config-driven via DeclarationRule::arraySuffixRule.
    S_NonConstantArrayLength      = 0xE00B,
    // An array declarator whose length IS a constant integer literal but is too
    // large to represent as the signed length the type lattice stores (it would
    // wrap to a negative length). Distinct from S_NonConstantArrayLength (which
    // is "not a constant at all"); kept separate so the message matches reality.
    S_ArrayLengthOutOfRange       = 0xE00C,
    // D5.1: a `.` or `->` member access whose LHS resolves to a non-composite
    // type (not a TypeKind::Struct / Union). Distinct from S_TypeMismatch so
    // downstream LSP/fixits/error-recovery can match the specific shape
    // (parallels S_NotCallable). Emitted by the member-access resolution.
    S_NotAComposite               = 0xE00D,
    // D5.1: a `->` member access whose LHS resolves to a non-pointer type.
    // Distinct from S_TypeMismatch (and from S_NotAComposite) so downstream
    // can disambiguate the two arrow-form failure modes — the LHS isn't a
    // pointer at all (here), vs. the LHS IS Ptr<T> but T isn't composite
    // (S_NotAComposite).
    S_NotAPointer                 = 0xE00E,
    // D5.5: an enumerator's explicit `= expr` is not a constant integer
    // literal. v1 accepts integer-literal explicit values; arbitrary
    // const-expressions require CST-side const-eval (plan 12.5 §0.2 D6).
    // Implicit (no `= expr`) enumerators auto-increment from the prior;
    // this diagnostic is reserved for the explicit-non-literal case.
    S_NonConstantEnumeratorValue  = 0xE00F,

    // ── D0xxx — driver / compilation-unit (see 08-compilation-unit-plan §2.6) ──
    // Emitted into a CompilationUnit's driver-level reporter by UnitBuilder.
    // The 0xD block is shared with future driver codes (e.g. the artifact-
    // profile plan's D_ArtifactProfileNotSupported); append, never renumber.
    D_FileNotFound                = 0xD001,
    D_EmptyInput                  = 0xD002,
    D_DuplicateFile               = 0xD003,
    // Import resolution (08-compilation-unit-plan §2.8, CU4). A reference to
    // another translation unit could not be resolved within the CU:
    //   D_UnresolvedImport    — a c-subset `#include "x.h"` whose file was not
    //                           found in the including dir or any include dir.
    //   D_UnresolvedReference — a tsql table reference (qualifiedName in table
    //                           position) with no matching CREATE TABLE in the CU.
    // Both are Warnings: phase #8 / FFI / a system catalog may still provide
    // the target, so the driver does not treat them as build-fatal here.
    D_UnresolvedImport            = 0xD004,
    D_UnresolvedReference         = 0xD005,
    // HR11/CU5: in a MULTI-language CU, `addFile`'s path extension matched no
    // registered source language's `fileExtensions` — fail loud rather than
    // silently parse the file under the primary grammar. (A single-language CU
    // always routes to its one schema, so this never fires there.)
    D_UnknownFileExtension        = 0xD006,
    // LK10 cycle 2 (plan 14): driver-tier codes emitted by
    // `Program::compileFiles` / `compileDirectory` / `compileProject`
    // when the caller's invocation cannot be honored.
    //
    // D_InvalidTargetSpec: a `targets[i]` string did not parse as
    //   `"<targetName>:<formatName>"` — wrong separator count or
    //   either half empty. The driver does not infer a default for
    //   either half because that would silently route to an
    //   unintended target/format pair on typo.
    // D_SchemaLoadFailed: one of the three schema loads
    //   (`GrammarSchema::loadShipped(languageName)`,
    //   `TargetSchema::loadShipped(targetName)`,
    //   `ObjectFormatSchema::loadShipped(formatName)`) returned an
    //   unexpected result. The wrapped C_* / config-side diagnostic
    //   has the structural detail; this code surfaces the
    //   driver-tier failure so downstream tooling can route on it.
    // D_PlanNotLanded: an entry point reached an arm whose backing
    //   plan substrate is not yet shipped. Currently fires only on
    //   `compileProject` (plan 06 `.dsp` parser pending); appending
    //   future plan-gated arms re-uses this code.
    D_InvalidTargetSpec           = 0xD007,
    D_SchemaLoadFailed            = 0xD008,
    D_PlanNotLanded               = 0xD009,
    // LK10 cycle 2 post-fold review #1 split (silent-failure-hunter
    // F12): the original cycle-2 fold reused `D_FileNotFound` at 3
    // distinct sites (missing input dir / mid-scan iterator error /
    // driver mkdir failure). Tools that route on the diagnostic
    // code (not message text) couldn't triage which root cause
    // fired. Splits below — `D_FileNotFound` retains its original
    // meaning (input dir / input file missing).
    D_OutputDirCreateFailed       = 0xD00A,
    D_DirectoryScanFailed         = 0xD00B,
    // D_TargetFormatMismatch (deprecated alias of
    // D_TargetMachineCodeMismatch — kept for downstream
    // tooling already filtering on this code; new emissions use the
    // remediation-distinct codes below).
    D_TargetFormatMismatch        = 0xD00C,
    // D_TargetMachineCodeMismatch: D-LK6-8.2 closure — the machine
    // code declared on the FORMAT schema doesn't match the TARGET
    // schema's expected machine code for that format kind. Example:
    // `arm64:elf64-x86_64-linux-exec` declares `elf.machine=62`
    // (EM_X86_64) but the "arm64" target expects `elf.machine=183`
    // (EM_AARCH64). Pre-fold this dispatched silently into the wrong
    // PLT-stub emitter → SIGILL.
    // D_TargetAbiModelMismatch: D-LK6-8.2 post-fold #1 closure — the
    // target's `abiModel` (register-machine / operand-stack /
    // result-id) doesn't match the format's `kind` (Elf/Pe/MachO vs
    // Wasm vs Spirv). Example: register-machine x86_64 target paired
    // with a WASM format. Pre-fold this also silently passed because
    // the cited `abiModel()` "upstream gate" was fictitious.
    // The two codes are remediation-distinct: machine-code mismatch
    // is fixed by changing one of the two schema files' machine
    // values; abi-model mismatch is fixed by picking a different
    // target OR format entirely.
    D_TargetMachineCodeMismatch   = 0xD00D,
    D_TargetAbiModelMismatch      = 0xD00E,

    // ── H0xxx — HIR verifier / lowering (plan 09; the 0xF high nibble renders
    // as the letter `H`, see diagnosticCodePrefix) ──
    // Emitted by the language-agnostic HIR verifier (`src/hir/hir_verifier`) and
    // (later) the CST→HIR lowering. Reserved for verifier-/lowering-time
    // failures only — config-load errors in a `hirLowering` block use the C_*
    // band (plan §4 Q8). Append, never renumber.
    // H_TypeUnresolved: an expression / TypeRef / VarDecl node whose `typeId` is
    //   not valid() — i.e. lowering/semantic analysis failed to resolve its type.
    //   A node already flagged `HirFlags::HasError` is skipped (cascade
    //   suppression), so this fires only on a genuinely untyped, non-error node.
    H_TypeUnresolved              = 0xF001,
    // H_InvalidBreak: a BreakStmt/ContinueStmt whose nesting index does not name
    //   an enclosing loop/switch — index out of range, or a ContinueStmt whose
    //   resolved target is a switch (continue can only target a loop).
    H_InvalidBreak                = 0xF002,
    // H_VerifierFailure: a node violates a structural invariant — HR3 uses it for
    //   a wrong child-arity for the node's kind (e.g. a BinaryOp with 1 child, a
    //   ForStmt whose child count disagrees with its clause-presence mask). HR6
    //   extends it to: a statement after an unconditional terminator in a Block
    //   (dead code), a non-void function body that may fall through without
    //   returning, and a Call whose argument count/types disagree with the
    //   callee's FnSig.
    H_VerifierFailure             = 0xF003,
    // H_UnknownIntrinsic: an IntrinsicCall whose payload (intrinsic id) does not
    //   resolve to an intrinsic registered in the module's HirIntrinsicRegistry.
    H_UnknownIntrinsic            = 0xF004,
    // H_ShaderViolation: a node inside a `ShaderUsable`-flagged function subtree
    //   violates a shader restriction — recursion, an indirect / function-pointer
    //   call, or a call to a non-shader (host) function. (Dynamic allocation is
    //   not yet expressible in HIR; the check lands when an alloc intrinsic does.)
    H_ShaderViolation             = 0xF005,
    // H_TextMalformed: the `.dsshir` text-format parser (HR7) hit a token it did
    //   not expect at the current position — an unknown keyword, a missing
    //   delimiter, a bad integer/string, or a structurally malformed type/node.
    //   The diagnostic's `actual` carries what was seen and `expected` what was
    //   valid. One broad syntactic code (the analog of P_UnexpectedToken for the
    //   hand-rolled HIR-text grammar) keeps the HIR-text band distinct from the
    //   schema-driven source parser's P_* codes.
    H_TextMalformed               = 0xF006,
    // H_TextVersionMismatch: the `.dsshir` header's format-version integer is not
    //   the version this build understands. Parsing stops — a newer/older layout
    //   cannot be reconstructed safely.
    H_TextVersionMismatch         = 0xF007,
    // H_TextUnknownName: a body reference names something the preamble never
    //   declared — a symbol handle (`%sN`), or an extension kind / operator /
    //   intrinsic name. The text is internally inconsistent (a hand-edit that
    //   dropped a preamble entry, or a truncated file).
    H_TextUnknownName             = 0xF008,
    // H_UnsupportedLoweringForKind: the CST→HIR lowering engine (plan 09 HR8)
    //   reached a CST rule/construct it cannot lower — either the language's
    //   `hirLowering` config has no mapping for it, the mapping names an
    //   unknown HIR kind/op, or the construct is a known-deferred one (extern /
    //   typedef-of-pointer / compound-assign / ++ / arrays / strings — owned by
    //   a later plan). An `Error` HIR node is emitted as a recovery sentinel and
    //   lowering continues (collect-all); never a silent skip or a miscompile.
    H_UnsupportedLoweringForKind  = 0xF009,

    // ── I0xxx — MIR verifier (plan 12 ML3; the 0xA high nibble renders as "I"
    // for the IR-gen / mid-level layer). Each code names a structural-,
    // dominance-, or type-consistency invariant on the frozen `Mir` module
    // that `MirVerifier::verify()` checks AFTER the ML1 builder/freeze
    // sweep. Re-running the ML1 invariants on the frozen module catches
    // the direct-`Mir`-ctor path (test fixtures, future synthetic IR) that
    // bypasses the builder; the dom-tree / interner-gated checks are
    // genuinely beyond what ML1 can see.

    // I_VerifierFailure: catch-all for structural invariants that don't
    // have a dedicated code (re-run of ML1's opcode/arity/result-rule
    // checks on the frozen module).
    I_VerifierFailure         = 0xA001,
    // Exactly one block per function has StructCfMarker::EntryBlock AND
    // it is the function's first block (funcBlockAt(f, 0)).
    I_NoEntryBlock            = 0xA002,
    I_MultipleEntryBlocks     = 0xA003,
    I_EntryBlockNotFirst      = 0xA004,
    // The block's last instruction's opcode is not a terminator
    // (defense-in-depth: ML1 already enforces this at build time).
    I_BlockNotTerminated      = 0xA005,
    // A Phi's incoming.pred is not in the CFG-predecessor set of the
    // phi's enclosing block.
    I_PhiPredNotInCfg         = 0xA006,
    // An instruction's value operand is defined in a block that does
    // NOT dominate the use site (SSA invariant; needs the dom tree).
    I_NotDominated            = 0xA007,
    // CondBr condition is not a Bool, or Return value type doesn't
    // match the function's FnSig return type. Interner-gated.
    I_TerminatorTypeMismatch  = 0xA008,
    // Arg-instruction's argIndex is >= the function's FnSig.paramCount.
    // Interner-gated.
    I_ArgIndexOutOfRange      = 0xA009,
    // An instruction's typeId resolves to a TypeKind::Extension —
    // every extension type must have been resolved to a core lattice
    // kind at the HIR→MIR boundary. Interner-gated.
    I_ExtensionTypeInMir      = 0xA00A,
    // StructCfMarker pairing — IfThen has matching IfElse/IfJoin;
    // LoopHeader has matching LoopLatch/LoopExit; ExitBlock terminates
    // in Return/Unreachable.
    I_StructCfMismatch        = 0xA00B,
    // A block in a function is not reachable from the function's
    // entry block. Orphan CFG islands are a structural invariant
    // violation — every block must be reachable from entry.
    I_UnreachableBlock        = 0xA00C,
    // ML4 — `.dssir` text format diagnostics (mirrors HR7's H_Text*
    // family for HIR).
    I_TextMalformed           = 0xA00D,
    I_TextVersionMismatch     = 0xA00E,
    I_TextUnknownName         = 0xA00F,

    // ── LIR lowering + verifier (renders as `L`) ──────────────────────
    //
    // The MIR→LIR instruction-selection pass emits these when a MIR
    // opcode has no per-target lowering rule (or the target schema does
    // not declare the required LIR opcode). Same fail-loud-deferral
    // discipline as `H_UnsupportedLoweringForKind`. Additional `L_*`
    // codes (e.g. verifier failures) belong to this family.
    L_UnsupportedLoweringForOpcode = 0xB001,
    L_RequiredLirOpcodeMissing     = 0xB002,
    L_VirtualRegInPostRegalloc     = 0xB003,
    L_InvalidSpillSlotSentinel     = 0xB004,
    // Producer-side register-table integrity failure surfaced by the
    // LIR text emitter: an instruction references a physical-register
    // ordinal that is not in the target schema's register table. This
    // is distinct from "unsupported lowering" (which is an inability to
    // map a MIR opcode); the register is the bug, not the opcode.
    L_PhysRegOrdinalOutOfRange     = 0xB005,
    // Memory addressing-mode operand pairing failure: a Load/Store/Lea
    // instruction's operand list does not end with the required
    // `[MemBase, MemOffset]` pair. Surfaced by `LirVerifier`'s Rule 1
    // (verifyLir + verifyLirText). Distinct from
    // L_UnsupportedLoweringForOpcode (which is about opcode-lowering
    // coverage gaps): this code names the malformed substrate shape.
    L_MemOperandMalformed          = 0xB006,
    // ML7 cycle 2 (plan 12): calling-convention materialization codes.
    //
    // L_StackPassedArgUnsupported: an `arg k` or `call` site requires
    //   passing an argument on the stack because `k >= cc.argGprs.size()`
    //   (or `argFprs.size()` for an FPR-class arg). Stack-passed args
    //   need both a callee-side load from `[SP + caller-arg-offset]`
    //   and a caller-side store/push BEFORE the call. v1 register-only.
    //   Anchor: D-ML7-2.2.
    // L_CcRegLookupFailed: the cc declares a register name in
    //   `argGprs`/`argFprs`/`returnGprs`/`returnFprs` that does not
    //   resolve via `schema.registerByName(...)` — schema misconfiguration
    //   (e.g. a typo in the JSON between the `registers[]` and the cc's
    //   reg list). `TargetSchemaData::validate` is meant to be the first
    //   line of defense, but defense-in-depth at materialization time
    //   catches a future schema-loader bug that bypasses validate.
    //   Renamed from `L_ArgRegLookupFailed` at the post-fold review
    //   (silent-failure F7: the name said "arg" but the code is also
    //   used for the return-register lookup path).
    // L_MoveCycleUnsupported: call-site arg-passing produces a move
    //   cycle (e.g. swap two args between argGprs[0] and argGprs[1]).
    //   The v1 emit-in-order materialization would silently miscompile
    //   such a cycle — second mov reads a clobbered source. v1 detects
    //   loud; D-ML7-2.3 anchors the proper parallel-copy resolution.
    // L_IndirectCallUnsupported: the LIR `call` instruction's callee
    //   operand is not a `SymbolRef`. v1 only encodes direct calls
    //   (the schema's encoding variant guard is `["symbol"]`); indirect
    //   calls need a new schema variant + the materializer must emit
    //   `call <reg>` instead of `call <sym>`. Anchor: D-ML7-2.4.
    L_StackPassedArgUnsupported    = 0xB007,
    L_CcRegLookupFailed            = 0xB008,
    L_MoveCycleUnsupported         = 0xB009,
    L_IndirectCallUnsupported      = 0xB00A,

    // ── Register allocator (renders as `R`) ────────────────────────────
    //
    // The linear-scan register allocator emits these when a target
    // schema is missing required calling-convention data, when an LIR
    // input contains a vreg that should have been caught upstream by
    // the LirVerifier, or as informational notes about spill decisions
    // so consumers can understand register-pressure costs.
    R_NoCallingConventions         = 0x4001,
    R_CallingConventionLookupFailed = 0x4002,
    R_VRegHasNoClass               = 0x4003,
    R_SpilledDueToPressure         = 0x4004,
    R_SpilledDueToCrossCallExhaustion = 0x4005,

    // ── Assembler (renders as `A`) ────────────────────────────────────
    //
    // The byte-encoding pass (plan 13 AS1) emits these when a LIR
    // opcode arrives at the assembler without a usable `encoding`
    // facet on its TargetOpcodeInfo. Substrate-tier — cycle 1 declares
    // the family AND fires the two no-encoder cases below. Additional
    // codes (variant-guard mismatch, relocation-kind-undeclared) land
    // alongside their consumers in cycles AS2/AS3 — not pre-declared
    // here so each new code travels with the path that produces it.
    //
    // A_NoEncodingDeclared: the opcode's `encoding.format` is `none`
    //   (or the entire `encoding` block is absent). The substrate
    //   refuses to guess — without a declared shape there is no
    //   universal byte representation for the opcode.
    // A_NoEncodingShapeWalker: the opcode declares a non-`none` shape
    //   but the assembler has no registered walker for it. Fires for
    //   every non-`none` opcode while AS1 cycle 1's substrate is
    //   the only assembler code shipped; the walker registrations
    //   land in AS2 (`x86_variable`) and AS3 (`fixed32`).
    // A_LirToMirSizeMismatch: the caller passed a `lirToMir` span
    //   whose length is not equal to the LIR module's instruction-
    //   arena size. The substrate uses `lirToMir[LirInstId.v]` to
    //   stamp `SourceMapEntry::mirInst`; a shorter span would
    //   silently read out-of-bounds memory once AS2/AS3 wire the
    //   stamping. Fail loud at entry so the test fixture / pipeline
    //   builder catches the contract violation.
    // A_NoMatchingEncodingVariant: the opcode declares a shape walker
    //   that is registered, but the LIR instruction's operand kinds
    //   match none of the variant guards in `encoding.variants[]`. The
    //   substrate refuses to invent — there is no fallback variant. Fix
    //   either: (a) add a variant to the target JSON whose guard matches
    //   the operand shape the LIR produces, or (b) adjust the LIR pre-
    //   pass (e.g. 2-address legalization) to produce a shape covered
    //   by the declared variants.
    // A_RoundTripMismatch: the disassembler oracle (plan 13 AS5,
    //   §2.9) re-extracted operand values from the encoded bytes
    //   and they disagreed with what the encoder consumed. Catches
    //   the silent-failure class where the encoder produces valid
    //   but WRONG instructions (correct length, wrong reg/imm in a
    //   slot). Test-only diagnostic — production assembler does
    //   not run the round-trip pass.
    A_NoEncodingDeclared           = 0x1001,
    A_NoEncodingShapeWalker        = 0x1002,
    A_LirToMirSizeMismatch         = 0x1003,
    A_NoMatchingEncodingVariant    = 0x1004,
    A_RoundTripMismatch            = 0x1005,

    // ── Linker (renders as `K`) ───────────────────────────────────────
    //
    // The link pass (plan 14 LK4 substrate) emits these when an
    // AssembledModule + ObjectFormatSchema combination is internally
    // inconsistent. Substrate-tier — LK4 declares the family AND fires
    // the cases the format-blind engine itself can detect; per-format
    // codes (e.g. ELF header malformation, Mach-O load-command
    // overflow) join in their respective LK* cycles alongside the
    // format's JSON schema.
    //
    // K_SymbolUndefined: a Relocation's `target` symbol is not
    //   declared by any AssembledFunction in the module (single-CU
    //   resolution; cross-CU is LK11; FFI imports LK6).
    // K_RelocationKindMismatch: a Relocation's opaque `kind` tag does
    //   not resolve on BOTH sides of plan 13 §2.6's reloc unifier —
    //   target schema's `relocations[]` row (the formula) AND
    //   format schema's `relocations[]` row (the platform-native
    //   name) must both be declared. The diagnostic message names
    //   the missing side(s).
    //
    // Per-format codes join the family alongside their LK* cycles.
    //
    // K_NoMatchingObjectFormat fires in four scenarios:
    //   1. The linker engine's format-dispatch switch reaches
    //      `ObjectFormatKind::Unknown` (the invalid sentinel was
    //      not initialized by the format schema's loader path).
    //   2. The switch reaches a format whose walker is not yet
    //      registered (Wasm / Spirv until plan 18 / plan 17 plug
    //      them in). ELF / PE / Mach-O walkers landed at LK1 /
    //      LK2 / LK3 respectively.
    //   3. A format walker was invoked for a schema whose `kind()`
    //      does not match the walker (e.g. `elf::encode` called
    //      with a PE-tagged schema, or `pe::encode` called with an
    //      ELF-tagged schema).
    //   4. The format schema declares the right `kind()` but omits
    //      a `sections[]` row the walker requires (e.g. ELF writer
    //      requires SectionKind::Text/RelocTable/Symtab/Strtab/
    //      ShStrtab — any missing row fires this code; PE and
    //      Mach-O writers only require SectionKind::Text since
    //      their symbol/string tables don't carry section headers).
    // K_FormatLacksImportSupport: a format walker received an
    //   AssembledModule with non-empty `externImports` but its
    //   image-side arm doesn't yet emit import tables. The three
    //   eager-binding format arms are now all closed (PE IAT —
    //   LK6 cycle 2a; ELF GOT/PLT — LK6 cycle 2b.2; Mach-O
    //   LC_DYLD_INFO_ONLY — LK6 cycle 2c). This diagnostic now
    //   fires only on the lazy-binding upgrade paths (D-LK6-11
    //   ELF / D-LK6-12 PE delay-load / D-LK6-13 Mach-O lazy-bind)
    //   and on the ARM64-darwin chained-fixups path (D-LK6-14)
    //   until those anchors close. Format walkers that can't
    //   support the requested binding mode fire this loud rather
    //   than silently producing a binary missing its import table.
    // K_WalkerInputContractViolation: a format walker received an
    //   AssembledModule whose SHAPE (not whose imports) violates the
    //   walker's input contract — e.g. non-empty `functions` on the
    //   LK8 WASM skeleton (which produces a preamble-only output
    //   that doesn't consume native-ISA bytes), or `expectedFuncCount
    //   > 0` with empty `functions`. Distinct from
    //   `K_NoMatchingObjectFormat` (which signals "no walker registered
    //   for this kind") — the dispatch IS correct here, but the caller
    //   wired the wrong shape of input. (type-design fold, LK8
    //   post-fold review.)
    // K_ImageNotOk: `linker::writeImage()` refused to write a
    //   LinkedImage whose `ok()` is false (parallel-index gate
    //   failed upstream). The walker already emitted a diagnostic
    //   for the actual failure — this code surfaces the
    //   contract-violation at the write surface so a misconfigured
    //   build script can't bypass the gate. Remediation: check
    //   `reporter.errorCount()` before calling writeImage.
    // K_ImageEmpty: `linker::writeImage()` saw `ok() == true` but
    //   `bytes.empty()` — distinct from K_ImageNotOk in that the
    //   parallel-index gate PASSED, so this is a walker contract
    //   violation (the walker returned success with no output).
    //   Remediation: fix the walker, not the caller. Type-design
    //   post-fold split (LK10 cycle 1 post-fold #2 review).
    // K_ImageWriteParentMissing: parent directory of the output
    //   path doesn't exist. Caller responsibility — the substrate
    //   does not auto-create paths (silent mkdir masks config
    //   errors that ship artifacts to the wrong target dir).
    // K_ImageWriteOpenFailed: `std::ofstream::open()` set failbit.
    //   Causes: permission denied, path is a directory, invalid
    //   filename, parent dir vanished post-`exists()`-check (TOCTOU
    //   race).
    // K_ImageWriteShort: write returned with failbit after starting
    //   — disk full, I/O error mid-write.
    // K_ImageWriteCloseFailed: `close()` set failbit while flushing
    //   buffered writes. The bytes may be partially or fully on
    //   disk; the file is in an unknown state.
    // All five codes added plan 14 LK10 cycle 1; type-design
    // post-fold split from one generic code into four
    // remediation-distinct codes for log triage.
    K_SymbolUndefined              = 0x8001,
    K_RelocationKindMismatch       = 0x8002,
    K_NoMatchingObjectFormat       = 0x8003,
    K_FormatLacksImportSupport     = 0x8004,
    K_WalkerInputContractViolation = 0x8005,
    K_ImageNotOk                   = 0x8006,
    K_ImageWriteParentMissing      = 0x8007,
    K_ImageWriteOpenFailed         = 0x8008,
    K_ImageWriteShort              = 0x8009,
    K_ImageWriteCloseFailed        = 0x800A,
    K_ImageEmpty                   = 0x800B,

    // ── F_* — FFI binary-reader / C-header-parser (plan 11 §2.6) ──
    // F_FileOpenFailed: shared-library path doesn't exist / permission
    //   denied / I/O error during initial open. Distinct from
    //   D_FileNotFound (CLI input file) because the remediation is
    //   "check the project's importLibrary path or distro lib paths",
    //   not "fix the user's --compile argument".
    // F_FileEmpty: zero-byte file. Symptomatic of a broken build /
    //   truncated download.
    // F_UnknownBinaryFormat: no recognised magic bytes (not ELF, not
    //   PE, not Mach-O). Distinct from `UnsupportedBinaryFormat` —
    //   "unknown" means we can't even guess; "unsupported" means we
    //   recognised the format but the reader for it isn't shipped.
    // F_UnsupportedBinaryFormat: format recognised but binary-reader
    //   not yet shipped (e.g. PE during FF1-ELF-only cycle). Cites
    //   the per-format anchor (FF1-PE / FF1-MachO) so the user knows
    //   what's pending.
    // F_CorruptedBinary: file structurally invalid — section offset
    //   past EOF, string-table index out of range, etc.
    // F_UnsupportedElfClass: ELF32 / non-LE / ELFv0 — v1 supports
    //   ELF64 little-endian (gABI mainstream).
    // F_SectionNotFound: expected .dynsym / .dynstr / etc. missing
    //   from the binary. A stripped library is the typical cause.
    F_FileOpenFailed               = 0x5001,
    F_FileEmpty                    = 0x5002,
    F_UnknownBinaryFormat          = 0x5003,
    F_UnsupportedBinaryFormat      = 0x5004,
    F_CorruptedBinary              = 0x5005,
    F_UnsupportedElfClass          = 0x5006,
    F_SectionNotFound              = 0x5007,
    // ── FF2 C header parser (plan 11 §2.3) ──
    // Codes split per the type-design + silent-failure-hunter post-fold
    // review of FF2 baseline: distinct user-actionable remediations →
    // distinct codes (same discipline as the D_FileNotFound 3-way
    // split + D_TargetFormatMismatch 2-way split + K_Image* 5-way
    // split). A consumer routing on a single F_HeaderParseFailed
    // could not triage "caller passed empty importLibrary" (caller
    // API bug) from "shipped grammar broken" (build/install bug)
    // from "header syntax error" (user source bug).
    //
    // F_HeaderParseFailed: c-subset frontend rejected the header text
    //   (tokenize / parse / semantic / lowering diagnostics).
    //   Remediation: fix the source header.
    // F_HeaderHasFunctionBody: a non-extern function DEFINITION (with
    //   `{ ... }` body) appeared at top level. Headers in v1 are
    //   declaration-only — function bodies belong in `.c` translation
    //   units, not headers.
    // F_HeaderHasNonExternDecl: a top-level decl that is neither
    //   `extern` nor `typedef` (e.g. a bare `int x;` global). Header
    //   mode accepts only the declaration surface FFI ingestion
    //   consumes.
    // F_HeaderEmptyImportLibrary: the caller passed an empty
    //   `importLibrary` — silent-failure surface if accepted (every
    //   row downstream would be unlinkable). Remediation: caller
    //   supplies a non-empty library identity (`"libc.so.6"`,
    //   `"msvcrt.dll"`, etc.).
    // F_HeaderGrammarLoadFailed: the shipped c-subset grammar JSON
    //   failed to load. Remediation: ship the grammar artifact /
    //   investigate the underlying C_* diagnostics also reported.
    // F_HeaderHasUnsupportedTopLevel: a top-level HIR kind reached the
    //   header walker that is neither extern decl nor typedef and not
    //   the function-body / non-extern-global case either (e.g. an
    //   `ImportGroup` from `#include`, or a future HirKind addition).
    //   Remediation: remove the construct from the curated header OR
    //   extend FF2 to accept it.
    // F_HeaderInternalInvariant: an internal-invariant violation
    //   reached the header walker — a compiler bug, not a user-fixable
    //   issue. Remediation: file a bug.
    // F_HeaderHasExternInitializer: an `extern` declaration carries an
    //   initializer (`extern int x = 5;`) — silently dropping the
    //   initializer would be a definition-vs-declaration semantic
    //   mismatch. Remediation: drop the `extern` (it's a definition)
    //   or drop the initializer (it's a declaration).
    F_HeaderParseFailed            = 0x5008,
    F_HeaderHasFunctionBody        = 0x5009,
    F_HeaderHasNonExternDecl       = 0x500A,
    F_HeaderEmptyImportLibrary     = 0x500B,
    F_HeaderGrammarLoadFailed      = 0x500C,
    F_HeaderHasUnsupportedTopLevel = 0x500D,
    F_HeaderInternalInvariant      = 0x500E,
    F_HeaderHasExternInitializer   = 0x500F,
};

// Symbolic name like "P_UnexpectedToken" / "C_MalformedJson" / "P0042".
[[nodiscard]] DSS_EXPORT std::string_view diagnosticCodeName(DiagnosticCode c) noexcept;

// Formatted prefix like "P0001" / "C0010" — used by the renderer for the
// header line ("error[P0001]: ...").
[[nodiscard]] DSS_EXPORT std::string diagnosticCodePrefix(DiagnosticCode c);

// A secondary location attached to a diagnostic — "matching opener here",
// "previously declared here", etc. May reference a *different* buffer than
// the diagnostic's primary span (cross-file includes, when we get them).
struct DSS_EXPORT RelatedLocation {
    BufferId    buffer;
    SourceSpan  span;
    std::string note;
};

// One diagnostic record. Pure data — formatting lives in DiagnosticReporter.
struct DSS_EXPORT ParseDiagnostic {
    DiagnosticCode      code     = DiagnosticCode::None;
    DiagnosticSeverity  severity = DiagnosticSeverity::Error;

    BufferId    buffer;             // primary buffer
    SourceSpan  span = SourceSpan::empty(0);

    std::optional<RuleId> ruleContext;   // which expected shape was active

    // What the schema would have accepted at this position. Pre-rendered
    // strings — the builder populates these from `schemaTokens().name()` /
    // `rules().name()` at emit time, e.g. {"';'", "expression"}.
    std::vector<std::string> expected;

    // What was actually seen — lexeme text or token-kind name.
    std::string actual;

    // Builder-captured scope stack at the moment of error. Powers
    // "got '>' while inside Generic scope" without state reconstruction.
    std::vector<ScopeKind> scopeStack;

    // Secondary locations (matching opener, previous declaration, ...).
    std::vector<RelatedLocation> related;

    // Optional human-friendly hint ("did you forget a semicolon?").
    std::string suggestion;
};

} // namespace dss
