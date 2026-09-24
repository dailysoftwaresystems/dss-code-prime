#pragma once

#include "core/types/strong_ids.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// ── THE CURRENT OBJECT OF A BRACE-ENCLOSED INITIALIZER LIST ─────────────────────
//
// P68 round 9 (lane `cs`). C 6.7.9p17-p20 (C23 6.7.10p17-p21): each brace-enclosed
// list has a CURRENT OBJECT, and an element initializes "the next subobject" of it
// in order, or the subobject its designation names — after which "initialization
// continues forward in order, beginning with the next subobject after that
// described by the designator". A subobject that is itself an aggregate or a union
// takes a brace-enclosed list of its own, or — when its initializer does NOT begin
// with a brace — "only enough initializers from the list are taken to account for
// [its] elements or members" and the rest continue with the enclosing aggregate
// (BRACE ELISION). A union takes its FIRST member positionally, one initializer's
// worth, and is then complete.
//
// So the position is a PATH from the list's object down to a subobject, not an
// index into the list's object — and DSS kept an index. ✔MEASURED 2026-09-23, each
// reference separately and every build RUN (the lane's `.temp/probe/r9`, `r9b`):
// gcc 13.3.0 and clang 18.1.3 at `-std=c17 -pedantic-errors` and `-std=c2x`,
// mingw-w64 13.2.0 at both and MSVC 19.51 at `/std:c17` and `/std:clatest` build and
// run every elided shape to its C placement — `int a[2][2] = {1, 2, 3, 40}`, `int
// a[][2] = {1, 2, 3, 40}` (two rows), `struct P ps[] = {1, 2, 3, 39}`, a union's
// first member elided, a string in an elided character-array position, an unnamed
// bit-field skipped, file scope and compound literals — and continue after a DEEP
// designator inside the designated aggregate (`int a[][2] = {[1][0] = 1, 41}` puts
// 41 in a[1][1]; `struct S s = {.u.i = 40, 2}` puts 2 in the member after the
// union). DSS refused the elided shapes (a verifier failure: the value met the
// aggregate slot as a scalar) and resumed after a deep designator at the next
// TOP-LEVEL slot.
//
// ★ ONE OWNER, BOTH TIERS. The semantic tier sizes an array of unknown size from
// its list (C 6.7.9p22: the largest index initialized, plus one — and with elision
// that index is reached only after the subaggregates are filled), and the HIR tier
// places every element in its slot. Both ask THIS cursor, element by element, so
// the size one tier computes is the extent the other one fills. The cursor reads no
// CST and lowers nothing: the caller resolves each element's designator to member /
// element indices and answers, per candidate subobject type, whether the element's
// VALUE initializes that subobject whole (a compatible structure or union
// expression, or a string literal for a character array — C 6.7.9p13-p14).
//
// ★ NOT RECURSIVE. The path is a vector; descending (elision) pushes, completing a
// subaggregate pops (feedback-no-input-proportional-recursion).
namespace dss::initializer_cursor {

enum class Shape : std::uint8_t { Scalar, Struct, Union, Array };

// What the cursor asks about a type. Each tier answers from its own view of the
// lattice (the HIR tier's slot types are its representation projections, the
// semantic tier's are the declared types); the SHAPE of every answer is the same.
class TypeView {
public:
    virtual ~TypeView() = default;
    [[nodiscard]] virtual Shape shape(TypeId t) const = 0;
    // A structure's or union's member count, an array's element count; nullopt for
    // an array whose length is not a constant (unknown size, variable length).
    [[nodiscard]] virtual std::optional<std::uint32_t> count(TypeId t) const = 0;
    // Member `i`'s type; an array's element type (for any `i`).
    [[nodiscard]] virtual TypeId member(TypeId t, std::uint32_t i) const = 0;
    // An UNNAMED bit-field, which positional initialization skips (C 6.7.9p9: "unnamed
    // members of objects of structure and union type do not participate in
    // initialization"). An anonymous structure or union member is not one — it is a
    // member, and a positional initializer enters it.
    [[nodiscard]] virtual bool unnamedBitField(TypeId t, std::uint32_t i) const = 0;
};

struct Placement {
    enum class Kind : std::uint8_t {
        Assign,       // the element initializes the subobject at `path`, of type `type`
        Excess,       // a positional element after the object's last subobject (C 6.7.9p2)
        OutOfRange,   // a designator that names no subobject of the object
    };
    Kind                       kind = Kind::Assign;
    std::vector<std::uint32_t> path;   // from the list's object; never empty for Assign
    TypeId                     type{};
};

class Cursor {
public:
    // `object` is the list's current object — an array (of unknown size only at the
    // top of a declaration or a compound literal), a structure or a union.
    Cursor(TypeId object, TypeView const& view) : view_(&view), object_(object) {
        restartAtFirst();
    }

    // Place ONE element. `designator` holds its resolved designation (a member or
    // element index per step; empty for a positional element). `isBraceList`: the
    // value is itself a brace-enclosed list, which initializes whatever subobject
    // the position names — scalar or not — and is never elided through.
    // `initializesWhole(t)`: whether the (non-list) value initializes an aggregate
    // or union subobject of type `t` WHOLE; asked only while eliding.
    template <class InitializesWhole>
    [[nodiscard]] Placement place(std::span<std::uint32_t const> designator,
                                  bool isBraceList, InitializesWhole&& initializesWhole) {
        std::vector<std::uint32_t> path;
        std::vector<TypeId>        containers;
        if (!designator.empty()) {
            TypeId t = object_;
            for (std::size_t d = 0; d < designator.size(); ++d) {
                std::uint32_t const i = designator[d];
                Shape const sh = view_->shape(t);
                if (sh == Shape::Scalar) return Placement{Placement::Kind::OutOfRange, {}, {}};
                std::optional<std::uint32_t> const n = view_->count(t);
                bool const unbounded = !n.has_value() && d == 0 && sh == Shape::Array;
                if (!unbounded && (!n.has_value() || i >= *n))
                    return Placement{Placement::Kind::OutOfRange, {}, {}};
                containers.push_back(t);
                path.push_back(i);
                t = view_->member(t, i);
            }
        } else {
            if (exhausted_) return Placement{Placement::Kind::Excess, {}, {}};
            path       = pos_;
            containers = containers_;
        }
        TypeId type = view_->member(containers.back(), path.back());
        if (!isBraceList) {
            // BRACE ELISION: a value that does not initialize the aggregate / union
            // subobject whole initializes its FIRST (positional) member instead, as
            // deep as it takes. An aggregate with no positional member (every member
            // an unnamed bit-field, a flexible array member) stops the descent: the
            // value then meets that subobject as it is, and the lowering judges it.
            while (isAggregate(view_->shape(type)) && !initializesWhole(type)) {
                auto const first = firstPositional(type, 0, /*isObject=*/false);
                if (!first.has_value()) break;
                containers.push_back(type);
                path.push_back(*first);
                type = view_->member(type, *first);
            }
        }
        extent_ = std::max<std::int64_t>(extent_, std::int64_t{path.front()} + 1);
        Placement out{Placement::Kind::Assign, path, type};
        pos_        = std::move(path);
        containers_ = std::move(containers);
        advance();
        return out;
    }

    // The largest top-level index any element initialized, plus one (C 6.7.9p22) —
    // the length an array of unknown size takes from this list. 0 when nothing was
    // placed.
    [[nodiscard]] std::int64_t extent() const noexcept { return extent_; }

private:
    [[nodiscard]] static bool isAggregate(Shape s) noexcept {
        return s == Shape::Struct || s == Shape::Union || s == Shape::Array;
    }

    // The first member / element at or after `from` that positional initialization
    // reaches, or nullopt when there is none. Only the list's OBJECT may be an array
    // of unknown size (it is being sized); a nested one has no positional element.
    [[nodiscard]] std::optional<std::uint32_t> firstPositional(TypeId t, std::uint32_t from,
                                                               bool isObject) const {
        Shape const sh = view_->shape(t);
        std::optional<std::uint32_t> const n = view_->count(t);
        if (sh == Shape::Array) {
            if (!n.has_value()) return isObject ? std::optional<std::uint32_t>{from} : std::nullopt;
            return from < *n ? std::optional<std::uint32_t>{from} : std::nullopt;
        }
        if (sh != Shape::Struct && sh != Shape::Union) return std::nullopt;
        if (!n.has_value()) return std::nullopt;
        for (std::uint32_t i = from; i < *n; ++i)
            if (!view_->unnamedBitField(t, i)) return i;
        return std::nullopt;
    }

    void restartAtFirst() {
        pos_.clear();
        containers_.clear();
        exhausted_ = false;
        auto const first = firstPositional(object_, 0, /*isObject=*/true);
        if (!first.has_value()) {
            exhausted_ = true;
            return;
        }
        pos_.push_back(*first);
        containers_.push_back(object_);
    }

    // Move `pos_` from the subobject just initialized to the next one positional
    // initialization reaches: the next member / element at the deepest level, or —
    // once that level is complete — the next one of the level above. A UNION is
    // complete after one member. The list's object running out makes every further
    // positional element excess.
    void advance() {
        exhausted_ = false;
        while (!pos_.empty()) {
            std::size_t const d = pos_.size() - 1;
            TypeId const container = containers_[d];
            if (view_->shape(container) != Shape::Union) {
                if (auto const next = firstPositional(container, pos_[d] + 1, d == 0)) {
                    pos_[d] = *next;
                    return;
                }
            }
            if (d == 0) {
                exhausted_ = true;
                return;
            }
            pos_.pop_back();
            containers_.pop_back();
        }
        exhausted_ = true;
    }

    TypeView const*            view_;
    TypeId                     object_;
    std::vector<std::uint32_t> pos_;          // the next positional subobject
    std::vector<TypeId>        containers_;   // containers_[d] holds pos_[d]
    bool                       exhausted_ = false;
    std::int64_t               extent_    = 0;
};

}  // namespace dss::initializer_cursor
