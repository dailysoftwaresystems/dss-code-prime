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

// What a lexeme's resolution does to the tokenizer's mode stack. Set
// on `LexemeMeaning::modeOp` by the loader; consumed by the tokenizer
// (when authored) immediately after producing the token.
//
//   None         — no mode-stack effect (the default; matches v1).
//   PushMode     — push `modeArg` onto the top of the stack.
//   PopMode      — pop the current mode off; `modeArg` is ignored.
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
// will produce next. Nested-interpolation languages push a new mode
// when entering each `{ ... }` interpolation block, and pop when the
// closing `}` is matched.
//
// PR5 ships this type for the future tokenizer to consume. TreeBuilder
// does NOT integrate it yet — when the tokenizer phase lands, it will
// own the live stack and surface snapshot/restore to TreeBuilder's
// Checkpoint (see schema-expressiveness-v2 §5.4).
class DSS_EXPORT LexerModeStack {
public:
    // Snapshot for speculative rollback. Opaque-by-friendship —
    // produced/consumed only by LexerModeStack itself. Captures the
    // full stack contents (not just depth) because PushMode under
    // speculation may interleave with the existing stack and there's
    // no constant-time inverse for "what was here before."
    class DSS_EXPORT Snapshot {
    private:
        friend class LexerModeStack;
        std::vector<LexerModeId> frames_;
    };

    LexerModeStack() noexcept = default;

    void push(LexerModeId mode);
    void pop();                                  // no-op on empty
    void replaceTop(LexerModeId mode);           // no-op on empty
    void apply(ModeOp op, LexerModeId arg);

    [[nodiscard]] bool        empty() const noexcept { return frames_.empty(); }
    [[nodiscard]] std::size_t depth() const noexcept { return frames_.size(); }
    [[nodiscard]] LexerModeId top()   const noexcept;
    [[nodiscard]] std::span<LexerModeId const> frames() const noexcept { return frames_; }

    [[nodiscard]] Snapshot snapshot() const;
    void                   restore(Snapshot const& snap);

private:
    std::vector<LexerModeId> frames_;
};

// Metadata for a single named lexer mode. Loader fills these in;
// readers consume via GrammarSchema::lexerModes(). The per-mode tokens
// table (lexeme → meanings) lives on GrammarSchemaData keyed by id,
// not on this struct, so we can keep LexerMode itself a small POD
// without dragging LexemeMeaning's definition into this header.
struct DSS_EXPORT LexerMode {
    std::string                  name;
    LexerModeId                  id;
    // Empty = no default-token; the tokenizer falls back to its error
    // path. Present = produce this meaning whenever no lexeme entry
    // matches (used inside e.g. a `string-body` mode to scan free text).
    std::optional<SchemaTokenId> defaultToken;
};

} // namespace dss
