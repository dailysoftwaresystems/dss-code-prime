#include "core/types/predefined_macro_json.hpp"

#include "core/types/object_format_kind.hpp"

#include <algorithm>
#include <format>

namespace dss {

// Declared in `core/types/preprocess_config.hpp` (nlohmann-free, so tests can
// reach it); defined here beside its only production caller.
std::expected<long long, std::string>
packVersionComponents(std::string_view           versionText,
                      std::span<const long long> weights) {
    // Split on '.' into non-negative decimal components. Anything else — an
    // empty field ("1..2"), a non-digit ("1.0.2a"), a leading/trailing dot —
    // is a malformed version, not something to salvage.
    std::vector<long long> comps;
    std::size_t            pos = 0;
    while (true) {
        const std::size_t      dot  = versionText.find('.', pos);
        const std::string_view part = versionText.substr(
            pos, dot == std::string_view::npos ? std::string_view::npos
                                               : dot - pos);
        if (part.empty()) {
            return std::unexpected(std::format(
                "version '{}' is not a dot-separated list of non-negative "
                "integers", versionText));
        }
        long long v = 0;
        for (char const c : part) {
            if (c < '0' || c > '9') {
                return std::unexpected(std::format(
                    "version '{}' is not a dot-separated list of non-negative "
                    "integers", versionText));
            }
            v = v * 10 + (c - '0');
            if (v > 1000000000LL) {
                return std::unexpected(std::format(
                    "version '{}' has a component too large to encode",
                    versionText));
            }
        }
        comps.push_back(v);
        if (dot == std::string_view::npos) break;
        pos = dot + 1;
    }
    if (comps.size() != weights.size()) {
        return std::unexpected(std::format(
            "'componentWeights' declares {} component(s) but the version '{}' "
            "has {}", weights.size(), versionText, comps.size()));
    }
    // ★ THE ENCODING HAS A BOUND, AND IT IS DERIVED, NOT HARD-CODED.
    // Component i must not reach the NEXT-more-significant weight, or it
    // carries into that field and the packing silently collapses: with
    // [1000000,1000,1], `0.0.1000` would encode identically to `0.1.0` and
    // a `#if`-time `>=` against the macro would start lying. (No identity
    // macro is NAMED here on purpose — the engine knows the `version` KIND and
    // the `componentWeights` key; which macro uses them is config's business,
    // and a name in this comment is exactly what the agnosticism guard in
    // tests/analysis/preprocess catches.) The bound for component i is
    // weights[i-1]/weights[i] — read off the very weights the config declared,
    // so a different encoding gets its own correct bound for free. The MOST
    // significant component (i == 0) has no bound above it and is unbounded by
    // construction. Loud, naming the component.
    for (std::size_t i = 1; i < comps.size(); ++i) {
        const long long bound = weights[i - 1] / weights[i];
        if (comps[i] >= bound) {
            return std::unexpected(std::format(
                "version '{}' component #{} ({}) reaches its weight bound {} — "
                "the packed encoding would collapse and compare wrongly",
                versionText, i, comps[i], bound));
        }
    }
    long long packed = 0;
    for (std::size_t i = 0; i < comps.size(); ++i) {
        packed += comps[i] * weights[i];
    }
    return packed;
}

namespace detail {

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
        } else if (kind == "version") {
            // TF-C83 (D-CSUBSET-TOOLCHAIN-IDENTITY-PREDEFINES). The BUILD's own version, packed
            // into ONE integer by config-declared `componentWeights`.
            //
            // WHY A KIND AND NOT A `{{version}}` PLACEHOLDER. A templating
            // syntax would be a NEW vocabulary the engine has to parse, with
            // its own unknown-placeholder failure mode to invent. This table
            // already has the exact shape needed: `date`/`time` are macros
            // whose NAME is config and whose VALUE the engine derives. A
            // version macro is the same shape with a different source, so it
            // is the same mechanism — and an unknown KIND already fails loud
            // (the `else` below), which is precisely the guarantee a
            // placeholder scheme would have had to re-create.
            //
            // It LOWERS TO Constant: the input is build-invariant, so the
            // derivation is a LOAD-time concern and the result is exactly the
            // constant it claims to be. The expansion path is untouched —
            // `date`/`time` stay the only genuinely per-run derived kinds.
            pm.kind = PredefinedMacroKind::Constant;
            if (!e.contains("componentWeights")) {
                coll.emit(DiagnosticCode::C_MissingField,
                          mpath + "/componentWeights",
                          "a 'version' predefinedMacros entry requires "
                          "'componentWeights' (e.g. [1000000, 1000, 1])");
                continue;
            }
            json const& cw = e.at("componentWeights");
            if (!cw.is_array() || cw.empty()) {
                coll.emit(entryCode, mpath + "/componentWeights",
                          "'componentWeights' must be a non-empty array of "
                          "positive integers, most-significant FIRST");
                continue;
            }
            std::vector<long long> weights;
            bool                   wOk = true;
            for (std::size_t wi = 0; wi < cw.size(); ++wi) {
                if (!cw[wi].is_number_unsigned() || cw[wi].get<long long>() <= 0) {
                    coll.emit(entryCode, mpath + "/componentWeights",
                              "each 'componentWeights' entry must be a "
                              "positive integer");
                    wOk = false;
                    break;
                }
                const long long w = cw[wi].get<long long>();
                // STRICTLY DESCENDING. Equal or ascending weights make the
                // packing non-injective (two versions encode alike) and break
                // the ordering the encoding exists to provide.
                if (!weights.empty() && w >= weights.back()) {
                    coll.emit(entryCode, mpath + "/componentWeights",
                              std::format("'componentWeights' must be strictly "
                                          "descending; {} does not precede {}",
                                          weights.back(), w));
                    wOk = false;
                    break;
                }
                weights.push_back(w);
            }
            if (!wOk) continue;
            if (weights.back() != 1) {
                coll.emit(entryCode, mpath + "/componentWeights",
                          "the LAST 'componentWeights' entry must be 1 (the "
                          "least-significant component is unscaled)");
                continue;
            }
            // The version STRING is injected by the build from the repo-root
            // `VERSION` file, which CMake already reads as the single source
            // of truth. Nothing is duplicated: VERSION -> CMake -> here.
            //
            // The packing itself lives in `packVersionComponents` so that every
            // failure mode below is reachable from a UNIT TEST with an
            // arbitrary version string. Baking `kBuildVersionText` into this
            // function would make the bound check testable only by editing
            // VERSION and reconfiguring the build — i.e. effectively untested.
            auto const packed =
                packVersionComponents(kBuildVersionText, weights);
            if (!packed.has_value()) {
                coll.emit(entryCode, mpath + "/componentWeights",
                          packed.error());
                continue;
            }
            pm.value = std::format("{}", *packed);
        } else {
            coll.emit(entryCode, mpath + "/kind",
                      std::format("unknown predefined-macro kind '{}' "
                                  "(expected line/file/constant/date/time/"
                                  "version)",
                                  kind));
            continue;
        }
        // `componentWeights` belongs to the `version` kind alone. Silently
        // ignoring it elsewhere would let a typo'd `kind` ship a macro whose
        // declared encoding never ran.
        if (kind != "version" && e.contains("componentWeights")) {
            coll.emit(entryCode, mpath + "/componentWeights",
                      "'componentWeights' is valid only on a 'version' "
                      "predefinedMacros entry");
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
                auto const fmtKind = objectFormatKindFromName(fmt);
                if (!fmtKind.has_value()) {
                    coll.emit(entryCode, mpath + "/availableObjectFormats",
                              std::format("unknown object-format name '{}' "
                                          "(expected \"pe\"/\"elf\"/\"macho\")",
                                          fmt));
                    afOk = false;
                    break;
                }
                // The `unknown` sentinel spells correctly, so the name lookup
                // accepts it — and the filter then matches no real format, so
                // the macro is silently predefined NOWHERE. A macro listed as
                // available that never gets predefined is the same silent
                // no-op a typo'd name would produce; both fail loud here.
                if (!isSelectableObjectFormatKind(*fmtKind)) {
                    coll.emit(entryCode, mpath + "/availableObjectFormats",
                              std::string{kObjectFormatKindSentinelRejection});
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

} // namespace detail

} // namespace dss
