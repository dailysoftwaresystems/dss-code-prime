#include "ffi/shipped_lib_descriptor.hpp"

#include "core/types/data_model.hpp"             // dataModelFromName (signatureByDataModel keys)
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"     // objectFormatKindFromName (library-map key vocabulary)
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"            // InvalidType
#include "core/types/type_lattice/core_type.hpp"   // TypeKind (constant integer-scalar gate)
#include "core/types/type_lattice/type_interner.hpp" // TypeInterner::kind (constant type gate)
#include "core/types/number_decode.hpp"          // decodeFloat (the ONE float-literal decoder)
#include "hir/hir_text.hpp"                      // parseTypeFromText (the ONE type decoder)

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <span>
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The UNIVERSAL shipped-library descriptor reader. Mirrors the JSON-loader
// shape shared by every shipped config (`optimizer_json.cpp` etc.): parse →
// shape-check → required keys → optional keys → enum-resolve → reject-unknown
// → return. Errors are reported as `F_ShippedLibDescriptorMalformed` (shape /
// key / type / enum problems) or `F_ShippedLibUnsupportedType` (a symbol whose
// hir-text `signature` failed to decode). Both are unsuppressable.
//
// Agnostic: NO `if (lang/arch/format == ...)`. Every TypeId is built by
// `parseTypeFromText` against the caller's interner.

namespace dss::ffi {

namespace {

using json = nlohmann::json;

void emitMalformed(DiagnosticReporter& reporter, std::string what) {
    dss::report(reporter, DiagnosticCode::F_ShippedLibDescriptorMalformed,
                DiagnosticSeverity::Error, std::move(what));
}

// c112 (compile-perf): a THREAD-LOCAL parse cache for shipped descriptors, keyed
// by canonical path. Shipped descriptors are IMMUTABLE config, yet a SINGLE TU
// re-opens + re-`json::parse`s the SAME descriptor up to 4× — the front-end
// availability + typedef-name + macro reads AND the semantic symbol/type read —
// and a big descriptor (windows.json) dwarfs the decode, so that was O(reads ×
// json-size) filesystem+parse churn (the sqlite pe64 compile's preprocess/semantic
// regression). Caching the ifstream+parse makes every read after the first O(1).
// thread_local (not a mutex-guarded static) because the driver compiles CUs on a
// per-TU thread pool — each thread owns its cache, no lock, no cross-thread race;
// the within-TU 4×→1× dedup is where the win is. Returns nullptr on an I/O / parse
// / non-object failure (diagnostic emitted to `reporter`); failures are NOT cached,
// so a malformed descriptor still fails loud on every reader exactly as before.
json const* cachedDescriptorJson(std::filesystem::path const& path,
                                 DiagnosticReporter& reporter) {
    thread_local std::unordered_map<std::string, json> cache;
    std::error_code ec;
    auto const canon = std::filesystem::weakly_canonical(path, ec);
    std::string key = (ec ? path.lexically_normal() : canon).string();
    if (auto const it = cache.find(key); it != cache.end()) return &it->second;

    std::ifstream in{path, std::ios::binary};
    if (!in) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor: failed to open '"}
                + path.generic_string() + "' for reading");
        return nullptr;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    json doc;
    try {
        doc = json::parse(ss.str());
    } catch (json::parse_error const& e) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor '"} + path.generic_string()
                + "': JSON parse error: " + e.what());
        return nullptr;
    }
    if (!doc.is_object()) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor '"} + path.generic_string()
                + "': top-level value must be a JSON object");
        return nullptr;
    }
    auto const [it, _] = cache.emplace(std::move(key), std::move(doc));
    return &it->second;
}

// D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD enforcement (mirrors
// optimizer_json.cpp::rejectUnknownKeys). Reports one
// F_ShippedLibDescriptorMalformed per key not in the allow-list. Returns true
// iff every key was known.
[[nodiscard]] bool rejectUnknownKeys(DiagnosticReporter& reporter,
                                     json const& obj, std::string const& objPath,
                                     std::initializer_list<std::string_view> allowed) {
    bool ok = true;
    for (auto const& kv : obj.items()) {
        bool known = false;
        for (auto k : allowed) { if (kv.key() == k) { known = true; break; } }
        if (!known) {
            ok = false;
            emitMalformed(reporter,
                std::string{"shipped-lib descriptor "} + objPath
                    + ": unknown key '" + kv.key()
                    + "' (D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD)");
        }
    }
    return ok;
}

// Closed-table enum resolution. A miss is a malformed descriptor (the JSON
// named an enumerator outside the closed set) → reported by the caller.
[[nodiscard]] std::optional<ShippedSymbolKind> kindFromName(std::string_view s) {
    if (s == "function") return ShippedSymbolKind::Function;
    if (s == "object")   return ShippedSymbolKind::Object;
    return std::nullopt;
}
[[nodiscard]] std::optional<ShippedSymbolLinkage> linkageFromName(std::string_view s) {
    if (s == "external") return ShippedSymbolLinkage::External;
    if (s == "weak")     return ShippedSymbolLinkage::Weak;
    return std::nullopt;
}

// True for the core integer SCALAR kinds (signed + unsigned, I8..U128). A
// shipped CONSTANT's `type` must be one of these: the HIR fold derives the
// literal's core directly from this kind, so a float/pointer/string/aggregate
// macro is out of scope and fails loud (a function-like macro is not a constant
// at all).
[[nodiscard]] bool isIntegerScalarKind(TypeKind k) {
    return k >= TypeKind::I8 && k <= TypeKind::U128;
}

// True for the float SCALAR kinds (F16..F128). A shipped FLOAT CONSTANT's `type`
// (the `floatConstants` surface, c52) must be one of these — the sibling gate to
// `isIntegerScalarKind`. F32/F64 are the host-backed kinds the fold materializes;
// F16/F128 decode + validate here but have no host literal backing downstream (no
// math.h float constant needs them today).
[[nodiscard]] bool isFloatScalarKind(TypeKind k) {
    return k == TypeKind::F16 || k == TypeKind::F32
        || k == TypeKind::F64 || k == TypeKind::F128;
}

// Decode a FLOAT constant's STRING `value` into a `double`. JSON has no
// Infinity/NaN literal, so the value is a string: the explicit tokens
// "inf"/"+inf"/"-inf" (case-insensitive) map to the IEEE-754 ±infinity bit
// patterns; any other string is a finite float literal handed to the ONE float
// decoder (`decodeFloat`, ns=nullptr → plain decimal / C99 hex-float via strtod).
// Returns nullopt (the caller emits the error) on a non-string value, an empty
// string, an un-parseable literal, OR a FINITE literal that OVERFLOWS to infinity
// (only the explicit "inf" token may yield an infinity — never a silent overflow).
[[nodiscard]] std::optional<double> decodeFloatConstantValue(json const& v) {
    if (!v.is_string()) return std::nullopt;
    std::string const s = v.get<std::string>();
    if (s.empty()) return std::nullopt;
    // Case-insensitive match of the infinity tokens (a small, closed set).
    auto eqi = [](std::string const& a, char const* b) {
        if (a.size() != std::char_traits<char>::length(b)) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i]))
                != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    };
    if (eqi(s, "inf") || eqi(s, "+inf")) return std::numeric_limits<double>::infinity();
    if (eqi(s, "-inf")) return -std::numeric_limits<double>::infinity();
    // A finite float literal. ns=nullptr → plain decimal / hex-float (strtod);
    // `ok` is false on a partial parse OR an ERANGE overflow (overflow → infinity
    // is rejected here — an infinity must be spelled "inf", never reached by an
    // out-of-range finite literal).
    bool ok = false;
    double const d = decodeFloat(s, /*ns=*/nullptr, ok);
    if (!ok || std::isinf(d) || std::isnan(d)) return std::nullopt;
    return d;
}

// Validate a JSON integer `value` fits the integer-scalar `kind` and return its
// int64 BIT-PATTERN (for an unsigned kind the uint64 value reinterpreted; the
// HIR fold re-reads it per the kind's signedness, so the full unsigned range --
// e.g. `UINT_MAX` / `SIZE_MAX` -- round-trips losslessly). Returns nullopt (the
// caller emits the error) on a non-integer / float JSON value, a negative value
// for an unsigned kind, a value exceeding i64 for a signed kind, or a value
// outside the kind's declared width.
[[nodiscard]] std::optional<std::int64_t>
decodeConstantValue(json const& v, TypeKind kind) {
    bool const isSigned = (kind == TypeKind::I8 || kind == TypeKind::I16
                        || kind == TypeKind::I32 || kind == TypeKind::I64
                        || kind == TypeKind::I128);
    // An I128/U128 constant is range-limited to 64-bit MAGNITUDE: `value` is an
    // int64 carrier, and a JSON literal wider than 64 bits parses as a float and
    // is rejected below — so a 128-bit constant cannot be WRONG, only capped at
    // 64 bits. Widen `ShippedConstant::value` if a true >64-bit macro ever ships.
    int const bits = (kind == TypeKind::I8  || kind == TypeKind::U8)  ? 8
                   : (kind == TypeKind::I16 || kind == TypeKind::U16) ? 16
                   : (kind == TypeKind::I32 || kind == TypeKind::U32) ? 32
                   : (kind == TypeKind::I64 || kind == TypeKind::U64) ? 64
                                                                      : 128;
    if (isSigned) {
        if (!v.is_number_integer()) return std::nullopt;   // float / non-integer
        if (v.is_number_unsigned()
            && v.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX))
            return std::nullopt;                           // exceeds i64
        std::int64_t const x = v.get<std::int64_t>();
        if (bits < 64) {
            std::int64_t const lo = -(std::int64_t{1} << (bits - 1));
            std::int64_t const hi =  (std::int64_t{1} << (bits - 1)) - 1;
            if (x < lo || x > hi) return std::nullopt;
        }
        return x;
    }
    if (!v.is_number_unsigned()) return std::nullopt;      // negative / float / non-integer
    std::uint64_t const x = v.get<std::uint64_t>();
    if (bits < 64) {
        std::uint64_t const hi = (std::uint64_t{1} << bits) - 1;
        if (x > hi) return std::nullopt;
    }
    return static_cast<std::int64_t>(x);                   // bit-pattern (re-read per kind)
}

// The tri-state outcome of testing ONE variant's `when` selector against the
// active compile target. `Error` means the `when` itself was malformed (a bad
// key / non-string value / unknown object-format) and was reported — the caller
// must abort the whole variants block (never select). `Match`/`NoMatch` are the
// clean selection outcomes.
enum class WhenMatch { Match, NoMatch, Error };

// Decode + test a variant's `when` object (the per-target SELECTOR shared by the
// `structs` / `constants` / `typedefs` / `macros` variant surfaces). The contract
// is MATCH-ALL-SPECIFIED: every key the `when` SPECIFIES must equal the active
// value (generic string equality — never an `if (arch == "x86_64")` here); an
// unspecified key is a wildcard. A key tested against an UNKNOWN active value
// (activeTarget / activeFormat nullopt — direct-API/LSP/test callers) can never
// match. `allowArch` gates whether `arch` is a legal key: the typed surfaces
// (struct/constant/typedef) carry BOTH {arch,format}; the preprocessor `macros`
// surface is FORMAT-ONLY (arch is not threaded into preprocess — c9 build-key
// avoidance), so its `when` keys are {format} alone and an `arch` key there fails
// loud. `activeFormatName` is the precomputed `objectFormatKindName(*activeFormat)`
// (empty when activeFormat is nullopt). `whenCtx` is the caller's diagnostic
// context for the `when` object (e.g. "structs[0] variants[1].when"). Reports via
// `emitMalformed` on a malformed `when`; the format VALUE is validated against the
// closed `objectFormatKindFromName` vocabulary so a typo'd "elff" fails loud rather
// than silently never matching.
[[nodiscard]] WhenMatch
matchVariantWhen(json const& when, bool allowArch, std::string const& whenCtx,
                 std::optional<std::string_view> activeTarget,
                 std::optional<ObjectFormatKind> activeFormat,
                 std::string const& activeFormatName,
                 DiagnosticReporter& reporter) {
    // Closed key vocabulary: {arch,format} for the typed surfaces, {format} only
    // for macros. An unknown/forbidden key (e.g. `arch` in a macro `when`, or a
    // typo'd "ach") fails loud — a silently-ignored key would match more broadly
    // than intended.
    if (allowArch) {
        if (!rejectUnknownKeys(reporter, when, whenCtx, {"arch", "format"}))
            return WhenMatch::Error;
    } else {
        if (!rejectUnknownKeys(reporter, when, whenCtx, {"format"}))
            return WhenMatch::Error;
    }
    bool matches = true;
    if (allowArch && when.contains("arch")) {
        if (!when.at("arch").is_string()) {
            emitMalformed(reporter, "shipped-lib descriptor " + whenCtx
                                        + ": 'arch' must be a string");
            return WhenMatch::Error;
        }
        std::string const wantArch = when.at("arch").get<std::string>();
        // The arch name is OPEN (it lives only in the target schemas this reader
        // does not load) — an unknown arch simply never matches.
        if (!activeTarget.has_value() || *activeTarget != wantArch) matches = false;
    }
    if (when.contains("format")) {
        if (!when.at("format").is_string()) {
            emitMalformed(reporter, "shipped-lib descriptor " + whenCtx
                                        + ": 'format' must be a string");
            return WhenMatch::Error;
        }
        std::string const wantFormat = when.at("format").get<std::string>();
        // The format VALUE is matched against the CLOSED object-format vocabulary
        // (a typo'd "elff" would otherwise silently never match → the entry
        // vanishes on every target).
        if (!objectFormatKindFromName(wantFormat).has_value()) {
            emitMalformed(reporter, "shipped-lib descriptor " + whenCtx
                                        + ": 'format' has unknown object-format name '"
                                        + wantFormat
                                        + "' (expected \"pe\"/\"elf\"/\"macho\")");
            return WhenMatch::Error;
        }
        if (!activeFormat.has_value() || activeFormatName != wantFormat) matches = false;
    }
    return matches ? WhenMatch::Match : WhenMatch::NoMatch;
}

// Decode the optional `macros` array (the preprocessor-macro surface) into
// `out`. Collect-all: a malformed entry is reported (the caller's errorCount
// delta then fails the whole read) and the loop continues. Interner-FREE — a
// macro is pure preprocessor token TEXT (no types), so the preprocessor (which
// has no interner) reuses this. Shared by `readShippedLibDescriptor` (full read)
// and `readShippedLibMacros` (the preprocessor's macros-only read).
//
// PER-FORMAT VARIANTS (plan 25 extension): a macro entry may declare a flat
// `{params?, replacement, variadic?}` OR per-FORMAT `variants` (each a
// `when:{format}` + its own {replacement, params?, variadic?}), so a macro can
// carry a different replacement per object-format (the errno case:
// `__errno_location` on elf vs `__error` on macho). FORMAT-ONLY — the
// preprocessor runs once per (file × format-kind) and arch is NOT threaded into
// it (c9 build-key avoidance), so a macro variant's `when` carries `format`
// alone (an `arch` key fails loud). `activeFormat` nullopt (direct-API / a test
// caller / no target) ⇒ no variant can be selected → a variants-only macro is
// not injected. The MATCH-ALL-SPECIFIED + exactly-one contract is the same as
// the typed surfaces; >1 match ⇒ F_ShippedMacroVariantAmbiguous.
void decodeShippedMacros(json const& doc, std::string const& pathStr,
                         DiagnosticReporter& reporter,
                         std::vector<ShippedMacro>& out,
                         std::optional<ObjectFormatKind> activeFormat = std::nullopt) {
    if (!doc.contains("macros")) return;
    if (!doc.at("macros").is_array()) {
        emitMalformed(reporter, "shipped-lib descriptor '" + pathStr
                                    + "': 'macros' must be an array");
        return;
    }
    json const& macros = doc.at("macros");
    out.reserve(out.size() + macros.size());
    // A macro field (name / a param / replacement) containing a newline would
    // break the synthetic `#define name(params) replacement\n` line the
    // preprocessor splices — terminating the directive early and leaking the
    // remainder into the synth buffer as source. Reject it FAIL-LOUD (a `\n`/`\r`
    // is never legitimate in a macro name/param/replacement-text), never a silent
    // buffer corruption.
    auto const hasLineBreak = [](std::string const& s) {
        return s.find('\n') != std::string::npos || s.find('\r') != std::string::npos;
    };
    std::string const activeFormatName =
        activeFormat.has_value() ? std::string{objectFormatKindName(*activeFormat)}
                                 : std::string{};
    // Decode the BODY fields (params / replacement / variadic) of a macro entry
    // OR a macro variant `obj` into `macro` (whose `name` the caller already set).
    // Returns true iff the body decoded; reports + returns false on any malformed
    // field. `ctx` is the diagnostic context. Shared by the flat path AND every
    // variant so an inactive variant's bad body fails the read on every target.
    auto decodeMacroBody = [&](json const& obj, std::string const& ctx,
                               ShippedMacro& macro) -> bool {
        // params: ABSENT = object-like; PRESENT (even []) = function-like.
        if (obj.contains("params")) {
            if (!obj.at("params").is_array()) {
                emitMalformed(reporter, "shipped-lib descriptor " + ctx
                                            + ": 'params' must be an array of strings");
                return false;
            }
            std::vector<std::string> params;
            for (auto const& p : obj.at("params")) {
                if (!p.is_string() || p.get<std::string>().empty()) {
                    emitMalformed(reporter, "shipped-lib descriptor " + ctx
                                                + ": every 'params' entry must be a non-empty "
                                                  "string");
                    return false;
                }
                params.push_back(p.get<std::string>());
            }
            macro.params = std::move(params);
        }
        // replacement: optional string (default empty — a null macro `#define X`).
        if (obj.contains("replacement")) {
            if (!obj.at("replacement").is_string()) {
                emitMalformed(reporter, "shipped-lib descriptor " + ctx
                                            + ": 'replacement' must be a string");
                return false;
            }
            macro.replacement = obj.at("replacement").get<std::string>();
        }
        // variadic: optional bool; an object-like macro cannot be variadic.
        if (obj.contains("variadic")) {
            if (!obj.at("variadic").is_boolean()) {
                emitMalformed(reporter, "shipped-lib descriptor " + ctx
                                            + ": 'variadic' must be a boolean");
                return false;
            }
            macro.variadic = obj.at("variadic").get<bool>();
            if (macro.variadic && !macro.params.has_value()) {
                emitMalformed(reporter, "shipped-lib descriptor " + ctx
                                            + ": 'variadic' requires 'params' (an object-like "
                                              "macro cannot be variadic)");
                return false;
            }
        }
        // Final field-shape gate (covers name + every param + replacement at one
        // chokepoint): no field may carry a directive-breaking newline.
        bool fieldHasLineBreak =
            hasLineBreak(macro.name) || hasLineBreak(macro.replacement);
        if (macro.params.has_value()) {
            for (auto const& pn : *macro.params) {
                if (hasLineBreak(pn)) fieldHasLineBreak = true;
            }
        }
        if (fieldHasLineBreak) {
            emitMalformed(reporter, "shipped-lib descriptor " + ctx
                                        + ": a macro field ('name'/'params'/'replacement') must "
                                          "not contain a newline");
            return false;
        }
        return true;
    };

    std::size_t midx = 0;
    for (auto const& m : macros) {
        std::string const at = "'" + pathStr + "' macros[" + std::to_string(midx) + "]";
        ++midx;
        if (!m.is_object()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at + ": must be an object");
            continue;
        }
        (void)rejectUnknownKeys(reporter, m, "macros[" + std::to_string(midx - 1) + "]",
                                {"name", "params", "replacement", "variadic", "variants"});
        if (!m.contains("name") || !m.at("name").is_string()
            || m.at("name").get<std::string>().empty()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at
                                        + ": missing or empty 'name'");
            continue;
        }
        std::string const mname = m.at("name").get<std::string>();

        // Exactly ONE of a flat body (single replacement, back-compat) or per-
        // format `variants`. The flat path is signalled by ANY body key
        // (`params`/`replacement`/`variadic`); the variant path by `variants`. A
        // bare `{name}` (null object-like macro `#define X`) is the LEGITIMATE
        // empty-flat form — it has neither, so treat (no body keys AND no variants)
        // as FLAT. Both a body key AND `variants` is ambiguous intent → fail loud.
        bool const mHasVariants = m.contains("variants");
        bool const mHasBodyKey  = m.contains("params") || m.contains("replacement")
                                  || m.contains("variadic");
        if (mHasBodyKey && mHasVariants) {
            emitMalformed(reporter, "shipped-lib descriptor " + at
                                        + ": a macro must declare EITHER a flat body "
                                          "('params'/'replacement'/'variadic') or 'variants' "
                                          "(per-format replacements), not both");
            continue;
        }

        if (!mHasVariants) {
            // FLAT (including the bare `{name}` null macro).
            ShippedMacro macro;
            macro.name = mname;
            if (!decodeMacroBody(m, at, macro)) continue;
            out.push_back(std::move(macro));
            continue;
        }

        // PER-FORMAT VARIANTS. Decode EVERY variant's body EAGERLY, then select the
        // variant whose `when` matches the active format.
        if (!m.at("variants").is_array() || m.at("variants").empty()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at
                                        + ": 'variants' must be a non-empty array");
            continue;
        }
        bool okVariants = true;
        std::size_t matchCount = 0;
        ShippedMacro selected;
        selected.name = mname;
        std::size_t vidx = 0;
        for (auto const& vdef : m.at("variants")) {
            std::string const vat = at + " variants[" + std::to_string(vidx) + "]";
            ++vidx;
            if (!vdef.is_object()) {
                emitMalformed(reporter, "shipped-lib descriptor " + vat + ": must be an object");
                okVariants = false; break;
            }
            (void)rejectUnknownKeys(reporter, vdef, vat,
                                    {"when", "params", "replacement", "variadic"});
            if (!vdef.contains("when") || !vdef.at("when").is_object()) {
                emitMalformed(reporter, "shipped-lib descriptor " + vat
                                            + ": missing or non-object 'when' "
                                              "(e.g. {\"format\":\"elf\"})");
                okVariants = false; break;
            }
            // Decode this variant's body EAGERLY (every variant) into a scratch
            // macro, so a malformed inactive variant fails the read on every target.
            ShippedMacro vMacro;
            vMacro.name = mname;
            if (!decodeMacroBody(vdef, vat, vMacro)) { okVariants = false; break; }
            // FORMAT-ONLY selector (allowArch=false — arch is not threaded into the
            // preprocessor). A nullopt activeFormat can never match (no selection).
            WhenMatch const wm = matchVariantWhen(
                vdef.at("when"), /*allowArch=*/false, vat + ".when",
                /*activeTarget=*/std::nullopt, activeFormat, activeFormatName, reporter);
            if (wm == WhenMatch::Error) { okVariants = false; break; }
            if (wm == WhenMatch::Match) {
                ++matchCount;
                if (matchCount == 1) selected = std::move(vMacro);
            }
        }
        if (!okVariants) continue;
        if (matchCount > 1) {
            dss::report(reporter, DiagnosticCode::F_ShippedMacroVariantAmbiguous,
                        DiagnosticSeverity::Error,
                        "shipped-lib descriptor " + at + ": macro '" + mname
                            + "' has " + std::to_string(matchCount)
                            + " 'variants' matching the active object-format ('"
                            + (activeFormat.has_value() ? activeFormatName
                                                        : std::string{"<none>"})
                            + "') — exactly one variant may match (refusing an "
                              "ambiguous per-format macro replacement)");
            continue;
        }
        // matchCount 0 ⇒ no variant for this format ⇒ NOT injected (the macro
        // simply does not exist for this target — never a silent wrong replacement).
        if (matchCount == 1) out.push_back(std::move(selected));
    }
}

// Decode the optional `availableObjectFormats` array (per-target AVAILABILITY)
// into `out`. Each entry must be a known object-format name (the SAME
// `objectFormatKindFromName` vocabulary the `library` keys use; a typo fails loud
// HERE). Empty/absent ⇒ available on every format. Shared chokepoint: the full
// read AND the fast interner-free `readShippedLibAvailability` (the front-end
// availability gate) both decode through this, so they can never drift.
void decodeShippedAvailability(json const& doc, std::string const& pathStr,
                               DiagnosticReporter& reporter,
                               std::vector<std::string>& out) {
    if (!doc.contains("availableObjectFormats")) return;
    if (!doc.at("availableObjectFormats").is_array()) {
        emitMalformed(reporter, "shipped-lib descriptor '" + pathStr
                                    + "': 'availableObjectFormats' must be an array of "
                                      "object-format names, e.g. [\"elf\",\"macho\"]");
        return;
    }
    for (auto const& v : doc.at("availableObjectFormats")) {
        if (!v.is_string()) {
            emitMalformed(reporter, "shipped-lib descriptor '" + pathStr
                                        + "': 'availableObjectFormats' entries must be strings");
            continue;
        }
        std::string fmt = v.get<std::string>();
        if (!objectFormatKindFromName(fmt).has_value()) {
            emitMalformed(reporter, "shipped-lib descriptor '" + pathStr
                                        + "': 'availableObjectFormats' has unknown object-format "
                                          "name '" + fmt + "' (expected \"pe\"/\"elf\"/\"macho\")");
            continue;
        }
        out.push_back(std::move(fmt));
    }
}

// Decode a struct `fields` JSON array (non-empty, each `{name,type}`) into
// `outFields` + the parallel `outFieldTypes`. Each field type decodes via the ONE
// `parseTypeFromText` codec; a duplicate field name or an undecodable type FAILS
// LOUD (reported here). Returns true iff EVERY field decoded. `at` is the caller's
// already-built diagnostic context string (e.g. "'p' structs[0]" or
// "'p' structs[0] variants[1]"). Shared by the flat-`fields` path AND every
// variant's field list — so a malformed field in ANY (active or inactive) variant
// fails the same way (the F2 anti-lurking property). `fields` must be a non-empty
// array (the caller validates that before calling).
[[nodiscard]] bool decodeStructFieldList(json const& fields, std::string const& at,
                                         TypeInterner& interner, TypeRegistry& typeReg,
                                         DiagnosticReporter& reporter,
                                         std::vector<ShippedField>& outFields,
                                         std::vector<TypeId>& outFieldTypes,
                                         std::span<NamedTypeBinding const> namedTypes) {
    std::size_t fidx = 0;
    for (auto const& f : fields) {
        std::string const fat = at + " fields[" + std::to_string(fidx) + "]";
        ++fidx;
        if (!f.is_object()) {
            emitMalformed(reporter, "shipped-lib descriptor " + fat + ": must be an object");
            return false;
        }
        (void)rejectUnknownKeys(reporter, f, fat, {"name", "type", "offset"});
        if (!f.contains("name") || !f.at("name").is_string()
            || f.at("name").get<std::string>().empty()) {
            emitMalformed(reporter, "shipped-lib descriptor " + fat
                                        + ": missing or empty 'name'");
            return false;
        }
        std::string fname = f.at("name").get<std::string>();
        // Reject a DUPLICATE field name (invalid C; a last-writer-wins scope
        // binding would silently lose a field slot — fail loud, never a
        // wrong-but-runs aggregate). Few fields → linear scan.
        for (auto const& ef : outFields) {
            if (ef.name == fname) {
                emitMalformed(reporter, "shipped-lib descriptor " + fat
                                            + ": duplicate field name '" + fname + "'");
                return false;
            }
        }
        if (!f.contains("type") || !f.at("type").is_string()) {
            emitMalformed(reporter, "shipped-lib descriptor " + fat
                                        + ": missing or non-string 'type'");
            return false;
        }
        std::string const fTypeText = f.at("type").get<std::string>();
        TypeId const fty = parseTypeFromText(fTypeText, interner, typeReg, reporter, namedTypes);
        if (!fty.valid() || fty == InvalidType) {
            dss::report(reporter, DiagnosticCode::F_ShippedLibUnsupportedType,
                        DiagnosticSeverity::Error,
                        "shipped-lib descriptor " + fat + ": field type '"
                            + fTypeText + "' failed to decode");
            return false;
        }
        // c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): an optional explicit byte `offset`
        // (a non-negative integer). All-or-none per struct is enforced by the caller
        // once every field is decoded (it sees the full set).
        std::optional<std::uint64_t> foff;
        if (f.contains("offset")) {
            if (!f.at("offset").is_number_unsigned()) {
                emitMalformed(reporter, "shipped-lib descriptor " + fat
                                            + ": 'offset' must be a non-negative integer");
                return false;
            }
            foff = f.at("offset").get<std::uint64_t>();
        }
        outFields.push_back(ShippedField{std::move(fname), fty, foff});
        outFieldTypes.push_back(fty);
    }
    return true;
}

// Decode a constant's `type` + `value` (the SHARED scalar-constant codec used by
// the flat-`{name,value,type}` path AND every variant's `{when,value,type}`). The
// `type` must decode to an INTEGER SCALAR and the `value` must fit its width +
// signedness; both fail loud (reported here). `at` is the caller's diagnostic
// context. On success fills `outValue`/`outType` and returns true. EAGER: every
// variant calls this regardless of which is active, so a malformed inactive
// variant fails the read on every target (anti-lurking, mirrors the struct path).
[[nodiscard]] bool
decodeConstantValueAndType(json const& obj, std::string const& at,
                           std::string const& cname, TypeInterner& interner,
                           TypeRegistry& typeReg, DiagnosticReporter& reporter,
                           std::int64_t& outValue, TypeId& outType,
                           std::span<NamedTypeBinding const> namedTypes) {
    if (!obj.contains("type") || !obj.at("type").is_string()) {
        emitMalformed(reporter, "shipped-lib descriptor " + at
                                    + ": missing or non-string 'type'");
        return false;
    }
    std::string const typeText = obj.at("type").get<std::string>();
    TypeId const cty = parseTypeFromText(typeText, interner, typeReg, reporter, namedTypes);
    if (!cty.valid() || cty == InvalidType) {
        dss::report(reporter, DiagnosticCode::F_ShippedLibUnsupportedType,
                    DiagnosticSeverity::Error,
                    "shipped-lib descriptor " + at + ": constant '" + cname
                        + "' has a 'type' that failed to decode ('" + typeText + "')");
        return false;
    }
    if (!isIntegerScalarKind(interner.kind(cty))) {
        dss::report(reporter, DiagnosticCode::F_ShippedLibUnsupportedType,
                    DiagnosticSeverity::Error,
                    "shipped-lib descriptor " + at + ": constant '" + cname
                        + "' type '" + typeText + "' is not an integer scalar "
                          "(a shipped constant must be an integer; a float / "
                          "pointer / function-like macro is out of scope)");
        return false;
    }
    if (!obj.contains("value") || !obj.at("value").is_number()) {
        emitMalformed(reporter, "shipped-lib descriptor " + at
                                    + ": missing or non-numeric 'value'");
        return false;
    }
    auto const bits = decodeConstantValue(obj.at("value"), interner.kind(cty));
    if (!bits.has_value()) {
        emitMalformed(reporter, "shipped-lib descriptor " + at + ": constant '" + cname
            + "' value does not fit its declared integer type '" + typeText
            + "' (out of range, negative-for-unsigned, or non-integer)");
        return false;
    }
    outValue = *bits;
    outType  = cty;
    return true;
}

} // namespace

std::optional<ShippedLibDescriptor>
readShippedLibDescriptor(std::filesystem::path const&    path,
                         TypeInterner&                   interner,
                         TypeRegistry&                   typeReg,
                         DiagnosticReporter&             reporter,
                         DataModel                       dataModel,
                         std::optional<std::string_view> activeTarget,
                         std::optional<ObjectFormatKind> activeFormat,
                         std::span<NamedTypeBinding const> namedTypes) {
    std::size_t const errBefore = reporter.errorCount();

    // (0)+(1) Read + parse the file — via the thread-local parse cache (the same
    // descriptor is read up to 4× per TU; a big windows.json dwarfs the decode).
    // A missing/unreadable/malformed descriptor fails loud there (a real I/O fault,
    // not a soft miss — the resolver already verified existence). The cached JSON is
    // decoded read-only below; every `doc` access is const (.contains/.at/.get).
    json const* const docPtr = cachedDescriptorJson(path, reporter);
    if (!docPtr) return std::nullopt;
    json const& doc = *docPtr;

    ShippedLibDescriptor out;

    // (1.5) Required `header` provenance string (non-empty). Every shipped
    // descriptor must declare which header its symbols come from — a missing
    // header is a provenance hole (the user must be able to know where a
    // symbol like `strlen` originates).
    if (!doc.contains("header") || !doc.at("header").is_string()
        || doc.at("header").get<std::string>().empty()) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor '"} + path.generic_string()
                + "': missing or empty required 'header' provenance string "
                  "(e.g. \"stdio.h\")");
        return std::nullopt;
    }
    out.header = doc.at("header").get<std::string>();

    // (1.6) Optional `standard` provenance string (e.g. "c89"/"c99"/"posix").
    if (doc.contains("standard")) {
        if (!doc.at("standard").is_string()) {
            emitMalformed(reporter,
                std::string{"shipped-lib descriptor '"} + path.generic_string()
                    + "': 'standard' must be a string");
            return std::nullopt;
        }
        out.standard = doc.at("standard").get<std::string>();
    }

    // (2) Optional `library` MAP (Model 3): per-object-format runtime image,
    // keyed by the canonical `objectFormatKindName` vocabulary
    // ("pe"/"elf"/"macho"). Absent ⇒ empty map (the lowering then falls back to
    // the language's externLibraryByFormat default for every format). A present
    // map: each key MUST be a known object-format name (a typo like "pee" fails
    // loud HERE, not at a user's link), each value MUST be a string. A map that
    // omits a format is legal — that format inherits the language default at
    // resolution. AGNOSTIC: the key set is the `objectFormatKindFromName`
    // vocabulary, never an `if (key == "pe")` identity branch.
    if (doc.contains("library")) {
        if (!doc.at("library").is_object()) {
            emitMalformed(reporter,
                std::string{"shipped-lib descriptor '"} + path.generic_string()
                    + "': 'library' must be a per-object-format object, e.g. "
                      "{\"pe\":\"msvcrt.dll\",\"elf\":\"libc.so.6\"}");
            return std::nullopt;
        }
        for (auto const& kv : doc.at("library").items()) {
            if (!objectFormatKindFromName(kv.key()).has_value()) {
                emitMalformed(reporter,
                    std::string{"shipped-lib descriptor '"} + path.generic_string()
                        + "': 'library' has unknown object-format key '" + kv.key()
                        + "' (expected one of the object-format names, e.g. "
                          "\"pe\"/\"elf\"/\"macho\")");
                continue;
            }
            if (!kv.value().is_string()) {
                emitMalformed(reporter,
                    std::string{"shipped-lib descriptor '"} + path.generic_string()
                        + "': 'library." + kv.key() + "' must be a string");
                continue;
            }
            out.library.emplace(kv.key(), kv.value().get<std::string>());
        }
    }

    // (2.5) Optional `availableObjectFormats` — the per-target AVAILABILITY set
    // (which object-formats this header EXISTS on). Absent/empty ⇒ available on
    // every format (back-compat). Decoded through the SHARED chokepoint so the
    // full read + the fast front-end reader never drift.
    decodeShippedAvailability(doc, path.generic_string(), reporter,
                              out.availableObjectFormats);

    // (3) `symbols` array — OPTIONAL. A header may carry only `constants`
    // (e.g. <limits.h>, all `#define`s, no linkable symbols) or only
    // `typedefs`; the "declares SOMETHING" requirement is enforced across
    // symbols/constants/typedefs AFTER all three decode (a descriptor that
    // declares nothing is a no-op artifact and still fails loud). If present,
    // it must be an array.
    if (doc.contains("symbols") && !doc.at("symbols").is_array()) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor '"} + path.generic_string()
                + "': 'symbols' must be an array");
        return std::nullopt;
    }
    json const emptyArray = json::array();
    json const& symbols =
        doc.contains("symbols") ? doc.at("symbols") : emptyArray;

    // Reject unknown top-level keys (closed key set). `$comment` is the
    // repo-wide config-documentation convention (a `$`-prefixed key carrying a
    // human note, e.g. the LP64-vs-LLP64 deferral rationale in stdio/stdlib) —
    // accepted + ignored, never consumed by lowering.
    (void)rejectUnknownKeys(reporter, doc, "(root)",
                            {"header", "standard", "library", "availableObjectFormats",
                             "symbols", "constants", "floatConstants", "typedefs",
                             "structs", "macros", "$comment"});

    // (4) Each symbol. Collect-all: a malformed symbol is reported but the
    // loop continues so the operator sees every problem in one pass; the
    // overall read still fails (errorCount delta below) so a malformed
    // descriptor never yields a usable result.
    out.symbols.reserve(symbols.size());
    std::size_t idx = 0;
    for (auto const& sym : symbols) {
        std::string const at =
            std::string{"'"} + path.generic_string() + "' symbols[" + std::to_string(idx) + "]";
        ++idx;
        if (!sym.is_object()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at + ": must be an object");
            continue;
        }

        // name (required, non-empty string).
        if (!sym.contains("name") || !sym.at("name").is_string()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at
                                        + ": missing or non-string 'name'");
            continue;
        }
        std::string name = sym.at("name").get<std::string>();
        if (name.empty()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at + ": 'name' is empty");
            continue;
        }

        // signature (required string → decoded type).
        if (!sym.contains("signature") || !sym.at("signature").is_string()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at
                                        + ": missing or non-string 'signature'");
            continue;
        }
        std::string const sigText = sym.at("signature").get<std::string>();

        // kind (optional, closed enum, default Function).
        ShippedSymbolKind kind = ShippedSymbolKind::Function;
        if (sym.contains("kind")) {
            if (!sym.at("kind").is_string()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": 'kind' must be a string");
                continue;
            }
            auto k = kindFromName(sym.at("kind").get<std::string>());
            if (!k) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                    + ": unknown 'kind' '" + sym.at("kind").get<std::string>()
                    + "' (expected \"function\" or \"object\")");
                continue;
            }
            kind = *k;
        }

        // linkage (optional, closed enum, default External).
        ShippedSymbolLinkage linkage = ShippedSymbolLinkage::External;
        if (sym.contains("linkage")) {
            if (!sym.at("linkage").is_string()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": 'linkage' must be a string");
                continue;
            }
            auto l = linkageFromName(sym.at("linkage").get<std::string>());
            if (!l) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                    + ": unknown 'linkage' '" + sym.at("linkage").get<std::string>()
                    + "' (expected \"external\" or \"weak\")");
                continue;
            }
            linkage = *l;
        }

        // FC16 (D-CSUBSET-NORETURN): optional `noreturn` bool (default false) —
        // TRUE for abort/exit. Threaded onto the injected symbol's isNoreturn so a
        // direct call is wrapped `Block{ ExprStmt(call), Unreachable }` at HIR.
        bool noreturn = false;
        if (sym.contains("noreturn")) {
            if (!sym.at("noreturn").is_boolean()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": 'noreturn' must be a boolean");
                continue;
            }
            noreturn = sym.at("noreturn").get<bool>();
        }

        // Optional per-SYMBOL `availableObjectFormats` — which object-formats this
        // symbol EXISTS on (errno's __error is ["macho"], __errno_location ["elf"];
        // the Linux-only fdatasync/fallocate/mremap are ["elf"]). EMPTY/absent =
        // every format. Reuses the SAME chokepoint as the header-level set (read
        // from the per-symbol json `sym`); an unknown format name fails loud HERE.
        // Gated at semantic injection by the active format — a format-absent symbol
        // is never declared, so it is never imported (DSS imports every DECLARED
        // shipped extern). (D-SHIPPED-SYMBOL-PER-TARGET-AVAILABILITY)
        std::vector<std::string> symAvail;
        decodeShippedAvailability(sym, at, reporter, symAvail);

        // FC3 c1: optional per-data-model signature override
        // (D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH closure for the
        // LP64-merged libc symbols — fseek/ftell/atol/strtol/strtoul/
        // labs carry the C `long`, whose width is the FORMAT's data
        // model, not one signature). Shape mirrors the Model-3
        // per-format `library` map: the BASE `signature` is the
        // LP64-correct text; `signatureByDataModel` keys data-model
        // names ("LLP64"/"ILP32") to replacement type texts. The
        // ACTIVE model's entry (when present) becomes the effective
        // signature; EVERY declared override must parse (a malformed
        // override under a model not currently selected would
        // otherwise lurk until that model is first compiled). Unknown
        // model keys fail loud (closed vocabulary).
        std::string effectiveSigText = sigText;
        bool overridesOk = true;
        if (sym.contains("signatureByDataModel")) {
            if (!sym.at("signatureByDataModel").is_object()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": 'signatureByDataModel' must be an "
                                              "object mapping data-model names to "
                                              "signature strings");
                continue;
            }
            for (auto const& kv : sym.at("signatureByDataModel").items()) {
                auto const dm = dataModelFromName(kv.key());
                if (!dm) {
                    emitMalformed(reporter, "shipped-lib descriptor " + at
                        + ": 'signatureByDataModel' has unknown data-model key '"
                        + kv.key() + "' (expected one of \"LP64\"/\"LLP64\"/\"ILP32\")");
                    overridesOk = false;
                    continue;
                }
                if (!kv.value().is_string()) {
                    emitMalformed(reporter, "shipped-lib descriptor " + at
                        + ": 'signatureByDataModel." + kv.key()
                        + "' must be a signature string");
                    overridesOk = false;
                    continue;
                }
                std::string const ovText = kv.value().get<std::string>();
                TypeId const ovSig =
                    parseTypeFromText(ovText, interner, typeReg, reporter, namedTypes);
                if (!ovSig.valid() || ovSig == InvalidType) {
                    dss::report(reporter, DiagnosticCode::F_ShippedLibUnsupportedType,
                                DiagnosticSeverity::Error,
                                "shipped-lib descriptor " + at + ": symbol '" + name
                                    + "' has a 'signatureByDataModel." + kv.key()
                                    + "' that failed to decode as a type ('" + ovText
                                    + "') — refusing a descriptor whose override "
                                      "would fail when that data model is selected");
                    overridesOk = false;
                    continue;
                }
                if (*dm == dataModel) effectiveSigText = ovText;
            }
        }
        if (!overridesOk) continue;

        // Reject unknown per-symbol keys (closed key set).
        (void)rejectUnknownKeys(reporter, sym, "symbols[" + std::to_string(idx - 1) + "]",
                                {"name", "signature", "signatureByDataModel",
                                 "kind", "linkage", "availableObjectFormats",
                                 "noreturn"});

        // Decode the signature via the ONE type-text decoder. A decode failure
        // is the CRITICAL fail-loud: F_ShippedLibUnsupportedType, and the
        // symbol is NEVER appended with InvalidType (it is dropped from `out`,
        // and the whole read fails via the errorCount delta below). The BASE
        // text is decoded even when an override is active (both must be
        // valid); the EFFECTIVE signature is the active model's.
        TypeId const baseSig = parseTypeFromText(sigText, interner, typeReg, reporter, namedTypes);
        if (!baseSig.valid() || baseSig == InvalidType) {
            dss::report(reporter, DiagnosticCode::F_ShippedLibUnsupportedType,
                        DiagnosticSeverity::Error,
                        "shipped-lib descriptor " + at + ": symbol '" + name
                            + "' has a 'signature' that failed to decode as a "
                              "type ('" + sigText + "') — refusing to synthesize "
                              "an extern with an unresolved signature");
            continue;
        }
        TypeId sig = baseSig;
        if (effectiveSigText != sigText) {
            sig = parseTypeFromText(effectiveSigText, interner, typeReg, reporter, namedTypes);
            // Already validated above; a second-parse failure here would be
            // interner drift — covered by the errorCount delta either way.
            if (!sig.valid() || sig == InvalidType) continue;
        }

        out.symbols.push_back(
            ShippedSymbol{std::move(name), sig, kind, linkage, std::move(symAvail),
                          noreturn});
    }

    // (5) Optional `constants` array — the neutral form of a header's object-
    // like `#define` macros that ARE compile-time constants (e.g. `CHAR_BIT`).
    // Each: required non-empty `name`; required hir-text `type` that MUST decode
    // to an INTEGER SCALAR (I8..U128); required integer `value` that MUST fit the
    // type's width + signedness. Collect-all (continue on error; the read still
    // fails via the errorCount delta). A non-integer-scalar type or an out-of-
    // range value FAILS LOUD — never a silent wrong constant.
    if (doc.contains("constants")) {
        if (!doc.at("constants").is_array()) {
            emitMalformed(reporter,
                std::string{"shipped-lib descriptor '"} + path.generic_string()
                    + "': 'constants' must be an array");
            return std::nullopt;
        }
        json const& constants = doc.at("constants");
        out.constants.reserve(constants.size());
        std::size_t cidx = 0;
        for (auto const& c : constants) {
            std::string const at = std::string{"'"} + path.generic_string()
                + "' constants[" + std::to_string(cidx) + "]";
            ++cidx;
            if (!c.is_object()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at + ": must be an object");
                continue;
            }
            (void)rejectUnknownKeys(reporter, c,
                                    "constants[" + std::to_string(cidx - 1) + "]",
                                    {"name", "value", "type", "variants"});
            if (!c.contains("name") || !c.at("name").is_string()
                || c.at("name").get<std::string>().empty()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": missing or empty 'name'");
                continue;
            }
            std::string cname = c.at("name").get<std::string>();

            // Exactly ONE of a flat `{value,type}` (single value, back-compat) or
            // per-target `variants` (plan 25 extension): a constant whose VALUE /
            // TYPE diverges per target (e.g. a per-platform `O_NONBLOCK`). The flat
            // path is signalled by EITHER `value` or `type` being present; the
            // variant path by `variants`. Both, or neither, is a malformed entry —
            // fail loud (the same XOR contract as the struct surface).
            bool const cHasFlat     = c.contains("value") || c.contains("type");
            bool const cHasVariants = c.contains("variants");
            if (cHasFlat == cHasVariants) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": a constant must declare EXACTLY one of a flat "
                                              "'value'+'type' (single value) or 'variants' "
                                              "(per-target values)");
                continue;
            }

            std::int64_t selValue = 0;
            TypeId       selType;
            bool         selected = false;

            if (cHasFlat) {
                // FLAT: decode {value,type} via the shared scalar-constant codec.
                if (!decodeConstantValueAndType(c, at, cname, interner, typeReg,
                                                reporter, selValue, selType,
                                                namedTypes)) {
                    continue;
                }
                selected = true;
            } else {
                // PER-TARGET VARIANTS. Decode EVERY variant's {value,type} (eager —
                // a malformed inactive variant fails the read on every target), then
                // select the variant whose `when` matches the active target.
                if (!c.at("variants").is_array() || c.at("variants").empty()) {
                    emitMalformed(reporter, "shipped-lib descriptor " + at
                                                + ": 'variants' must be a non-empty array");
                    continue;
                }
                std::string const activeFormatName =
                    activeFormat.has_value()
                        ? std::string{objectFormatKindName(*activeFormat)}
                        : std::string{};
                bool okVariants = true;
                std::size_t matchCount = 0;
                std::size_t vidx = 0;
                for (auto const& vdef : c.at("variants")) {
                    std::string const vat = at + " variants[" + std::to_string(vidx) + "]";
                    ++vidx;
                    if (!vdef.is_object()) {
                        emitMalformed(reporter, "shipped-lib descriptor " + vat
                                                    + ": must be an object");
                        okVariants = false; break;
                    }
                    (void)rejectUnknownKeys(reporter, vdef, vat, {"when", "value", "type"});
                    if (!vdef.contains("when") || !vdef.at("when").is_object()) {
                        emitMalformed(reporter, "shipped-lib descriptor " + vat
                                                    + ": missing or non-object 'when' "
                                                      "(e.g. {\"arch\":\"x86_64\",\"format\":\"elf\"})");
                        okVariants = false; break;
                    }
                    // Decode this variant's {value,type} EAGERLY (every variant), so
                    // a malformed inactive variant fails the read on every target.
                    std::int64_t vValue = 0;
                    TypeId       vType;
                    if (!decodeConstantValueAndType(vdef, vat, cname, interner, typeReg,
                                                    reporter, vValue, vType,
                                                    namedTypes)) {
                        okVariants = false; break;
                    }
                    WhenMatch const wm = matchVariantWhen(
                        vdef.at("when"), /*allowArch=*/true, vat + ".when",
                        activeTarget, activeFormat, activeFormatName, reporter);
                    if (wm == WhenMatch::Error) { okVariants = false; break; }
                    if (wm == WhenMatch::Match) {
                        ++matchCount;
                        if (matchCount == 1) { selValue = vValue; selType = vType; }
                    }
                }
                if (!okVariants) continue;
                if (matchCount > 1) {
                    dss::report(reporter, DiagnosticCode::F_ShippedConstantVariantAmbiguous,
                                DiagnosticSeverity::Error,
                                "shipped-lib descriptor " + at + ": constant '" + cname
                                    + "' has " + std::to_string(matchCount)
                                    + " 'variants' matching the active target (arch='"
                                    + (activeTarget.has_value() ? std::string{*activeTarget}
                                                                : std::string{"<none>"})
                                    + "', format='"
                                    + (activeFormat.has_value() ? activeFormatName
                                                                : std::string{"<none>"})
                                    + "') — exactly one variant may match (refusing an "
                                      "ambiguous per-target constant value)");
                    continue;
                }
                // matchCount 0 ⇒ no variant for this target ⇒ NOT injected (a
                // reference fails loud as an unknown identifier, never a silent wrong
                // value). matchCount 1 ⇒ select it.
                selected = (matchCount == 1);
            }

            if (!selected) continue;   // no variant matched → inject nothing
            out.constants.push_back(ShippedConstant{std::move(cname), selValue, selType});
        }
    }

    // (5.5) Optional `floatConstants` array (c52, D-FFI-MATH-INFINITY) — the
    // FLOAT-valued sibling of `constants` (which is integer-ONLY; a float there
    // still fails loud). A header's float object-like macros (`INFINITY`, `M_PI`,
    // `DBL_MAX`) ship here. Each: required non-empty `name`; required hir-text
    // `type` that MUST decode to a FLOAT SCALAR (F32/F64); required STRING `value`
    // (JSON has no Infinity literal — "inf"/"+inf"/"-inf" map to ±infinity, any
    // other string is a finite float literal). Collect-all (continue on error; the
    // read still fails via the errorCount delta). A non-float-scalar type or an
    // un-parseable / silently-overflowing value FAILS LOUD — never a silent wrong
    // constant. No per-target `variants` (every float constant here — INFINITY — is
    // target-invariant IEEE-754; a future per-target float would be its own cycle).
    if (doc.contains("floatConstants")) {
        if (!doc.at("floatConstants").is_array()) {
            emitMalformed(reporter,
                std::string{"shipped-lib descriptor '"} + path.generic_string()
                    + "': 'floatConstants' must be an array");
            return std::nullopt;
        }
        json const& fconstants = doc.at("floatConstants");
        out.floatConstants.reserve(fconstants.size());
        std::size_t fcidx = 0;
        for (auto const& c : fconstants) {
            std::string const at = std::string{"'"} + path.generic_string()
                + "' floatConstants[" + std::to_string(fcidx) + "]";
            ++fcidx;
            if (!c.is_object()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at + ": must be an object");
                continue;
            }
            (void)rejectUnknownKeys(reporter, c,
                                    "floatConstants[" + std::to_string(fcidx - 1) + "]",
                                    {"name", "value", "type"});
            if (!c.contains("name") || !c.at("name").is_string()
                || c.at("name").get<std::string>().empty()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": missing or empty 'name'");
                continue;
            }
            std::string cname = c.at("name").get<std::string>();

            // `type` must decode to a FLOAT SCALAR (F32/F64). A non-float-scalar
            // (or undecodable) type fails loud F_ShippedLibUnsupportedType — the
            // float-surface sibling of the integer gate (so an INTEGER in
            // `floatConstants` is just as out-of-scope as a float in `constants`).
            if (!c.contains("type") || !c.at("type").is_string()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": missing or non-string 'type'");
                continue;
            }
            std::string const typeText = c.at("type").get<std::string>();
            TypeId const cty = parseTypeFromText(typeText, interner, typeReg, reporter, namedTypes);
            if (!cty.valid() || cty == InvalidType) {
                dss::report(reporter, DiagnosticCode::F_ShippedLibUnsupportedType,
                            DiagnosticSeverity::Error,
                            "shipped-lib descriptor " + at + ": float constant '" + cname
                                + "' has a 'type' that failed to decode ('" + typeText + "')");
                continue;
            }
            if (!isFloatScalarKind(interner.kind(cty))) {
                dss::report(reporter, DiagnosticCode::F_ShippedLibUnsupportedType,
                            DiagnosticSeverity::Error,
                            "shipped-lib descriptor " + at + ": float constant '" + cname
                                + "' type '" + typeText + "' is not a float scalar "
                                  "(a 'floatConstants' entry must be f32/f64; an integer "
                                  "constant belongs in 'constants')");
                continue;
            }
            if (!c.contains("value")) {
                emitMalformed(reporter, "shipped-lib descriptor " + at + ": missing 'value'");
                continue;
            }
            auto const dv = decodeFloatConstantValue(c.at("value"));
            if (!dv.has_value()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at + ": float constant '"
                    + cname + "' has an invalid 'value' (expected a string: \"inf\"/\"+inf\"/"
                              "\"-inf\" or a finite float literal; an out-of-range finite "
                              "literal that overflows to infinity is rejected)");
                continue;
            }
            out.floatConstants.push_back(
                ShippedFloatConstant{std::move(cname), *dv, cty});
        }
    }

    // (6) Optional `typedefs` array — the neutral form of a header's `typedef`s
    // (e.g. `size_t`). Each: required non-empty `name`; required hir-text `type`
    // (any decodable type — scalar, pointer, struct ref, fn ptr). The semantic
    // phase injects each as a `DeclarationKind::Type` symbol.
    if (doc.contains("typedefs")) {
        if (!doc.at("typedefs").is_array()) {
            emitMalformed(reporter,
                std::string{"shipped-lib descriptor '"} + path.generic_string()
                    + "': 'typedefs' must be an array");
            return std::nullopt;
        }
        json const& typedefs = doc.at("typedefs");
        out.typedefs.reserve(typedefs.size());
        std::size_t tidx = 0;
        for (auto const& t : typedefs) {
            std::string const at = std::string{"'"} + path.generic_string()
                + "' typedefs[" + std::to_string(tidx) + "]";
            ++tidx;
            if (!t.is_object()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at + ": must be an object");
                continue;
            }
            (void)rejectUnknownKeys(reporter, t,
                                    "typedefs[" + std::to_string(tidx - 1) + "]",
                                    {"name", "type", "variants"});
            if (!t.contains("name") || !t.at("name").is_string()
                || t.at("name").get<std::string>().empty()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": missing or empty 'name'");
                continue;
            }
            std::string tname = t.at("name").get<std::string>();

            // Decode the `type` field of a typedef entry/variant (any decodable
            // type — scalar, pointer, struct ref, fn ptr) via the ONE codec; fail
            // loud on a missing/undecodable type. Returns the TypeId, or InvalidType
            // (the caller skips on invalid). `ctx` is the diagnostic context.
            auto decodeTypedefType = [&](json const& obj, std::string const& ctx) -> TypeId {
                if (!obj.contains("type") || !obj.at("type").is_string()) {
                    emitMalformed(reporter, "shipped-lib descriptor " + ctx
                                                + ": missing or non-string 'type'");
                    return InvalidType;
                }
                std::string const typeText = obj.at("type").get<std::string>();
                TypeId const ty = parseTypeFromText(typeText, interner, typeReg, reporter, namedTypes);
                if (!ty.valid() || ty == InvalidType) {
                    dss::report(reporter, DiagnosticCode::F_ShippedLibUnsupportedType,
                                DiagnosticSeverity::Error,
                                "shipped-lib descriptor " + ctx + ": typedef '" + tname
                                    + "' has a 'type' that failed to decode ('" + typeText
                                    + "')");
                    return InvalidType;
                }
                return ty;
            };

            // Exactly ONE of a flat `type` (single, back-compat) or per-target
            // `variants` (plan 25 extension): the name is INVARIANT; only the
            // type/width varies per target (e.g. a `wchar_t` that is 32-bit on elf
            // but 16-bit on pe). Both, or neither, is malformed — fail loud.
            bool const tHasFlat     = t.contains("type");
            bool const tHasVariants = t.contains("variants");
            if (tHasFlat == tHasVariants) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": a typedef must declare EXACTLY one of a flat "
                                              "'type' (single) or 'variants' (per-target types)");
                continue;
            }

            TypeId selType;
            bool   selected = false;

            if (tHasFlat) {
                TypeId const tty = decodeTypedefType(t, at);
                if (tty == InvalidType) continue;
                selType  = tty;
                selected = true;
            } else {
                // PER-TARGET VARIANTS. Decode EVERY variant's `type` EAGERLY, then
                // select the variant whose `when` matches the active target.
                if (!t.at("variants").is_array() || t.at("variants").empty()) {
                    emitMalformed(reporter, "shipped-lib descriptor " + at
                                                + ": 'variants' must be a non-empty array");
                    continue;
                }
                std::string const activeFormatName =
                    activeFormat.has_value()
                        ? std::string{objectFormatKindName(*activeFormat)}
                        : std::string{};
                bool okVariants = true;
                std::size_t matchCount = 0;
                std::size_t vidx = 0;
                for (auto const& vdef : t.at("variants")) {
                    std::string const vat = at + " variants[" + std::to_string(vidx) + "]";
                    ++vidx;
                    if (!vdef.is_object()) {
                        emitMalformed(reporter, "shipped-lib descriptor " + vat
                                                    + ": must be an object");
                        okVariants = false; break;
                    }
                    (void)rejectUnknownKeys(reporter, vdef, vat, {"when", "type"});
                    if (!vdef.contains("when") || !vdef.at("when").is_object()) {
                        emitMalformed(reporter, "shipped-lib descriptor " + vat
                                                    + ": missing or non-object 'when' "
                                                      "(e.g. {\"arch\":\"x86_64\",\"format\":\"elf\"})");
                        okVariants = false; break;
                    }
                    TypeId const vType = decodeTypedefType(vdef, vat);   // EAGER
                    if (vType == InvalidType) { okVariants = false; break; }
                    WhenMatch const wm = matchVariantWhen(
                        vdef.at("when"), /*allowArch=*/true, vat + ".when",
                        activeTarget, activeFormat, activeFormatName, reporter);
                    if (wm == WhenMatch::Error) { okVariants = false; break; }
                    if (wm == WhenMatch::Match) {
                        ++matchCount;
                        if (matchCount == 1) selType = vType;
                    }
                }
                if (!okVariants) continue;
                if (matchCount > 1) {
                    dss::report(reporter, DiagnosticCode::F_ShippedTypedefVariantAmbiguous,
                                DiagnosticSeverity::Error,
                                "shipped-lib descriptor " + at + ": typedef '" + tname
                                    + "' has " + std::to_string(matchCount)
                                    + " 'variants' matching the active target (arch='"
                                    + (activeTarget.has_value() ? std::string{*activeTarget}
                                                                : std::string{"<none>"})
                                    + "', format='"
                                    + (activeFormat.has_value() ? activeFormatName
                                                                : std::string{"<none>"})
                                    + "') — exactly one variant may match (refusing an "
                                      "ambiguous per-target typedef type)");
                    continue;
                }
                selected = (matchCount == 1);   // 0 ⇒ not injected
            }

            if (!selected) continue;   // no variant matched → inject nothing
            out.typedefs.push_back(ShippedTypedef{std::move(tname), selType});
        }
    }

    // (6.5) STRUCTS (named-field aggregate types). Each entry interns a struct
    // type (name + positional field types) the semantic phase injects as a TAG +
    // a field scope; the layout engine DERIVES the ABI byte offsets from the
    // field sizes (the descriptor declares names + types, never offsets).
    //
    // A struct entry declares EITHER a flat `fields` (single layout, back-compat)
    // OR per-target `variants` (plan 25): a list of `{ "when": {arch?,format?},
    // "fields":[…] }`, the variant matching the active (arch,format) selected so
    // a struct can carry the correct per-target byte layout. The CRUX
    // (plan-lock-VERIFIED): x86_64/arm64 AggregateLayoutParams are byte-identical +
    // computeLayout is param-driven (no arch branch) → the per-target offset delta
    // comes ENTIRELY from the selected FIELD LIST. The active identity is
    // (arch = `*activeTarget`, format = `objectFormatKindName(*activeFormat)`); a
    // variant matches iff EVERY key its `when` specifies equals the active value
    // (GENERIC string equality — never an `if (arch == "x86_64")` here). >1 match
    // ⇒ fail loud (F_ShippedStructVariantAmbiguous: an under-specified `when` would
    // otherwise silently pick the first → a wrong layout). 0 match (variants
    // present) ⇒ NOT injected (a reference fails loud as an undefined type, never a
    // silent wrong layout). EAGER: EVERY variant's field list is decoded regardless
    // of which is active, so a malformed INACTIVE variant fails the read on EVERY
    // target (anti-lurking, mirrors `signatureByDataModel`).
    if (doc.contains("structs")) {
        if (!doc.at("structs").is_array()) {
            emitMalformed(reporter, "shipped-lib descriptor '" + path.generic_string()
                                        + "': 'structs' must be an array");
        } else {
            std::size_t sidx = 0;
            for (auto const& sdef : doc.at("structs")) {
                std::string const at =
                    "'" + path.generic_string() + "' structs[" + std::to_string(sidx) + "]";
                ++sidx;
                if (!sdef.is_object()) {
                    emitMalformed(reporter, "shipped-lib descriptor " + at + ": must be an object");
                    continue;
                }
                (void)rejectUnknownKeys(reporter, sdef,
                                        "structs[" + std::to_string(sidx - 1) + "]",
                                        {"name", "fields", "variants"});
                if (!sdef.contains("name") || !sdef.at("name").is_string()
                    || sdef.at("name").get<std::string>().empty()) {
                    emitMalformed(reporter, "shipped-lib descriptor " + at
                                                + ": missing or empty 'name'");
                    continue;
                }
                std::string const sname = sdef.at("name").get<std::string>();

                // Exactly ONE of `fields` (flat, single layout) or `variants`
                // (per-target). Both, or neither, is a malformed entry — fail loud
                // (a struct with neither declares no layout; with both is ambiguous
                // intent).
                bool const hasFields   = sdef.contains("fields");
                bool const hasVariants = sdef.contains("variants");
                if (hasFields == hasVariants) {
                    emitMalformed(reporter, "shipped-lib descriptor " + at
                                                + ": a struct must declare EXACTLY one of "
                                                  "'fields' (single layout) or 'variants' "
                                                  "(per-target layouts)");
                    continue;
                }

                // The SELECTED field list (flat path = `fields`; variant path = the
                // matching variant's `fields`). `selected` stays false on the
                // no-variant-matches case → the struct is simply not injected.
                ShippedStruct sst;
                sst.name = sname;
                std::vector<TypeId> fieldTypes;
                bool selected = false;

                if (hasFields) {
                    if (!sdef.at("fields").is_array() || sdef.at("fields").empty()) {
                        emitMalformed(reporter, "shipped-lib descriptor " + at
                                                    + ": 'fields' must be a non-empty array");
                        continue;
                    }
                    if (!decodeStructFieldList(sdef.at("fields"), at, interner, typeReg,
                                               reporter, sst.fields, fieldTypes,
                                               namedTypes)) {
                        continue;
                    }
                    selected = true;
                } else {
                    // PER-TARGET VARIANTS. Decode EVERY variant's field list (eager —
                    // a malformed inactive variant fails the read on every target),
                    // then select the variant whose `when` matches the active target.
                    if (!sdef.at("variants").is_array() || sdef.at("variants").empty()) {
                        emitMalformed(reporter, "shipped-lib descriptor " + at
                                                    + ": 'variants' must be a non-empty array");
                        continue;
                    }
                    std::string const activeFormatName =
                        activeFormat.has_value()
                            ? std::string{objectFormatKindName(*activeFormat)}
                            : std::string{};
                    bool okVariants = true;
                    std::size_t matchCount = 0;
                    std::size_t vidx = 0;
                    for (auto const& vdef : sdef.at("variants")) {
                        std::string const vat = at + " variants[" + std::to_string(vidx) + "]";
                        ++vidx;
                        if (!vdef.is_object()) {
                            emitMalformed(reporter, "shipped-lib descriptor " + vat
                                                        + ": must be an object");
                            okVariants = false; break;
                        }
                        (void)rejectUnknownKeys(reporter, vdef,
                                                "structs[" + std::to_string(sidx - 1)
                                                    + "] variants[" + std::to_string(vidx - 1) + "]",
                                                {"when", "fields"});
                        // `when` — the match selector. REQUIRED object; keys closed to
                        // {arch, format}. An EMPTY `when:{}` matches every target
                        // (always-match) — legal but typically ambiguous if any other
                        // variant also matches (the ambiguity gate below catches it).
                        if (!vdef.contains("when") || !vdef.at("when").is_object()) {
                            emitMalformed(reporter, "shipped-lib descriptor " + vat
                                                        + ": missing or non-object 'when' "
                                                          "(e.g. {\"arch\":\"x86_64\",\"format\":\"elf\"})");
                            okVariants = false; break;
                        }
                        // Decode this variant's field list (eager, every variant) —
                        // BEFORE the match test so a malformed INACTIVE variant still
                        // fails the read on every target (anti-lurking).
                        if (!vdef.contains("fields") || !vdef.at("fields").is_array()
                            || vdef.at("fields").empty()) {
                            emitMalformed(reporter, "shipped-lib descriptor " + vat
                                                        + ": 'fields' must be a non-empty array");
                            okVariants = false; break;
                        }
                        std::vector<ShippedField> vFields;
                        std::vector<TypeId>       vFieldTypes;
                        if (!decodeStructFieldList(vdef.at("fields"), vat, interner, typeReg,
                                                   reporter, vFields, vFieldTypes, namedTypes)) {
                            okVariants = false; break;
                        }

                        // Does this variant's `when` MATCH the active target? (the
                        // SHARED selector — typed surfaces allow {arch,format}.)
                        WhenMatch const wm = matchVariantWhen(
                            vdef.at("when"), /*allowArch=*/true, vat + ".when",
                            activeTarget, activeFormat, activeFormatName, reporter);
                        if (wm == WhenMatch::Error) { okVariants = false; break; }
                        if (wm == WhenMatch::Match) {
                            ++matchCount;
                            if (matchCount == 1) {
                                // First match — take its fields (and keep scanning so a
                                // second match is detected → ambiguity fail-loud).
                                sst.fields = std::move(vFields);
                                fieldTypes = std::move(vFieldTypes);
                            }
                        }
                    }
                    if (!okVariants) continue;
                    if (matchCount > 1) {
                        // >1 variant matched the active target — a silent
                        // wrong-layout risk (which would be picked?). Fail loud.
                        dss::report(reporter, DiagnosticCode::F_ShippedStructVariantAmbiguous,
                                    DiagnosticSeverity::Error,
                                    "shipped-lib descriptor " + at + ": struct '" + sname
                                        + "' has " + std::to_string(matchCount)
                                        + " 'variants' matching the active target (arch='"
                                        + (activeTarget.has_value() ? std::string{*activeTarget}
                                                                    : std::string{"<none>"})
                                        + "', format='"
                                        + (activeFormat.has_value() ? activeFormatName
                                                                    : std::string{"<none>"})
                                        + "') — exactly one variant may match (refusing an "
                                          "ambiguous per-target layout)");
                        continue;
                    }
                    // matchCount 0 ⇒ no variant for this target ⇒ NOT injected
                    // (a reference fails loud as an undefined type, never a silent
                    // wrong layout). matchCount 1 ⇒ select it.
                    selected = (matchCount == 1);
                }

                if (!selected) continue;   // no variant matched → inject nothing
                // c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): if the SELECTED field list
                // carries explicit offsets, intern the struct WITH them (an
                // overlapping FFI layout). ALL-or-NONE within the struct; a mix is
                // malformed. The offsets enter the content identity so this tag type
                // matches the bare typedef's inline `struct "X" { T @off }` (same
                // TypeId → the injected field scope resolves .member on a bare-typedef
                // value). Empty → the ordinary natural-alignment struct (unchanged).
                std::size_t withOffset = 0;
                for (auto const& fld : sst.fields)
                    if (fld.offset.has_value()) ++withOffset;
                if (withOffset != 0 && withOffset != sst.fields.size()) {
                    emitMalformed(reporter, "shipped-lib descriptor " + at
                                                + ": struct field 'offset' must be "
                                                  "all-or-none (an overlapping layout "
                                                  "declares every field's offset)");
                    continue;
                }
                if (withOffset == sst.fields.size() && !sst.fields.empty()) {
                    std::vector<std::uint64_t> offsets;
                    offsets.reserve(sst.fields.size());
                    for (auto const& fld : sst.fields) offsets.push_back(*fld.offset);
                    std::span<std::int64_t const> const noWidths{};
                    sst.typeId = interner.structType(sname, fieldTypes, noWidths, offsets);
                } else {
                    sst.typeId = interner.structType(sname, fieldTypes);
                }
                out.structs.push_back(std::move(sst));
            }
        }
    }

    // (7) MACROS (the preprocessor-macro surface, interner-free). A function-like
    // or object-like `#define` the preprocessor injects when this header is
    // included (e.g. `assert(e) -> ((void)0)`). Per-FORMAT macro variants select
    // on the active format (the semantic read carries it; arch is not part of a
    // macro selector).
    decodeShippedMacros(doc, path.generic_string(), reporter, out.macros, activeFormat);

    // (8) A descriptor must declare SOMETHING — a file with no symbols, no
    // constants, no typedefs, AND no macros is a no-op artifact that should not
    // ship silently (mirrors the old non-empty-`symbols` rule, now spanning all
    // surfaces). Plan 25: a descriptor whose ONLY surface is PER-TARGET `variants`
    // (structs OR constants OR typedefs OR macros) injects ZERO of that surface
    // when decoded with no active target/format (the nullopt direct-API / LSP /
    // `AllShippedDescriptorsDecode`-provenance path) — yet it genuinely DECLARES
    // that surface (target-conditional, e.g. the real variants-only <sys/stat.h>,
    // or a per-format errno macro descriptor). Count the JSON DECLARATION, not the
    // post-selection injection, for EVERY variant-capable surface so a well-formed
    // variants-only header is not a false "declares nothing".
    auto const declaresArray = [&](char const* key) {
        return doc.contains(key) && doc.at(key).is_array() && !doc.at(key).empty();
    };
    bool const declaredStructs        = declaresArray("structs");
    bool const declaredConstants      = declaresArray("constants");
    bool const declaredTypedefs       = declaresArray("typedefs");
    bool const declaredMacroVariants  = declaresArray("macros");
    if (out.symbols.empty() && out.constants.empty() && out.floatConstants.empty()
        && out.typedefs.empty() && out.structs.empty() && out.macros.empty()
        && !declaredStructs && !declaredConstants && !declaredTypedefs
        && !declaredMacroVariants) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor '"} + path.generic_string()
                + "': declares nothing — needs at least one of 'symbols', "
                  "'constants', 'floatConstants', 'typedefs', 'structs', or 'macros'");
        return std::nullopt;
    }

    // Fail loud, never partial: if ANY diagnostic was emitted while reading
    // (shape, key, enum, or a signature that didn't decode), the descriptor is
    // not usable — return nullopt rather than hand back a partial surface that
    // would silently drop symbols.
    if (reporter.errorCount() != errBefore) return std::nullopt;
    return out;
}

std::optional<std::vector<ShippedMacro>>
readShippedLibMacros(std::filesystem::path const&    path,
                     DiagnosticReporter&             reporter,
                     std::optional<ObjectFormatKind> activeFormat) {
    std::size_t const errBefore = reporter.errorCount();

    // Read + parse — same provenance gate as readShippedLibDescriptor, but the
    // typed surfaces (which need a TypeInterner) are NOT read here; the semantic
    // phase reads + validates those separately via readShippedLibDescriptor.
    json const* const docPtr = cachedDescriptorJson(path, reporter);
    if (!docPtr) return std::nullopt;
    json const& doc = *docPtr;
    // NOTE: the `header` provenance gate + the typed-surface validation are the
    // SEMANTIC read's job (readShippedLibDescriptor) — NOT repeated here. The
    // macros-only read must be no STRICTER than the full read (a header-less or
    // symbols-only descriptor is read for its macros [usually none] WITHOUT a new
    // error; the semantic read reports any real provenance/typed-surface defect).
    // Only MALFORMED macros (decodeShippedMacros below) + a broken JSON fail loud.

    std::vector<ShippedMacro> out;
    decodeShippedMacros(doc, path.generic_string(), reporter, out, activeFormat);
    if (reporter.errorCount() != errBefore) return std::nullopt;
    return out;  // empty when the descriptor declares no `macros` (typed-only)
}

std::optional<std::vector<std::string>>
readShippedLibAvailability(std::filesystem::path const& path,
                           DiagnosticReporter&          reporter) {
    // Interner-FREE per-target AVAILABILITY read for the FRONT-END gate (the
    // preprocessor `__has_include` callback + the import resolver's `#include`,
    // neither of which has a TypeInterner). Returns the `availableObjectFormats`
    // set (EMPTY ⇒ available on every format = back-compat); std::nullopt on a
    // broken JSON / malformed availability. No `header` or typed-surface gate —
    // the semantic read owns those (this must be no STRICTER than the full read).
    std::size_t const errBefore = reporter.errorCount();
    json const* const docPtr = cachedDescriptorJson(path, reporter);
    if (!docPtr) return std::nullopt;
    json const& doc = *docPtr;
    std::vector<std::string> out;
    decodeShippedAvailability(doc, path.generic_string(), reporter, out);
    if (reporter.errorCount() != errBefore) return std::nullopt;
    return out;  // empty ⇒ available on every format
}

std::optional<std::vector<std::string>>
readShippedLibTypedefNames(std::filesystem::path const& path,
                           DiagnosticReporter&          reporter) {
    // Interner-FREE TYPEDEF-NAME read for the parse-time cast-vs-call ORACLE
    // (D-CSUBSET-SHIPPED-TYPEDEF-CAST-PARSE): the post-parse typedef-resolution
    // reparse (compilation_unit.cpp `finish()`) seeds these names as parse-time
    // global TYPE NAMES so a shipped-typedef `(size_t)(expr)` parses as a CAST, not
    // a call. Only the NAMES are needed (not the decoded `type`), so no
    // TypeInterner — mirrors readShippedLibAvailability. LENIENT: a malformed entry
    // is skipped (no name to harvest); the SEMANTIC read (readShippedLibDescriptor)
    // owns strict typedef validation, so this stays no STRICTER than the full read
    // and never double-reports. nullopt only on a broken JSON.
    std::size_t const errBefore = reporter.errorCount();
    json const* const docPtr = cachedDescriptorJson(path, reporter);
    if (!docPtr) return std::nullopt;
    json const& doc = *docPtr;
    std::vector<std::string> out;
    if (doc.contains("typedefs") && doc.at("typedefs").is_array()) {
        for (auto const& t : doc.at("typedefs")) {
            if (t.is_object() && t.contains("name") && t.at("name").is_string()) {
                std::string name = t.at("name").get<std::string>();
                if (!name.empty()) out.push_back(std::move(name));
            }
        }
    }
    if (reporter.errorCount() != errBefore) return std::nullopt;
    return out;  // empty ⇒ no typedef surface (the oracle learns nothing new)
}

bool objectFormatInAvailabilitySet(std::span<std::string const> availableObjectFormats,
                                   ObjectFormatKind fmt) {
    if (availableObjectFormats.empty()) return true;  // empty ⇒ every format
    std::string const name{objectFormatKindName(fmt)};
    return std::find(availableObjectFormats.begin(), availableObjectFormats.end(), name)
           != availableObjectFormats.end();
}

bool shippedHeaderAvailableForFormat(std::filesystem::path const& descriptorPath,
                                     ObjectFormatKind fmt) {
    // Interner-free read with a THROWAWAY reporter: a malformed availability is
    // surfaced by the macros / typed reads on the SAME descriptor (never silent),
    // so __has_include / the splice must not double-report it here — and a header
    // whose descriptor EXISTS satisfies this existence-class test regardless.
    DiagnosticReporter throwaway;
    auto avail = readShippedLibAvailability(descriptorPath, throwaway);
    if (!avail) return true;
    return objectFormatInAvailabilitySet(*avail, fmt);
}

} // namespace dss::ffi
