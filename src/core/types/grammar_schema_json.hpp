#pragma once

#include "core/export.hpp"
#include "core/types/grammar_schema.hpp"

#include <memory>
#include <string_view>

// INTERNAL header — never included by consumers of grammar_schema.hpp.
// Lives here so the JSON-aware load step has a forward declaration to
// hand back to grammar_schema.cpp without leaking <nlohmann/json.hpp>
// into the public include surface.

namespace dss::detail {

[[nodiscard]] LoadResult<std::shared_ptr<GrammarSchema>> buildSchemaFromJsonText(
    std::string_view jsonText,
    std::string_view sourceLabel);

} // namespace dss::detail
