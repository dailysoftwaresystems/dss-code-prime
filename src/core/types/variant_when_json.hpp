#pragma once

#include "core/export.hpp"
#include "core/types/variant_when.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

// The DECODE half of the ONE `when` selector (see `variant_when.hpp`), in the
// sanctioned `_json` shape (src/core/CMakeLists.txt): a header whose job IS to
// accept a `nlohmann::json const&`, included only by translation units that
// already hold a parsed document — the shipped-descriptor reader and the
// language-document loader.
namespace dss {

// Decode + validate ONE `when` object for the mode `axes`. Every defect is
// reported, none is skipped:
//   • `fail(body)` — once per malformed key or value, the BODY only ("'dataModel'
//     must be a string"); the caller prepends its own context and emits under its
//     own diagnostic code;
//   • `unknownKey(sentence)` — once per key outside the mode's vocabulary, with
//     the shared key check's sentence (`detail::rejectUnknownKeys`, labelled
//     `'<whenCtx>'`).
// nullopt ⇔ at least one defect was reported.
[[nodiscard]] DSS_EXPORT std::optional<WhenSpec>
decodeWhen(nlohmann::json const& when, WhenAxes axes, std::string_view whenCtx,
           std::function<void(std::string)> const& fail,
           std::function<void(std::string)> const& unknownKey);

}  // namespace dss
