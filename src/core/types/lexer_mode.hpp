#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

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

// Stack of active lexer modes the tokenizer is currently inside. The
// top of the stack is the mode that decides which tokens the tokenizer
// will produce next.
//
// Strict-ops policy: `pop`/`replaceTop`/`top` abort via the project's
// `*Fatal` pattern when called on an empty stack. Reaching this state
// means the schema and tokenizer disagree on Push/Pop pairing — a real
// bug, not a recoverable condition. Lenient callers explicitly opt in
// via `tryPop()` / `topOrInvalid()`.
//
// Tokenizer integration deferred — TreeBuilder::Checkpoint will route
// snapshot/restore through this type when the tokenizer phase lands.
class DSS_EXPORT LexerModeStack {
public:
    // Snapshot for speculative rollback. Captures the full frames
    // vector (not just depth) — PushMode under speculation may
    // arbitrarily reshape the stack, and there's no constant-time
    // inverse for "what was here before." Stamped with the originating
    // stack's identity so `restore` can refuse cross-stack mixups.
    class DSS_EXPORT Snapshot {
    private:
        friend class LexerModeStack;
        std::vector<LexerModeId> frames_;
        // Identity stamp captured at snapshot() time. Asserted equal
        // to `&stack` in restore() to catch the speculative-Checkpoint
        // class of bug where a Snapshot from one stack is restored
        // into a different one. Stored as raw uintptr_t (not a back-
        // pointer) so the Snapshot stays trivially copyable / movable.
        std::uintptr_t           owner_ = 0;
    };

    LexerModeStack() noexcept = default;

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
    std::vector<LexerModeId> frames_;
};

// Metadata for a single named lexer mode. Construct via the factory
// `make(name, id, defaultToken)` so id is set atomically. Field access
// stays public (POD discipline) so the loader can fill auxiliary
// per-mode tables without going through accessors.
struct DSS_EXPORT LexerMode {
    std::string                  name;
    LexerModeId                  id;
    std::optional<SchemaTokenId> defaultToken;

    [[nodiscard]] static LexerMode make(std::string name,
                                        LexerModeId id,
                                        std::optional<SchemaTokenId> defaultToken = {}) {
        return LexerMode{std::move(name), id, defaultToken};
    }
};

} // namespace dss
