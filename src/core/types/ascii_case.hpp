#pragma once

#include <string>
#include <string_view>

// ASCII case-folding utilities — operator-typed CLI flags / JSON
// discriminator strings benefit from case-insensitive accept. Hoisted
// at LK10 cycle 3 post-fold #2 + D-LK6-1 post-fold #2 from byte-
// identical loops that landed in `program/cli_args.cpp::parseCompileConfig`
// and `core/types/target_schema.cpp::parseRelocFormulaKind` (the LK10
// `--config` precedent the second use cited). 2-consumer state today;
// pre-emptive 3rd-consumer hoist per "best long-term solution only"
// rule + code-simplifier post-fold #2 REQUIRED.
//
// Header-only: each consumer's call site becomes `dss::asciiToLower(s)`.
// ASCII-only: non-ASCII bytes (UTF-8 continuation bytes >= 0x80, etc.)
// pass through unchanged. Locale-independent by construction.

namespace dss {

[[nodiscard]] inline std::string asciiToLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(
            (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c));
    }
    return out;
}

} // namespace dss
