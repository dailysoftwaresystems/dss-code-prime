#pragma once

#include <string_view>

namespace dss {

// Strip the leading + trailing `__` of a `__name__` dunder spelling
// (`__deprecated__` -> `deprecated`, `__packed__` -> `packed`). Shared by BOTH:
//   * the C preprocessor's `__has_c_attribute` lookup (C 6.10.1: the lookup
//     ignores a surrounding double underscore, so `__deprecated__` matches a
//     declared `deprecated`); and
//   * the composite type-attribute scan (D-CSUBSET-PACKED: an
//     `__attribute__((__packed__))` spelling normalizes to `packed`).
// A name not in `__x__` form is returned unchanged. ONE normalizer so the two
// sites can never drift apart. Pure / no allocation (the result is a view into
// `name`), `constexpr` so it folds at compile time where the input is constant.
[[nodiscard]] constexpr std::string_view stripDunder(std::string_view name) {
    if (name.size() >= 4 && name.substr(0, 2) == "__"
        && name.substr(name.size() - 2) == "__") {
        return name.substr(2, name.size() - 4);
    }
    return name;
}

} // namespace dss
