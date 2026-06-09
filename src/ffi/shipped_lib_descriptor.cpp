#include "ffi/shipped_lib_descriptor.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"     // objectFormatKindFromName (library-map key vocabulary)
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"            // InvalidType
#include "hir/hir_text.hpp"                      // parseTypeFromText (the ONE type decoder)

#include <nlohmann/json.hpp>

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

} // namespace

std::optional<ShippedLibDescriptor>
readShippedLibDescriptor(std::filesystem::path const& path,
                         TypeInterner&                interner,
                         TypeRegistry&                typeReg,
                         DiagnosticReporter&          reporter) {
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

    // (3) Required `symbols` array (non-empty — a descriptor that declares no
    // symbol is a no-op artifact that should not ship silently).
    if (!doc.contains("symbols") || !doc.at("symbols").is_array()) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor '"} + path.generic_string()
                + "': missing or non-array 'symbols'");
        return std::nullopt;
    }
    json const& symbols = doc.at("symbols");
    if (symbols.empty()) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor '"} + path.generic_string()
                + "': 'symbols' array must contain at least one symbol");
        return std::nullopt;
    }

    // Reject unknown top-level keys (closed key set). `$comment` is the
    // repo-wide config-documentation convention (a `$`-prefixed key carrying a
    // human note, e.g. the LP64-vs-LLP64 deferral rationale in stdio/stdlib) —
    // accepted + ignored, never consumed by lowering.
    (void)rejectUnknownKeys(reporter, doc, "(root)",
                            {"header", "standard", "library", "symbols", "$comment"});

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

        // Reject unknown per-symbol keys (closed key set).
        (void)rejectUnknownKeys(reporter, sym, "symbols[" + std::to_string(idx - 1) + "]",
                                {"name", "signature", "kind", "linkage"});

        // Decode the signature via the ONE type-text decoder. A decode failure
        // is the CRITICAL fail-loud: F_ShippedLibUnsupportedType, and the
        // symbol is NEVER appended with InvalidType (it is dropped from `out`,
        // and the whole read fails via the errorCount delta below).
        TypeId const sig = parseTypeFromText(sigText, interner, typeReg, reporter);
        if (!sig.valid() || sig == InvalidType) {
            dss::report(reporter, DiagnosticCode::F_ShippedLibUnsupportedType,
                        DiagnosticSeverity::Error,
                        "shipped-lib descriptor " + at + ": symbol '" + name
                            + "' has a 'signature' that failed to decode as a "
                              "type ('" + sigText + "') — refusing to synthesize "
                              "an extern with an unresolved signature");
            continue;
        }

        out.symbols.push_back(ShippedSymbol{std::move(name), sig, kind, linkage});
    }

    // Fail loud, never partial: if ANY diagnostic was emitted while reading
    // (shape, key, enum, or a signature that didn't decode), the descriptor is
    // not usable — return nullopt rather than hand back a partial surface that
    // would silently drop symbols.
    if (reporter.errorCount() != errBefore) return std::nullopt;
    return out;
}

} // namespace dss::ffi
