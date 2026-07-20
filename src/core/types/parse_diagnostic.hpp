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

    // C-preprocessor pass (FC13; `src/analysis/preprocess/`). The
    // preprocessor is a config-SELECTED pre-parse phase; its diagnostics
    // render with the `P` prefix (parse family) since they are pre-parse
    // source errors, not config-load errors (`C_InvalidPreprocess`).
    P_PreprocessorDirective       = 0x0013,  // malformed directive (e.g. `#define` with no name)
    P_PreprocessorMacroRedefinition = 0x0014,  // incompatible `#define` of an existing macro
    P_PreprocessorUnsupported     = 0x0015,  // a recognised-but-unimplemented directive form (variadic macro def)
    P_PreprocessorIncludeError    = 0x0016,  // quote-`#include` target not found / unreadable / recursion overflow
    P_PreprocessorMacroArgument   = 0x0017,  // function-like macro INVOCATION error (arity mismatch / unterminated arg list)
    // FC15a (`#`/`##` operators): the STRINGIZE (`#`, C 6.10.3.2) operator is
    // malformed -- a `#` in a function-like macro's replacement list is not
    // followed by a parameter (the only valid `#` operand).
    P_PreprocessorStringize       = 0x0019,
    // FC15a: the TOKEN-PASTE (`##`, C 6.10.3.3) operator is malformed -- a `##`
    // at the start or end of a replacement list (no operand on one side), OR a
    // paste whose concatenated spelling is NOT a single valid token
    // (C 6.10.3.3p3: the result must be a single preprocessing token).
    P_PreprocessorPaste           = 0x001A,
    // FC15b (predefined macros; C 6.10.8.1): a `#define` or `#undef` of a
    // PREDEFINED macro name (`__FILE__`/`__LINE__`/`__STDC__`/...). C 6.10.8.1p2:
    // none of these macro names shall be the subject of a `#define` or `#undef`.
    // The rejected directive does NOT alter the macro table. The predefined-macro
    // SET is config-driven (`predefinedMacros`); the engine never hard-codes a
    // name.
    P_PreprocessorPredefinedMacro = 0x001B,
    // FC15c (`__has_include` -- C23 6.10.1p4): a malformed `__has_include`
    // operator in a `#if`/`#elif` controlling expression -- a missing `(`, an
    // empty filename, a missing closing `>`/`"`, or a missing `)`. The operator
    // takes `(<header>)` or `("header")`; the angle delimiters are matched by
    // config token KIND (`hasIncludeAngleOpenToken`/`...CloseToken`), never by
    // scanning for the literal `<`/`>` bytes (agnosticism). A well-formed
    // operator yields 1 (the header is found) or 0 -- never this diagnostic.
    P_PreprocessorHasInclude      = 0x001C,
    // FC17.9(h) (`#embed` -- C23 6.10.4 / N3096 6.10.3): a malformed or
    // unsupported `#embed` directive (D-PP-EMBED). ONE message-differentiated
    // code (the `P_PreprocessorIncludeError` multi-message precedent): the
    // resource is not found / unreadable / has an empty-or-missing quoted name
    // / uses the deferred angle or macro-argument form / carries an unsupported
    // standard parameter (limit/prefix/suffix/if_empty/vendor) / exceeds the
    // cycle-1 splice size budget, and the `__has_embed` operator is malformed.
    // Positioned on the directive word / operator token. Fail-loud invariant:
    // every non-bare-quote-filename shape emits THIS code -- never a silent drop
    // and never a silent partial embed.
    P_PreprocessorEmbed           = 0x001D,

    // Expression-nesting depth guard (Pratt walker). A too-deeply-nested
    // expression (parens / right-assoc / prefix / ternary recursion past
    // ParserConfig::maxExpressionDepth) is reported HERE at the offending
    // token and RECOVERED (Error leaf + graceful unwind) -- never a raw
    // C++ stack overflow and never a fatal-abort.
    P_ExpressionTooDeep           = 0x0018,

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
    // C_InvalidShippedFfiHeaderPath: RETIRED 2026-06-03. The shipped
    // FFI-headers tree (`src/dss-config/ffi-headers/`) + the
    // `findShippedFfiHeader` resolver + `readCHeaderShipped` consumer
    // were removed in the OPT2 cycle 1 commit — the only caller path
    // was tests of the shipped headers themselves, with no production
    // consumer (production routes through `synthesizeFfiFromSourceDecls`).
    // The number is kept reserved (NOT renumbered) so historical
    // diagnostics remain decodeable. The FF1/FF2/FF5 in-memory +
    // arbitrary-path header substrate (readCHeader / readCHeaderFromText)
    // stays as unused-feature substrate awaiting its trigger.
    C_InvalidShippedFfiHeaderPath = 0xC034,  // RETIRED — see comment
    // D-CONFIG-DIAGNOSTIC-CODE-PER-KIND closure (cycle 10m, 2026-06-04):
    // pre-cycle `findShippedConfig`'s `invalidNameCode` field carried
    // `C_InvalidLanguageName` regardless of whether the lookup was for
    // a language, target, or object-format config. The kindLabel prose
    // differentiated; the diagnostic code did not. These per-kind
    // codes let downstream consumers (LSP, diff-verify harnesses,
    // human readers) attribute "invalid-name" errors to the correct
    // config tier without parsing the message string. Mirrors
    // `D-OPT-DIAGNOSTIC-CODE-SPLIT-OOR-VS-FILE` precedent.
    C_InvalidTargetName           = 0xC035,
    C_InvalidFormatName           = 0xC036,
    // Config-load error in the `preprocess` block (schema v4; FC13). The
    // language-agnostic C-preprocessor pass is config-SELECTED: a malformed
    // `preprocess` object, a missing required directive-keyword string, or a
    // wrong-typed field surfaces here (mirrors `C_InvalidImports`). An
    // unknown TOKEN-name field still routes to `C_UnknownToken`, a missing
    // required string to `C_MissingField`.
    C_InvalidPreprocess           = 0xC037,

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
    // FC2: an explicit cast (`semantics.casts` rule, e.g. C's `(T)expr`)
    // whose (target, operand) type pair is outside the language's legal
    // cast matrix — struct/union VALUE casts (C forbids casts to
    // composite types), casts to/from `void`, array-typed operands, or
    // any pair the MIR cast lattice cannot lower. Distinct from
    // S_TypeMismatch (implicit-conversion failures) so tooling can route
    // on the explicit-cast shape.
    S_InvalidCast                 = 0xE010,
    // FC3 c1: a type-specifier keyword multiset (e.g. C's `unsigned long
    // int`) with NO matching row in the language's `typeSpecifiers` table.
    // C's invalid combinations (`unsigned float`, `short long`) reject by
    // ABSENCE from the declared table — the engine never hardcodes which
    // combos are legal; the message carries the offending source text.
    S_InvalidTypeSpecifierCombination = 0xE011,
    // FC3 c1: an integer literal whose decoded magnitude exceeds the RANGE
    // of every candidate type in the language's `integerLiteralTyping`
    // ladder for its (suffix-class × radix-class) — e.g. a decimal literal
    // above LLONG_MAX in C. Distinct from the decode-tier overflow (a
    // value that doesn't fit the 64-bit accumulator at all, reported by
    // the lowering as out-of-range): this is "decodable, but no declared
    // type can hold it".
    S_IntegerLiteralTooLarge      = 0xE012,
    // FC3 c1: the active object format declares a `dataModel` the semantic
    // tier has no exercised width path for (ILP32 is declared-only on the
    // wasm/spirv skeleton formats). Fail loud at analysis rather than
    // silently typing `long`/pointers with untested widths.
    S_UnsupportedDataModel        = 0xE013,
    // FC4 c1: a declaration carries the language's `volatile`-class marker
    // token, declared via a `DeclarationRule.gatedMarkers` entry — the
    // qualifier is grammar-ADMITTED but its semantics (no caching / no
    // reordering of accesses) are NOT implemented, so every use fails loud
    // rather than silently compiling the object as plain memory. Config-
    // driven: WHICH token gates and THAT it maps to this code are both
    // per-language config (the engine only honors the declared token→code
    // pair; nothing here hardcodes the word "volatile").
    S_VolatileNotSupported        = 0xE014,
    // S_IndirectCallNotSupported: RETIRED 2026-06-12 (FC4 c2). The
    // indirect-call encoding landed end-to-end (semantic Ptr<FnSig>
    // unwrap → HIR deref-decay + arg coercion → LIR call-reg
    // materialization + the regalloc callee/arg-reg exclusion rule →
    // x86 FF /2 + arm64 BLR encoding variants), so the semantic wall
    // this code gated is gone: a Ptr<FnSig> callee now routes through
    // the SAME result-stamp + arity + per-arg checking as a direct
    // call. Both emit sites removed. The number is kept reserved (NOT
    // renumbered) so historical diagnostics remain decodeable —
    // 0xC034 precedent.
    S_IndirectCallNotSupported    = 0xE015,  // RETIRED — see comment
    // FC4 c1 stage 2a: C 6.7.6.3p10 — a `(void)` parameter list declares
    // zero parameters; a NAMED void parameter (`int f(void x)`) or void
    // mixed with other parameters (`int f(void, int)`) is ill-formed.
    // Emitted by the engine's param-harvest normalization when the
    // language declares `parameters.soleVoidMeansEmpty`.
    S_InvalidVoidParam            = 0xE016,
    // FC4 c1 stage 2a: a declaration position that REQUIRES named
    // declarators (`DeclarationRule.requireNamedDeclarators` — C's
    // locals/globals/typedefs) carries an ABSTRACT declarator: `int *;`,
    // `int (int);` — the declaration declares nothing. Config-driven per
    // declaration row; parameter-like positions legally stay abstract.
    S_DeclarationDeclaresNothing  = 0xE017,
    // FC4 c1 stage 2a: a function-shaped declarator in an unsupported
    // form/position — a definition whose init-declarator list has more
    // than one declarator or an initializer, a definition whose named
    // direct-declarator carries no function suffix (`int (*fp)(int) {}`),
    // or a bare function-TYPED object declaration (a C prototype
    // `int f();` — declaration-without-definition is deferred; externs
    // carry that role today). Always an ERROR — never silent.
    S_InvalidFunctionDeclarator   = 0xE018,
    // FC5: two `goto` labels with the SAME name in one function (C 6.8.1 — a
    // label name has function scope, so a duplicate is a constraint violation).
    // Positioned at the redefinition. Emitted at the label-resolution chokepoint
    // (CST→HIR's per-function label pre-scan, where label names are assigned
    // ordinals — the single collection site, so the check rides the resolution
    // rather than duplicating a separate semantic-tier label walk).
    S_DuplicateLabel              = 0xE019,
    // FC5: a `goto` whose target label is not defined anywhere in the enclosing
    // function (C 6.8.6.1). Positioned at the goto's target identifier.
    S_UndefinedLabel              = 0xE01A,
    // FC6: flexible-array-member (FAM) constraint violations (C99 §6.7.2.1).
    // A FAM (`T x[];` — an incomplete-array struct field) must be the LAST
    // member; a non-last FAM is rejected (its trailing siblings would overlay
    // the unsized tail). Positioned at the offending field. The layout engine
    // also fails loud on a non-last FAM (`computeLayout` nullopt) — this is the
    // positioned SEMANTIC diagnostic that surfaces it earlier + with a span.
    S_FlexibleArrayNotLast        = 0xE01B,
    // FC6: a struct whose ONLY member is a FAM (C99 §6.7.2.1p3: the struct shall
    // have more than one named member). Without this, `sizeof` of a sole-FAM
    // struct would silently fold to 0. Positioned at the FAM field.
    S_FlexibleArraySoleMember     = 0xE01C,
    // FC6: a FAM-bearing struct used as a struct member or an array element
    // (C99 §6.7.2.1p18 — a structure containing a FAM shall not be a member of a
    // structure or an element of an array). Positioned at the embedding field.
    S_FlexibleArrayInAggregate    = 0xE01D,
    // FC8 D-CSUBSET-BITFIELD: a bit-field (`T x : W`) whose base type T is NOT an
    // integer type (C 6.7.2.1p5 — a bit-field's type shall be _Bool / signed int
    // / unsigned int / an implementation-defined integer type). A `float`/pointer/
    // struct/enum base fails loud here rather than silently mis-sizing the unit.
    S_BitFieldNonIntegerType      = 0xE01E,
    // FC8 D-CSUBSET-BITFIELD: a bit-field width that is negative, exceeds the base
    // type's bit-size (C 6.7.2.1p4), or is zero on a NAMED field (C 6.7.2.1p3 — a
    // zero-width bit-field shall have no declarator, i.e. be anonymous).
    S_BitFieldWidthOutOfRange     = 0xE01F,
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): the active CC's `vaListLayout.strategy`
    // is a variadic-callee ABI not yet realized (Aapcs64DualCursor — FC12c). The
    // semantic `va_list`-type injection fails loud rather than inject a wrong-sized
    // `va_list` (which would mis-size the `ap` local and corrupt the stack). SysV
    // (register-save) + Win64 (homogeneous-pointer) are realized and never hit this.
    S_VariadicCalleeUnsupported   = 0xE020,
    // D-CSUBSET-LOCAL-STATIC (MF-2): a `static` storage-class specifier in a
    // for-statement init-declaration is a C 6.8.5p3 constraint violation ("the
    // declaration part of a for statement shall only declare identifiers for
    // objects having storage class auto or register"). The grammar admits the
    // shared `localDeclSpecifiers` prefix (so `static` PARSES in the for-init),
    // and a block-scope `static` IS supported — but in for-init it must fail
    // loud, never silently lower as an automatic local. Config-driven: the
    // for-init declaration row declares the `StaticKeyword`→this-code gated
    // marker; nothing here hardcodes the word "static".
    S_StaticStorageInForInit      = 0xE021,
    // D-CSUBSET-FN-PROTOTYPE: two declarations of the same function name have
    // INCOMPATIBLE signatures (a return-type or parameter-list mismatch — C
    // 6.7p4 / 6.9.1). A bare prototype (`int f(int);`) merges with a later
    // definition or a redundant declaration only when their interned FnSig
    // TypeIds are structurally equal; a mismatch (`int f(int); long f(int){…}`)
    // fails loud here rather than silently picking one signature. Positioned at
    // the absorbed (later/redundant) declaration with a related-location at the
    // surviving declaration. Emitted after Pass 1.5 (both FnSigs resolved).
    S_IncompatibleRedeclaration   = 0xE022,
    // D-CSUBSET-LABEL-BEFORE-CASE: a `case`/`default` labeled statement (the
    // C 6.8.1 `caseStmt` form, which exists so a goto-label may precede a case —
    // `foo: case 1: stmt`) appeared where it is NOT a direct switch-body item:
    // either truly outside any switch, or nested inside an inner block of a
    // switch arm (the flat-switch model only groups top-level switch-body items).
    // lowerSwitch consumes a direct (label-wrapped) caseStmt before it reaches
    // the statement dispatch; reaching the dispatch means it is misplaced -> fail
    // loud (C 6.8.1) rather than emit a stray arm-less case. Positioned at the
    // case/default keyword.
    S_CaseLabelNotInSwitch        = 0xE023,
    // Cluster F1 (C 6.5.2.4 / 6.5.3.1): a prefix or postfix `++`/`--` whose
    // operand is not a modifiable lvalue. Emitted at CST→HIR lowering, where the
    // ++/-- sites classify the operand: a manifest rvalue (e.g. a literal `5++`,
    // `++5`) has no object to read-modify-write, so it fails loud here rather
    // than synthesize a write-back to a non-object. (A `const`-qualified lvalue
    // `const int x; x++;` is a SEPARATE, pre-existing gap — `classifyLvalue` does
    // not yet model `const` — anchored as D-CSUBSET-INCDEC-CONST-LVALUE, shared
    // with the same gap on plain assignment.) Positioned at the ++/-- expression.
    S_IncDecNeedsModifiableLvalue = 0xE024,
    // RETIRED by c27 (D-CSUBSET-VOLATILE-POINTEE, 2026-06-27): formerly the
    // pointer-to-volatile-POINTEE reject (`volatile int *p`) under c21's model B,
    // which threaded `volatile` as a per-symbol `isVolatile` bool and could not
    // express a volatile pointee. c27 makes `volatile` a TYPE qualifier
    // (TypeKind::VolatileQual): `volatile int *` now builds Ptr<VolatileQual(int)>
    // and the deref carries MirInstFlags::Volatile from the pointee type — so this
    // code is NEVER EMITTED anymore (and was removed from kUnsuppressableCodes). The
    // enum value + name are KEPT for ordinal stability and historical golden
    // references; do not reuse the ordinal.
    S_VolatilePointeeNotSupported = 0xE025,
    // D-CSUBSET-SELF-REFERENTIAL-STRUCT: a DIRECT (non-pointer) member of an
    // INCOMPLETE composite type — `struct N { struct N n; }` (a struct cannot
    // contain itself by value; its size would be infinite) or a member of a
    // forward-declared-but-not-yet-defined `struct B b;` (C 6.7.2.1p3: a struct/
    // union member shall have a COMPLETE type). A POINTER to an incomplete type
    // (`struct N *next;`) is LEGAL (pointer size is known) and never trips this.
    // Positioned at the offending member. Without it a self-by-value member would
    // silently fold its size to 0 (a silent miscompile).
    S_IncompleteTypeMember        = 0xE026,
    // c26 D-CSUBSET-ABSTRACT-DECLARATOR-TYPE-NAME: a TYPE-NAME position (a
    // cast / sizeof / compound-literal / va_arg type — C 6.7.7 type-name) whose
    // abstract declarator illegally carries a NAME: `(int x)expr`, `sizeof(int
    // y)`. C type-names are ABSTRACT (declarator without an identifier); a named
    // one is a constraint violation. The INVERSE of S_DeclarationDeclaresNothing
    // (which fires when a NAME is required but absent); fired by the type-name
    // resolver when an abstract declarator (the fn-ptr/array type-name tail)
    // resolves to a name: the resolver returns InvalidType UNCONDITIONALLY (only
    // the emit is gated by emitOnMiss; the reject is not), so the name is NEVER
    // silently dropped and mis-parsed as the bare base type (`(int x)` → `(int)`).
    S_TypeNameDeclaratorNotAbstract = 0xE027,
    // c35 D-CSUBSET-FORWARD-STRUCT-DECLARATION: an OBJECT (a variable / global,
    // NOT a member) declared with an INCOMPLETE composite type by VALUE —
    // `struct S v;` where `struct S` is forward-declared but never defined (C
    // 6.7p7 / 6.2.5: an object shall not have an incomplete type, except as
    // permitted for a tentative array). c35 made an opaque tag forward-MINT an
    // incomplete TypeId (so an opaque `struct S *` pointer compiles); the SAME
    // mint means `struct S v;` now resolves the tag — so the by-VALUE object of
    // an incomplete type must be REJECTED HERE at the semantic tier (the earliest
    // point with the full type), rather than only at MIR lowering (the
    // allocaForLocal computeLayout guard). A POINTER to an incomplete type
    // (`struct S *p`) is LEGAL and never trips this; an ARRAY of incomplete
    // element (`struct S a[4]`) does (its element has no size). The sibling of
    // S_IncompleteTypeMember (the by-value MEMBER case) — together they keep a
    // by-value use of an incomplete composite from EVER silently folding to size 0.
    S_IncompleteTypeObject        = 0xE028,

    // C11/C23 6.7.10: a `_Static_assert`/`static_assert` whose constant-expression
    // condition evaluated to ZERO (the assertion FAILED) OR could not be folded to
    // an integer constant expression (a non-constant / float / unresolved condition
    // — C requires an integer constant expression). ONE code for both: the message
    // (`.actual`) discriminates "assertion failed: <string-literal>" from
    // "condition is not an integer constant expression". Emitted at the SEMANTIC
    // tier (the point with sizeof/enum folding), so a passed assertion produces no
    // HIR and the program runs; a failed one fails loud here.
    S_StaticAssertFailed          = 0xE029,
    // FC16 C11/C23 6.5.1.1 (D-CSUBSET-GENERIC-SELECTION): a `_Generic` generic
    // selection whose controlling expression's type matched NONE of the typed
    // associations and there was NO `default` association (a constraint
    // violation — C requires exactly one match or the default). Emitted at the
    // SEMANTIC tier (the point with the resolved controlling type + resolved
    // association types); a silent no-selection would leave the `_Generic` node
    // untyped and mis-lower, so this is unsuppressable.
    S_GenericSelectionNoMatch     = 0xE02A,
    // FC16 C11/C23 6.5.1.1 (D-CSUBSET-GENERIC-SELECTION): a `_Generic` whose
    // controlling type matched MORE THAN ONE typed association (a constraint
    // violation — 6.5.1.1p2 forbids two associations naming compatible types).
    // With interned TypeId equality this means two associations named the SAME
    // type. Emitted at the SEMANTIC tier; unsuppressable (an ambiguous selection
    // has no well-defined value).
    S_GenericSelectionAmbiguous   = 0xE02B,
    // C11/C23 6.7.5 (D-CSUBSET-ALIGNAS): an `_Alignas`/`alignas` alignment
    // specifier whose operand — the value form `alignas(N)` or the type form
    // `alignas(T)` (which contributes _Alignof(T)) — is not a POSITIVE POWER OF
    // TWO. 6.7.5p3 requires the alignment be a valid fundamental/extended
    // alignment (a power of two). `alignas(0)` is NOT this error — it is an
    // explicit NO-OP (6.7.5p3 "an alignment specification of zero has no
    // effect"), handled as "no override" before this check. Emitted at the
    // SEMANTIC tier (where the operand const-folds / the type's alignment is
    // computed); unsuppressable (a constraint violation whose suppression would
    // fail the build with zero diagnostics — the S_StaticAssertFailed precedent).
    S_AlignasNotPowerOfTwo        = 0xE02C,
    // C11/C23 6.7.5 (D-CSUBSET-ALIGNAS-EXCEEDS-MAX): an `alignas(N)` whose value
    // exceeds the maximum representable alignment (256 bytes — the `Alignment`
    // newtype's cap, alignment.hpp; no producer in the pipeline emits > 256).
    // A distinct code from the power-of-two check so an over-large-but-pow2
    // value (`alignas(512)`) reports the precise reason. Unsuppressable.
    S_AlignasExceedsMax           = 0xE02D,
    // C11/C23 6.7.5p4 (D-CSUBSET-ALIGNAS): an `alignas` specifier weaker than the
    // declared type's natural alignment. 6.7.5p4: alignas may only STRENGTHEN
    // (raise) alignment, never weaken it — `alignas(1) double d;` (1 < 8) is a
    // constraint violation. Compared against the declared type's
    // `computeLayout(...)->align`. Unsuppressable.
    S_AlignasWeakerThanNatural    = 0xE02E,
    // C11/C23 6.7.5 (D-CSUBSET-ALIGNAS): an `alignas` in a context 6.7.5 forbids —
    // on a typedef, a function, a function PARAMETER, or a bit-field member.
    // 6.7.5p2: an alignment specifier may appear only in the declaration of an
    // OBJECT that is not a bit-field, a parameter, or a function/typedef. The
    // `.actual` text names the specific rejected context. Unsuppressable.
    S_AlignasInvalidContext       = 0xE02F,
    // C11/C23 6.7.5 (D-CSUBSET-ALIGNAS): an `alignas(expr)` whose value-form
    // operand does not fold to an integer constant expression (a non-constant /
    // float / unresolved expression). 6.7.5p3 requires an integer constant
    // expression. Emitted at the SEMANTIC tier via the SAME `constIntExpr`
    // evaluator static_assert / array-dimension folding uses. Unsuppressable.
    S_AlignasNonConstant          = 0xE030,
    // FC16 (D-CSUBSET-PACKED): a composite `__attribute__((...))` / `[[...]]`
    // attribute in a HONORED position (the struct/union tag) whose identifier is not
    // a recognized composite type-attribute (a typo like `__attribute__((pakced))`,
    // or an unsupported GNU attribute), OR a recognized `packed` spelling in an
    // UNHONORED position (a leading `[[gnu::packed]] struct S`, which the linkage
    // scan would otherwise skip wholesale). Typo protection mirroring
    // `H_UnknownLinkageSpecifier`: fail loud rather than silently drop an attribute
    // the program may depend on. The `.actual` names the offending spelling.
    S_UnknownTypeAttribute        = 0xE031,
    // FC16 (D-CSUBSET-PACKED / D-CSUBSET-PACKED-BITFIELD-INTERACTION): a `packed`
    // struct/union that ALSO contains a bit-field member. Bit-granular packed
    // packing is a distinct algorithm (a named, deferred gap); combining the two is
    // UNSUPPORTED — fail loud at the SEMANTIC tier rather than silently emit a
    // NON-packed layout (the layout engine's nullopt belt is the backstop). Emitted
    // at the composite-completion site; unsuppressable (a suppressed one would ship
    // the wrong — padded — bytes).
    S_PackedBitfieldUnsupported   = 0xE032,
    // C23 §6.5 (D-CSUBSET-NULLPTR): the predefined constant `nullptr` (type
    // nullptr_t) used as an operand where nullptr_t is not permitted — any
    // arithmetic/bitwise/shift binary (`nullptr + 1`), any relational (`nullptr <
    // p`), unary `-`/`~` (`-nullptr`), or `==`/`!=` against a non-pointer /
    // non-nullptr peer (`nullptr == 5`). WITHOUT this explicit fail-loud gate the
    // HIR lowering (nullptr → the integer-0 null constant) would SILENTLY compile
    // `nullptr + 1` as `0 + 1` — a silent accept of ill-formed code. `nullptr` IS
    // admissible as an `==`/`!=` operand against a pointer or another nullptr, and
    // in the pointer/bool CONVERSION contexts (handled by isAssignable). The
    // `.actual` names the offending operand. Unsuppressable.
    S_NullptrInvalidOperand       = 0xE033,
    // C23 §6.7.2.2 (D-CSUBSET-ENUM-UNDERLYING-TYPE): the type-specifier in an enum's
    // explicit underlying-type clause (`enum E : T { … }`) is NOT an integer type
    // (`enum E : float`, `enum E : struct S`). C23 requires the underlying type to be
    // an integer type; a non-integer is a constraint violation. The `.actual` names
    // the enum (or the offending type). Unsuppressable — without the fail-loud gate
    // the enum would silently fall back to the default int and lay out at the wrong
    // width/signedness.
    S_InvalidEnumUnderlyingType   = 0xE034,
    // C23 §6.7.2.2 (D-CSUBSET-ENUM-UNDERLYING-TYPE): an enumerator's value does NOT
    // fit the enum's EXPLICIT underlying type (`enum E : unsigned char { A = 256 }`,
    // `enum E : unsigned char { A = -1 }`). C23 requires every enumerator to be
    // representable in the underlying type. The `.actual` names the enumerator.
    // Unsuppressable — a suppressed diagnostic would let the out-of-range value be
    // truncated/wrapped into the underlying type silently (a wrong constant value).
    // Only fires for the EXPLICIT-underlying case; a default-int enum is unchanged.
    S_EnumeratorValueOutOfRange   = 0xE035,
    // C23 §6.7.2.5 (D-CSUBSET-TYPEOF): the operand of a `typeof`/`typeof_unqual`
    // is a BIT-FIELD member access (`typeof(s.flag)` where `flag` is a bit-field).
    // A bit-field has no nameable, portable type — its width/representation are
    // implementation-defined — so C constrains typeof away from it. The `.actual`
    // names the offending member access. Unsuppressable: without the fail-loud gate
    // the typeof would silently resolve to the bit-field's DECLARED (widened) type,
    // masking the constraint and yielding a wrong type in a downstream declaration.
    S_TypeofBitfieldOperand       = 0xE036,
    // C23 §6.7.1 (D-CSUBSET-CONSTEXPR): a `constexpr` object's initializer is NOT
    // a compile-time constant — an arithmetic-typed initializer that does not fold
    // through the shared CST const-eval engine (`constexpr int x = argc;`), or a
    // pointer-typed initializer that is not a null pointer constant
    // (`constexpr int *p = &g;`; the `(T*)0` cast form is a named loud deferral,
    // D-CSUBSET-CONSTEXPR-POINTER-CAST-NULL). THE constexpr-vs-const empirical
    // delta: `const int x = argc;` stays legal (const-ness is initializer-blind;
    // only an ICE consumer errors lazily), `constexpr` must fail AT ITS OWN
    // DECLARATION (6.7.1p10 — the value must be computable at translation time).
    // The `.actual` names the offending initializer. Unsuppressable — a suppressed
    // violation would silently degrade constexpr to plain const (a later
    // const-expr consumer would then mis-diagnose, or a runtime init would ship).
    S_ConstexprNonConstantInitializer = 0xE037,
    // C23 §6.7.1 (D-CSUBSET-CONSTEXPR): a `constexpr` object declarator carries NO
    // initializer (`constexpr int x;`, the `b` in `constexpr int a = 1, b;` —
    // fires per-declarator). 6.7.1p10 requires an initializer (the object IS its
    // compile-time value). The `.actual` names the uninitialized declarator.
    // Unsuppressable — a suppressed violation would ship a zero-initialized
    // "constant" whose reads mean nothing the author wrote.
    S_ConstexprMissingInitializer = 0xE038,
    // C23 §6.7.1 (D-CSUBSET-CONSTEXPR / D-CSUBSET-CONSTEXPR-AGGREGATE-TYPE): a
    // `constexpr` object of ARRAY / STRUCT / UNION type (`constexpr int a[3] =
    // {1,2,3};`, `constexpr char s[] = "hi";`). Aggregate constexpr semantics
    // (element-wise compile-time validation) are a NAMED loud deferral — no
    // CST-tier aggregate evaluator exists; fail loud rather than validate a
    // guessed subset. A UNIFORM boundary: the char-array-from-string-literal form
    // is deliberately NOT carved out. Also the catch-all for any other
    // non-scalar/non-pointer constexpr object type (fail-loud, never silent).
    // The `.actual` names the declarator. Unsuppressable.
    S_ConstexprUnsupportedType    = 0xE039,
    // C23 §6.7.1 (D-CSUBSET-CONSTEXPR): `constexpr` on a FUNCTION — a prototype
    // (`constexpr int f(void);`) or a definition (`constexpr int f(void) {…}`).
    // C23 constexpr is the OBJECT storage-class only (C++ constexpr functions do
    // not exist in C23); 6.7.1p5 restricts constexpr to objects. Fail loud rather
    // than silently treat the function as ordinary (and — file scope — wrongly
    // give it internal linkage via the constexpr linkage row). The `.actual`
    // names the function declarator. Unsuppressable.
    S_ConstexprFunctionNotSupported = 0xE03A,
    // C23 §6.7.1p11 (D-CSUBSET-CONSTEXPR): a `constexpr` OBJECT whose type is
    // volatile-qualified at the TOP level (`constexpr volatile int v = 1;`).
    // C23 prohibits a constexpr object of volatile-qualified type (its reads
    // could not be constant-folded without dropping the volatile access). A
    // volatile POINTEE stays legal (`constexpr volatile int *p = nullptr;` — the
    // OBJECT is the pointer, not volatile itself). The `.actual` names the
    // declarator. Unsuppressable — a suppressed violation would either fold away
    // volatile reads or silently drop the constexpr constant-ness.
    S_ConstexprInvalidQualifier   = 0xE03B,
    // C23 §6.7.13 (D-CSUBSET-ATTRIBUTE-SEMANTICS): a C23 `[[...]]` standard
    // attribute whose name matches NO row of the language's attribute-semantics
    // table (`[[frobnicate]] int x;`). A WARNING and SUPPRESSIBLE — C23 (and the
    // WG21 P2552 posture) forbids treating an unknown standard attribute as
    // fatal: the program is conforming, the attribute is simply ignored. The
    // GNU `__attribute__((...))` form keeps its own PRE-EXISTING loud gates
    // (file-scope H_UnknownLinkageSpecifier / composite S_UnknownTypeAttribute)
    // — this code is the stdAttr-form-only vocabulary warning. The `.actual`
    // names the unrecognized attribute clause.
    S_UnknownAttribute            = 0xE03C,
    // C23 §6.7.13.3 (D-CSUBSET-ATTRIBUTE-SEMANTICS): a use of a symbol declared
    // `[[deprecated]]` / `[[deprecated("msg")]]` / GNU `__attribute__((deprecated))`
    // (fires once per use site, incl. a call's callee). A WARNING and
    // SUPPRESSIBLE — deprecation is lint-tier advice, not a constraint
    // violation; the program's semantics are unchanged. The `.actual` is the
    // symbol name, or `name: msg` when the attribute carried a message.
    S_DeprecatedSymbolUsed        = 0xE03D,
    // C23 §6.7.13.2 (D-CSUBSET-ATTRIBUTE-SEMANTICS): a call to a function
    // declared `[[nodiscard]]` / GNU `__attribute__((warn_unused_result))` whose
    // result is DISCARDED — the call is the entire expression of an expression
    // statement (`f();`). The `(void)f();` cast idiom and any value use
    // (`x=f()`, `g(f())`, `return f()`) do NOT fire. A WARNING and SUPPRESSIBLE
    // — discarding a nodiscard result is diagnosable advice per C23, not a
    // constraint violation. The `.actual` is the callee name, or `name: msg`.
    S_NodiscardResultDiscarded    = 0xE03E,
    // C23 §6.7.10 (D-CSUBSET-EMPTY-INITIALIZER): a brace initializer for a
    // SCALAR object that is not one of the two valid scalar forms — `{}` (empty,
    // zero-initializes) or `{ expr }` (exactly one non-brace, non-designated
    // expression, C 6.7.10p12). Fires on excess elements (`int v = {1, 2};`), a
    // designator (`int v = {.x = 1};`), or a nested brace list (`int v =
    // {{42}};` — 6.7.10p12's "shall be a single expression"). A plain Error
    // (constraint violation); the lowering returns an Error node either way, so
    // a suppressed diagnostic can never silently accept the malformed
    // initializer (no wrong-bytes path — deliberately NOT in the
    // unsuppressable table).
    S_InvalidScalarInitializer    = 0xE03F,
    // C99 §6.4.2.2 (D-CSUBSET-FUNC-PREDEFINED-IDENTIFIER): a predefined
    // function-name identifier (`__func__` / the configured aliases) used as a
    // MODIFIABLE lvalue — the ++/--/compound-assign classifiers would otherwise
    // synthesize a write-back to a symbol with no storage slot (`__func__` reads
    // FOLD to a string-literal constant; there is no object to read-modify-
    // write), which today surfaces as an engine-level "no storage slot" failure
    // at MIR. This is the REAL diagnostic for that misuse. Plain reads,
    // `&__func__` (legal C99 — the fold's rodata global provides the address),
    // and indexing are unaffected. Simple assignment / `+=` are caught EARLIER
    // by SE4's const check (the synthetic symbol is `isConst`) →
    // S_ConstViolation; this code covers the inc/dec class the const check does
    // not model. A plain Error; the classifiers bail to an Error path either
    // way (NOT in the unsuppressable table — no silent-accept route).
    S_PredefinedIdentifierNotAddressable = 0xE040,
    // C23 §6.7.9 (D-CSUBSET-AUTO-TYPE-INFERENCE): an initializer-inferred
    // declaration (`auto x = expr;`) declares MORE THAN ONE declarator
    // (`auto a = 1, b = 2;` — 6.7.9p2: "shall contain ... a single
    // declarator"). The `.actual` names the declaration. UNSUPPRESSABLE —
    // the inference arm is the ONLY tier that types these symbols at Pass
    // 1.5; a suppressed violation would fall through to Pass 2's
    // initializer-type backfill and silently adopt each initializer's type
    // (the exact multi-declarator form the constraint forbids).
    S_AutoRequiresSingleDeclarator = 0xE041,
    // C23 §6.7.9 (D-CSUBSET-AUTO-TYPE-INFERENCE): an initializer-inferred
    // declaration whose declarator is NOT a plain identifier — a pointer
    // (`auto *p = …`), array (`auto a[] = …`), or function (`auto f(void);`)
    // declarator (6.7.9p2: "the declarator shall be ... an identifier"; the
    // derived-declarator forms are a WG14 v2-paper extension — the named
    // deferral D-CSUBSET-AUTO-DERIVED-DECLARATOR). The `.actual` names the
    // declarator. UNSUPPRESSABLE — same backfill seam as 0xE041: suppressed,
    // the symbol would silently adopt the initializer's un-derived type.
    S_AutoRequiresPlainIdentifier = 0xE042,
    // C23 §6.7.9 (D-CSUBSET-AUTO-TYPE-INFERENCE): an initializer-inferred
    // declaration with NO initializer (`auto x;` / `auto T;` — there is
    // nothing to infer from; 6.7.9p2 requires the `= assignment-expression`
    // form). The `.actual` names the declarator. UNSUPPRESSABLE — suppressed,
    // the symbol would stay untyped and surface as a cascade H_TypeUnresolved
    // with the REAL reason hidden (a confusing silent-failure REASON).
    S_AutoRequiresInitializer = 0xE043,
    // C23 §6.7.9 (D-CSUBSET-AUTO-TYPE-INFERENCE): the inference itself is
    // INVALID — one code, distinct `.actual` messages (generic
    // "initializer-inferred declaration ..." wording, never a keyword
    // identity):
    //   • the declaration's specifier prefix lacks the language's REQUIRED
    //     inference specifier (`requiredSpecifierToken` — C23 6.7.9p1's
    //     `auto`): `static x = 5;` / `register y = 2;` / `alignas(4) z = 9;`
    //     / `[[maybe_unused]] w = 3;` all parse into the headless rule and
    //     must STAY the errors they were (C89 implicit-int is not C23);
    //   • the initializer's type is VOID (`auto v = voidFn();` — no object
    //     type to declare);
    //   • the initializer is the bare null-pointer keyword (`auto p =
    //     nullptr;` — nullptr_t is a semantic-tier-only type that must never
    //     reach MIR; folded into D-CSUBSET-NULLPTR-T-DECLARABLE);
    //   • the initializer's type cannot be resolved at the declaration's own
    //     Pass-1.5 visit (incl. the self-reference `auto x = x;` — the name
    //     resolves to the symbol being declared, whose type is exactly what
    //     is being inferred).
    // UNSUPPRESSABLE — the C3 backfill seam: Pass 2's decl arm backfills
    // `rec.type = initializer-type` for ANY unresolved declarator-mode
    // symbol, so a suppressed rejection would silently compile the void/
    // nullptr_t/self-referential form with a wrong or tripwire-tripping type.
    S_AutoInferenceInvalid = 0xE044,
    // C11 §6.7.1 / C23 §6.7.1 (D-CSUBSET-THREAD-LOCAL): `_Thread_local` /
    // `thread_local` on a FUNCTION declarator (`thread_local int f(void);` —
    // prototype or definition, intra-module or extern). Thread storage
    // duration applies to OBJECTS only (6.7.1p4: "_Thread_local shall not
    // appear in the declaration specifiers of a function declaration").
    // UNSUPPRESSABLE — suppressed, the function would silently compile with
    // the specifier dropped (and a file-scope declaration would carry a
    // storage-class the codegen tiers never validated).
    S_ThreadLocalOnFunction = 0xE045,
    // C11 §6.7.1p3 (D-CSUBSET-THREAD-LOCAL): a BLOCK-scope object declared
    // `thread_local` without `static` or `extern` in the same declaration
    // (`void f(void) { thread_local int x; }` — the standard REQUIRES one of
    // the two; a for-init `for (thread_local int i…)` is the same violation
    // via the row's gated marker, since a for-init admits neither).
    // UNSUPPRESSABLE — suppressed, the object would silently lower as a
    // plain AUTOMATIC (a per-CALL stack slot: aliasing per-thread semantics
    // with per-invocation storage — a silent miscompile of the storage
    // duration the program declared).
    S_ThreadLocalRequiresStaticOrExtern = 0xE046,
    // C11 §6.7.1p3 (D-CSUBSET-THREAD-LOCAL): a same-TU REDECLARATION pair
    // disagrees on thread storage — one declaration names the object
    // `thread_local` and the other does not (`extern int g; thread_local int
    // g = 5;` — 6.7.1p3: "it shall be present in the declaration of every
    // declared name with thread storage duration"; BOTH directions).
    // UNSUPPRESSABLE — suppressed, the merge would keep ONE record's flag
    // and half the program's accesses would silently target the wrong
    // storage (process-shared vs per-thread).
    S_ThreadLocalRedeclarationMismatch = 0xE047,
    // C11 §6.6p9 (D-CSUBSET-THREAD-LOCAL): the ADDRESS of a thread-local
    // object used in a STATIC-storage-duration initializer (`thread_local
    // int t; int *p = &t;` — scalar, aggregate member, or a block-scope
    // `static int *q = &t;`). A thread-local object's address is NOT an
    // address constant: it differs per thread and is only computable at
    // runtime against the executing thread's TLS block. UNSUPPRESSABLE —
    // suppressed, the emitted data item would carry an abs64 relocation
    // whose resolved value is the link-time tpoff bit-cast to a pointer (a
    // silent garbage pointer in .data — the exact CRIT-1 miscompile).
    S_ThreadLocalAddressNotConstant = 0xE048,
    // C11 §6.7.1p2 + C23 §6.7.1 (D-CSUBSET-THREAD-LOCAL): `thread_local`
    // combined with a storage-class specifier the standard forbids — ONE
    // code, distinct `.actual` messages:
    //   • with `constexpr` (C23 6.7.1: constexpr may pair only with auto /
    //     register / static — never thread_local);
    //   • with `register` (6.7.1p2 admits only static / extern beside
    //     thread_local; the c-subset parses `register` as an inert
    //     storage-class specifier, so the pairing must reject here).
    // (`typedef thread_local` cannot co-occur grammatically — typedefDecl
    // has no storage-specifier prefix — so it stays a loud parse error.)
    // UNSUPPRESSABLE — suppressed, the declaration would silently drop
    // whichever specifier the downstream tiers don't model.
    S_ThreadLocalInvalidCombination = 0xE049,
    // C23 §6.2.5/§6.7.2 (D-CSUBSET-BITINT): the `_BitInt(N)` width constant-
    // expression is NOT an integer constant expression (`_BitInt(n)` with a runtime
    // `n`, `_BitInt(x+y)` over non-constants). C23 requires N to be an ICE.
    // UNSUPPRESSABLE — suppressed, the type would have no computable width and the
    // masking/layout would silently pick a garbage N.
    S_BitIntWidthNotConstant = 0xE04A,
    // C23 §6.2.5 (D-CSUBSET-BITINT): `_BitInt(N)` with N ≤ 0 (`_BitInt(0)`,
    // `_BitInt(-3)`). A bit-precise integer must have a positive width.
    // UNSUPPRESSABLE — a non-positive width has no representation.
    S_BitIntWidthNotPositive = 0xE04B,
    // C23 §6.2.5 (D-CSUBSET-BITINT): a SIGNED `_BitInt(1)` — a signed bit-precise
    // integer needs at least 1 sign bit + 1 value bit, so the minimum signed width
    // is 2 (`unsigned _BitInt(1)` IS legal — one value bit). UNSUPPRESSABLE — a
    // 1-bit signed integer has no value range.
    S_BitIntSignedWidthTooSmall = 0xE04C,
    // C23 §6.2.5 (D-CSUBSET-BITINT): `_BitInt(N)` with N > __BITINT_MAXWIDTH__
    // (8388608). The width exceeds the implementation's maximum bit-precise width.
    // UNSUPPRESSABLE — an over-max width is a hard constraint violation.
    S_BitIntWidthExceedsMax = 0xE04D,
    // D-CSUBSET-BITINT — the C1 cycle boundary: `_BitInt(N)` with N > 64. RETIRED in
    // C2 (N>64 is now a runnable multi-limb type — the semantic gate no longer emits
    // this). The code + its span slot are KEPT (never renumber — the append-only
    // discipline; still on the unsuppressable list) so every historical 0xE04E golden
    // stays stable; no live site references it after the C2 gate relaxation.
    S_BitIntWidthAboveC1Limit = 0xE04E,
    // D-CSUBSET-BITINT-C2-WIDE — the C3 cycle boundary: `* / %` on a WIDE `_BitInt(N>64)`.
    // C2 ships the multi-limb storage + the EASY ops (+ - & | ^ ~ << >> compare convert);
    // wide MULTIPLY / DIVIDE / MODULO (schoolbook UMulH / long-division) land in C3. A
    // dedicated positioned diagnostic emitted at the MIR by-address wide-BinaryOp arm
    // (a wide `a*b` result is materialized by address) — NOT a silent scalar op / the
    // incidental i128 ALU wall. UNSUPPRESSABLE — suppressed, the op would reach codegen
    // with no multi-limb lowering and silently miscompile. ★ RETIRED C3 (2026-07-12): wide
    // `* / %` now LOWER (multi-limb schoolbook mul + long-division) — this code is no longer
    // emitted; kept append-only (stable-id), pinned unreachable by the flipped
    // WideBitIntMulDivModLowersAtC3 unit test (asserts nDiag(0xE04F)==0).
    S_BitIntWideMulDivUnsupported = 0xE04F,
    // D-CSUBSET-BITINT-FLOAT-CHAR-ENUM-CONV — conversion between a FLOATING type and a
    // WIDE `_BitInt(N>64)` (`(_BitInt(128))1.5`, `(double)wide`). C2 ships integer<->wide
    // and wide<->wide; a correct multi-limb float<->wide conversion (the full FP
    // significand<->limbs path) is genuinely hard and lands in a later cycle. The naive
    // scalar path keys signedness off the source and touches only limb 0 — wrong sign,
    // wrong value, dropped upper limbs — so a wide float conversion FAILS LOUD at the MIR
    // cast site (materializeWideCast for a wide TARGET, combineCast's wide-SOURCE arm for
    // a wide SOURCE) rather than silently miscompiling. NARROW (N<=64) float<->`_BitInt`
    // is unaffected (it rides the native container, C1). UNSUPPRESSABLE.
    S_BitIntWideFloatConvUnsupported = 0xE050,
    // VLA C1a (D-CSUBSET-VLA, C99/C11 §6.7.6.2p2): a block-scope variable-length
    // array declared with STATIC or EXTERN storage (`static int a[n];`). A VLA
    // requires AUTOMATIC storage duration — a static/thread/extern object may not
    // have a variably modified type. Emitted by the Pass-2 `validateVlaDeclarator`
    // (the thread_local-validator model): the type arm builds the `vlaArray` at
    // block scope regardless of storage; this validator rejects the non-automatic
    // ones. UNSUPPRESSABLE — a suppressed static VLA would carry a runtime-sized
    // type into the static-local→hidden-global lowering, whose layout has no static
    // size (a wrong-storage miscompile). (File-scope `int g[n]` never becomes a VLA
    // — the scope gate leaves it S_NonConstantArrayLength.)
    S_VlaWithStaticStorage = 0xE051,
    // VLA C1a (D-CSUBSET-VLA) — the C3 multi-dimensional boundary: a VLA whose
    // ELEMENT is itself an array or a VLA (`int a[n][m]`, `int a[5][n]`,
    // `int a[n][5]`). C1a ships 1-D VLAs only; a runtime STRIDE through index/GEP
    // for a multi-dimensional VLA lands in C3. Rejected on BOTH the VLA arm (any
    // array/VLA element) AND the constant arms (a VLA element — `typeContains-
    // FlexibleArray`/`isIncompleteArray` check -1, so they would silently build
    // `array(vlaArray)`). UNSUPPRESSABLE — a suppressed multi-dim VLA would build a
    // nested array-of-VLA / VLA-of-array type that no lowering tier handles.
    S_VlaMultiDimUnsupported = 0xE052,
    // VLA C1a (D-CSUBSET-VLA, C11 §6.7.6.2p1): a variable-length array whose size
    // expression does NOT have integer type (`int a[1.5]` — float; `int a[nullptr]` —
    // nullptr_t; a pointer; etc.). C requires the VLA size to have integer type.
    // Enforced at the SEMANTIC tier (Pass-2 `validateVlaDeclarator`, after expression
    // typing) because a MIR-tier integer check CANNOT catch `nullptr` — it lowers to
    // an I32 0 by MIR (NullptrT is semantic-tier-only). UNSUPPRESSABLE — a suppressed
    // non-integer length would reach codegen as a bogus VLA: a float bound
    // `FPToSI`-truncates to a garbage element count, a nullptr bound is a silent
    // 0-byte array. Integer kinds ACCEPTED: the standard integers, Bool, Char, Byte,
    // Enum, and `_BitInt` (a `_BitInt(N)` bound is a legal VLA size).
    S_VlaSizeNotInteger = 0xE053,
    // VLA C4c (D-CSUBSET-VLA, C99 §6.7.6.2/6.7.6.3): an array declarator carries a
    // `static` and/or cv-qualifier and/or the unspecified-size `*` INSIDE its `[ ]`
    // (`int a[static 3]`, `int a[const n]`, `int a[*]`) in a position that is NOT a
    // function parameter — a local, struct field, typedef, or file-scope object. C
    // permits these array-size decorations ONLY in a function-parameter declarator
    // (they inform the callee that the pointer is non-null / of at-least-N elements,
    // or defer the size in a prototype); anywhere else they are a constraint
    // violation. The grammar is deliberately permissive (the ONE shared array suffix
    // admits them) and this SEMANTIC gate rejects the non-parameter use, mirroring
    // the typeSpecifierSeq → S_InvalidTypeSpecifierCombination discipline. Emitted at
    // BOTH array-suffix sites (the declarator-mode `applyDeclaratorSuffix`, gated on
    // the param-only `paramDecay` signal, and the legacy externDecl `applyArray-
    // Suffix`, always a non-parameter). UNSUPPRESSABLE — suppressing it would let a
    // decorated non-parameter array through with the decoration silently dropped (a
    // mis-typed / mis-sized object), the same silent-miscompile-guard class as the
    // S_Vla* siblings.
    S_ArrayParamQualifierNonParameter = 0xE054,

    // FC17.9(d) atomic 1b-i (D-CSUBSET-ATOMIC-NONLOCKFREE): `_Atomic` is applied to a
    // type that is NOT a naturally-aligned lock-free SCALAR — an aggregate (struct /
    // union / by-value array) or a scalar wider than the lock-free width (`_BitInt`>64,
    // `__int128`, `long double`). Such a type needs a lock-table / large-atomic runtime
    // path (C11 7.17.5) DEFERRED beyond atomic cycle-1. FAIL LOUD at type resolution
    // rather than wrap it: the qualifier is a TRANSPARENT skin, so a wrapped aggregate
    // would reach codegen, `computeLayout` would strip the skin, and the copy would
    // decompose to plain non-atomic field/byte Load/Store that the type-based
    // atomic-access belt cannot see — a SILENT non-atomic access. Same silent-
    // miscompile-guard class as I_AtomicAccessNotLowered (the scalar belt).
    S_AtomicNonLockFree = 0xE055,

    // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): `long double` is used (a declaration /
    // cast / literal) but the active object format declares NO `longDoubleFormat`
    // axis (wasm/spirv skeletons, direct-API callers) — the type's REPRESENTATION
    // is genuinely unknowable (64-bit IEEE vs x87 80-bit vs binary128 is
    // ABI-divergent per format), so the typeSpecifiers row is left UNREALIZED and
    // this precise diagnostic replaces the generic S_InvalidTypeSpecifier-
    // Combination miss. Deliberately NOT a silent base-core fallback: binding F64
    // under an undeclared axis is the representation mis-bind (wrong sizeof,
    // wrong ABI class) the axis exists to prevent — the `long`/LLP64 lesson.
    // Suppressible like its S0011 sibling: a suppressed emission leaves the type
    // unresolved (InvalidType), which cannot reach codegen.
    S_LongDoubleFormatUndeclared = 0xE056,

    // FC17.9(i) (D-CSUBSET-INLINE-ASM): an `__asm__` inline-asm statement whose
    // template does NOT decode to strictly zero bytes — non-empty text
    // (`__asm__("hlt")`), whitespace-only (`__asm__("  ")`), or a malformed escape.
    // Cycle-1 implements ONLY the empty-template optimizer barrier (`__asm__
    // volatile("")` → MirOpcode::CompilerBarrier, zero target instructions); a
    // non-empty template carries real per-target machine instructions we cannot yet
    // emit (the per-target asm-text arc is deferred, D-CSUBSET-INLINE-ASM-TEXT). FAIL
    // LOUD rather than silently lower it to a no-op barrier — that would DROP the
    // instructions (e.g. an `asm("hlt")` becoming a no-op), a genuine miscompile. In
    // kUnsuppressableCodes (unsuppressable_codes.cpp): `--suppress` must never be able
    // to turn a dropped `asm(...)` into a silent non-emission. Renders error[S0057].
    S_InlineAsmNonEmptyTemplate = 0xE057,

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
    //   `transpile` (plan 10 source-translation pending); the original
    //   `compileProject` consumer landed for real (plan 06 AP2,
    //   `.dss-project.json`). Future plan-gated arms re-use this code.
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
    // D_TargetAbiModelUnsupportedByDriver: compileOneTarget reached an
    //   abiModel (operand-stack / result-id) that the register-machine
    //   LIR pipeline does not lower. Permanent architectural exclusion
    //   — plans 17 (SPIR-V) and 18 (WASM) own their own lowering tiers.
    //   Distinct from `D_PlanNotLanded` (pending-arrival surface).
    //   (post-fold #6 silent-failure C2 fix.)
    D_TargetAbiModelUnsupportedByDriver = 0xD00F,
    // D_ArtifactProfileNotSupported (plan 06 AP2): a project config
    //   (`.dss-project.json`) requested an `artifactProfile` the
    //   selected language does not declare in its `artifactProfiles[]`
    //   set (grammar schema, AP1). Two sub-cases share this one code
    //   (one logical failure — "the requested profile is not in the
    //   supported set"; the message discriminates):
    //     * the language declares a non-empty set that excludes the
    //       requested profile (e.g. c-subset declares {cli,lib,
    //       staticlib} and the project asks for "gui");
    //     * the language declares NO profiles at all (empty set) — a
    //       fail-CLOSED reject, aligning with §2.1's trajectory toward
    //       making `artifactProfiles[]` required (a language must
    //       declare ≥1 profile to be project-buildable).
    //   The check belongs at project-load time (plan 06 §1) so the
    //   failure surfaces here, not "deep in codegen". AP3/AP4 own the
    //   downstream consumption (CompilationContext + codegen).
    D_ArtifactProfileNotSupported = 0xD010,
    // D_ArtifactProfileFormatMismatch (plan 06 AP3): a project config's
    //   `artifactProfile` is not SERVED by the chosen object format —
    //   i.e. the profile is not in that format's declared
    //   `artifactProfiles[]` set (the format-side symmetric twin of AP1's
    //   language-side set). Example: a `cli` project pointed at a
    //   shared-library format, or a `lib` project pointed at an executable
    //   format, or any profile pointed at a format that declares no served
    //   profiles (e.g. a relocatable `.o` format — fail-CLOSED). Distinct
    //   from `D_ArtifactProfileNotSupported` (0xD010): that is
    //   "the LANGUAGE doesn't declare this profile" (fix the `.lang.json`);
    //   this is "the chosen FORMAT doesn't produce this profile" (fix the
    //   target/format, or ship the backend that emits it). Remediation-
    //   distinct → distinct code.
    D_ArtifactProfileFormatMismatch = 0xD011,
    // c105 (D-PP-USER-DEFINE): `--define` was passed but the language declares
    // no preprocess block — the macros could never be consumed. Silent
    // acceptance would let a typo'd invocation build something other than what
    // the user asked for; fail loud instead (a language without a preprocessor
    // has no -D semantics).
    D_DefineRequiresPreprocess    = 0xD012,
    // c171 (D-FF1-AR-STATICLIB-DRIVER-WIRING): a `--resolve-library` STATIC
    // archive was passed while the target format's output is itself a static
    // library (`container: archive`). Bundling input archives' members INTO a
    // new static library (a "fat"/merged archive, à la `libtool`) is a real
    // but UNBUILT feature (D-FF1-STATICLIB-FAT-ARCHIVE); silently dropping the
    // input archive would produce a library missing the members the user asked
    // for. Fail loud instead. (Dynamic `--resolve-library` libraries — for a
    // member's extern FFI surface — are still accepted; only INPUT static
    // archives hit this.)
    D_StaticLibFatArchiveUnsupported = 0xD013,

    // ── H0xxx — HIR-tier diagnostics (plan 09; the 0xF high nibble renders
    // as the letter `H`, see diagnosticCodePrefix) ──
    // Codes emitted by HIR-tier subsystems — verifier, CST→HIR lowering,
    // AND the `.dsshir` text-format parser. Four root-cause categories
    // legitimately coexist in this band:
    //   - engine config bugs (H_UnsupportedLoweringForKind,
    //     H_ExternDeclMalformed);
    //   - user-source contradictions discovered at lowering
    //     (H_ExternHasInitializer, H_InvalidBreak, H_ShaderViolation);
    //   - verifier-time structural invariant breaches (H_TypeUnresolved,
    //     H_VerifierFailure, H_UnknownIntrinsic);
    //   - HIR-text serialization (`.dsshir`) parse-time failures
    //     (H_TextMalformed, H_TextVersionMismatch, H_TextUnknownName) —
    //     these share the band by phase-letter convention rather than by
    //     verifier/lowering lifecycle alignment, since the text-format
    //     parser is the HIR-tier analog of the P_* source-parser band.
    // Config-load errors in a `hirLowering` block (i.e. failures BEFORE any
    // verifier/lowering runs) use the C_* band (plan §4 Q8). Append, never
    // renumber.
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
    //   extends it to: a non-void function body that may fall through without
    //   returning, and a Call whose argument count/types disagree with the
    //   callee's FnSig. (Dead code after an unconditional terminator is NOT a
    //   failure — it is ISO-C-valid; the verifier reports it as the
    //   `H_UnreachableCode` WARNING below, not as an error.)
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
    //   unknown HIR kind/op, or the construct is a known-deferred one
    //   (typedef-of-pointer / compound-assign / ++ / arrays / strings —
    //   owned by a later plan; extern decls are fully lowered, and
    //   `extern int x = 5;` rejects via `H_ExternHasInitializer`). An
    //   `Error` HIR node is emitted as a recovery sentinel and lowering
    //   continues (collect-all); never a silent skip or a miscompile.
    H_UnsupportedLoweringForKind  = 0xF009,
    // H_ExternHasInitializer: an `extern` declaration carries an
    //   initializer (e.g. `extern int x = 5;`, `extern int y = z;`,
    //   `extern int a[2] = {0,1};`, even `extern int b = {};`). Extern
    //   announces a symbol whose storage lives in another translation
    //   unit — an initializer would either redefine the symbol locally
    //   (contradicting `extern`) or be silently dropped at lowering
    //   (D-FF2-3 fold replaces that drop). Detection is shape-based:
    //   any non-arrayDeclSuffix internal child of `varDeclTail` IS the
    //   init subtree. Distinct remediation from
    //   `H_UnsupportedLoweringForKind`: "remove the initializer", not
    //   "extend the engine".
    H_ExternHasInitializer        = 0xF00A,
    // H_ExternDeclMalformed: two user-facing triggers — both surface
    //   a malformed user-source extern declaration the analyzer
    //   couldn't recover. Remediation in both arms: fix the input.
    //   (1) The lowering's defensive arm reached an `externDecl`
    //   whose CST cannot be navigated to the varDeclTail-equivalent
    //   subtree — the language config IS correct (kindByChild
    //   present), but `descend(childPath)` returned invalid or a
    //   non-Internal node for this particular instance. Today's
    //   c-subset short-circuits at semantic-error time, so this arm
    //   is structurally unreachable through shipped grammars — but
    //   a future grammar permitting recovery shapes that reach
    //   lowering would trip it. D-FF2 H2 audit fold (post-fold
    //   #8/#9).
    //   (2) D-CSUBSET-EXTERN-LIBRARY-SYNTAX closure (step 13.3a,
    //   2026-06-02): the optional trailing `"libname"` string-literal
    //   inside `externFuncTail` had a malformed C-escape sequence
    //   (e.g. `\xZZ`) — `decodeStringLiteralBody` returned nullopt
    //   and the lowerer could neither honor the override nor
    //   silently fall back to the format-level default (which would
    //   leak to a wrong-DLL link with no breadcrumb). 6-agent
    //   2nd-order audit F4 fold.
    //   Distinct from `H_UnsupportedLoweringForKind` (engine config
    //   error — language hasn't configured kindByChild) and from
    //   `H_ExternHasInitializer` (user-source init contradiction).
    H_ExternDeclMalformed         = 0xF00B,
    // H_UnknownLinkageSpecifier: a declaration's specifier-prefix subtree held a
    // token that is neither a declared structural-syntax kind
    // (`linkageSpecifierIgnoredKinds` — e.g. `__attribute__`, parens) nor a
    // recognized entry in the language's `linkageSpecifiers` map — a typo
    // (`__attribute__((wek))`) or an unsupported attribute (`((noinline))`). Fail
    // loud rather than silently ignore it (D-CSUBSET-LINKAGE-UNKNOWN-SPECIFIER-
    // DIAGNOSTIC). Source-agnostic: the recognized + ignored sets are both
    // per-language config; the engine never hardcodes a specifier identity.
    H_UnknownLinkageSpecifier     = 0xF00C,
    // H_UnreachableCode: a statement following an unconditional terminator
    //   (Return / Unreachable / Break / Continue) within a Block — control can
    //   never reach it. ISO C permits this (C 6.8.x has no reachability
    //   constraint on statements); real compilers warn rather than reject, so
    //   the HIR verifier emits this as a WARNING (NOT the `H_VerifierFailure`
    //   error) and the module still compiles. The dead statement flows to MIR
    //   where the generic Block-lowering's fresh-dead-block + the mandatory MIR
    //   unreachable-prune drop it, so runtime is unaffected. A WARNING, not an
    //   error — and intentionally suppressible (NOT in the unsuppressable
    //   closed-table): silencing it cannot mask a miscompile.
    H_UnreachableCode             = 0xF00D,

    // ── c115 SEH (D-WIN64-SEH-FUNCLETS) — HirVerifier::checkSehContext ──
    // H_SehBuiltinContext: `_exception_code()` outside every enclosing __except
    //   filter-expression/handler-body, or `_exception_info()` outside every
    //   enclosing filter expression (MSVC: the intrinsics are dispatch-context
    //   reads; there is no value for them elsewhere).
    H_SehBuiltinContext           = 0xF00E,
    // H_SehJumpIntoRegion: a goto whose target label sits inside a part of a
    //   __try statement (guarded body / handler) that does not lexically enclose
    //   the goto — entering a guarded PC range sideways would give it a filter
    //   it must never have (MSVC rejects the construct too).
    H_SehJumpIntoRegion           = 0xF00F,
    // H_SehEarlyExit (D-CSUBSET-SEH-EARLY-EXIT, trigger-gated): a return /
    //   goto-out / break-out / continue-out from INSIDE a __try guarded body.
    //   Option (C) of the c115 design-audit: the guarded body has exactly ONE
    //   exit (the fall-through) so c116's scope-table region membership stays
    //   CFG-derivable; sqlite's ~13 SEH sites have ZERO early exits
    //   (amalgamation-swept). MSVC-legal — the anchor carries the
    //   mark-every-exit design for when a real consumer fires the trigger.
    H_SehEarlyExit                = 0xF010,
    // H_SehLabelAddress (D-CSUBSET-SEH-LABEL-ADDR, trigger-gated): `&&label`
    //   naming a label inside any part of a __try statement — a computed goto
    //   could then enter the guarded range undetectably at compile time.
    H_SehLabelAddress             = 0xF011,
    // H_WideCharSurrogateUnsupported (C11/C23 6.4.5): a wide/UTF string literal
    //   whose CST→HIR lowering could not represent its (escape-decoded) body in the
    //   requested element width. Two live triggers, both fail-loud (never a silent
    //   wrong code unit): (1) ill-formed UTF-8 in the body bytes; (2) a code point
    //   past U+10FFFF. A supplementary-plane code point (> U+FFFF) under a 16-bit
    //   element (`u"…"` / pe-`L"…"`) is NO LONGER a trigger for strings — it now
    //   encodes as a UTF-16 surrogate PAIR (Cycle C). The `actual` names the
    //   offending reason. An `Error` HIR node is emitted as a recovery sentinel and
    //   lowering continues (collect-all), exactly like H_UnsupportedLoweringForKind.
    //   (The name is retained for ordinal stability; the surrogate trigger is gone.)
    H_WideCharSurrogateUnsupported = 0xF012,
    // H_Utf8CharLiteralOutOfRange (C23 6.4.4.4): a `u8'…'` character constant whose
    //   single code point exceeds U+007F. A char8_t constant must be representable
    //   as ONE UTF-8 code unit (the ASCII range) — a multi-byte code point (`u8'β'`,
    //   `u8'€'`) has no single-unit value and is a constraint violation. Fail-loud
    //   (never a silently truncated low byte); an `Error` HIR node continues the
    //   collect-all lowering, exactly like H_WideCharSurrogateUnsupported.
    H_Utf8CharLiteralOutOfRange   = 0xF013,
    // H_WideCharValueUnrepresentable (C11/C23 6.4.4.4): a wide/UTF CHARACTER constant
    //   (`L'…'`/`u'…'`/`U'…'`) that does not denote exactly one code unit of its
    //   element type. Fail-loud triggers (never a silent wrong/truncated unit): a
    //   supplementary-plane code point (> U+FFFF) under a 16-bit element (`u'😀'`, or
    //   pe-`L'😀'`) — one char16_t/wchar_t holds ONE code unit, a surrogate pair is
    //   two; a body that is empty (`L''`) or multi-character (`L'ab'`); ill-formed
    //   UTF-8; or a code point past U+10FFFF. The `actual` names the specific cause.
    //   An `Error` HIR node continues the collect-all lowering.
    H_WideCharValueUnrepresentable = 0xF014,
    // H_InvalidUniversalCharacterName (C11/C23 6.4.3): a `\u`/`\U` universal
    //   character name that is MALFORMED (fewer than the required 4 / 8 hex digits,
    //   or a non-hex digit) or INVALID (names a UTF-16 surrogate half U+D800..U+DFFF
    //   or a value past U+10FFFF). Fail-loud (never a silently CESU-8 / overlong /
    //   truncated code unit) — the narrow byte path has no downstream UTF-8 check, so
    //   an unvalidated UCN would emit wrong bytes. C23 6.4.3 relaxed the <0x00A0
    //   basic-character restriction for LITERALS, so a sub-0xA0 UCN (`A`) is
    //   VALID and never trips this. An `Error` HIR node continues the collect-all
    //   lowering, exactly like H_WideCharValueUnrepresentable.
    H_InvalidUniversalCharacterName = 0xF015,
    // H_WideByteEscapeUnsupported (C11/C23 6.4.5, D-CSUBSET-WIDE-HEX-OCTAL-ESCAPE-VALUE):
    //   a `\x` hex / `\ooo` octal escape inside a wide/UTF literal (`u"…"`/`U"…"`/
    //   `u8"…"`/`L"…"` or the char forms). A byte escape names a raw code-unit VALUE,
    //   not a code point; assembling that value directly is a deferred feature, so it
    //   FAILS LOUD here rather than the old silent UTF-8-collapse (`u"\xC3\xA9"` once
    //   became ONE 0x00E9 unit instead of two intended units). Narrow `"…"`/`'…'`
    //   keep `\x`/octal (byte-producing, correct for a narrow element). Use `\u`/`\U`
    //   for a code point. An `Error` HIR node continues the collect-all lowering.
    H_WideByteEscapeUnsupported   = 0xF016,
    // H_ConflictingStringLiteralPrefixes (C11/C23 6.4.5p5, Cycle D): a run of
    //   ADJACENT string literals mixes TWO DIFFERENT non-narrow encoding prefixes
    //   (`u"a" U"b"`, `u8"a" u"b"`, `L"a" u"b"`, …). 6.4.5p5 leaves "whether
    //   differently-prefixed [wide] string literals can be concatenated"
    //   IMPLEMENTATION-DEFINED; this implementation REJECTS it (as gcc/clang do)
    //   rather than silently resolving to one prefix — a silent resolve MISCOMPILES
    //   (drops the other prefix's element width / code-unit encoding). A SINGLE
    //   non-narrow prefix with narrow segments (`"a" L"b"`) is NOT a conflict: the
    //   narrow segments widen to it (6.4.5p5 "if any of the tokens has an encoding
    //   prefix, the resulting sequence is treated as having that prefix"). The
    //   conflict is decided on opener TOKEN KINDS (format-agnostic), NEVER resolved
    //   cores (`u"`/`L"` both resolve to U16 on pe, so a core-keyed check would
    //   diverge by target). Both tiers fail loud: the semantic typer leaves the node
    //   UNTYPED + emits this (so a `sizeof` of it reports the real reason), and HIR
    //   lowering emits it + an `Error` node, continuing the collect-all lowering.
    H_ConflictingStringLiteralPrefixes = 0xF017,
    // ── VLA C5 (D-CSUBSET-VLA) — HirVerifier::checkVlaJumpScoping ──
    // H_VlaJumpIntoScope (C99 6.8.6.1p1): a `goto`, a `switch` case/default label,
    //   or an `&&label` whose target label sits inside the scope of a
    //   variably-modified (VLA) object PAST that object's declaration — the jump
    //   would bypass the VLA's runtime allocation, so on arrival the array's
    //   storage (and its size) is undefined. Fail-loud (never a jump into
    //   uninitialized dynamic stack). This is ALSO the dominance guarantor for the
    //   C5 teardown: banning entry-past-a-decl makes every LEGAL goto's restore-
    //   target StackSave dominate the goto in the CFG. Mirrors the SEH
    //   H_SehJumpIntoRegion / H_SehLabelAddress ancestor-walk.
    H_VlaJumpIntoScope            = 0xF018,
    // H_VlaComputedGotoInScope (D-CSUBSET-VLA): a computed `goto *expr`
    //   (GNU IndirectGotoStmt) lexically inside a VLA scope. Its target set is a
    //   runtime value, so the SP-restore watermark to unwind to cannot be proven
    //   at compile time — fail-loud rather than leak or over-free the dynamic
    //   stack. Runs fine when no VLA scope is involved. Mirrors the SEH
    //   H_SehEarlyExit IndirectGotoStmt arm.
    H_VlaComputedGotoInScope      = 0xF019,

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
    // A reachable block's stored StructCfMarker differs from the
    // canonical CFG derivation (`deriveStructCfMarkers`,
    // mir/mir_struct_markers.hpp) — the verifier recomputes the
    // derivation independently and requires stored == derived per
    // reachable block; the diagnostic names both markers.
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
    // A REACHABLE non-Phi operand use whose definition dominates it
    // (so SSA dominance holds — I_NotDominated does NOT fire) but whose
    // defining block appears LATER in the function's block LAYOUT
    // (funcBlockAt order) than the using block. Dominance is necessary
    // but NOT sufficient for the linear MIR→LIR lowering: every linear
    // consumer (MirFunctionRebuilder's rewrite map, mir_to_lir's
    // regForValue) requires a TOPOLOGICAL layout — a def must be EMITTED
    // before its use. A dominating-but-layout-later def is a producer
    // contract violation surfaced AT the producing pass (verify-after-
    // every-pass) rather than as a downstream rebuilder abort / silent
    // miscompile. Phi incomings are EXEMPT (loop back-edges legitimately
    // carry a def whose layout follows the use — the dominance arm owns
    // their semantics). Closes the D-OPT2 layout-contract class.
    I_LayoutUseBeforeDef      = 0xA010,
    // c115 SEH (D-WIN64-SEH-FUNCLETS): the region-skeleton pairing rules —
    // a SehTryBegin's filter block (succ[1]) must have exactly one CFG
    // predecessor and terminate in a SehFilterReturn with the MATCHING payload
    // (region id); the handler (the filter's succ[0]) must have exactly one
    // predecessor; every SehTryEnd's payload must name a SehTryBegin region in
    // the same function; SehExceptionCode/Info may appear only in a function
    // containing a SehTryBegin. Guards the optimizer contract (SimplifyCfg's
    // no-touch rule on SEH successors) — a merge/thread that damages the
    // skeleton reds HERE, at the pass that did it (verify-after-every-pass).
    I_SehStructure            = 0xA011,
    // D-OPT-RELEASE-SYSV-MIXED-CLASS-REG-ARG-DROP: two `Arg` instructions in
    // one function carry the SAME flat call-operand `position` (arg_payload.hpp).
    // Positions index the caller's actual-argument list, so a duplicate means
    // a payload wipe (a rebuild/merge site dropping the position → both
    // defaulting to a colliding ordinal) — the inliner would then map two
    // callee params to the same actual. Caught at every verify point.
    I_ArgPositionDuplicate    = 0xA012,
    // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN: a MIR `Alloca`'s secondary payload
    // (`payload2` = the local's EFFECTIVE alignment in bytes) is not 0 and not a
    // power of two ≤ 256. The alignment drives the frame-layout's per-alloca
    // slot placement; a corrupt/dropped value (a rebuild/merge site zeroing or
    // garbling payload2) would mis-align the slot — a silent stack miscompile.
    // Caught at every verify point (verify-after-every-pass in release).
    I_AllocaAlignmentNotPowerOfTwo = 0xA013,
    // C23 nullptr_t (D-CSUBSET-NULLPTR): a MIR instruction result type resolves to
    // TypeKind::NullptrT. By construction NEVER fires — the `nullptr` literal lowers
    // to the target-agnostic integer-0 null constant at the HIR tier, so NullptrT is
    // a SEMANTIC-TIER-ONLY kind that must not reach MIR. A never-fires backstop that
    // would catch a regression of that keystone invariant (e.g. a future change that
    // lets a NullptrT-typed Const materialize). Caught at every verify point.
    I_NullptrTypeInMir             = 0xA014,
    // C23 _BitInt(N) (D-CSUBSET-BITINT): a MIR value typed `_BitInt(N)` whose
    // producers disagree on the width — a tripwire for the by-construction wrap
    // chokepoint (CRIT-2). A `_BitInt(N)` value must always be N-significant-bits
    // (masked/sign-extended at materialization); a producer emitting a differently-
    // masked value would silently miscompile the wrap. Never fires under the
    // chokepoint discipline; the backstop that catches a regression of it.
    I_BitIntWidthInconsistent      = 0xA015,
    // VLA C1a (D-CSUBSET-VLA): a MIR `Alloca` whose operand/payload shape breaks the
    // runtime-sized invariant. A VLA-typed alloca (its pointee `isVlaArray`) MUST
    // carry exactly ONE operand (the total runtime byte size) and a ZERO primary
    // payload (the "runtime-sized" sentinel, distinct from a fixed alloca's non-zero
    // byte-size payload); a NON-VLA (fixed) alloca MUST carry NO operand. A mismatch
    // (a VLA alloca that lost its size operand → a silently under-sized fixed slot,
    // or a fixed alloca that grew a spurious runtime operand) is a by-construction
    // break of the PIECE-4 lowering. Interner-gated (needs `isVlaArray`); the
    // operand↔payload consistency half also runs interner-free. Caught at every
    // verify point.
    I_VlaAllocaOperandInvalid      = 0xA016,
    // VLA C5 (D-CSUBSET-VLA): a MIR `StackRestore` whose pairing invariant breaks.
    // A StackRestore's operand[0] MUST be a `StackSave`, and its scopeId payload
    // MUST equal that StackSave's payload (the pairing key). The generic SSA
    // dominance check already enforces that the StackSave dominates the restore
    // (the restore references it as an operand); this code adds the STRUCTURAL
    // pairing the flat IR can't otherwise express. The flat CFG cannot prove
    // "every exit edge is covered", so this is a pairing/containment check, NOT a
    // coverage claim (audit fix #6). Caught at every verify point (so an optimizer
    // transform that mis-pairs a save/restore reds AT the pass that did it).
    I_VlaStackRestorePairing       = 0xA017,
    // FC17.9(d) cycle 1b (D-CSUBSET-ATOMIC): a plain MIR `Load` or `Store` whose
    // ACCESSED type is `_Atomic`-qualified — a MISSED atomic-lowering funnel site.
    // Every scalar `_Atomic` access must lower to `AtomicLoad`/`AtomicStore` at the
    // hir_to_mir scalar-access chokepoint; a plain Load/Store still carrying an
    // atomic-qualified accessed type would SILENTLY perform a non-atomic access
    // (the exact miscompile the `_Atomic` qualifier exists to prevent). The belt
    // converts that silent gap into a LOUD failure at every verify point — for a
    // current OR a future new emit site. Load's accessed type is its result type;
    // Store's is the pointee of its address operand. Object-INITIALIZATION stores
    // (MirInstFlags::AtomicInitExempt; C11 7.17.2.1 — init is not itself atomic)
    // are the ONE exemption and do not trip it. Interner-gated (needs
    // isAtomicQualified). Caught at every verify point (verify-after-every-pass).
    I_AtomicAccessNotLowered       = 0xA018,

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
    //   operand is neither a `SymbolRef` (direct call) nor a `Reg`
    //   (indirect call through a register — FC4 c2 landed that
    //   encoding: schema `call` variant guard `["reg"]`, x86 FF /2 /
    //   arm64 BLR). Any OTHER operand kind in callee position is a
    //   lowering bug upstream — fail-loud totality, the code stays
    //   ALIVE as the residual-kind backstop.
    // L_IndirectCalleeClobberedByArgSetup (FC4 c2): an indirect call's
    //   post-regalloc callee REGISTER is also the destination of one
    //   of that same call's arg-passing moves (or the cc's variadic
    //   vector-count register on a variadic call). The materializer
    //   emits those moves BETWEEN the callee's definition and the
    //   `call <reg>` — the callee would be overwritten and the call
    //   would jump THROUGH AN ARGUMENT VALUE (silent garbage jump).
    //   The regalloc-tier rules (the indirect-callee arg-reg exclusion
    //   in lir_regalloc.cpp + the spill-reload scratch filter in
    //   lir_rewrite.cpp) make this unreachable; this code is the
    //   BACKSTOP that converts any future regression of either rule
    //   from a silent garbage jump into a loud compile error.
    L_StackPassedArgUnsupported    = 0xB007,
    L_CcRegLookupFailed            = 0xB008,
    L_MoveCycleUnsupported         = 0xB009,
    L_IndirectCallUnsupported      = 0xB00A,
    L_IndirectCalleeClobberedByArgSetup = 0xB00B,
    // D-CSUBSET-ALIGNAS-OVERALIGNED-STACK-LOCAL: `computeFrameLayout` rejected a
    //   function whose body-local requires MORE alignment than one stack slot
    //   (a C11/C23 `alignas(32)` / `alignas(64)` local, or an over-aligned
    //   struct/union used as a local). This static frame layout aligns the
    //   local area up to at most the slot width; a stricter requirement needs a
    //   dynamically realigned stack pointer (an AND-mask of RSP + a frame
    //   pointer to find spills), which is not built. The message reports the
    //   COMPUTED slot bound (agnostic — never an arch name). ≤ slot-width
    //   over-alignment (e.g. `alignas(16)`) is HONORED via a local-area pad, not
    //   this code — so this fires only past the representable bound.
    L_OverAlignedStackLocal        = 0xB00C,
    // VLA C1a → C1b boundary (D-CSUBSET-VLA): `lowerAlloca` reached a MIR `Alloca`
    //   carrying a RUNTIME size operand (a variable-length array `int a[n]`). The
    //   static frame model + `lea_frame_slot` rematerialization assume a fixed
    //   compile-time slot; a dynamic `sub rsp,<size>` + frame-pointer addressing is
    //   the NAMED C1b cycle. Fails loud BEFORE `emitInst` so no bogus fixed slot is
    //   recorded. UNSUPPRESSABLE — suppressed, the alloca would fall through to the
    //   fixed-slot path and silently lose the runtime size (a `lea` of a 1-slot
    //   scalar for the whole array — a stack miscompile).
    L_VlaDynamicAllocaUnsupported  = 0xB00D,
    // VLA C1b LEAF-scope gate (D-CSUBSET-VLA-NONLEAF-CALL-FRAME): a function with a
    //   variable-length array ALSO makes a call OR calls `va_start`. C1b builds the
    //   dynamic-stack frame model for a LEAF function only: after `sub sp,<vlaSize>`
    //   the outgoing-args area (call args) and the va-area leas are SP-relative, and
    //   under the moved SP NO base (neither SP nor the frame pointer) addresses them
    //   correctly while the `call`/va-walk runs at the moved SP. The non-leaf VLA
    //   frame model (outgoing-args placement under a runtime-moved SP) is a separate
    //   designed cycle. Fails loud (never a silent outgoing-arg/va miscompile).
    //   UNSUPPRESSABLE — suppressed, a non-leaf VLA would emit call args INSIDE the
    //   VLA region (an ABI break). Red-on-disable via the non-leaf fail-loud pins.
    L_VlaNonLeafFrameUnsupported   = 0xB00E,

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
    // D-ASM-ENCODE-FAILURE-FUNCTION-ROLLBACK (step 13.5 cycle 1
    // post-fold, silent-failure CRITICAL #2): function-level
    // summary emitted when one or more of its instructions failed
    // to encode AND the function was dropped from the
    // AssembledModule. A per-inst A_NoEncodingDeclared /
    // A_NoMatchingEncodingVariant already reported the root cause;
    // this code communicates the CONSEQUENCE (the whole function
    // is missing) so the user/test isn't surprised by the bytes
    // being absent without a function-tier diagnostic.
    A_FunctionEncodeAborted        = 0x1006,
    // A_ImmediateOperandOutOfRange (D-LK10-ENTRY-ARM64, v0.0.2 V2-1):
    //   a fixed32 encoder wire targeting an immediate slot (e.g. the
    //   AArch64 MOVZ `Imm16` slot) received an operand value that does
    //   not fit the slot's bit width (negative, or wider than the
    //   field). The encoder REFUSES to silently truncate — a truncated
    //   immediate is a wrong machine-code constant (e.g. a wrong
    //   syscall number), the exact miscompile class the round-trip
    //   oracle exists to prevent. Fix: materialize the constant with a
    //   multi-instruction sequence (MOVZ+MOVK / shifted MOVZ) once that
    //   lowering lands, or narrow the value. Unsuppressable.
    A_ImmediateOperandOutOfRange   = 0x1007,

    // ── Optimizer (renders as `X`) ────────────────────────────────────
    //
    // X_* family at 0x2xxx per plan 22 PR1. The optimizer (OPT1+ — the
    // `src/opt/` tier) emits these for pass-engine + pass-internal
    // failures. Stable across optimizer versions.
    //
    // X_UnknownPassId: the `optimize()` engine dispatched a `PassId`
    //   value with no matching arm in `runPass`'s switch. This is a
    //   substrate-shape violation (a new PassId enumerator was added
    //   without a handler) — fires before any MIR mutation. Closes
    //   the silent-fallback gap code-reviewer C2 flagged in OPT1
    //   cycle 1. D-OPT1-PASS-ID-STABILITY's enforcement surface.
    X_UnknownPassId                = 0x2001,
    // X_PipelineVersionMismatch: a `*.pipeline.json` file is missing
    //   `dssPipelineVersion` or carries a version this build doesn't
    //   speak. Same shape as C_VersionMismatch for target/format
    //   schemas — fail-loud at load time so a config-tier mismatch
    //   never silently maps to the wrong pipeline.
    X_PipelineVersionMismatch      = 0x2002,
    // X_UnknownPassName: a `*.pipeline.json` `passes[]` entry names a
    //   string that `optPassIdFromName` does not recognize. The
    //   config-load-time analog of X_UnknownPassId (which fires at
    //   runtime dispatch). Catches typos + drift between JSON and
    //   the PassId enum.
    X_UnknownPassName              = 0x2003,
    // X_PipelineMalformed: `*.pipeline.json` has a structural issue
    //   the loader can't recover from — missing required field,
    //   wrong type, unknown sub-key in a closed-key object (per
    //   D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD).
    X_PipelineMalformed            = 0x2004,
    // X_PipelineNameResolutionFailed: compile_pipeline asked for a
    //   pipeline by name (e.g. "release") but `loadShipped(name)`
    //   couldn't locate a matching `*.pipeline.json` under
    //   `src/dss-config/pipelines/`. Distinguished from
    //   X_PipelineMalformed (which means the file was found but
    //   broken).
    X_PipelineNameResolutionFailed = 0x2005,
    // X_OptReturnFalseWithoutDiagnostic: the optimize() engine's
    //   belt-and-suspenders guard fired — a pass returned ok=false
    //   WITHOUT reporting any new diagnostic. Distinct from
    //   X_UnknownPassId (which means a fabricated PassId reached
    //   the switch fallback). Pre-fold both shared X_UnknownPassId
    //   making it impossible to distinguish enum-drift from contract-
    //   violation in test pins.
    X_OptReturnFalseWithoutDiagnostic = 0x2006,
    // X_OptPassSkipped: emitted at Info severity when a pass declines
    //   to run on a specific module because of a feature carve-out
    //   (e.g. ConstFold skipping modules with runtime-init globals —
    //   D-OPT2-CONST-FOLD-RUNTIME-INIT-GLOBALS). Lets the user / tooling
    //   learn that the optimizer ran but produced no rewrite for THIS
    //   module, distinct from "ran 0 mutations because the code was
    //   already optimal."
    X_OptPassSkipped                  = 0x2007,
    // X_InlineMalformedCallSite: the Inlining pass (OPT7) selected a
    //   call site for inlining (callee passed the §2.9 legality gate —
    //   a defined, non-Weak, non-recursive, non-escaping single-block
    //   leaf) but the call's argument count does NOT match the callee's
    //   Arg-parameter count, so the Arg(i)→actual-operand substitution
    //   cannot preserve SSA. This is a structural violation of the
    //   MIR contract (the HIR→MIR lowering pairs a Call's args 1:1 with
    //   the callee signature), NOT a normal "decline to inline" — the
    //   ordinary gate refusals leave the call as-is silently. Fail loud
    //   so a malformed call never silently miscompiles into a wrong-arity
    //   splice (D-OPT7-INLINE-LEGALITY-GATE).
    X_InlineMalformedCallSite         = 0x2008,

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
    // (0x800C retired — was K_ChainedFixupsNotYetIntegrated, a
    // substrate-gap signal removed at D-LK6-14-INTEGRATION-PAYLOAD
    // closure. Companion D-LK6-14-INTEGRATION-GOT-SLOTS is open but
    // its failure surfaces at dyld load, not at the walker — outside
    // K_* scope.)
    // K_EntryPointResolvesToExtern: format.entryPoint resolved to an
    //   ExternImport rather than an AssembledFunction. Semantically
    //   invalid — an extern is a SYMBOL REFERENCE to code that lives
    //   in another module; it cannot SERVE AS the user entry point.
    //   Closes D-LK10-ENTRY-EXTERN-ENTRY-DIAG — split from the
    //   generic K_SymbolUndefined so triage tooling can distinguish
    //   "entry point name doesn't exist anywhere" from "entry point
    //   resolved to an extern (the user almost certainly named the
    //   wrong symbol in the format JSON)". Both fire at trampoline-
    //   injection time, but the user-visible remediation differs:
    //   K_SymbolUndefined → check that the user's source declares
    //   the named entry function; K_EntryPointResolvesToExtern →
    //   check that the format JSON's `entryPoint` field names a
    //   declared function rather than an imported symbol.
    K_EntryPointResolvesToExtern   = 0x800D,
    // K_DuplicateDataSymbol: two `AssembledData` items in the same
    //   module share the same non-sentinel `SymbolId`. The linker's
    //   symbol→VA join is keyed by `SymbolId`; duplicates would
    //   silently let "whichever item was processed last" win the
    //   resolution. Distinct from `K_SymbolUndefined` (a symbol
    //   referenced but never declared) — this is the symmetric
    //   "doubly declared" failure. Closes the silent-failure
    //   F-3 + type-design Q5 + comment-analyzer C5 convergence at
    //   the 3rd-order audit of D-LK4-RODATA-BSS-INVARIANT.
    // K_BssDataHasBytes: an `AssembledData` item has
    //   `section == DataSectionKind::Bss` but non-empty `bytes`.
    //   BSS is zero-fill — the wire format reserves `sh_size`
    //   without storing bytes; a non-empty Bss item is a substrate-
    //   shape violation. Distinct from `K_NoMatchingObjectFormat`
    //   (a format-dispatch failure) — this is a producer-side
    //   AssembledData invariant violation. Closes the same
    //   convergence as K_DuplicateDataSymbol above.
    K_DuplicateDataSymbol          = 0x800E,
    K_BssDataHasBytes              = 0x800F,
    // K_CrossCuMergeUnsupported: a cross-CU link the engine cannot perform. At LK11a
    //   (2026-06-04) this narrowed to the N==0 caller error (link() received no
    //   modules to merge). The former "extern import vs cross-CU definition" use is
    //   GONE — LK11a's reference resolution now BINDS such a reference to the sibling
    //   definition (the definition shadows the extern declaration; see
    //   `LinkedImage::resolvedCrossCuRefs`). Reserved for any future genuinely-
    //   unsupported merge interaction so callers keep a code distinct from the
    //   resolution diagnostics (K_SymbolRedefinedAcrossUnits / K_CrossCuImageEmitDeferred).
    K_CrossCuMergeUnsupported      = 0x8010,
    // K_SymbolRedefinedAcrossUnits: two or more STRONG (Global) definitions of the
    //   same symbol NAME across CUs. The cross-CU analog of K_DuplicateDataSymbol
    //   (which is within-module). Weak defs never trip this (a strong def shadows
    //   weak; multiple weak resolve to a deterministic pick); Local defs never trip
    //   it (they stay module-private and are matched only within their own module).
    K_SymbolRedefinedAcrossUnits   = 0x8011,
    // K_CrossCuImageEmitDeferred: RETIRED at LK11b (2026-06-04). It signalled
    //   "cross-CU resolution succeeded, merged bytes pending" while LK11a deferred
    //   emission. LK11b now PRE-MERGES the resolved CUs into one combined module that
    //   the existing format walker emits — there is no deferral, so this code is no
    //   longer emitted. Retained (not reused — diagnostic codes are append-only) for
    //   log-decode back-compat; the slot stays burned.
    K_CrossCuImageEmitDeferred     = 0x8012,
    // K_AbsolutePointerRelocMissing: the cross-CU merge (LK11b) needs an
    //   ABSOLUTE 64-bit pointer relocation kind to mint a GOT-like thunk
    //   slot (so an INDIRECT cross-CU call — `call qword ptr [slot]` — reads
    //   a slot containing the sibling definition's address). The merge finds
    //   that kind AGNOSTICALLY by formula (`widthBytes == 8 && !pcRelative`)
    //   on the active `TargetSchema` — never by a hardcoded "abs64" name /
    //   kind constant. This fires when NO relocation row on the target schema
    //   satisfies that formula: the target cannot express a 64-bit absolute
    //   pointer fixup, so a thunk slot would be a broken (un-relocated) zero.
    //   Fail loud rather than emit an image whose cross-CU calls dereference
    //   a null slot. (A target that genuinely has no abs64 reloc must add the
    //   row to its `*.target.json` before it can host cross-CU indirect calls.)
    //   c154: SCOPED to formats whose declared `externCallDispatch` is
    //   `indirect-slot` — the only dispatch whose call sites dereference a
    //   slot. A `direct-plt`/undeclared format binds the reference directly
    //   to the sibling definition (no slot, no abs64 needed), so this never
    //   fires there (pinned by Abs64GateFiresOnlyOnTheIndirectSlotArm).
    K_AbsolutePointerRelocMissing  = 0x8013,
    // K_ImageExecBitFailed: setting the POSIX execute bit on a just-written
    //   EXECUTABLE-flavor output (writer.cpp `--output` path) failed —
    //   std::filesystem::permissions returned an error_code. The bytes ARE
    //   on disk and correct (distinct from K_ImageWriteCloseFailed, which
    //   signals possibly-corrupt bytes); only the `+x` add failed, so the
    //   binary needs a manual `chmod +x` to run directly. WARNING severity:
    //   the artifact is valid, the exec bit a best-effort convenience
    //   (D-OUTPUT-EXEC-BIT). No-op on Windows (PE ignores Unix modes).
    K_ImageExecBitFailed           = 0x8014,
    // K_FormatLacksThreadLocalSupport (D-CSUBSET-THREAD-LOCAL): a format
    //   walker received a thread-local data item (a `.tdata`/`.tbss`-kind
    //   section) but has NO TLS image machinery for it (ELF PT_TLS / the PE
    //   TLS directory / Mach-O __thread_vars). The anti-static-alias
    //   backstop: a format JSON that prematurely opts its data-section list
    //   into "tdata"/"tbss" without walker support would otherwise lay the
    //   template out as ORDINARY data — every thread silently sharing one
    //   copy (a miscompile of the declared storage duration). Declared with
    //   the C1 semantic tier so the C1/C2/C3 walker slices share one code;
    //   fires from the walker tiers (slices B/C).
    K_FormatLacksThreadLocalSupport = 0x8015,
    // K_ThreadLocalOveralignedForFormat (D-CSUBSET-THREAD-LOCAL-PE-OVERALIGN):
    //   a thread-local object requires an alignment the OUTPUT FORMAT's
    //   per-thread TLS block cannot guarantee. On PE/x64 the loader allocates
    //   each thread's static-TLS block at only MEMORY_ALLOCATION_ALIGNMENT
    //   (16 bytes), and IMAGE_TLS_DIRECTORY64 carries NO block-base-alignment
    //   field to request more — so an `_Alignas(32) thread_local` var would be
    //   SILENTLY under-aligned (a SIMD/atomic thread_local relying on it is
    //   UB). ELF has no such limit (PT_TLS p_align honors any alignment), so
    //   this is a PE-format-LOCAL fail-loud gate (the format plugin's own
    //   knowledge — never a shared-substrate branch); the alignment axis is
    //   otherwise fully honored. Fires from the PE walker (pe.cpp) when the
    //   max TLS-block var alignment exceeds the format's guaranteed 16.
    K_ThreadLocalOveralignedForFormat = 0x8016,
    // K_ArchiveMemberNameInvalid (D-LK-STATIC-ARCHIVE-WRITER): the `ar`
    //   static-archive writer was handed a member whose file name is empty
    //   or contains a byte that would corrupt the container framing -- a
    //   '/' (the GNU short-name terminator + the reserved "/"/"//" special
    //   member spellings) or a '\n' (the "//" long-name table entry
    //   terminator). Fail loud rather than emit an archive whose member
    //   list a reader would mis-resolve. Also fires on an empty EXPORTED
    //   symbol name (an armap entry with no name is a caller bug -- the
    //   c161 reader would skip+warn it, silently shrinking the index).
    K_ArchiveMemberNameInvalid     = 0x8017,
    // K_ArchiveFieldOverflow (D-LK-STATIC-ARCHIVE-WRITER): an archive field
    //   cannot represent a value -- a member/armap/long-name-table payload
    //   longer than the 60-byte `ar_hdr`'s 10-digit ASCII-decimal `size`
    //   field can hold (>= 10^10 bytes), OR a member-header offset exceeding
    //   the SysV armap's 32-bit big-endian offset range (a >4 GiB archive --
    //   the GNU `/SYM64/` 64-bit-armap case, out of scope, see
    //   D-FF1-AR-BSD-VARIANT). Fail loud rather than truncate a field.
    K_ArchiveFieldOverflow         = 0x8018,
    // K-NEXT-SLOT: 0x8019 — grep this marker before adding a K_* code.

    // ── F_* — FFI binary-reader (plan 11 §2.2) + C-header-parser (plan 11 §2.3) ──
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
    //   not yet shipped (Mach-O after FF1-ELF + FF1-PE landed). Cites
    //   the per-format anchor (FF1-MachO) so the user knows what's
    //   pending.
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
    //   the function-body / non-extern-global case either. Fires from
    //   TWO arms: explicit `HirKind::ImportGroup` (#include) AND the
    //   residual default arm covering Module / statements / expressions
    //   / Unreachable / Extension / future HirKinds (with the
    //   numeric kind embedded in the diagnostic detail). Remediation:
    //   remove the construct from the curated header OR extend FF2 to
    //   accept it.
    // F_HeaderInvalidShippedPath: RETIRED 2026-06-03. The shipped-
    //   header consumer (`readCHeaderShipped`) was deleted alongside
    //   `findShippedFfiHeader` + `src/dss-config/ffi-headers/` — no
    //   production caller. Number kept reserved to preserve the band
    //   layout. (FF1/FF2 substrate stays — `readCHeader` /
    //   `readCHeaderFromText` are unused-feature substrate awaiting
    //   their trigger; only the shipped-path-loading variant retired.)
    // F_HeaderInternalInvariant: an internal-invariant violation
    //   reached the header walker — a compiler bug, not a user-fixable
    //   issue. Remediation: file a bug.
    // (D-FF2-3 CLOSED 2026-06-01 via `H_ExternHasInitializer`
    // (0xF00A) at the lowering tier — the FFI walker reuses the
    // c-subset frontend, so the reject reaches it through the
    // shared lowering pipeline; no separate F_* code needed.)
    F_HeaderParseFailed            = 0x5008,
    F_HeaderHasFunctionBody        = 0x5009,
    F_HeaderHasNonExternDecl       = 0x500A,
    F_HeaderEmptyImportLibrary     = 0x500B,
    F_HeaderGrammarLoadFailed      = 0x500C,
    F_HeaderHasUnsupportedTopLevel = 0x500D,
    F_HeaderInternalInvariant      = 0x500E,
    F_HeaderInvalidShippedPath     = 0x500F,  // RETIRED — see comment
    // ── FF3 ABI catalog (plan 11 §2.4) ──
    // F_AbiUnknownTuple: the (target.name, format.kind) pair has no
    //   row in FF3's catalog. Remediation: ship the format.json for
    //   this combination, OR add the catalog row if the combination
    //   is genuinely supported.
    // F_AbiNoMatchingCcInTarget: FF3 catalog says (target, format)
    //   needs a specific calling-convention name (e.g. "ms_x64"), but
    //   the target.json does not ship a callingConventions row by
    //   that name. Remediation: extend the target.json's
    //   callingConventions array OR drop the (target, format) pair.
    // F_AbiFormatAbiModelMismatch: defensive — (format.kind, target.abiModel)
    //   pair reached FF3 in an unexpected state (e.g. an
    //   `operand-stack` abi-model target paired with a non-WASM
    //   format-kind, or a `result-id` abi-model target paired with
    //   a non-SPIR-V format-kind). crossValidateTargetFormat
    //   should reject this upstream; FF3 emits the code if it slips
    //   past as a defense-in-depth.
    F_AbiUnknownTuple              = 0x5010,
    F_AbiNoMatchingCcInTarget      = 0x5011,
    F_AbiFormatAbiModelMismatch    = 0x5012,
    // F_AbiCcRegistersInconsistent: a target.json's
    //   `callingConventions[i]` row carries one or more register
    //   names (`argGprs`/`argFprs`/`returnGprs`/`returnFprs`/
    //   `callerSaved`/`calleeSaved`) that do not resolve in
    //   `target.registers[]`. Most common cause: paste-error from
    //   an unrelated arch (e.g. `ms_arm64` cc declared with
    //   `rcx,rdx,r8,r9` copied from `ms_x64`). Closes the
    //   silent-failure surface where FF3 would return a `cc *`
    //   into structurally-wrong data when a caller bypasses the
    //   JSON loader (TargetSchema ctor is public + skips
    //   validate()). (D-FF3-Coherence un-retired 2026-06-01 at
    //   post-fold #4 once the schema-loader-singleton premise
    //   was disproved.)
    F_AbiCcRegistersInconsistent   = 0x5013,
    // F_MangleMissingExpectedPrefix: `unapplyCManglingStrict` was
    //   called on a decorated input that lacks the per-format
    //   prefix the rule expects (e.g. a Mach-O symbol passed in as
    //   `printf` instead of `_printf`). Distinct from the
    //   conservative `unapplyCMangling` which silently passes such
    //   input through. Used by FF5 ingest where the format-kind
    //   is authoritative and a missing prefix is a structural
    //   anomaly. (D-FF4-3 post-fold-#3.)
    F_MangleMissingExpectedPrefix  = 0x5014,
    // F_FfiIngestDuplicateSymbol: FF5 ingest() saw the same canonical
    //   symbol exposed by more than one IngestionSource. First-source-
    //   wins per FFI design; this Warning records the shadowed
    //   definition so an operator can re-order sources or remove the
    //   redundant declaration. Distinct from F_HeaderParseFailed
    //   (which means "a single header failed to parse") — the
    //   duplicate-shadow is a CROSS-source concern. (post-fold #5
    //   silent-failure C3 / code-reviewer #88 fold.)
    F_FfiIngestDuplicateSymbol     = 0x5015,
    // F_FfiIngestAbiModelUnsupported: FF5 ingest() saw a (target, format)
    //   abiModel pair (operand-stack / result-id) that FF4 C-mangling
    //   does not cover. Permanent architectural exclusion — plan 17
    //   (SPIR-V) and plan 18 (WASM) own their own ingest surfaces.
    //   Distinct from `D_PlanNotLanded` which means "this entry point
    //   is pending plan landing" — this code means "this entry point
    //   will NEVER apply to this abiModel". (post-fold #6 silent-
    //   failure C2 fix.)
    F_FfiIngestAbiModelUnsupported = 0x5016,
    // F_FfiIngestEmptyCanonical: FF5 ingest() saw an empty canonical
    //   name — either a binary-reader / header-parser bug emitted a
    //   zero-length mangledName, OR a caller passed an
    //   `ExternDeclRef{node, ""}` value. Both are structural
    //   anomalies — proceeding with an empty key would silently
    //   shadow legitimately-distinct symbols in the by-name lookup.
    //   (post-fold #6 silent-failure C1 fix / D-FF5-EXTERNDECLREF-VALIDATE
    //   promoted from anchor to close-now.)
    F_FfiIngestEmptyCanonical      = 0x5017,
    // F_BinaryReaderPartialCorruption: an FF1 binary reader (ELF / PE /
    //   Mach-O) skipped one or more symbol-table entries during parse
    //   due to structural anomalies — out-of-bounds name indices,
    //   RVAs that don't resolve to any section, NUL-byte indices into
    //   the string table that wrap past EOF, etc. The reader continues
    //   parsing other entries; the surviving rows ARE returned. This
    //   Warning documents the partial loss so operators can investigate
    //   the source binary's integrity (truncated download, mismatched
    //   build artifact, ABI breakage at the library boundary).
    //   Distinct from F_CorruptedBinary (aborts the parse): a
    //   partially-corrupted .so / .dll / .dylib may still be usable
    //   for linking, but the operator should verify the library was
    //   built correctly. Emitted at Warning severity — under
    //   `--warnings-as-errors` this elevates to fail-loud, which is
    //   the correct strict-mode behavior. Member of `kUnsuppressableCodes`
    //   per the silent-failure-hunter 2nd-order audit: cap saturation
    //   would otherwise silently drop the very signal this code was
    //   introduced to surface.
    //   D-FF1-PARTIAL-CORRUPTION-MACHO CLOSED (FF1-MachO cycle
    //   2026-06-01): Mach-O reader emits the same counter+Warning
    //   contract; see src/ffi/binary_readers/macho_reader.cpp.
    F_BinaryReaderPartialCorruption = 0x5018,
    // F_FfiNoImportLibraryForFormat: FF5 `synthesizeFfiFromSourceDecls`
    //   (the source-declared sibling of `ingest`, used when the
    //   language's extern declarations ARE their own authority — no
    //   header / binary surface read) was invoked with an empty
    //   `importLibrary` for the active `ObjectFormatKind`. Means the
    //   language's `DeclarationRule.externLibraryByFormat` map has no
    //   entry for this format. Remediation: extend the language's
    //   semantics JSON (e.g. c-subset.lang.json) with an
    //   `externLibraryByFormat: { "pe": "msvcrt.dll", ... }` entry
    //   for the missing format. Distinct from
    //   `F_HeaderEmptyImportLibrary` (which means "a HEADER-source
    //   caller forgot the library identity at FF2 read time") — this
    //   code means "the LANGUAGE CONFIG has no per-format library
    //   declaration for source-declared externs". (FF6 Slice 2,
    //   2026-06-02.)
    F_FfiNoImportLibraryForFormat  = 0x5019,
    // F_ShippedHeaderNotFound: FF11 angle-include resolution. A
    //   `#include <h.h>` (the SYSTEM/angle form) named a header that
    //   was NOT found on any of the language's `shippedLibDirs` (the
    //   /usr/include analogue). A missing SYSTEM header is a HARD error
    //   in C — distinct from the missing QUOTE-include
    //   (`#include "x.h"`), which surfaces as the soft
    //   `D_UnresolvedImport` Warning (a local include may legitimately
    //   be provided by a later build step). Remediation: ship the
    //   header under `src/dss-config/<shippedLibDir>/<name>`, OR add
    //   the dir to the language's `semantics.shippedLibDirs`, OR fix
    //   the spelling. Member of `kUnsuppressableCodes` — a dropped
    //   system-header miss would silently compile a program that calls
    //   an undeclared library symbol. (FF11, 2026-06-05.)
    F_ShippedHeaderNotFound        = 0x501A,
    // F_ShippedLibDescriptorMalformed: the LANGUAGE-NEUTRAL shipped-
    //   library JSON descriptor (`src/dss-config/shippedLibs/<platform>/
    //   <name>.json`, read by `dss::ffi::readShippedLibDescriptor`) is
    //   not well-formed: invalid JSON, a missing required key
    //   (`library` / `symbols` / a symbol's `name` or `signature`), a
    //   value of the wrong type, an unknown key (closed-key rejection,
    //   mirroring D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD), or an
    //   unrecognized `kind` / `linkage` enum string. Remediation: fix
    //   the descriptor JSON shape. Member of `kUnsuppressableCodes` — a
    //   dropped descriptor-malformed miss would silently synthesize no
    //   externs (or the wrong ones), compiling a program whose
    //   `#include <stdio.h>` symbols resolve to nothing. (Neutral
    //   shipped-lib descriptor, closes D-FFI-SHIPPED-LIB-DESCRIPTOR-
    //   AGNOSTIC, 2026-06-06.)
    F_ShippedLibDescriptorMalformed = 0x501B,
    // F_ShippedLibUnsupportedType: a shipped-library descriptor symbol's
    //   `signature` hir-text type string failed to decode —
    //   `dss::parseTypeFromText` returned `InvalidType` (a truncated
    //   `"fn(ptr<"`, an unknown type keyword, or trailing tokens).
    //   CRITICAL fail-loud: a symbol whose signature does not decode
    //   MUST error rather than synthesize an `ExternFunction` carrying
    //   `InvalidType` — that would be a silently dropped / wrong-typed
    //   import. Remediation: fix the symbol's `signature` to a
    //   well-formed hir-text type. Member of `kUnsuppressableCodes`.
    //   (Neutral shipped-lib descriptor, 2026-06-06.)
    F_ShippedLibUnsupportedType    = 0x501C,
    // F_ShippedHeaderUnavailableForTarget: an angle `#include <h>` resolved to a
    //   shipped descriptor that EXISTS, but the descriptor's
    //   `availableObjectFormats` set excludes the active compile target's
    //   object-format (e.g. a POSIX `<sys/time.h>` declaring {"elf","macho"}
    //   included for a windows-pe target). Fail-loud: a header that does not
    //   exist on the target must error like a real toolchain (MSVC C1083), never
    //   silently resolve — and `__has_include` answers the per-target truth.
    //   Remediation: guard the include per platform, or build for a format the
    //   header supports. (D-SHIPPED-HEADER-PER-TARGET-AVAILABILITY, 2026-06-25.)
    F_ShippedHeaderUnavailableForTarget = 0x501D,
    // F_ShippedStructVariantAmbiguous: a shipped `structs` entry declares
    //   per-target `variants` (each a `when:{arch?,format?}` + its own field list,
    //   so a struct can carry the correct per-target byte layout — plan 25), and
    //   MORE THAN ONE variant matches the active compile target's (arch, format).
    //   The selection contract is MATCH-ALL-SPECIFIED + exactly-one — an
    //   under-specified `when` (e.g. `{"arch":"x86_64"}` matching BOTH x86_64-elf
    //   AND x86_64-pe) would otherwise silently pick the first → a wrong struct
    //   layout (e.g. the linux 144B `struct stat` on windows). Fail-loud: a
    //   SILENT-MISCOMPILE guard (unsuppressable). Remediation: fully-specify each
    //   variant's `when` so exactly one matches the target.
    //   (D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH, 2026-06-26.)
    F_ShippedStructVariantAmbiguous = 0x501E,
    // F_ShippedConstantVariantAmbiguous: a shipped `constants` entry declares
    //   per-target `variants` (each a `when:{arch?,format?}` + its own
    //   {value,type}), and MORE THAN ONE variant matches the active compile
    //   target's (arch, format). Same MATCH-ALL-SPECIFIED + exactly-one contract
    //   as the struct-variant sibling — an under-specified `when` would otherwise
    //   silently pick the first → a WRONG constant VALUE on this target (e.g. a
    //   per-platform `O_NONBLOCK`). Fail-loud: a SILENT-MISCOMPILE guard
    //   (unsuppressable). Remediation: fully-specify each variant's `when` so
    //   exactly one matches. (D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH, 2026-06-26.)
    F_ShippedConstantVariantAmbiguous = 0x501F,
    // F_ShippedTypedefVariantAmbiguous: a shipped `typedefs` entry declares
    //   per-target `variants` (each a `when:{arch?,format?}` + its own `type`),
    //   and MORE THAN ONE variant matches the active compile target's
    //   (arch, format). Same contract as the constant/struct siblings — an
    //   under-specified `when` would silently pick the first → a WRONG typedef
    //   WIDTH on this target (e.g. a `wchar_t` that is 32-bit on elf but 16-bit on
    //   pe). Fail-loud: a SILENT-MISCOMPILE guard (unsuppressable). Remediation:
    //   fully-specify each variant's `when`.
    //   (D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH, 2026-06-26.)
    F_ShippedTypedefVariantAmbiguous = 0x5020,
    // F_ShippedMacroVariantAmbiguous: a shipped `macros` entry declares
    //   per-FORMAT `variants` (each a `when:{format}` + its own
    //   {replacement, params?}), and MORE THAN ONE variant matches the active
    //   object-format. Macros are FORMAT-ONLY (arch is not threaded into the
    //   preprocessor — c9 build-key avoidance), so the `when` carries `format`
    //   alone. An under-specified set (two variants both selecting the active
    //   format) would silently pick the first → a WRONG macro REPLACEMENT on this
    //   target (e.g. errno's `__errno_location` on elf vs `__error` on macho).
    //   Fail-loud: a SILENT-MISCOMPILE guard (unsuppressable). Remediation:
    //   fully-specify / de-duplicate each variant's `when.format`.
    //   (D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH, 2026-06-26.)
    F_ShippedMacroVariantAmbiguous = 0x5021,
    // F_FfiResolveLibrarySymbolAbsent: under the `--resolve-library <path>`
    //   driver surface (c162, D-FF1-READER-CONSUMER), compile_pipeline routed
    //   a source-declared "binary-governed" extern (no per-symbol library
    //   override, not a bare no-library reference) to the live `ingest()`
    //   binary-reader consumer, and it matched NO row in ANY named binary's
    //   real export table -- AND the symbol is NOT a known system symbol
    //   (absent from every shipped-library descriptor too). So it is a
    //   GENUINE typo or a missing library (e.g. `dss_lib_answr` for
    //   `dss_lib_answer`), not a bare-`extern`'d libc call the user forgot to
    //   #include. Failing loud NOW -- naming the symbol + the searched
    //   libraries -- catches an own-library typo at compile time (reading a
    //   real export table is proof the symbol exists) instead of letting it
    //   mis-bind to the format-default library and fail at link/load. A
    //   governed extern that IS a known system symbol falls through to its
    //   format-default library (gcc implicit-libc), NOT this diagnostic, so a
    //   legitimate `bare extern puts + --resolve-library ownlib` program is
    //   never wrongly rejected. The extern TYPE still comes from the inline
    //   declaration -- the reader supplies existence + binding only. Fail-loud:
    //   a SILENT-DANGLING-IMPORT guard (unsuppressable). Remediation: fix the
    //   spelling, #include the header that declares it, or add the defining
    //   library to `--resolve-library`. (D-FF1-READER-CONSUMER, c162.)
    F_FfiResolveLibrarySymbolAbsent = 0x5022,
    // F_ShippedTypeIdentityConflict: two shipped descriptors resolved for the
    //   SAME compile target declare the same struct/union TAG NAME (or the same
    //   typedef NAME) as DIFFERENT types — OR a descriptor spells a vocabulary
    //   identity tag (`i64 "long"`) whose width contradicts what the ACTIVE
    //   LANGUAGE gives that name under the active data model. Both are
    //   SILENT-MISCOMPILE surfaces the per-file reader structurally cannot see:
    //   descriptor injection is FIRST-WINS BY NAME and only the WINNER gets a
    //   `compositeScopeByType` field scope, so a divergent second declaration
    //   interns a SECOND TypeId whose members are unreachable — the user gets an
    //   INCLUDE-ORDER-DEPENDENT `S000D member access '.' requires a
    //   composite-typed operand` (`struct timeval` spelled `{i64, i64}` in
    //   sys/resource.json but `{i64 "long", i64 "long"}` in sys/time.json), and a
    //   tag whose width the data model cannot produce is a PHANTOM type matching
    //   no `_Generic` association and no pointer of that spelling (`ssize_t` as
    //   `i64 "long"` on LLP64). Remediation: make every declaration of the name
    //   byte-identical per target (per-format / per-dataModel `variants` when the
    //   type genuinely diverges). Member of `kUnsuppressableCodes`.
    //   (D-LANG-TYPE-IDENTITY-VOCABULARY, 2026-07-20.)
    F_ShippedTypeIdentityConflict = 0x5023,
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

    // Optional rendering-only prefix prepended to the expected/actual
    // prose at `DiagnosticReporter::format()` time (e.g.
    // `[target=x86_64:elf64-x86_64-linux] ` stamped by
    // `program::mergeWithTargetContext` so multi-target runs route
    // per-target context to tooling without polluting the dedup hash).
    // The CLI `drainDiagnosticsToStderr` and LSP `composeMessage`
    // render paths perform the symmetric prepend. Excluded from
    // `hashKey` so cross-source legitimate duplicates (same diagnostic
    // from different targets) collapse correctly at the merge
    // destination. D-MERGE-DEDUP-PREFIX-COLLISION fold.
    //
    // Anchored D-MERGE-CONTEXT-PREFIX-SIDE-TABLE: the empty-string
    // overhead (~24 bytes SSO buffer) lands on every ParseDiagnostic,
    // even single-target runs that never set the field. Negligible
    // today; trigger to migrate to a side-table on the reporter (or a
    // `std::unique_ptr<std::string>` 8-byte slot) is "ParseDiagnostic
    // exceeds 200 bytes" (sizeof check) OR "Tracy/perf-record on a
    // diagnostic-heavy compile attributes >5% of compile time to
    // ParseDiagnostic alloc/copy".
    //
    // Anchored D-MERGE-CONTEXT-PREFIX-CLOBBER: `mergeWithTargetContext`
    // uses `copy.contextPrefix = prefix;` (assignment, not append).
    // Today this is invariant because `mergeWithTargetContext` is the
    // ONLY producer; a future second producer (e.g. per-file `[file=]`
    // stamping under a multi-source merge wrapping a multi-target
    // merge) would have its prefix silently clobbered. Trigger: first
    // additional contextPrefix writer outside mergeWithTargetContext.
    // Resolution: switch the merge to append (`copy.contextPrefix +=
    // prefix`) OR move contextPrefix to a stack (vector<string>) so
    // nested merges preserve outer context.
    std::string contextPrefix;
};

} // namespace dss
