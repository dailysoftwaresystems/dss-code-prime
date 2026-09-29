#pragma once

// ★★★ GNU as's NUMERIC LOCAL LABELS — THE ONE RESOLVER, shared by a standalone
// `.s` (`asm_text_to_lir.cpp`) and an inline-asm template (the bundle body built
// by `asm_template_to_lir.cpp`). P68 round 8,
// D-ASM-LABELS-INSIDE-A-TEMPLATE-AND-NUMERIC-LOCAL-LABELS-REFUSED.
//
// A numeric label `N:` (N a decimal integer) may be DEFINED any number of times
// in one text. A reference `Nb` names the NEAREST definition of N BEFORE the
// reference, and `Nf` the nearest one AFTER it — 📄DOCUMENTED, GNU as manual,
// "Local Symbol Names": "`Nb` refers to the most recent previous definition of
// that label" and "`Nf` refers to the next definition of a local label".
// ⇒ a reference is resolved by POSITION, never by name: two definitions of `1:`
// are two labels, and which one `1b` means depends on where `1b` is written.
//
// ★ WHY ONE RESOLVER. The `.s` walker and the template host each own a label
// TABLE (a `.s` label becomes a function block, a template label a block of the
// statement's own body), but "which definition does this reference mean" has
// one answer, and two copies of the nearest-before / nearest-after rule would
// be two chances to disagree about a `9f` that crosses an unrelated `9:`. The
// tables are the callers'; the rule is here.
//
// ★ NO INPUT-PROPORTIONAL SCAN PER REFERENCE. Definitions are grouped by number,
// each group in text order, and a reference is one binary search in its group —
// a `.s` with thousands of `1:` loops costs O(log n) per `1b`, not O(n).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dss::asm_local_labels {

enum class Direction : std::uint8_t { Backward, Forward };

// A reference as its spelling states it: the label number and the direction
// its suffix selects.
struct Reference {
    std::uint64_t number    = 0;
    Direction     direction = Direction::Backward;
};

// The dialect's two reference suffixes — DATA, from the dialect document, never
// spelled here. gas writes `b` and `f`; nothing in this header knows that.
struct Suffixes {
    std::string_view backward;
    std::string_view forward;
};

// Decode a reference spelling (`1b`, `9f`, `0b`) with the dialect's suffixes:
// one or more decimal digits followed by EXACTLY one of the two suffixes.
// nullopt for anything else — including `0b101`, which a dialect's number
// grammar reads as a binary literal before it could ever reach here.
[[nodiscard]] inline std::optional<Reference>
parseReference(std::string_view spelling, Suffixes const& sfx) noexcept {
    auto const tryOne = [&](std::string_view suffix,
                            Direction dir) -> std::optional<Reference> {
        if (suffix.empty() || spelling.size() <= suffix.size()) return std::nullopt;
        if (!spelling.ends_with(suffix)) return std::nullopt;
        std::string_view const digits =
            spelling.substr(0, spelling.size() - suffix.size());
        std::uint64_t n = 0;
        for (char const c : digits) {
            if (c < '0' || c > '9') return std::nullopt;
            std::uint64_t const d = static_cast<std::uint64_t>(c - '0');
            if (n > (UINT64_MAX - d) / 10) return std::nullopt;   // no silent wrap
            n = n * 10 + d;
        }
        return Reference{n, dir};
    };
    if (auto r = tryOne(sfx.backward, Direction::Backward)) return r;
    return tryOne(sfx.forward, Direction::Forward);
}

// Parse a DEFINITION's number (`1` in `1:`): decimal digits only.
[[nodiscard]] inline std::optional<std::uint64_t>
parseDefinitionNumber(std::string_view spelling) noexcept {
    if (spelling.empty()) return std::nullopt;
    std::uint64_t n = 0;
    for (char const c : spelling) {
        if (c < '0' || c > '9') return std::nullopt;
        std::uint64_t const d = static_cast<std::uint64_t>(c - '0');
        if (n > (UINT64_MAX - d) / 10) return std::nullopt;
        n = n * 10 + d;
    }
    return n;
}

// Every numeric-label definition of ONE text, and the one question asked of
// them. `Handle` is the caller's own name for a definition (an index into its
// label table); this class only ever hands back what it was given.
template <class Handle>
class Table {
public:
    // Record a definition. ⚠ Definitions MUST arrive in TEXT ORDER — both
    // callers pre-scan their text front to back — and a position that went
    // BACKWARDS is refused (returns false) rather than silently mis-sorted.
    [[nodiscard]] bool define(std::uint64_t number, std::uint32_t position,
                              Handle handle) {
        auto& group = byNumber_[number];
        if (!group.empty() && group.back().position >= position) return false;
        group.push_back(Entry{position, handle});
        return true;
    }

    // The definition `ref`, written at `position`, names — or nullopt when
    // there is none on that side (a `1b` before any `1:`, a `1f` after the last).
    [[nodiscard]] std::optional<Handle> resolve(Reference ref,
                                                std::uint32_t position) const {
        auto const it = byNumber_.find(ref.number);
        if (it == byNumber_.end()) return std::nullopt;
        auto const& group = it->second;
        auto const  after = std::upper_bound(
            group.begin(), group.end(), position,
            [](std::uint32_t p, Entry const& e) { return p < e.position; });
        if (ref.direction == Direction::Forward) {
            if (after == group.end()) return std::nullopt;
            return after->handle;
        }
        // Backward: the last definition strictly BEFORE `position`. A
        // definition AT the reference's own position cannot exist (a definition
        // and a reference are different tokens), so `upper_bound` is exact.
        if (after == group.begin()) return std::nullopt;
        return std::prev(after)->handle;
    }

    [[nodiscard]] bool empty() const noexcept { return byNumber_.empty(); }

private:
    struct Entry {
        std::uint32_t position = 0;
        Handle        handle{};
    };
    std::unordered_map<std::uint64_t, std::vector<Entry>> byNumber_;
};

} // namespace dss::asm_local_labels
