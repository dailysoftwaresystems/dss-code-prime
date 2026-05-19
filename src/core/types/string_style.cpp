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

} // namespace dss
