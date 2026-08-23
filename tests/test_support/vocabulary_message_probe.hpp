#pragma once

// ── READING A DIAGNOSTIC BACK — ONE OWNER, AND NO JSON ───────────────────────
//
// D-TEST-VOCABULARY-PROBE-MESSAGE-HALF-IS-UNREACHABLE-AND-JSON-COUPLED.
//
// ★★★ WHY THIS HEADER IS SEPARATE FROM `tests/core/vocabulary_projection_probe.hpp`
// RATHER THAN A SECOND COPY OF IT. That header does two unrelated jobs: it
// LOCATES a shipped config document (`shippedLanguageDoc`, `at()`) and it READS
// A DIAGNOSTIC BACK (the four functions below). Only the first needs
// `nlohmann/json.hpp`, and a pin that merely reads a refusal back was being
// asked to pay for the other half — ✔MEASURED on one consumer: +0.35 s of
// compile and +245 KB of object, plus a `target_link_libraries` line the target
// did not otherwise need. That price is exactly the argument an author reaches
// for when writing the next private copy, so it is removed rather than
// documented.
//
// ★★ AND THE PLACEMENT IS THE OTHER HALF OF THE FIX, measured rather than
// chosen by taste. `dss_add_test` (tests/CMakeLists.txt) gives EVERY test
// target `${CMAKE_SOURCE_DIR}/src` and `${CMAKE_SOURCE_DIR}/tests/test_support`
// and nothing else, so `tests/core` is on NO other suite's `-I` path — which is
// precisely the reason `tests/lir/test_lir_return_pool_projection.cpp` wrote
// down for keeping its own predicate. Living here, this header is reachable by
// a bare quoted include from every test target in the tree, with no
// `target_include_directories` edit anywhere and no `../core/…` relative-parent
// include (which would be six copies of a PATH instead of six copies of a
// function).
//
// ⚠ THE MERGES RESOLVED TOWARD THE MORE INFORMATIVE FORM, as every merge in
// this class has:
//   * `quotedTokens` returned a `std::vector<std::string>` in four places and a
//     `std::set<std::string>` in `tests/link/test_weak_definition_dialect.cpp`.
//     THE VECTOR WINS: it keeps ORDER and MULTIPLICITY, and the set is derivable
//     from it while the reverse is not. ✔MEASURED that the narrowing had already
//     cost an assertion — that file's call sites read
//     `quoted.count(name) == 1`, which on a `std::set` is 0-or-1 and so asserts
//     only PRESENCE; the `== 1` an author would read as "named exactly once"
//     was unfalsifiable. Over the vector it is a real count again.
//   * The apostrophe-pairing constraint below was recorded in ONE of the copies
//     (`tests/link/test_extern_dispatch_and_block_shape_projection.cpp`) and
//     absent from the other four, including the shared one. It is kept.
//
// The config-document half stays in `tests/core/vocabulary_projection_probe.hpp`,
// which includes this file — a consumer that needs both still writes one
// include.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dss::test_support {

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
//
// ⚠ IT PAIRS APOSTROPHES, so a possessive `'` in the prose would scramble every
// token after it. That is a constraint on the MESSAGES, not a weakness here: a
// sentence advertising a quoted set is written without a possessive, and the two
// sites that had one were reworded when these pins landed.
//
// ⚠ ORDER AND MULTIPLICITY ARE PRESERVED ON PURPOSE. A caller that wants set
// semantics can build a `std::set` from the result; a caller handed a set can
// never recover "the message named this spelling twice", and one of the copies
// this header replaced had lost exactly that.
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
