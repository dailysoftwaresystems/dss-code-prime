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
    A_NoEncodingDeclared           = 0x1001,
    A_NoEncodingShapeWalker        = 0x1002,
    A_LirToMirSizeMismatch         = 0x1003,
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
