#pragma once

// ── THE VOCABULARY-PROJECTION PROBE, WITH ONE OWNER ─────────────────────────
//
// D-TEST-VOCABULARY-PROJECTION-PROBE-HELPERS-ARE-COPIED-PER-FILE.
//
// ★★★ WHY THIS HEADER EXISTS, AND WHY THE IRONY IS THE POINT RATHER THAN A
// JOKE. Every file that includes this one exists BECAUSE A SPELLING HAD TWO
// OWNERS AND THEY DRIFTED — that is the entire subject of
// D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET. The probe
// helpers those files use to prove it were themselves copied per file, and
// they had ALREADY DRIFTED when they were merged here:
//
//   * `shippedLanguageDoc` existed in TWO of the three, with DIFFERENT
//     signatures — one hard-wired `"c-subset"` and took no argument, the other
//     took the language name. The parameterised one is kept: a probe that can
//     only ever read one document cannot state which document it read.
//   * `at()` existed in the same TWO, also with different signatures, and only
//     one of them carried the `where` label that says WHICH row's pointer went
//     stale. The labelled one is kept, because "a pointer that went stale
//     asserts nothing" is only actionable if the reader learns which pointer.
//   * `findVocabularyMessage` existed in two files with DIFFERENT RETURN TYPES
//     (`std::optional<std::string>` vs `std::string const*`) — the same search,
//     one of them copying the message it found. The pointer form is kept; the
//     diagnostics outlive every use.
//   * `summarize` and `quotedTokens` were byte-identical in three files, which
//     is the state the other two were in before somebody edited one of them.
//
// ⇒ a change to how a shipped document is LOCATED, or to what counts as a
// QUOTED TOKEN, had to be made in three places or the three files would
// silently measure different things. This header is the one place.
//
// ⚠ NOT YET UNIVERSAL: `tests/core/test_vocabulary_projection_ffi_and_lir.cpp`
// carries a FOURTH copy of `quotedTokens` and does not include this header —
// reported, not silently left (see the anchor above). It was outside the lane
// that wrote this file.

#include "core/types/config_path_walk.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <fstream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dss::test_support {

// ★ A FIXTURE FAULT — never a verdict about the config. Thrown, never
// returned: a fixture fault delivered through the loader's own diagnostic
// envelope is indistinguishable from the refusal these pins exist to read.
// Dedicated type so a `catch` can name exactly this fault, exactly as
// `TargetSchemaMutationError` does for the mutation contract.
class VocabularyProbeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The shipped `.lang.json` document for `name`, parsed. Named rather than
// hard-wired: a probe that can only ever read one document cannot say which
// document it read, and the second copy of this helper had already lost that.
[[nodiscard]] inline nlohmann::json
shippedLanguageDoc(std::string_view name) {
    auto pathR = findShippedConfig(
        ShippedConfigLocator{name, "sources", ".lang.json", "language",
                             DiagnosticCode::C_InvalidLanguageName});
    if (!pathR.has_value()) {
        throw VocabularyProbeError{
            std::string{"cannot resolve shipped language document '"}
            + std::string{name} + "'"};
    }
    std::ifstream in{*pathR};
    if (!in.is_open()) {
        throw VocabularyProbeError{std::string{"cannot open "} + pathR->string()};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return nlohmann::json::parse(buf.str());
}

// The JSON-pointer guard. `where` names the ROW whose pointer is being
// resolved, so a stale pointer reports which pin went blind instead of only
// that one did.
[[nodiscard]] inline nlohmann::json&
at(nlohmann::json& doc, std::string_view pointer, std::string_view where) {
    try {
        return doc.at(nlohmann::json::json_pointer{std::string{pointer}});
    } catch (std::exception const& e) {
        throw VocabularyProbeError{
            std::string{"["} + std::string{where} + "] the JSON pointer '"
            + std::string{pointer}
            + "' does not resolve in the shipped document (" + e.what()
            + "). A row whose pointer went stale asserts NOTHING — re-aim it at "
              "the node it names."};
    }
}

// Every diagnostic, one per line, for a failure message. The pins print this
// when an expected refusal is absent, so the reader sees what WAS reported.
[[nodiscard]] inline std::string summarize(auto const& diags) {
    std::string s;
    for (auto const& d : diags) s += "\n  " + d.path + ": " + d.message;
    return s.empty() ? std::string{"<no diagnostics>"} : s;
}

// Every `'…'`-quoted token in a message. `renderAllowedList` quotes each
// spelling, so this is how a pin reads back WHAT THE MESSAGE CLAIMS — as
// opposed to re-deriving it from the table, which would make the comparison
// circular and green by construction.
[[nodiscard]] inline std::vector<std::string>
quotedTokens(std::string const& msg) {
    std::vector<std::string> out;
    std::size_t              i = 0;
    while (true) {
        auto const open = msg.find('\'', i);
        if (open == std::string::npos) break;
        auto const close = msg.find('\'', open + 1);
        if (close == std::string::npos) break;
        out.push_back(msg.substr(open + 1, close - open - 1));
        i = close + 1;
    }
    return out;
}

// Membership over a projected name set — the half of the honesty check that
// asks "is this quoted token one the vocabulary owns?".
[[nodiscard]] inline bool
namesContain(std::span<std::string_view const> names, std::string_view needle) {
    for (auto const& n : names) {
        if (n == needle) return true;
    }
    return false;
}

// (B) COMPLETENESS — SOME diagnostic in the failed load names every spelling
// the table owns, and that diagnostic is the one the honesty check then reads.
//
// ⚠ THE SEARCH IS THE ASSERTION, and it has to be, because a refused load emits
// MORE than the vocabulary refusal: a bad `encoding.format` also produces
// eleven cascade diagnostics about slots belonging to a different shape, each
// quoting real slot spellings; a bad `aggregateLayout` produces the block's own
// "required field missing" cascade. Running the honesty check over the UNION of
// every message reports those cascades as "advertised tokens" — measured on the
// first run of both callers, and it is the pin lying, not the loader. The
// vocabulary sentence is the one that names the WHOLE set.
//
// Returns a pointer INTO `diags` (never a copy — the caller's diagnostics
// outlive every use), or nullptr when no message names the whole set.
[[nodiscard]] inline std::string const*
findVocabularyMessage(auto const&                       diags,
                      std::span<std::string_view const> names) {
    for (auto const& d : diags) {
        auto const quoted = quotedTokens(d.message);
        bool       all    = true;
        for (std::string_view const n : names) {
            bool found = false;
            for (auto const& q : quoted) {
                if (q == n) { found = true; break; }
            }
            if (!found) { all = false; break; }
        }
        if (all) return &d.message;
    }
    return nullptr;
}

} // namespace dss::test_support
