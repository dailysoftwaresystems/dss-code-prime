#include "core/types/string_style.hpp"

namespace dss {

std::string_view escapeKindName(EscapeKind k) noexcept {
    switch (k) {
        case EscapeKind::None:             return "none";
        case EscapeKind::Char:             return "char";
        case EscapeKind::DoubledDelimiter: return "doubled-delimiter";
    }
    return "none";   // unreachable; satisfies non-exhaustive compilers
}

std::string bracketInnerText(std::string_view src, std::size_t open) {
    if (open >= src.size() || src[open] != '[') return {};
    std::string out;
    // Scan from just past the opener. A `]]` is an escaped literal `]`
    // (append one, skip both); a lone `]` is the close (stop).
    for (std::size_t i = open + 1; i < src.size(); ++i) {
        if (src[i] == ']') {
            if (i + 1 < src.size() && src[i + 1] == ']') {
                out.push_back(']');
                ++i;  // consume the second `]` of the escaped pair
                continue;
            }
            return out;  // lone `]` — the close delimiter
        }
        out.push_back(src[i]);
    }
    return {};  // ran off the end with no close — malformed `[...`
}

} // namespace dss
