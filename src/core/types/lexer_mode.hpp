#pragma once

#include "core/export.hpp"
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

[[nodiscard]] DSS_EXPORT std::string_view modeOpName(ModeOp op) noexcept;

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

    [[nodiscard]] static LexerMode make(std::string name,
                                        LexerModeId id,
                                        std::optional<DefaultTokenSpec> defaultToken,
                                        UnterminatedFlavor flavor = UnterminatedFlavor::String) {
        return LexerMode{std::move(name), id, defaultToken, flavor};
    }
};

} // namespace dss
