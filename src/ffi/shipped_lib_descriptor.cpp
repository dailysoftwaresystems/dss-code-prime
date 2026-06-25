#include "ffi/shipped_lib_descriptor.hpp"

#include "core/types/data_model.hpp"             // dataModelFromName (signatureByDataModel keys)
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"     // objectFormatKindFromName (library-map key vocabulary)
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"            // InvalidType
#include "core/types/type_lattice/core_type.hpp"   // TypeKind (constant integer-scalar gate)
#include "core/types/type_lattice/type_interner.hpp" // TypeInterner::kind (constant type gate)
#include "hir/hir_text.hpp"                      // parseTypeFromText (the ONE type decoder)

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>
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

// Decode the optional `macros` array (the preprocessor-macro surface) into
// `out`. Collect-all: a malformed entry is reported (the caller's errorCount
// delta then fails the whole read) and the loop continues. Interner-FREE — a
// macro is pure preprocessor token TEXT (no types), so the preprocessor (which
// has no interner) reuses this. Shared by `readShippedLibDescriptor` (full read)
// and `readShippedLibMacros` (the preprocessor's macros-only read).
void decodeShippedMacros(json const& doc, std::string const& pathStr,
                         DiagnosticReporter& reporter,
                         std::vector<ShippedMacro>& out) {
    if (!doc.contains("macros")) return;
    if (!doc.at("macros").is_array()) {
        emitMalformed(reporter, "shipped-lib descriptor '" + pathStr
                                    + "': 'macros' must be an array");
        return;
    }
    json const& macros = doc.at("macros");
    out.reserve(out.size() + macros.size());
    std::size_t midx = 0;
    for (auto const& m : macros) {
        std::string const at = "'" + pathStr + "' macros[" + std::to_string(midx) + "]";
        ++midx;
        if (!m.is_object()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at + ": must be an object");
            continue;
        }
        (void)rejectUnknownKeys(reporter, m, "macros[" + std::to_string(midx - 1) + "]",
                                {"name", "params", "replacement", "variadic"});
        if (!m.contains("name") || !m.at("name").is_string()
            || m.at("name").get<std::string>().empty()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at
                                        + ": missing or empty 'name'");
            continue;
        }
        ShippedMacro macro;
        macro.name = m.at("name").get<std::string>();
        // params: ABSENT = object-like; PRESENT (even []) = function-like.
        if (m.contains("params")) {
            if (!m.at("params").is_array()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": 'params' must be an array of strings");
                continue;
            }
            std::vector<std::string> params;
            bool okParams = true;
            for (auto const& p : m.at("params")) {
                if (!p.is_string() || p.get<std::string>().empty()) {
                    emitMalformed(reporter, "shipped-lib descriptor " + at
                                                + ": every 'params' entry must be a non-empty "
                                                  "string");
                    okParams = false;
                    break;
                }
                params.push_back(p.get<std::string>());
            }
            if (!okParams) continue;
            macro.params = std::move(params);
        }
        // replacement: optional string (default empty — a null macro `#define X`).
        if (m.contains("replacement")) {
            if (!m.at("replacement").is_string()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": 'replacement' must be a string");
                continue;
            }
            macro.replacement = m.at("replacement").get<std::string>();
        }
        // variadic: optional bool; an object-like macro cannot be variadic.
        if (m.contains("variadic")) {
            if (!m.at("variadic").is_boolean()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": 'variadic' must be a boolean");
                continue;
            }
            macro.variadic = m.at("variadic").get<bool>();
            if (macro.variadic && !macro.params.has_value()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": 'variadic' requires 'params' (an object-like "
                                              "macro cannot be variadic)");
                continue;
            }
        }
        out.push_back(std::move(macro));
    }
}

} // namespace

std::optional<ShippedLibDescriptor>
readShippedLibDescriptor(std::filesystem::path const& path,
                         TypeInterner&                interner,
                         TypeRegistry&                typeReg,
                         DiagnosticReporter&          reporter,
                         DataModel                    dataModel) {
    std::size_t const errBefore = reporter.errorCount();

    // (0) Read the file. A missing/unreadable descriptor is malformed-shaped
    // from this reader's perspective (the resolver already verified existence
    // before recording the path, so a failure here is a real I/O fault, not a
    // soft miss). Fail loud rather than synthesize nothing silently.
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor: failed to open '"}
                + path.generic_string() + "' for reading");
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string const text = ss.str();

    // (1) Parse JSON. A parse failure is C_MalformedJson — but route the
    // descriptor-specific surface through F_ShippedLibDescriptorMalformed so
    // the operator sees the descriptor context (the codebase pattern emits
    // C_MalformedJson; here the dedicated FFI code is the right remediation
    // audience and is unsuppressable).
    json doc;
    try {
        doc = json::parse(text);
    } catch (json::parse_error const& e) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor '"} + path.generic_string()
                + "': JSON parse error: " + e.what());
        return std::nullopt;
    }

    if (!doc.is_object()) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor '"} + path.generic_string()
                + "': top-level value must be a JSON object");
        return std::nullopt;
    }

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
                            {"header", "standard", "library", "symbols",
                             "constants", "typedefs", "macros", "$comment"});

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
                    parseTypeFromText(ovText, interner, typeReg, reporter);
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
                                 "kind", "linkage"});

        // Decode the signature via the ONE type-text decoder. A decode failure
        // is the CRITICAL fail-loud: F_ShippedLibUnsupportedType, and the
        // symbol is NEVER appended with InvalidType (it is dropped from `out`,
        // and the whole read fails via the errorCount delta below). The BASE
        // text is decoded even when an override is active (both must be
        // valid); the EFFECTIVE signature is the active model's.
        TypeId const baseSig = parseTypeFromText(sigText, interner, typeReg, reporter);
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
            sig = parseTypeFromText(effectiveSigText, interner, typeReg, reporter);
            // Already validated above; a second-parse failure here would be
            // interner drift — covered by the errorCount delta either way.
            if (!sig.valid() || sig == InvalidType) continue;
        }

        out.symbols.push_back(ShippedSymbol{std::move(name), sig, kind, linkage});
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
                                    {"name", "value", "type"});
            if (!c.contains("name") || !c.at("name").is_string()
                || c.at("name").get<std::string>().empty()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": missing or empty 'name'");
                continue;
            }
            std::string cname = c.at("name").get<std::string>();
            if (!c.contains("type") || !c.at("type").is_string()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": missing or non-string 'type'");
                continue;
            }
            std::string const typeText = c.at("type").get<std::string>();
            TypeId const cty = parseTypeFromText(typeText, interner, typeReg, reporter);
            if (!cty.valid() || cty == InvalidType) {
                dss::report(reporter, DiagnosticCode::F_ShippedLibUnsupportedType,
                            DiagnosticSeverity::Error,
                            "shipped-lib descriptor " + at + ": constant '" + cname
                                + "' has a 'type' that failed to decode ('" + typeText
                                + "')");
                continue;
            }
            if (!isIntegerScalarKind(interner.kind(cty))) {
                dss::report(reporter, DiagnosticCode::F_ShippedLibUnsupportedType,
                            DiagnosticSeverity::Error,
                            "shipped-lib descriptor " + at + ": constant '" + cname
                                + "' type '" + typeText + "' is not an integer scalar "
                                  "(a shipped constant must be an integer; a float / "
                                  "pointer / function-like macro is out of scope)");
                continue;
            }
            if (!c.contains("value") || !c.at("value").is_number()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": missing or non-numeric 'value'");
                continue;
            }
            auto const bits = decodeConstantValue(c.at("value"), interner.kind(cty));
            if (!bits.has_value()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at + ": constant '"
                    + cname + "' value does not fit its declared integer type '"
                    + typeText + "' (out of range, negative-for-unsigned, or non-integer)");
                continue;
            }
            out.constants.push_back(ShippedConstant{std::move(cname), *bits, cty});
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
                                    {"name", "type"});
            if (!t.contains("name") || !t.at("name").is_string()
                || t.at("name").get<std::string>().empty()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": missing or empty 'name'");
                continue;
            }
            std::string tname = t.at("name").get<std::string>();
            if (!t.contains("type") || !t.at("type").is_string()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": missing or non-string 'type'");
                continue;
            }
            std::string const typeText = t.at("type").get<std::string>();
            TypeId const tty = parseTypeFromText(typeText, interner, typeReg, reporter);
            if (!tty.valid() || tty == InvalidType) {
                dss::report(reporter, DiagnosticCode::F_ShippedLibUnsupportedType,
                            DiagnosticSeverity::Error,
                            "shipped-lib descriptor " + at + ": typedef '" + tname
                                + "' has a 'type' that failed to decode ('" + typeText
                                + "')");
                continue;
            }
            out.typedefs.push_back(ShippedTypedef{std::move(tname), tty});
        }
    }

    // (7) MACROS (the preprocessor-macro surface, interner-free). A function-like
    // or object-like `#define` the preprocessor injects when this header is
    // included (e.g. `assert(e) -> ((void)0)`).
    decodeShippedMacros(doc, path.generic_string(), reporter, out.macros);

    // (8) A descriptor must declare SOMETHING — a file with no symbols, no
    // constants, no typedefs, AND no macros is a no-op artifact that should not
    // ship silently (mirrors the old non-empty-`symbols` rule, now spanning all
    // four surfaces).
    if (out.symbols.empty() && out.constants.empty() && out.typedefs.empty()
        && out.macros.empty()) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor '"} + path.generic_string()
                + "': declares nothing — needs at least one of 'symbols', "
                  "'constants', 'typedefs', or 'macros'");
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
readShippedLibMacros(std::filesystem::path const& path,
                     DiagnosticReporter&          reporter) {
    std::size_t const errBefore = reporter.errorCount();

    // Read + parse — same provenance gate as readShippedLibDescriptor, but the
    // typed surfaces (which need a TypeInterner) are NOT read here; the semantic
    // phase reads + validates those separately via readShippedLibDescriptor.
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor: failed to open '"}
                + path.generic_string() + "' for reading");
        return std::nullopt;
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
        return std::nullopt;
    }
    if (!doc.is_object()) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor '"} + path.generic_string()
                + "': top-level value must be a JSON object");
        return std::nullopt;
    }
    // NOTE: the `header` provenance gate + the typed-surface validation are the
    // SEMANTIC read's job (readShippedLibDescriptor) — NOT repeated here. The
    // macros-only read must be no STRICTER than the full read (a header-less or
    // symbols-only descriptor is read for its macros [usually none] WITHOUT a new
    // error; the semantic read reports any real provenance/typed-surface defect).
    // Only MALFORMED macros (decodeShippedMacros below) + a broken JSON fail loud.

    std::vector<ShippedMacro> out;
    decodeShippedMacros(doc, path.generic_string(), reporter, out);
    if (reporter.errorCount() != errBefore) return std::nullopt;
    return out;  // empty when the descriptor declares no `macros` (typed-only)
}

} // namespace dss::ffi
