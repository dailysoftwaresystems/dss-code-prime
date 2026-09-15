#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss {

// ─────────────────────────────────────────────────────────────────────────
// SPECULATIVE STATE THAT COSTS NOTHING TO REMEMBER
//
// A backtracking parser has to be able to put its state back the way it was.
// The obvious way to do that is to COPY the state at every checkpoint, and
// that is what this engine did: `TreeBuilder::checkpoint` copied the open-
// frame stack + the scope stack, and `SchemaWalker::snapshot` copied the
// cursor stack — once for the parser's walker and once more for the
// builder's. Each copy is O(depth), one copy per LIVE probe, so D nested
// probes sitting at depth ~D cost Θ(D²) bytes. ✔MEASURED end-to-end through
// the real parser on nested casts: 2048 → 332 MiB, 4096 → 1.2 GiB, 8192 →
// 4.6 GiB, 65536 → `std::bad_alloc`. That, and not any host-stack limit, is
// what held the shipped speculation ceiling two orders of magnitude below
// gcc's measured working depth.
//
// The primitives here replace the copy with the backtracking engine's
// TRAIL: leave the containers exactly as they are — contiguous, `span`-able,
// `back()`-cheap — and journal only the values a mutation SUPERSEDES.
//
// ★ THE PART THAT IS NOT OBVIOUS: A PUSH NEEDS NO RECORD.
//
// Restoring a LIFO stack to a mark M (size `S`, journal position `J`) is
//
//     resize(S);                                    // undo the net push/pop
//     for rec in journal[J..) in REVERSE order:
//         if (rec.index < S) v[rec.index] = rec.value;
//     journal.resize(J);
//
// and that is EXACT. A slot `i < S` can hold something other than its
// value-at-M only if it was popped (or written in place) after M — and the
// LAST record replayed for `i`, being the EARLIEST supersession after M,
// carries precisely the value it held at M. Slots never superseded are
// already right and are not touched. So one record per POP and one per
// IN-PLACE WRITE is the whole obligation, and a push — the overwhelmingly
// common mutation on a descent — is free.
//
// Cost, stated as a claim a measurement can refute: `mark()` is O(1);
// `rewindTo()` is O(records written since that mark); the journal grows with
// the number of frames a probe CLOSES, never with the depth it sits at, so a
// D-deep chain under one probe journals O(D) records rather than O(D²)
// bytes.
//
// ⚠ THE JOURNAL IS ARMED BY `mark()` AND DISARMED ONLY BY `discardJournal()`.
// Nothing here can know when the last mark retires — that is the owner's
// fact — so the owner MUST call `discardJournal()` at the moment no mark can
// still be rewound to, or the journal grows for the life of the parse.
// `TreeBuilder` does it when `checkpointStack_` empties; `SchemaWalker` does
// it from `Snapshot`'s destructor when the live-mark count reaches zero.
// ─────────────────────────────────────────────────────────────────────────

namespace detail {

[[noreturn]] inline void trailFatal(char const* what) noexcept {
    std::fputs("dss::speculation trail fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace detail

// A LIFO stack whose state can be restored to an earlier mark in time
// proportional to the mutations since that mark, rather than by copying it.
//
// `T` must be trivially copyable (the journal holds values rather than
// references, and every accessor returns by value — a non-trivial element
// would make an undo record's copy the very cost this type exists to avoid)
// and default-constructible (`rewindTo` re-extends the stack when a branch
// drained it below the mark).
//
// ⚠ THOSE TWO REQUIREMENTS ARE `static_assert`s IN THE MEMBER BODIES, NOT A
// `requires` CLAUSE ON THE TEMPLATE, AND THE REASON IS A REAL COMPILER
// BEHAVIOUR RATHER THAN A STYLE CHOICE. Both element types this serves are
// PRIVATE NESTED structs (`TreeBuilder::Frame`, `SchemaWalker::WalkerFrame`),
// and the stack is declared as a MEMBER of that same class — so a
// requires-clause is checked while the enclosing class is still incomplete,
// where GCC 13 reports `is_default_constructible_v` as FALSE for a perfectly
// default-constructible nested type. ✔MEASURED with a four-line repro. Member
// bodies instantiate later, at the call site in the .cpp, where the enclosing
// class is complete and the trait answers correctly.
template <class T>
class TrailedStack {
public:
    // Opaque restore token. Two integers: where the stack was, and where the
    // journal was. Taking one is the O(1) that replaces the O(depth) copy.
    struct Mark {
        std::size_t size    = 0;
        std::size_t journal = 0;
    };

    // ── reads (a drop-in for the vector this replaces) ──
    [[nodiscard]] bool        empty() const noexcept { return v_.empty(); }
    [[nodiscard]] std::size_t size()  const noexcept { return v_.size(); }
    [[nodiscard]] T operator[](std::size_t i) const noexcept { return v_[i]; }
    [[nodiscard]] T back() const noexcept {
        if (v_.empty()) detail::trailFatal("back() on an empty TrailedStack");
        return v_.back();
    }
    [[nodiscard]] std::span<T const> span() const noexcept {
        return std::span<T const>{v_};
    }
    [[nodiscard]] auto begin()  const noexcept { return v_.begin(); }
    [[nodiscard]] auto end()    const noexcept { return v_.end(); }
    [[nodiscard]] auto rbegin() const noexcept { return v_.rbegin(); }
    [[nodiscard]] auto rend()   const noexcept { return v_.rend(); }

    // ── mutations ──
    //
    // A push supersedes nothing, so it writes no record; `rewindTo`'s resize
    // is its complete undo. Every other mutation records the value it
    // displaces.
    void push(T value) {
        static_assert(std::is_trivially_copyable_v<T>,
                      "TrailedStack element must be trivially copyable — an "
                      "undo record holds a VALUE, and a non-trivial copy "
                      "there is the cost this type exists to remove");
        v_.push_back(value);
        if (v_.size() > maxSizeSeen_) maxSizeSeen_ = v_.size();
    }

    void pop() {
        if (v_.empty()) detail::trailFatal("pop() on an empty TrailedStack");
        record_(v_.size() - 1, v_.back());
        v_.pop_back();
    }

    // Shrink to `n`. Deliberately cannot grow: growing would mint
    // default-constructed elements a caller almost never means, and every
    // caller here is truncating a frame's region back to its start.
    void truncate(std::size_t n) {
        if (n > v_.size()) {
            detail::trailFatal("truncate() would grow a TrailedStack");
        }
        while (v_.size() > n) pop();
    }

    // In-place write. The one mutation shape that is NOT a push or a pop, and
    // the one a size-only checkpoint silently loses.
    void assign(std::size_t i, T value) {
        if (i >= v_.size()) {
            detail::trailFatal("assign() past the top of a TrailedStack");
        }
        record_(i, v_[i]);
        v_[i] = value;
    }
    void assignBack(T value) {
        if (v_.empty()) {
            detail::trailFatal("assignBack() on an empty TrailedStack");
        }
        assign(v_.size() - 1, value);
    }

    // Hand the underlying storage out, consuming the stack. For an owner that
    // finishes by MOVING its buffer somewhere else (the builder's child-index
    // table becomes the finished Tree's) — without it, wrapping a container in
    // this type would cost a full copy at hand-off, which is exactly the kind
    // of price that makes a correctness wrapper get skipped.
    [[nodiscard]] std::vector<T> take() && noexcept {
        journal_.clear();
        armed_ = false;
        return std::move(v_);
    }

    // ── journal ──
    [[nodiscard]] Mark mark() noexcept {
        armed_ = true;
        return Mark{v_.size(), journal_.size()};
    }

    // ★ THE RESIZE MAY GROW, AND EVERY SLOT IT ADDS IS OVERWRITTEN BELOW.
    // A branch is allowed to drain the stack BELOW the mark (the builder
    // closes a pre-checkpoint frame; the parser pops scopes it did not open),
    // so the stack can be SHORTER than the mark. Every slot in that gap was
    // popped after the mark, hence journaled, hence assigned by the replay —
    // the placeholder the resize writes is never observed. `T`'s default
    // value is required for that reason and no other.
    // ★★ `noexcept`, AND ALL THREE `rewindTo`s AGREE ON IT. A rewind that
    // stopped half way would leave the container in a state that is neither
    // the pre-branch one nor the post-branch one — some slots replayed, the
    // rest still holding the abandoned branch's values, and no caller able to
    // tell which. That is precisely the silent corruption the fail-loud rule
    // forbids, so an allocation failure inside a rewind TERMINATES instead of
    // unwinding out of a partial restore. Same call `SchemaWalker::leaveRule`
    // makes for its journaled pop, for the same reason, and stated once here
    // for the three of them. (`TrailedSet::rewindTo` already read `noexcept`
    // while `unordered_set::insert` can throw; this makes that deliberate
    // rather than an inconsistency between siblings.)
    void rewindTo(Mark m) noexcept {
        static_assert(std::is_default_constructible_v<T>,
                      "TrailedStack element must be default-constructible — "
                      "a rewind re-extends the stack when a branch drained it "
                      "below the mark");
        if (m.journal > journal_.size() || m.size > maxSizeSeen_) {
            detail::trailFatal(
                "rewindTo() with a mark this stack cannot have produced "
                "(stale mark, or a mark from another stack)");
        }
        v_.resize(m.size);
        while (journal_.size() > m.journal) {
            Record const& r = journal_.back();
            if (r.index < m.size) v_[r.index] = r.value;
            journal_.pop_back();
        }
    }

    // Retire the journal. CAPACITY IS DELIBERATELY KEPT: speculation in a
    // real grammar fires constantly and shallowly, so the same buffer is
    // re-filled thousands of times per parse and handing it back to the
    // allocator each time would trade the quadratic for allocator churn.
    void discardJournal() noexcept {
        journal_.clear();
        armed_ = false;
    }

    [[nodiscard]] bool        journalArmed() const noexcept { return armed_; }
    [[nodiscard]] std::size_t journalSize()  const noexcept { return journal_.size(); }

private:
    struct Record {
        std::size_t index;
        T           value;
    };

    void record_(std::size_t index, T value) {
        if (armed_) journal_.push_back(Record{index, value});
    }

    std::vector<T>      v_;
    std::vector<Record> journal_;
    // High-water mark of `v_.size()`, kept ONLY so `rewindTo` can refuse a
    // mark this stack could never have issued. Without it a foreign mark
    // would silently `resize` the stack UP and hand back default-constructed
    // frames — the exact silent-corruption shape the fail-loud rule exists
    // to prevent.
    std::size_t         maxSizeSeen_ = 0;
    bool                armed_       = false;
};

// A set whose membership can be restored to an earlier mark. Same discipline
// as `TrailedStack`: record only the transitions that actually changed the
// set, and replay them in reverse.
template <class K>
class TrailedSet {
public:
    using Mark = std::size_t;

    [[nodiscard]] bool        contains(K key) const noexcept { return s_.contains(key); }
    [[nodiscard]] bool        empty()         const noexcept { return s_.empty(); }
    [[nodiscard]] std::size_t size()          const noexcept { return s_.size(); }

    // Returns whether the set actually changed — a no-op insert records
    // nothing, because its undo (erase) would remove a key that was already
    // there before the mark.
    bool insert(K key) {
        if (!s_.insert(key).second) return false;
        if (armed_) journal_.push_back(Record{key, false});
        return true;
    }

    bool erase(K key) {
        if (s_.erase(key) == 0) return false;
        if (armed_) journal_.push_back(Record{key, true});
        return true;
    }

    [[nodiscard]] Mark mark() noexcept {
        armed_ = true;
        return journal_.size();
    }

    void rewindTo(Mark m) noexcept {
        if (m > journal_.size()) {
            detail::trailFatal("rewindTo() with a stale TrailedSet mark");
        }
        while (journal_.size() > m) {
            Record const& r = journal_.back();
            if (r.wasPresent) {
                s_.insert(r.key);
            } else {
                s_.erase(r.key);
            }
            journal_.pop_back();
        }
    }

    void discardJournal() noexcept {
        journal_.clear();
        armed_ = false;
    }

    [[nodiscard]] std::size_t journalSize() const noexcept { return journal_.size(); }

private:
    struct Record {
        K    key;
        bool wasPresent;   // undo = re-insert; otherwise undo = erase
    };

    std::unordered_set<K> s_;
    std::vector<Record>   journal_;
    bool                  armed_ = false;
};

// A journal of IN-PLACE writes to elements of a container this type does not
// own — the arena, whose slots are addressed by id rather than by stack
// position. `record` captures the value a write is about to displace;
// `rewindTo` hands each captured value back to the owner to re-install.
//
// ⚠ The owner is expected to record ONLY writes to slots that will SURVIVE
// its rollback (for the arena: an index below the innermost checkpoint's
// node count). A write to a slot the rollback truncates away needs no undo,
// and recording it would be a per-close copy the design is trying to avoid.
template <class Id, class V>
class TrailedWriteLog {
public:
    using Mark = std::size_t;

    void record(Id id, V const& displaced) {
        static_assert(std::is_trivially_copyable_v<V>,
                      "TrailedWriteLog value must be trivially copyable — see "
                      "TrailedStack for why the requirement lives here rather "
                      "than in a requires-clause");
        if (armed_) journal_.push_back(Record{id, displaced});
    }

    [[nodiscard]] Mark mark() noexcept {
        armed_ = true;
        return journal_.size();
    }

    // `apply(id, value)` re-installs one displaced value. Replayed in
    // reverse, so the earliest displacement after the mark wins — the same
    // argument that makes `TrailedStack::rewindTo` exact.
    //
    // `noexcept` for the reason stated over `TrailedStack::rewindTo`, and
    // that puts a NO-THROW CONTRACT on `apply`: a callback that throws mid
    // replay would leave the owner's container half-restored, which is the
    // outcome the whole journal exists to prevent, so it terminates. Same
    // discipline as `SchemaWalker::DesyncCallback`. The one production caller
    // (`TreeBuilder::rollbackToId_`) assigns into an already-sized arena slot.
    template <class Apply>
    void rewindTo(Mark m, Apply&& apply) noexcept {
        if (m > journal_.size()) {
            detail::trailFatal("rewindTo() with a stale TrailedWriteLog mark");
        }
        while (journal_.size() > m) {
            Record const& r = journal_.back();
            apply(r.id, r.value);
            journal_.pop_back();
        }
    }

    void discardJournal() noexcept {
        journal_.clear();
        armed_ = false;
    }

    [[nodiscard]] bool        armed()       const noexcept { return armed_; }
    [[nodiscard]] std::size_t journalSize() const noexcept { return journal_.size(); }

private:
    struct Record {
        Id id;
        V  value;
    };

    std::vector<Record> journal_;
    bool                armed_ = false;
};

} // namespace dss
