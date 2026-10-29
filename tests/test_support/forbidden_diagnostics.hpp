#pragma once

// THE JUDGEMENT OF `forbidDiagnostics` — the ONE home of the rule both corpus runners apply (P69). An example's
// manifest may name diagnostic codes its compile must NOT report; an absence is satisfied by nothing happening, so
// the rule is worth exactly as much as the judgement that reads it, and two runners holding a copy each could drift
// apart while both stayed green. Each runner reads what a compile SAID in its own way — the in-process runner from
// its DiagnosticReporter, the CLI-subprocess runner from the log the CLI wrote — and hands the code names here; the
// verdict is this file's.
//
// What an arm's verdict covers: EVERY compile the arm made. A `dependsOn` library the arm builds from the example's
// own sources is the example's compile as much as `main.c` is, so each prerequisite (nested ones included) is judged
// by the same list, beside the arm's own compile. A runner records its prerequisites as it builds them and its own
// compile LAST, and asks once, after its own compile succeeded: one verdict, naming every compile that said a
// forbidden code — so a prerequisite's report never hides one of the arm's own, nor the reverse.
//
// SEVERITY IS NOT READ: the key names a code that must not appear at all. (An Error has already failed the arm
// before this is asked; a Warning, an Info and a Hint are what reach it.)
//
// THE INSTRUMENT'S LIMIT, said once: each runner's CALL of this judgement — recording a prerequisite, asking at the
// arm — is the runner's wiring, and a corpus runner is never an arm's runner (it takes an example as its argument,
// so started bare its control is red). That wiring is pinned by each runner's own self-test, which the gate runs
// (`ExamplesCorpusLint.ForbidDiagnosticsIsParsedJudgedAndRedsTheArm` in-process; `runForbidDiagnosticsPin` in the
// CLI runner's `cli-surface` entry). The rule itself is pinned here, by tests/test_support/test_forbidden_diagnostics.cpp.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace dss::test_support {

// One compile an arm made, as the judgement sees it: the words a failure names it by, and every diagnostic CODE NAME
// it reported — in report order, repeats kept, at any severity.
struct CompileSaid {
    std::string              who;
    std::vector<std::string> codes;
};

// One compile that reported forbidden codes: which compile, and which of the forbidden codes (in the manifest's
// declaration order, each once however many times it was reported).
struct ForbiddenSaid {
    std::string              who;
    std::vector<std::string> codes;

    friend bool operator==(ForbiddenSaid const&, ForbiddenSaid const&) = default;
};

// Which of the `forbidden` codes are among those `said`? In declaration order, each named once.
[[nodiscard]] inline std::vector<std::string> forbiddenCodesSaid(std::vector<std::string> const& forbidden,
                                                                 std::vector<std::string> const& said) {
    std::vector<std::string> hit;
    for (auto const& name : forbidden) {
        if (std::find(said.begin(), said.end(), name) != said.end()) hit.push_back(name);
    }
    return hit;
}

// THE ARM'S VERDICT: each compile of the arm that reported a forbidden code, in the order the compiles were
// recorded (prerequisites as they were built, the arm's own last). Empty = the arm said nothing it forbade.
[[nodiscard]] inline std::vector<ForbiddenSaid> forbiddenDiagnosticsOfAnArm(std::vector<std::string> const& forbidden,
                                                                           std::vector<CompileSaid> const& compiles) {
    std::vector<ForbiddenSaid> out;
    for (auto const& compile : compiles) {
        auto hit = forbiddenCodesSaid(forbidden, compile.codes);
        if (!hit.empty()) out.push_back(ForbiddenSaid{compile.who, std::move(hit)});
    }
    return out;
}

// The verdict in words, the same in both runners: "<who> emitted A, B; <who> emitted C".
[[nodiscard]] inline std::string describeForbiddenSaid(std::vector<ForbiddenSaid> const& said) {
    std::string text;
    for (auto const& one : said) {
        if (!text.empty()) text += "; ";
        text += one.who + " emitted ";
        for (std::size_t i = 0; i < one.codes.size(); ++i) {
            if (i != 0) text += ", ";
            text += one.codes[i];
        }
    }
    return text;
}

// Every diagnostic CODE NAME a CLI log reports, at ANY severity, in log order with repeats: the
// `<severity>[<CodeName>]:` header the CLI opens every diagnostic with (`DiagnosticReporter::format`), at column 0.
// A header quoted mid-line or indented (a source excerpt, a note that mentions one) is not a report and is not read;
// nor is a bracket that holds anything but an identifier.
[[nodiscard]] inline std::vector<std::string> diagnosticCodesInCliLog(std::string_view body) {
    std::vector<std::string> codes;
    std::size_t              lineStart = 0;
    while (lineStart < body.size()) {
        std::size_t lineEnd = body.find('\n', lineStart);
        if (lineEnd == std::string_view::npos) lineEnd = body.size();
        std::string_view const line = body.substr(lineStart, lineEnd - lineStart);
        std::size_t            i    = 0;
        while (i < line.size() && line[i] >= 'a' && line[i] <= 'z') ++i;
        if (i > 0 && i < line.size() && line[i] == '[') {
            std::size_t const close = line.find("]:", i);
            if (close != std::string_view::npos && close > i + 1) {
                std::string_view const code       = line.substr(i + 1, close - i - 1);
                bool                   identifier = true;
                for (char const c : code) {
                    identifier = identifier && (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_');
                }
                if (identifier) codes.emplace_back(code);
            }
        }
        lineStart = lineEnd + 1;
    }
    return codes;
}

}  // namespace dss::test_support
