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
//     signatures — one hard-wired `"c"` and took no argument, the other
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
// ✔ THE FOURTH COPY IS ROUTED (2026-08-23, cycle P28,
// D-TEST-VOCABULARY-PROBE-HELPER-FOURTH-COPY-OUTSIDE-THE-EXTRACTED-HEADER).
// `tests/core/test_vocabulary_projection_ffi_and_lir.cpp` carried a `quotedTokens`
// that was BYTE-IDENTICAL to this one, so the mutant that closed the parent row
// reddened 3 of 4 and that file stayed green over a helper that no longer worked.
// It includes this header now and the same mutant reds 4 of 4.
//
// ✔ AND THE CLASS IS NOW UNIVERSAL (2026-08-23, cycle P28,
// D-TEST-VOCABULARY-PROBE-MESSAGE-HALF-IS-UNREACHABLE-AND-JSON-COUPLED), by
// SPLITTING rather than by relocating. This file kept two unrelated jobs — it
// LOCATES a shipped config document, and it READS A DIAGNOSTIC BACK — and only
// the first needs `nlohmann/json.hpp`. Two measured obstacles, neither of them
// reluctance, kept four further copies of `quotedTokens` alive outside it:
//   * REACHABILITY. This header is found only by a same-directory quoted
//     include; `tests/core` is on NO other test target's `-I` path, which
//     `tests/lir/test_lir_return_pool_projection.cpp` had written down in its own
//     prose as the reason it kept a private predicate.
//   * DEPENDENCY SHAPE. `shippedLanguageDoc` and `at()` need `nlohmann/json.hpp`,
//     so including this header dragged that dependency into TUs that never touch
//     a JSON document — ✔MEASURED on the FFI/LIR consumer: +0.35 s of compile
//     time and +245 KB of object, plus a `target_link_libraries` line the target
//     did not otherwise need.
// ⇒ the message-reading half now lives in `tests/test_support/vocabulary_message_probe.hpp`,
// which is json-free and sits in the ONE directory `dss_add_test` puts on every
// test target's include path. This header includes it, so a consumer that needs
// both halves still writes one include and every name stays in
// `dss::test_support` — no call site changed spelling.

#include "vocabulary_message_probe.hpp"

#include "core/types/config_path_walk.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

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

// ★ `summarize`, `quotedTokens`, `namesContain` and `findVocabularyMessage` are
// NOT missing — they moved to `tests/test_support/vocabulary_message_probe.hpp`,
// included above, and are still `dss::test_support::…`. They read a diagnostic
// back and need no JSON; keeping them here made every such pin an nlohmann
// consumer, which is what kept four private copies of `quotedTokens` alive in
// suites this directory is not on the include path of.

} // namespace dss::test_support
