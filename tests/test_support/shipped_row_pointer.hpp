#pragma once

// ══ A TEST NAMES A ROW OF A SHIPPED DOCUMENT BY ITS KEY, NEVER BY ITS POSITION ══
// (P69 L2b2, lane `lm`)
//
// A pin that probes a shipped document at `/semantics/declarations/7/entryFunctions`
// means "the row of the `topLevelDecl` rule", and says "the eighth row". The day a row
// lands before it the pointer names ANOTHER row: the probe then fails on a key that row
// never had, or — worse — passes on a row it was never about. ✔MEASURED 2026-11-05 (the
// Mac leg, host run 20261105-194136-d2e3e46a): two `declarations` rows added beside
// their twins moved `forDecl` from 2 to 3 and `topLevelDecl` from 7 to 9, and three
// pins in two files went red on a pointer that had silently changed meaning; the
// assembly table's `operandSelectors` row had been re-aimed by hand twice before
// (40 → 42 → 43).
//
// So a pointer TEMPLATE may name an array element by a SELECTOR segment, resolved
// against the document the test reads, at test time:
//   `{member=value}`  the ONE element whose `member` is the string `value`
//                     (a declarations row by its `rule`); none, or more than one, THROWS;
//   `{member}`        the FIRST element that declares `member` — for a probe that needs
//                     "a row carrying this key", not one row's identity; none THROWS;
//   `{member*}`       EVERY element that declares `member` (only through
//                     `resolveRowPointers`); none THROWS.
// Every other segment is a JSON-pointer token, kept as written. A THROW (never an
// abort) fails the one calling test with the template and the reason.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dss::test_support {

struct ResolvedRowPointer {
    std::string              pointer;    // a plain JSON pointer
    std::vector<std::size_t> selected;   // the index each selector chose, in order
};

namespace row_pointer_detail {

[[noreturn]] inline void refuse(std::string_view tmpl, std::string const& why) {
    throw std::runtime_error("row pointer '" + std::string{tmpl} + "': " + why);
}

[[nodiscard]] inline nlohmann::json const& nodeAt(nlohmann::json const& doc,
                                                  std::string const& pointer,
                                                  std::string_view tmpl) {
    try {
        return doc.at(nlohmann::json::json_pointer{pointer});
    } catch (std::exception const& e) {
        refuse(tmpl, "'" + pointer + "' does not resolve in the document (" + e.what() + ")");
    }
}

}  // namespace row_pointer_detail

// Every pointer the template names: one, unless a `{member*}` segment fans it out.
[[nodiscard]] inline std::vector<ResolvedRowPointer>
resolveRowPointers(nlohmann::json const& doc, std::string_view tmpl) {
    using namespace row_pointer_detail;
    // The empty pointer is the WHOLE document (RFC 6901 §5), and a probe may name it.
    if (tmpl.empty()) return {ResolvedRowPointer{}};
    if (tmpl.front() != '/') refuse(tmpl, "a template starts with '/'");
    std::vector<ResolvedRowPointer> done{ResolvedRowPointer{}};
    std::size_t at = 1;
    while (at <= tmpl.size()) {
        std::size_t const end = std::min(tmpl.find('/', at), tmpl.size());
        std::string_view const seg = tmpl.substr(at, end - at);
        at = end + 1;
        bool const selector = seg.size() >= 2 && seg.front() == '{' && seg.back() == '}';
        if (!selector) {
            for (auto& r : done) r.pointer += "/" + std::string{seg};
            continue;
        }
        std::string_view const body = seg.substr(1, seg.size() - 2);
        std::size_t const eq = body.find('=');
        bool const every = eq == std::string_view::npos && !body.empty() && body.back() == '*';
        std::string const member{eq != std::string_view::npos ? body.substr(0, eq)
                                 : every                      ? body.substr(0, body.size() - 1)
                                                              : body};
        if (member.empty()) refuse(tmpl, "an empty selector '" + std::string{seg} + "'");
        std::vector<ResolvedRowPointer> next;
        for (auto const& r : done) {
            nlohmann::json const& arr = nodeAt(doc, r.pointer, tmpl);
            if (!arr.is_array()) {
                refuse(tmpl, "'" + r.pointer + "' is a " + arr.type_name()
                                 + ", and a selector picks an element of an ARRAY");
            }
            std::vector<std::size_t> hits;
            for (std::size_t i = 0; i < arr.size(); ++i) {
                auto const& e = arr[i];
                if (!e.is_object() || !e.contains(member)) continue;
                if (eq != std::string_view::npos) {
                    auto const& v = e.at(member);
                    if (!v.is_string() || v.get<std::string>() != body.substr(eq + 1)) continue;
                }
                hits.push_back(i);
                if (eq == std::string_view::npos && !every) break;   // `{member}`: the first
            }
            if (hits.empty()) {
                refuse(tmpl, "no element of '" + r.pointer + "' matches '" + std::string{seg}
                                 + "' — the row was renamed or removed, so the pin is aimed at nothing");
            }
            if (eq != std::string_view::npos && hits.size() > 1) {
                refuse(tmpl, std::to_string(hits.size()) + " elements of '" + r.pointer
                                 + "' match '" + std::string{seg}
                                 + "' — a key selector names ONE row, and two would be probed as one");
            }
            for (std::size_t const i : hits) {
                ResolvedRowPointer n = r;
                n.pointer += "/" + std::to_string(i);
                n.selected.push_back(i);
                next.push_back(std::move(n));
            }
        }
        done = std::move(next);
    }
    for (auto const& r : done) (void)nodeAt(doc, r.pointer, tmpl);   // the whole pointer resolves
    return done;
}

// The ONE pointer a template names; a fan-out THROWS.
[[nodiscard]] inline ResolvedRowPointer resolveRowPointer(nlohmann::json const& doc,
                                                          std::string_view tmpl) {
    auto all = resolveRowPointers(doc, tmpl);
    if (all.size() != 1) {
        row_pointer_detail::refuse(tmpl, "names " + std::to_string(all.size())
                                             + " pointers where one was asked for");
    }
    return std::move(all.front());
}

// `text` with every `{index}` replaced by the index the LAST selector chose — a loader's
// own spelling of the row (`'declarations[N]'`) follows the row, never a typed number.
[[nodiscard]] inline std::string withSelectedIndex(std::string_view text,
                                                   ResolvedRowPointer const& r) {
    std::string out{text};
    constexpr std::string_view kSlot = "{index}";
    for (std::size_t p = out.find(kSlot); p != std::string::npos; p = out.find(kSlot, p)) {
        if (r.selected.empty()) {
            throw std::runtime_error("'" + std::string{text}
                                     + "' names {index}, and its pointer chose no row");
        }
        std::string const n = std::to_string(r.selected.back());
        out.replace(p, kSlot.size(), n);
        p += n.size();
    }
    return out;
}

}  // namespace dss::test_support
