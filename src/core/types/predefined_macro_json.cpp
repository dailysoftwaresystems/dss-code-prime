#include "core/types/predefined_macro_json.hpp"

#include "core/types/object_format_kind.hpp"

#include <algorithm>
#include <format>

namespace dss::detail {

using json = nlohmann::json;

void parsePredefinedMacroArray(nlohmann::json const&           pms,
                               std::string_view                arrayPath,
                               DiagnosticCode                  entryCode,
                               substrate::DiagnosticCollector& coll,
                               std::vector<PredefinedMacroDef>& out) {
    for (std::size_t mi = 0; mi < pms.size(); ++mi) {
        const auto  mpath = std::format("{}/{}", arrayPath, mi);
        json const& e     = pms[mi];
        if (!e.is_object()) {
            coll.emit(entryCode, mpath,
                      "a 'predefinedMacros' entry must be an object");
            continue;
        }
        PredefinedMacroDef pm;
        // `name` -- REQUIRED, non-empty string.
        if (!e.contains("name")) {
            coll.emit(DiagnosticCode::C_MissingField, mpath + "/name",
                      "a 'predefinedMacros' entry requires 'name'");
            continue;
        }
        if (!e.at("name").is_string()) {
            coll.emit(entryCode, mpath + "/name",
                      "'predefinedMacros.name' must be a string");
            continue;
        }
        pm.name = e.at("name").get<std::string>();
        if (pm.name.empty()) {
            coll.emit(DiagnosticCode::C_MissingField, mpath + "/name",
                      "'predefinedMacros.name' must be non-empty");
            continue;
        }
        // `kind` -- REQUIRED, one of the CLOSED verb set.
        if (!e.contains("kind")) {
            coll.emit(DiagnosticCode::C_MissingField, mpath + "/kind",
                      "a 'predefinedMacros' entry requires 'kind'");
            continue;
        }
        if (!e.at("kind").is_string()) {
            coll.emit(entryCode, mpath + "/kind",
                      "'predefinedMacros.kind' must be a string");
            continue;
        }
        const std::string kind       = e.at("kind").get<std::string>();
        bool              isConstant = false;
        if (kind == "line") {
            pm.kind = PredefinedMacroKind::Line;
        } else if (kind == "file") {
            pm.kind = PredefinedMacroKind::File;
        } else if (kind == "constant") {
            pm.kind    = PredefinedMacroKind::Constant;
            isConstant = true;
        } else if (kind == "date") {
            pm.kind = PredefinedMacroKind::Date;
        } else if (kind == "time") {
            pm.kind = PredefinedMacroKind::Time;
        } else {
            coll.emit(entryCode, mpath + "/kind",
                      std::format("unknown predefined-macro kind '{}' "
                                  "(expected line/file/constant/date/time)",
                                  kind));
            continue;
        }
        // `value` -- REQUIRED iff kind==constant; the static replacement
        // spelling. Ignored for the derived kinds.
        if (isConstant) {
            if (!e.contains("value")) {
                coll.emit(DiagnosticCode::C_MissingField, mpath + "/value",
                          "a 'constant' predefinedMacros entry requires 'value'");
                continue;
            }
            if (!e.at("value").is_string()) {
                coll.emit(entryCode, mpath + "/value",
                          "'predefinedMacros.value' must be a string");
                continue;
            }
            pm.value = e.at("value").get<std::string>();
        }
        // c105 (D-PP-FUNCTION-LIKE-PREDEFINE): OPTIONAL `params` — a
        // FUNCTION-LIKE predefine (e.g. the MSVC-profile `__declspec(x)` →
        // empty erase). Constant-kind only (the derived kinds are inherently
        // object-like). Each param must be a non-empty unique string
        // (C 6.10.3p6 duplicate-param parity with the directive handler,
        // enforced HERE so a config typo fails at load).
        if (e.contains("params")) {
            if (!isConstant) {
                coll.emit(entryCode, mpath + "/params",
                          "'params' is valid only on a 'constant' "
                          "predefinedMacros entry");
                continue;
            }
            json const& prs = e.at("params");
            if (!prs.is_array()) {
                coll.emit(entryCode, mpath + "/params",
                          "'predefinedMacros.params' must be an array of "
                          "parameter-name strings");
                continue;
            }
            bool prOk = true;
            for (std::size_t pi = 0; pi < prs.size(); ++pi) {
                if (!prs[pi].is_string() || prs[pi].get<std::string>().empty()) {
                    coll.emit(entryCode, mpath + "/params",
                              "each 'params' entry must be a non-empty string");
                    prOk = false;
                    break;
                }
                std::string p = prs[pi].get<std::string>();
                // c105 audit L2: each param must BE an identifier
                // ([A-Za-z_][A-Za-z0-9_]*) — a config `"a b"` would otherwise
                // emit a malformed prologue #define that fails only at first
                // preprocess, not at load.
                bool idOk = !(p[0] >= '0' && p[0] <= '9');
                for (char const c : p) {
                    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                          || (c >= '0' && c <= '9') || c == '_')) {
                        idOk = false;
                        break;
                    }
                }
                if (!idOk) {
                    coll.emit(entryCode, mpath + "/params",
                              std::format("macro parameter '{}' is not an "
                                          "identifier",
                                          p));
                    prOk = false;
                    break;
                }
                if (std::find(pm.params.begin(), pm.params.end(), p)
                    != pm.params.end()) {
                    coll.emit(entryCode, mpath + "/params",
                              std::format("duplicate macro parameter '{}'", p));
                    prOk = false;
                    break;
                }
                pm.params.push_back(std::move(p));
            }
            if (!prOk) continue;
            pm.isFunctionLike = true;
        }
        // OPTIONAL `availableObjectFormats` — a per-format availability filter
        // (mirrors the shipped-lib descriptor field). Absent ⇒ available on
        // every format. Present: an array of object-format NAMES; each must be
        // a known name ("pe"/"elf"/"macho") or fail LOUD (never a silent typo).
        // Lets `_WIN32` be predefined pe-only, and (TF-C74) lets the Apple-only
        // `__arm64__` spelling be predefined macho-only.
        if (e.contains("availableObjectFormats")) {
            json const& afs = e.at("availableObjectFormats");
            if (!afs.is_array()) {
                coll.emit(entryCode, mpath + "/availableObjectFormats",
                          "'predefinedMacros.availableObjectFormats' must be an "
                          "array of object-format names, e.g. [\"pe\"]");
                continue;
            }
            bool afOk = true;
            for (std::size_t ai = 0; ai < afs.size(); ++ai) {
                json const& av = afs[ai];
                if (!av.is_string()) {
                    coll.emit(entryCode, mpath + "/availableObjectFormats",
                              "'availableObjectFormats' entries must be strings");
                    afOk = false;
                    break;
                }
                std::string fmt = av.get<std::string>();
                if (!objectFormatKindFromName(fmt).has_value()) {
                    coll.emit(entryCode, mpath + "/availableObjectFormats",
                              std::format("unknown object-format name '{}' "
                                          "(expected \"pe\"/\"elf\"/\"macho\")",
                                          fmt));
                    afOk = false;
                    break;
                }
                pm.availableObjectFormats.push_back(std::move(fmt));
            }
            if (!afOk) continue;
        }
        // TF-C74: a WITHIN-LIST duplicate name is a load error. Two entries
        // spelling one macro would make the effective definition depend on
        // which of the four preprocessor seed sites iterated last (the
        // `predefined_` map keeps the FIRST via `emplace`, while the pre-scan
        // value prefix and the "<built-in>" prologue are LAST-writer-wins
        // `#define` streams) — a silent divergence between the pre-scan and
        // the authoritative pass, exactly the P0016 seam the shared format
        // filter exists to prevent. Rejecting at load makes it impossible.
        // NOTE: this is a NEW rule (the pre-extraction language loader had NO
        // duplicate check — MEASURED by reading the loop, which only
        // `push_back`s). The shipped configs declare no duplicates, so the
        // shipped-load behaviour is unchanged.
        if (std::find_if(out.begin(), out.end(),
                         [&](PredefinedMacroDef const& prior) {
                             return prior.name == pm.name;
                         })
            != out.end()) {
            coll.emit(entryCode, mpath + "/name",
                      std::format("duplicate predefined macro '{}' — a name may "
                                  "be declared at most once per "
                                  "'predefinedMacros' list",
                                  pm.name));
            continue;
        }
        out.push_back(std::move(pm));
    }
}

} // namespace dss::detail
