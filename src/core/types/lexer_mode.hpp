#pragma once

#include "core/export.hpp"
#include "core/types/enum_name_table.hpp"  // EnumNameTable (kModeOpTable, kUnterminatedFlavorTable)
#include "core/types/strong_ids.hpp"
#include "core/types/tree_node.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dss {

// What a lexeme's resolution does to the tokenizer's mode stack.
//   None         — no mode-stack effect.
//   PushMode     — push `modeArg` onto the top of the stack.
//   PopMode      — pop the top; `modeArg` is ignored.
//   ReplaceMode  — swap the top of the stack with `modeArg` (no nest).
enum class ModeOp : std::uint8_t {
    None,
    PushMode,
    PopMode,
    ReplaceMode,
};

// ── THE SPELLINGS HAVE ONE OWNER (D-CONFIG-GRAMMAR-LOADER-INLINE-CHAIN-VOCABULARIES-REMAIN) ──
//
// ★★ WHAT THIS REPLACED, AND IT WAS TWO OWNERS BEFORE THE SENTENCES ARE EVEN
// COUNTED. `modeOpName` (`lexer_mode.cpp`) was a `switch` retyping all four
// spellings, and `grammar_schema_json.cpp` decided acceptance with an INLINE
// `opStr == "pushMode" / "popMode" / "replaceMode"` chain that knew nothing
// about it — a READER and a WRITER of one vocabulary, each hand-written, plus
// two diagnostics restating the accepted set beside the chain. The census that
// swept this class could not see the chain at all
// (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET), so nothing
// compared the two.
//
// ⚠ ROW ORDER IS LOAD-BEARING: `None` is row 0, so `name()`'s fall-back for an
// out-of-range value is `"none"` — byte-identical to the switch's own
// `return "none"` unreachable arm, and semantically the right sentinel (a
// lexeme with no mode effect).
inline constexpr EnumNameTable<ModeOp, 4> kModeOpTable{{{
    { ModeOp::None,        "none"        },
    { ModeOp::PushMode,    "pushMode"    },
    { ModeOp::PopMode,     "popMode"     },
    { ModeOp::ReplaceMode, "replaceMode" },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kModeOpTable);

[[nodiscard]] DSS_EXPORT std::string_view modeOpName(ModeOp op) noexcept;

[[nodiscard]] constexpr std::optional<ModeOp>
modeOpFromName(std::string_view s) noexcept {
    return kModeOpTable.fromName(s);
}

// The ops a schema may actually DECLARE — the table minus the sentinel, which
// `modeOpFromName("none")` resolves and the loader then refuses. A loader's
// "expected …" half renders THIS, never a retyped list. The `3` is checked by
// `namesWhere` at compile time, so a fifth DECLARABLE op cannot silently keep a
// three-name sentence.
//
// ★ THE `+ 1` BELOW IS THE HALF `namesWhere` CANNOT SEE.
// D-CORE-NAMESWHERE-LITERAL-COUNT-IS-BLIND-TO-A-SECOND-SENTINEL: `namesWhere<M>`
// compares `M` only against the rows the predicate ACCEPTS, so a fifth
// UNDECLARABLE op moves nothing it can observe. ✔MEASURED 2026-08-23 in a
// worktree with `g++ -std=c++23 -fsyntax-only -I src`: a fifth row plus the
// widened `isDeclarableModeOp` that rejects it COMPILED CLEAN before this
// assert existed. That is the direction that matters most for THIS vocabulary,
// because the sentinel is what the grammar loader's `"none"` arm turns on: a
// second unrefusable op would reach `modeOpFromName` and be accepted by a
// loader whose "expected …" sentence never mentioned it. The assert relates the
// table's own row total to this projection's literal — two numbers with
// DIFFERENT OWNERS, so it is not the tautology a `rows.size() - 1` spelling
// would have been
// (D-CORE-NAMESWHERE-COUNT-DERIVED-FROM-THE-TABLE-IS-A-TAUTOLOGY).
[[nodiscard]] constexpr bool isDeclarableModeOp(ModeOp op) noexcept {
    return op != ModeOp::None;
}
inline constexpr auto kDeclarableModeOpNames =
    namesWhere<3>(kModeOpTable, isDeclarableModeOp);
static_assert(kModeOpTable.rows.size()
                  == kDeclarableModeOpNames.size() + 1,
              "kModeOpTable must have exactly ONE undeclarable row (the 'none' "
              "sentinel) — a second one leaves `namesWhere`'s literal count "
              "matching while the grammar loader's 'expected …' half silently "
              "stops naming the set its `\"none\"` refusal arm enforces");

// Stack of active lexer modes. Top frame chooses which tokens the
// tokenizer produces next. `pop`/`replaceTop`/`top` abort on empty —
// disagreement between schema and tokenizer is a bug, not a runtime
// condition. Lenient callers opt in via `tryPop()` / `topOrInvalid()`.
class DSS_EXPORT LexerModeStack {
public:
    // Speculative-rollback token. Captures the full frames vector
    // (PushMode under speculation may reshape arbitrarily) plus a
    // per-instance id stamp (`owner_`); `restore` aborts when the
    // stamp doesn't match, defending against address-recycling
    // false-passes that a raw `this` pointer would slip past.
    class DSS_EXPORT Snapshot {
    private:
        friend class LexerModeStack;
        std::vector<LexerModeId> frames_;
        std::uint64_t            owner_ = 0;
    };

    LexerModeStack() noexcept;

    void push(LexerModeId mode);
    void pop();                                  // fatal on empty
    void replaceTop(LexerModeId mode);           // fatal on empty
    void apply(ModeOp op, LexerModeId arg);
    // Drop every frame. Used by the tokenizer to recover after a fatal
    // lex error or to reset between files.
    void clear() noexcept { frames_.clear(); }

    [[nodiscard]] bool        empty() const noexcept { return frames_.empty(); }
    [[nodiscard]] std::size_t depth() const noexcept { return frames_.size(); }
    // Strict accessor: fatal on empty. Use `topOrInvalid()` when an
    // empty stack is a legitimate observable state at the call site.
    [[nodiscard]] LexerModeId top() const noexcept;
    // Lenient peek: returns InvalidLexerMode when the stack is empty.
    [[nodiscard]] LexerModeId topOrInvalid() const noexcept {
        return frames_.empty() ? LexerModeId{} : frames_.back();
    }
    // Lenient pop: returns false (no-op) when empty; true on success.
    bool tryPop() noexcept {
        if (frames_.empty()) return false;
        frames_.pop_back();
        return true;
    }
    [[nodiscard]] std::span<LexerModeId const> frames() const noexcept { return frames_; }

    [[nodiscard]] Snapshot snapshot() const;
    void                   restore(Snapshot const& snap);

private:
    static std::atomic<std::uint64_t> nextInstanceId_;
    std::uint64_t            instanceId_;
    std::vector<LexerModeId> frames_;
};

// How the tokenizer should flavor a `P_Unterminated*` diagnostic for a
// mode that's still open at EOF. Schema-declared (not heuristically
// inferred from the mode name).
enum class UnterminatedFlavor : std::uint8_t {
    String,    // default for delimited-string-style modes
    Comment,   // line- or block-comment modes
    Generic,   // mode names not in either family
};

// ── THE SPELLINGS HAVE ONE OWNER (D-CONFIG-GRAMMAR-LOADER-INLINE-CHAIN-VOCABULARIES-REMAIN) ──
//
// `lexerModes.<name>.unterminatedAs`. Its refusal rendered the set as escaped
// double quotes (`"string", "comment", "generic"`) — correct on the day it was
// typed, and a second owner from that day on. Every enumerator is declarable;
// `String` is row 0, matching `UnterminatedFlavor`'s own default, so an
// out-of-range value renders as the flavour it would behave as.
inline constexpr EnumNameTable<UnterminatedFlavor, 3> kUnterminatedFlavorTable{{{
    { UnterminatedFlavor::String,  "string"  },
    { UnterminatedFlavor::Comment, "comment" },
    { UnterminatedFlavor::Generic, "generic" },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kUnterminatedFlavorTable);

[[nodiscard]] constexpr std::string_view
unterminatedFlavorName(UnterminatedFlavor f) noexcept {
    return kUnterminatedFlavorTable.name(f);
}
[[nodiscard]] constexpr std::optional<UnterminatedFlavor>
unterminatedFlavorFromName(std::string_view s) noexcept {
    return kUnterminatedFlavorTable.fromName(s);
}

// ★★★ WHAT THE END OF INPUT DOES TO A FRAME OF THIS MODE STILL OPEN THERE —
// `lexerModes.<name>.atEndOfInput`, beside `unterminatedAs` (which names the
// FLAVOUR of what is open; this names the VERDICT). ONE enum rather than a
// "closes" boolean plus a "warns" boolean, because two booleans admit "warns but
// does not close", a state no reference has and no reader could predict.
//
//   • `Unterminated` — the default and today's behaviour: an open frame at the
//     end of input is refused (`P_Unterminated*`, flavoured by `unterminatedAs`).
//   • `Closes` — the end of input closes the frame, silently. A C `//` comment:
//     ✔MEASURED 2026-09-23, gcc 13.3.0, clang 18.1.3, aarch64 gcc, mingw-w64 gcc
//     13.2.0 and MSVC 19.51 all accept a C file whose last line is a `//`
//     comment with no newline (clang warns only under `-pedantic`).
//   • `ClosesWithWarning` — the end of input closes the frame, and says so. A gas
//     `/*` comment: ✔MEASURED 2026-09-23, GNU as 2.42 accepts a `.s` ending
//     inside one ("end of file in comment" / "end of file in multiline comment")
//     and the program runs; clang 18.1.3 refuses it. The references split on
//     ACCEPTANCE, so the union accepts — and the warning is gas's, because a
//     missing `*/` silently swallowing the rest of a file must not be silent.
//
// ⚠ A `popAtNewline` MODE HAS NO DECLARABLE POLICY: its frame ends with its line,
// and the end of input ends the last line, so its verdict is `Closes` BY
// CONSTRUCTION. The loader derives it and REFUSES an explicit `atEndOfInput` on
// such a mode — one way to say one thing.
enum class EndOfInputPolicy : std::uint8_t {
    Unterminated,
    Closes,
    ClosesWithWarning,
};

// `Unterminated` is row 0, matching the field's own default, so an out-of-range
// value renders as the verdict it would behave as.
inline constexpr EnumNameTable<EndOfInputPolicy, 3> kEndOfInputPolicyTable{{{
    { EndOfInputPolicy::Unterminated,      "unterminated"      },
    { EndOfInputPolicy::Closes,            "closes"            },
    { EndOfInputPolicy::ClosesWithWarning, "closesWithWarning" },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kEndOfInputPolicyTable);

[[nodiscard]] constexpr std::string_view
endOfInputPolicyName(EndOfInputPolicy p) noexcept {
    return kEndOfInputPolicyTable.name(p);
}
[[nodiscard]] constexpr std::optional<EndOfInputPolicy>
endOfInputPolicyFromName(std::string_view s) noexcept {
    return kEndOfInputPolicyTable.fromName(s);
}

// Schema-declared specification for a body-mode's default token. The
// bundle makes illegal states unrepresentable: a mode without a
// defaultToken cannot accidentally carry flags meant for one.
struct DSS_EXPORT DefaultTokenSpec {
    SchemaTokenId kind;
    NodeFlags     flags = NodeFlags::None;
    // When true, the body mode emits ONE token of `kind` spanning the whole
    // literal body (between the opener and the close delimiter) instead of one
    // token per codepoint — the same single-token model `IntLiteral` uses. The
    // mode keeps all its escape / endsAt / unterminated logic; only emission
    // granularity changes. A coalesced kind is IN-grammar (the loader does NOT
    // add it to `bodyDefaultTokenKinds`), so a shape's `operand` can reference
    // it and the literal's value can be decoded. `false` = per-codepoint
    // emission (the off-grammar comment/string-char behavior). Set per body
    // mode that backs a value-bearing literal (char / string).
    bool          coalesce = false;
    // D-TOK-CLOSING-DELIMITER-HAS-NO-TOKEN. The token kind the tokenizer emits
    // for the body's CLOSE delimiter (`"` / `'` / `>`) when `coalesce` is true.
    // Loaded from `lexerModes.<name>.defaultToken.closeToken` — REQUIRED
    // whenever `coalesce` is true (the loader rejects `coalesce: true` with no
    // `closeToken` rather than defaulting one) and left invalid otherwise: a
    // per-codepoint mode already emits its closer through the ordinary body
    // path, so a `closeToken` there would be a knob that lies.
    //
    // Why it cannot simply reuse `kind`: the closer is a DELIMITER, not body
    // content. Every consumer that filters a literal's children BY KIND —
    // `decodeAdjacentStringBodies` (string_literal_decode.hpp) is the load-
    // bearing one — would otherwise fold the delimiter bytes into the decoded
    // value (`"abc"` → `abc"`) with the semantic and HIR tiers agreeing on the
    // same wrong length, so no cross-tier guard could catch it. The loader
    // rejects `closeToken == kind` for exactly that reason.
    //
    // The kind is IN-grammar (a shape names it as a real slot), so the loader
    // records it in `Data::modeIntroducedKinds` — NOT in `bodyDefaultTokenKinds`,
    // which is the off-grammar cursor-skip set.
    SchemaTokenId closeToken;
};

// Metadata for a single named lexer mode. Construct via `make(name, id,
// defaultToken, unterminatedFlavor)`. `id` is required — no default —
// to keep "Invalid" from sneaking into the factory's contract.
//
// `defaultToken` is a bundle (kind + flags). Absent when the mode is a
// pure main-style mode (only per-mode tokens, no per-codepoint
// fallback). Present for body modes — the flags propagate onto every
// per-codepoint emission (e.g. `EmptySpace` on a comment body so the
// AST cursor skips wholesale).
struct DSS_EXPORT LexerMode {
    std::string                       name;
    LexerModeId                       id;
    std::optional<DefaultTokenSpec>   defaultToken;
    UnterminatedFlavor                unterminatedFlavor = UnterminatedFlavor::String;
    // Line-scoped mode: when true, the tokenizer pops THIS frame the
    // moment it scans a newline (the newline token is still emitted in
    // this mode, then the frame is dropped). General capability for
    // line-oriented constructs — C preprocessor directives, assembly —
    // where a mode entered mid-line must not leak its lexing rules into
    // the next line. A defaultToken-bearing body mode (a "consume until
    // endsAt" scanner) may NOT set this — the two close mechanisms are
    // mutually exclusive (loader rejects the combination). Default false.
    bool                              popAtNewline = false;
    // What the end of input does to a frame of this mode still open there —
    // see `EndOfInputPolicy`. `Closes` for every `popAtNewline` mode (derived
    // by the loader, never declared).
    EndOfInputPolicy                  atEndOfInput = EndOfInputPolicy::Unterminated;

    [[nodiscard]] static LexerMode make(std::string name,
                                        LexerModeId id,
                                        std::optional<DefaultTokenSpec> defaultToken,
                                        UnterminatedFlavor flavor = UnterminatedFlavor::String,
                                        bool popAtNewline = false,
                                        EndOfInputPolicy atEndOfInput = EndOfInputPolicy::Unterminated) {
        // A line-scoped frame ends with its line and the end of input ends the
        // last line, so its verdict is `Closes` whatever was passed; the loader
        // REFUSES a declared `atEndOfInput` on such a mode, so no document can
        // ask for another.
        EndOfInputPolicy const policy =
            popAtNewline ? EndOfInputPolicy::Closes : atEndOfInput;
        return LexerMode{std::move(name), id, defaultToken, flavor, popAtNewline,
                         policy};
    }
};

} // namespace dss
