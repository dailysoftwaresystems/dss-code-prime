#pragma once

#include "core/export.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/schema_cursor.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace dss {

// Shared state machine for schema-cursor navigation. Embedded by
// TreeBuilder and (later) the parser so both drive identical dispatch
// — drift between them surfaces as `P_SchemaCursorDesync` or
// mis-shaped diagnostics in E2E.
//
// Owns:
//   - current cursor + saved parent-cursor stack (the per-rule
//     position graph state machine)
//   - one-shot desync latch (so the P_SchemaCursorDesync diagnostic
//     fires at most once per walker instance)
//
// Does NOT own:
//   - frame stack (consumer's responsibility — the cursor stack
//     mirrors the consumer's frames 1:1, but the walker treats them
//     as opaque)
//   - diagnostic emission (consumer registers a callback that fires
//     exactly once on the first valid→invalid cursor transition)
//
// Movable, not copyable. A walker is single-instance per consumer;
// duplication would invalidate the lock-step invariant.
class DSS_EXPORT SchemaWalker {
public:
    // Fires exactly once per walker instance — on the first
    // valid→invalid cursor transition. Consumer turns this into a
    // diagnostic (TreeBuilder emits P_SchemaCursorDesync info-severity).
    //
    // Contract: the callback MUST NOT throw. The walker invokes it
    // from `noexcept` sites; an escaping exception is a contract
    // violation and the walker fatal-aborts — matching the
    // discipline applied to other walker-contract violations
    // (`enterRule(InvalidRule)`, `leaveRule` underflow, cross-walker
    // `restore`). Production callers (TreeBuilder) wire a pure
    // diagnostic emission that cannot throw.
    using DesyncCallback =
        std::function<void(SourceSpan span, std::optional<RuleId> rule)>;

    explicit SchemaWalker(std::shared_ptr<GrammarSchema const> schema,
                          DesyncCallback onDesync = {}) noexcept;

    SchemaWalker(SchemaWalker const&)            = delete;
    SchemaWalker& operator=(SchemaWalker const&) = delete;
    SchemaWalker(SchemaWalker&&) noexcept;
    SchemaWalker& operator=(SchemaWalker&&) noexcept;
    ~SchemaWalker() = default;

    // ── navigation ──
    //
    // Routes the saved parent through `routeToRuleLeaf` so a later
    // `leaveRule` resumes on a `RuleLeaf` slot rather than the
    // `AltChoice` we entered from. **Precondition**: `rule.valid()`
    // — entering an unregistered rule yields downstream silent-state
    // corruption, so the walker fatal-aborts on this contract
    // violation per the project's fail-loud discipline.
    void enterRule(RuleId rule);

    // Pop the saved parent cursor and resume by calling
    // GrammarSchema::leaveRule on it. **Precondition**: parent stack
    // non-empty — underflow is a consumer-side frame-balance bug; the
    // walker fatal-aborts rather than silently masking it. `noexcept`
    // because every successful path is non-throwing (vector pop +
    // schema's `noexcept` methods + the `noexcept` desync callback
    // dispatch); the failure path is `[[noreturn]]`.
    void leaveRule(SourceSpan span, std::optional<RuleId> rule) noexcept;

    // Advance the current cursor by `tok`. Returns whether the cursor
    // is still valid AFTER the advance; consumers may use this to
    // skip expected-set lookups on the slow path. `span` + `rule`
    // populate the desync diagnostic if one fires. `noexcept` for the
    // same reason as `leaveRule`.
    bool advance(SchemaTokenId tok, SourceSpan span,
                 std::optional<RuleId> rule) noexcept;

    // ── introspection ──
    [[nodiscard]] SchemaCursor                  cursor()         const noexcept { return cursor_; }
    [[nodiscard]] SlotKind                      slotKind()       const noexcept;
    [[nodiscard]] std::span<SchemaTokenId const> expectedSet()   const noexcept;
    [[nodiscard]] bool                          isSpeculativeAlt() const noexcept;
    [[nodiscard]] std::uint16_t                 lookahead()      const noexcept;
    [[nodiscard]] bool                          isAtEndOfRule()  const noexcept;
    [[nodiscard]] bool                          canEndSource()   const noexcept;
    [[nodiscard]] RuleId                        slotRuleRef()    const noexcept;
    [[nodiscard]] std::size_t                   depth()          const noexcept { return cursorStack_.size(); }
    [[nodiscard]] bool                          isDesynced()     const noexcept { return cursorDesynced_; }

    // ── speculation (for TreeBuilder::Checkpoint + future Parser
    //                 checkpointing) ──
    //
    // Opaque snapshot of the walker's full state. Constructible only
    // by `SchemaWalker::snapshot()`; restorable only via a walker
    // bound to the same schema (cross-walker restore would index a
    // different schema's position table and silently corrupt —
    // guarded by a schema pointer-identity check on restore).
    //
    // Includes the one-shot desync latch, so a speculative branch
    // that tripped desync can be rolled back and the post-rollback
    // emission of legitimate desync events still fires.
    //
    // Default construction is deleted so a stray uninitialized
    // Snapshot cannot reach `restore()` — every Snapshot must
    // originate from a live `snapshot()` call.
    class DSS_EXPORT Snapshot {
    public:
        Snapshot()                                    = delete;
        Snapshot(Snapshot const&)                     = delete;
        Snapshot& operator=(Snapshot const&)          = delete;
        Snapshot(Snapshot&&) noexcept                 = default;
        Snapshot& operator=(Snapshot&&) noexcept      = default;
        ~Snapshot()                                   = default;

    private:
        friend class SchemaWalker;
        // schemaPtr is a raw observer pointer used ONLY for equality
        // comparison in `restore()`; it is never dereferenced. The
        // pointer's referent (the GrammarSchema) is held by the
        // walker via shared_ptr, so the Snapshot's lifetime is
        // inside the walker's schema-ownership window in all
        // legitimate uses.
        Snapshot(GrammarSchema const*       schemaPtr,
                 SchemaCursor               cursor,
                 std::vector<SchemaCursor>  cursorStack,
                 bool                       cursorDesynced) noexcept;

        GrammarSchema const*       schemaPtr_;
        SchemaCursor               cursor_;
        std::vector<SchemaCursor>  cursorStack_;
        bool                       cursorDesynced_;
    };

    [[nodiscard]] Snapshot snapshot() const;
    void                   restore(Snapshot snap);

private:
    // Latch: emit on the first valid→invalid edge only; snapshot/
    // restore re-arms via the stored `cursorDesynced` field, so a
    // speculative branch that tripped desync rolls back and the post-
    // rollback walk can fire legitimate desync events again.
    void noteDesync_(bool wasValid, bool nowValid,
                     SourceSpan span, std::optional<RuleId> rule) noexcept;

    std::shared_ptr<GrammarSchema const> schema_;
    DesyncCallback                       onDesync_;
    SchemaCursor                         cursor_{};
    std::vector<SchemaCursor>            cursorStack_;
    bool                                 cursorDesynced_ = false;
};

} // namespace dss
