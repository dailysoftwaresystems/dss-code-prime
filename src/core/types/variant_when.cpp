#include "core/types/variant_when_json.hpp"

#include "core/types/config_key_vocabulary.hpp"   // detail::rejectUnknownKeys, detail::renderAllowedList
#include "core/types/data_model.hpp"              // dataModelFromName, longDoubleFormatFromName, their tables
#include "core/types/enum_name_table.hpp"         // allNames

#include <array>

namespace dss {

namespace {

// The legal keys per mode. `FormatOnly` admits `format` alone; the typed modes admit every axis a pair has.
constexpr std::array<std::string_view, 1> kFormatOnlyKeys{"format"};
constexpr std::array<std::string_view, 4> kTypedKeys{"arch", "format", "dataModel", "longDoubleFormat"};

// The house list shape of the shipped-descriptor messages this decoder inherited ("'a'/'b'/'c'"): the reader's
// refusals were pinned in that shape before the evaluator moved here, and a moved evaluator must not rewrite them.
template <typename Names>
[[nodiscard]] std::string slashList(Names const& names) {
    return detail::renderAllowedList(names, "/");
}

}  // namespace

std::optional<WhenSpec>
decodeWhen(nlohmann::json const& when, WhenAxes axes, std::string_view whenCtx,
           std::function<void(std::string)> const& fail,
           std::function<void(std::string)> const& unknownKey) {
    if (!when.is_object()) {
        fail("'when' must be an object");
        return std::nullopt;
    }
    bool const typedKeysLegal = (axes != WhenAxes::FormatOnly);
    bool ok = true;
    auto const onUnknown = [&](std::string_view, std::string message) {
        ok = false;
        unknownKey(std::move(message));
    };
    std::string const label = "'" + std::string{whenCtx} + "'";
    if (typedKeysLegal) detail::rejectUnknownKeys(when, kTypedKeys, label, onUnknown);
    else                detail::rejectUnknownKeys(when, kFormatOnlyKeys, label, onUnknown);
    if (!ok) return std::nullopt;

    // AN EMPTY `when` IS REFUSED (D-FFI-DESCRIPTOR-MACRO-VARIANT-COVERAGE-AND-ARITY-UNCHECKED limb (c)): with no key
    // present nothing below runs and the arm would match EVERY pair — including one with no active format, which
    // the contract says can never select anything. The flat form is the target-invariant spelling.
    if (when.empty()) {
        fail("'when' must name at least one selector (an empty 'when' would match EVERY target, including one with "
             "no active format — use the flat, non-variant form for a target-invariant entry)");
        return std::nullopt;
    }

    WhenSpec spec;
    if (typedKeysLegal && when.contains("dataModel")) {
        auto const& v = when.at("dataModel");
        if (!v.is_string()) {
            fail("'dataModel' must be a string");
            ok = false;
        } else {
            std::string const want = v.get<std::string>();
            // CLOSED vocabulary — a typo would otherwise silently never match, and the entry vanish on every pair.
            if (!dataModelFromName(want).has_value()) {
                fail("'dataModel' has unknown data-model name '" + want + "' (expected "
                     + slashList(allNames(kDataModelTable)) + ")");
                ok = false;
            } else {
                spec.dataModel = want;
            }
        }
    }
    if (typedKeysLegal && when.contains("longDoubleFormat")) {
        auto const& v = when.at("longDoubleFormat");
        if (!v.is_string()) {
            fail("'longDoubleFormat' must be a string");
            ok = false;
        } else {
            std::string const want = v.get<std::string>();
            // CLOSED vocabulary, the format documents' own `longDoubleFormat` spellings. `None` is not a row of the
            // table (an undeclared axis is not spellable), so no arm can be written for "no long double".
            if (!longDoubleFormatFromName(want).has_value()) {
                fail("'longDoubleFormat' has unknown long-double format name '" + want + "' (expected "
                     + slashList(allNames(kLongDoubleFormatTable)) + ")");
                ok = false;
            } else {
                spec.longDoubleFormat = want;
            }
        }
    }
    if (typedKeysLegal && when.contains("arch")) {
        auto const& v = when.at("arch");
        if (!v.is_string()) {
            fail("'arch' must be a string");
            ok = false;
        } else {
            // OPEN vocabulary (the target schemas, which no `when` reader loads): an unknown arch never matches.
            spec.arch = v.get<std::string>();
        }
    }
    if (when.contains("format")) {
        auto const& v = when.at("format");
        if (!v.is_string()) {
            fail("'format' must be a string");
            ok = false;
        } else {
            std::string const want = v.get<std::string>();
            auto const kind = objectFormatKindFromName(want);
            if (!kind.has_value()) {
                fail("'format' has unknown object-format name '" + want + "' (expected "
                     + slashList(kSelectableObjectFormatKindNames) + ")");
                ok = false;
            } else if (!isSelectableObjectFormatKind(*kind)) {
                // The `unknown` SENTINEL spells correctly, so the lookup accepts it, and then it matches no real
                // pair: the entry would vanish on every target, exactly as a typo would. Same defect, same verdict.
                fail("'format' names the invalid sentinel — " + std::string{kObjectFormatKindSentinelRejection});
                ok = false;
            } else {
                spec.format = *kind;
            }
        }
    }
    if (!ok) return std::nullopt;
    return spec;
}

bool whenMatches(WhenSpec const& spec, WhenAxes axes, WhenFacts const& facts) noexcept {
    // LEGALITY AND PARTICIPATION ARE SEPARATE: `FormatReachability` validated every typed key at decode and lets
    // only `format` decide here, which is what makes it a strictly WEAKER test than `FullTarget`.
    bool const typedKeysParticipate = (axes == WhenAxes::FullTarget);
    if (typedKeysParticipate) {
        if (spec.dataModel.has_value() && facts.dataModelName != *spec.dataModel) return false;
        if (spec.longDoubleFormat.has_value()
            && (!facts.longDoubleFormatName.has_value() || *facts.longDoubleFormatName != *spec.longDoubleFormat))
            return false;
        if (spec.arch.has_value() && (!facts.arch.has_value() || *facts.arch != *spec.arch)) return false;
    }
    if (spec.format.has_value() && (!facts.format.has_value() || *facts.format != *spec.format)) return false;
    return true;
}

}  // namespace dss
