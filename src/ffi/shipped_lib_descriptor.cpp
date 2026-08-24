#include "ffi/shipped_lib_descriptor.hpp"

#include "core/substrate/checked_file_read.hpp"    // the ONE checked whole-file read
#include "core/types/config_key_vocabulary.hpp"   // the ONE closed-key check + the `$`-prose carve-out
#include "core/types/config_path_walk.hpp"       // findShippedConfigDir — shared src/dss-config/<dir> resolver
#include "core/types/data_model.hpp"             // dataModelFromName (signatureByDataModel keys)
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/enum_name_table.hpp"        // EnumNameTable/allNames (the descriptor-local closed sets)
#include "core/types/include_path_resolve.hpp"   // resolveSystemDescriptor (the `includes` closure walk)
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
#include <cstddef>       // std::size_t (was <cstdlib> for std::getenv, gone with the local walk)
#include <cstdint>
#include <deque>         // std::deque (Option C: address-stable typedef-name backing)
#include <fstream>
#include <functional>    // std::function (forEachDescriptorInClosure callbacks)
#include <initializer_list>
#include <mutex>         // std::mutex/lock_guard (the corpus-index memo)
#include <optional>
#include <span>
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
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
[[nodiscard]] core::PathIdentity descriptorPathKey(
    std::filesystem::path const& path);

json const* cachedDescriptorJson(std::filesystem::path const& path,
                                 DiagnosticReporter& reporter) {
    thread_local std::unordered_map<core::PathIdentity, json> cache;
    auto const key = descriptorPathKey(path);
    if (auto const it = cache.find(key); it != cache.end()) return &it->second;

    // THE ONE CHECKED READ (D-CORE-SHIPPED-CONFIG-LOADERS-DRAIN-A-STREAM-WITHOUT-CHECKING-IT).
    // ★ SHARPER HERE THAN ANYWHERE ELSE BECAUSE OF THE CACHE ABOVE: an unchecked
    // drain could park a TRUNCATED document in the thread-local map and re-serve
    // it to every later reader in this thread, so one transient I/O fault would
    // outlive itself. A read failure returns before the `cache.emplace` below,
    // and failures are not cached — so the fault stays a fault.
    auto text = core::readFileChecked(path);
    if (!text) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor: "}
                + std::move(text).error().message);
        return nullptr;
    }
    json doc;
    try {
        doc = json::parse(*text);
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

// D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD enforcement. Reports one
// F_ShippedLibDescriptorMalformed per key not in the allow-list. Returns true
// iff every key was known.
//
// ★★ AN ADAPTER OVER THE SHARED CHECK, AND THE MOVE FIXED A LIVE BUG HERE.
// This was one of four independently hand-written `rejectUnknownKeys` helpers
// and it applied the `$`-documentation carve-out at exactly ONE of its 19 call
// sites — as a literal `"$comment"` smuggled into the ROOT allow-list. So the
// convention held for the root object and for nothing else: a `$comment` on a
// symbol row, a struct, a macro or a `when` guard was REFUSED as a typo, and
// only `$comment` ever worked even there (`$abiComment`, the spelling shipped
// targets really use, never did). That is precisely the "carve-out remembered
// per site" failure the shared header exists to abolish, which is why the
// literal is now GONE from the root list: the PREFIX predicate applies to all
// 19 sites because the caller no longer writes the loop.
//
// `objPath` doubles as the object LABEL — it is already the most specific
// name this loader has for the object ("(root)", "symbols[3]", "macros[7]"),
// so the message names it once instead of twice.
[[nodiscard]] bool rejectUnknownKeys(DiagnosticReporter& reporter,
                                     json const& obj, std::string const& objPath,
                                     std::initializer_list<std::string_view> allowed) {
    bool ok = true;
    detail::rejectUnknownKeys(obj, allowed, "'" + objPath + "'",
        [&](std::string_view, std::string message) {
            ok = false;
            emitMalformed(reporter,
                std::string{"shipped-lib descriptor: "} + std::move(message)
                    + " (D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD)");
        });
    return ok;
}

// Closed-table enum resolution. A miss is a malformed descriptor (the JSON
// named an enumerator outside the closed set) → reported by the caller.
//
// ★ TABLES, NOT IF-CHAINS (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
// These were two hand-written if-chains, each with its accepted
// set RETYPED into the refusal message beside its call site — the same fact
// owned twice, drifting the moment a spelling is added or renamed. The rows are
// now the only owner and the refusals render `allNames(…)` over them.
//
// ⓘ The TABLES live here rather than beside the enums in
// `ffi/shipped_lib_descriptor.hpp` for the same reason `configName()` lives on
// each backend: these spellings are the DESCRIPTOR FILE's vocabulary, and this
// TU is the only thing that reads or writes one. (That header was owned by
// another lane this cycle; if the enums ever gain a second reader, the tables
// move next to them — the projection is unaffected either way.)
inline constexpr EnumNameTable<ShippedSymbolKind, 2> kShippedSymbolKindTable{{{
    { ShippedSymbolKind::Function, "function" },
    { ShippedSymbolKind::Object,   "object"   },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kShippedSymbolKindTable);
inline constexpr EnumNameTable<ShippedSymbolLinkage, 2>
kShippedSymbolLinkageTable{{{
    { ShippedSymbolLinkage::External, "external" },
    { ShippedSymbolLinkage::Weak,     "weak"     },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kShippedSymbolLinkageTable);

[[nodiscard]] std::optional<ShippedSymbolKind> kindFromName(std::string_view s) {
    return kShippedSymbolKindTable.fromName(s);
}
[[nodiscard]] std::optional<ShippedSymbolLinkage> linkageFromName(std::string_view s) {
    return kShippedSymbolLinkageTable.fromName(s);
}

// The "expected …" half of every closed-set refusal in this file, projected
// from the table the check consults. Never a literal: the descriptor corpus is
// authored by hand, so the sentence IS the documentation an author reads, and a
// sentence narrower than the check denies by name a spelling the loader takes.
//
// ✔THE CLASS HAD ALREADY FIRED HERE. MEASURED 2026-08-20 against
// `kSelectableObjectFormatKindNames` (FIVE spellings — the enum table minus its
// `unknown` sentinel, which is exactly what `objectFormatKindFromName` +
// `isSelectableObjectFormatKind` accept below): FOUR object-format refusals in
// this file advertised THREE (`"pe"/"elf"/"macho"`), omitting `wasm` and
// `spirv`.
template <typename Names>
[[nodiscard]] std::string allowedList(Names const& names,
                                      std::string_view sep = "/") {
    return detail::renderAllowedList(names, sep);
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
        || k == TypeKind::F64 || k == TypeKind::F80 || k == TypeKind::F128;
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

// WHICH AXES OF THE `when` SELECTOR PARTICIPATE IN THE MATCH. One evaluator,
// three modes — never a second evaluator (a `when` decoded by two readers is a
// `when` two readers can disagree about).
//
//  • `FormatOnly`     — `{format}` is the WHOLE legal key vocabulary; an `arch`
//                       or `dataModel` key FAILS LOUD. The preprocessor-facing
//                       surfaces (`macros`, and the `includes` edge gate) live
//                       here: neither arch nor the data model is threaded into
//                       preprocess (c9 build-key avoidance), so a key naming
//                       them could only ever be a config author's mistake.
//  • `FullTarget`     — `{arch,format,dataModel}` are all legal and all
//                       participate. The TYPED surfaces (structs / constants /
//                       typedefs / per-target `value` variants), which select a
//                       LAYOUT or a TYPE and therefore need every axis.
//  • `FormatReachability` — `{arch,format,dataModel}` are all legal and
//                       VALIDATED, but only `format` participates. This answers
//                       a strictly weaker question than `FullTarget`: "could
//                       this arm be selected on object format F, for SOME
//                       target?" — which is the right question for a NAME
//                       PRESENCE scan (`shippedSurfaceNamesForFormat`), because
//                       a variant set changes a name's TYPE or LAYOUT per arch,
//                       never whether the name exists at all. Answering it with
//                       `FullTarget` and no active arch would say NoMatch for
//                       every arch-keyed arm and under-report the surface;
//                       answering it by ignoring `variants` entirely would
//                       over-report it. Both are silent wrong answers to a
//                       question two fail-loud invariants are built on.
enum class WhenAxes { FormatOnly, FullTarget, FormatReachability };

// Decode + test a variant's `when` object (the per-target SELECTOR shared by the
// `structs` / `constants` / `typedefs` / `macros` variant surfaces). The contract
// is MATCH-ALL-SPECIFIED: every key the `when` SPECIFIES must equal the active
// value (generic string equality — never an `if (arch == "x86_64")` here); an
// unspecified key is a wildcard. A key tested against an UNKNOWN active value
// (activeTarget / activeFormat nullopt — direct-API/LSP/test callers) can never
// match. `axes` selects which keys are LEGAL and which PARTICIPATE — see the
// `WhenAxes` table above. EVERY mode VALIDATES every key it admits (a typo'd
// value fails loud in all three, including on an axis that does not
// participate). `activeFormatName` is the precomputed
// `objectFormatKindName(*activeFormat)` (empty when activeFormat is nullopt);
// `activeDataModelName` is the precomputed `dataModelName(dataModel)` — the
// descriptor reader always has a data model (a non-optional parameter), so unlike
// arch/format this axis can never be "unknown". `whenCtx` is the caller's
// diagnostic context for the `when` object (e.g. "structs[0] variants[1].when").
// Reports via `emitMalformed` on a malformed `when`; the format and data-model
// VALUES are validated against their closed vocabularies so a typo'd "elff" /
// "LP62" fails loud rather than silently never matching.
//
// D-LANG-TYPE-IDENTITY-VOCABULARY: `dataModel` is the axis that lets a descriptor
// spell a type C defines as a per-data-model ALIAS of a standard NAMED type —
// `size_t` IS `unsigned long` on LP64 and `unsigned long long` on LLP64, `int64_t`
// IS `long` / `long long`. A FIXED vocabulary tag would be wrong on one of the two
// models, and an untagged core is a THIRD type matching neither. It rides the SAME
// selector arch/format already use rather than a typedef-only `typeByDataModel`
// key, so structs/constants/versions gain the axis for free.
[[nodiscard]] WhenMatch
matchVariantWhen(json const& when, WhenAxes axes, std::string const& whenCtx,
                 std::optional<std::string_view> activeTarget,
                 std::optional<ObjectFormatKind> activeFormat,
                 std::string const& activeFormatName,
                 std::string_view activeDataModelName,
                 DiagnosticReporter& reporter) {
    // Closed key vocabulary: {arch,format,dataModel} for the typed surfaces,
    // {format} only for macros. An unknown/forbidden key (e.g. `arch` in a macro
    // `when`, or a typo'd "ach") fails loud — a silently-ignored key would match
    // more broadly than intended.
    //
    // ★ LEGALITY AND PARTICIPATION ARE SEPARATE. `FormatReachability` admits the
    // arch axes (they are legal in a typed surface's `when`) and still VALIDATES
    // their values, but does not let them decide the match — which is what makes
    // it a strictly WEAKER test than `FullTarget` rather than a different one.
    bool const archKeysLegal        = (axes != WhenAxes::FormatOnly);
    bool const archKeysParticipate  = (axes == WhenAxes::FullTarget);
    if (archKeysLegal) {
        if (!rejectUnknownKeys(reporter, when, whenCtx,
                               {"arch", "format", "dataModel"}))
            return WhenMatch::Error;
    } else {
        if (!rejectUnknownKeys(reporter, when, whenCtx, {"format"}))
            return WhenMatch::Error;
    }
    bool matches = true;
    if (archKeysLegal && when.contains("dataModel")) {
        if (!when.at("dataModel").is_string()) {
            emitMalformed(reporter, "shipped-lib descriptor " + whenCtx
                                        + ": 'dataModel' must be a string");
            return WhenMatch::Error;
        }
        std::string const wantModel = when.at("dataModel").get<std::string>();
        // CLOSED vocabulary (the same spellings `coreByDataModel` /
        // `signatureByDataModel` use) — a typo would otherwise silently never
        // match, making the entry vanish on every target.
        if (!dataModelFromName(wantModel).has_value()) {
            emitMalformed(reporter, "shipped-lib descriptor " + whenCtx
                                        + ": 'dataModel' has unknown data-model name '"
                                        + wantModel + "' (expected "
                                        + allowedList(allNames(kDataModelTable))
                                        + ")");
            return WhenMatch::Error;
        }
        if (archKeysParticipate && activeDataModelName != wantModel) matches = false;
    }
    if (archKeysLegal && when.contains("arch")) {
        if (!when.at("arch").is_string()) {
            emitMalformed(reporter, "shipped-lib descriptor " + whenCtx
                                        + ": 'arch' must be a string");
            return WhenMatch::Error;
        }
        std::string const wantArch = when.at("arch").get<std::string>();
        // The arch name is OPEN (it lives only in the target schemas this reader
        // does not load) — an unknown arch simply never matches.
        if (archKeysParticipate
            && (!activeTarget.has_value() || *activeTarget != wantArch))
            matches = false;
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
        auto const wantKind = objectFormatKindFromName(wantFormat);
        if (!wantKind.has_value()) {
            emitMalformed(reporter, "shipped-lib descriptor " + whenCtx
                                        + ": 'format' has unknown object-format name '"
                                        + wantFormat + "' (expected "
                                        + allowedList(kSelectableObjectFormatKindNames)
                                        + ")");
            return WhenMatch::Error;
        }
        // ...and the `unknown` SENTINEL has the identical consequence by the
        // rationale one line up: it spells correctly, so the lookup accepts it,
        // and then it matches no real active format — the entry vanishes on
        // every target, exactly as the typo would. Same defect, same verdict.
        if (!isSelectableObjectFormatKind(*wantKind)) {
            emitMalformed(reporter, "shipped-lib descriptor " + whenCtx
                                        + ": 'format' names the invalid sentinel — "
                                        + std::string{kObjectFormatKindSentinelRejection});
            return WhenMatch::Error;
        }
        if (!activeFormat.has_value() || activeFormatName != wantFormat) matches = false;
    }
    return matches ? WhenMatch::Match : WhenMatch::NoMatch;
}

// Decode ONE optional per-symbol PER-TARGET STRING field into `out`. The shape
// is the c156 `version` shape, generalized so its TF-C121 sibling `linkName`
// cannot drift from it:
//
//   "<key>": "flat"                                        (target-invariant)
//   "<key>": { "variants": [ { "when": {arch?,format?,dataModel?},
//                              "value": "…" }, … ] }        (per-target)
//
// ABSENT ⇒ `out` untouched (the caller pre-sets the empty default). 0 matching
// variants ⇒ `out` stays empty — LEGAL, and load-bearing for BOTH consumers: it
// is aarch64's single-versioned realpath, and it is arm64-Darwin's `fstat`,
// whose only ABI is the modern one so the plain name is already right.
// >1 match ⇒ ambiguous ⇒ fail loud (each target must select at most one).
//
// EAGER: every variant's SHAPE is validated regardless of which one is active,
// so a malformed INACTIVE variant fails the read on EVERY target rather than
// lurking until that target is first compiled (mirrors `signatureByDataModel`
// and the struct variants). Returns false when the entry is malformed — the
// caller `continue`s past this symbol and the read fails via its errorCount
// delta.
//
// ★ ONE DECODER FOR BOTH FIELDS IS THE POINT. `version` and `linkName` are the
// two per-symbol strings whose correct value depends on the active target; they
// were written as one block and a copy would let the second silently lose a
// validation the first gained (a `when` key vocabulary, the ambiguity check).
[[nodiscard]] bool
decodePerTargetSymbolString(json const& sym, std::string const& key,
                            std::string const& at, std::size_t symIdx,
                            std::optional<std::string_view> activeTarget,
                            std::optional<ObjectFormatKind> activeFormat,
                            std::string_view activeDataModelName,
                            DiagnosticReporter& reporter, std::string& out) {
    if (!sym.contains(key)) return true;
    json const& node = sym.at(key);
    if (node.is_string()) {
        out = node.get<std::string>();
        if (out.empty()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at + ": '" + key
                + "' string must be non-empty (omit the key entirely to leave "
                  "this symbol's " + key + " unset)");
            return false;
        }
        return true;
    }
    if (!node.is_object()) {
        emitMalformed(reporter, "shipped-lib descriptor " + at + ": '" + key
            + "' must be a STRING (target-invariant) or an OBJECT with a "
              "'variants' array (per-target)");
        return false;
    }
    std::string const objCtx = "symbols[" + std::to_string(symIdx) + "]." + key;
    if (!rejectUnknownKeys(reporter, node, objCtx, {"variants"}))
        return false;   // rejectUnknownKeys already reported
    if (!node.contains("variants") || !node.at("variants").is_array()
        || node.at("variants").empty()) {
        emitMalformed(reporter, "shipped-lib descriptor " + at + ": a '" + key
            + "' OBJECT must carry a non-empty 'variants' array (or be a flat "
              "string)");
        return false;
    }
    std::string const activeFormatName =
        activeFormat.has_value() ? std::string{objectFormatKindName(*activeFormat)}
                                 : std::string{};
    int         matchCount = 0;
    std::size_t vi         = 0;
    for (auto const& vdef : node.at("variants")) {
        std::string const vctx = objCtx + ".variants[" + std::to_string(vi) + "]";
        ++vi;
        if (!vdef.is_object()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at + ": '" + vctx
                + "' must be an object with 'when' + 'value'");
            return false;
        }
        if (!rejectUnknownKeys(reporter, vdef, vctx, {"when", "value"}))
            return false;   // already reported
        if (!vdef.contains("value") || !vdef.at("value").is_string()
            || vdef.at("value").get<std::string>().empty()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at + ": '" + vctx
                + "' must carry a non-empty string 'value'");
            return false;
        }
        if (!vdef.contains("when") || !vdef.at("when").is_object()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at + ": '" + vctx
                + "' must carry a 'when' object");
            return false;
        }
        WhenMatch const wm =
            matchVariantWhen(vdef.at("when"), WhenAxes::FullTarget, vctx + ".when",
                             activeTarget, activeFormat, activeFormatName,
                             activeDataModelName, reporter);
        if (wm == WhenMatch::Error) return false;
        if (wm == WhenMatch::Match) {
            ++matchCount;
            out = vdef.at("value").get<std::string>();
        }
    }
    if (matchCount > 1) {
        emitMalformed(reporter, "shipped-lib descriptor " + at + ": '" + key
            + "' has " + std::to_string(matchCount)
            + " variants matching the active target -- the selection is "
              "ambiguous (each target must match at most one)");
        return false;
    }
    // matchCount == 0 ⇒ `out` stays empty on this target. LEGAL, not an error.
    return true;
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
            // FORMAT-ONLY selector (WhenAxes::FormatOnly — arch is not threaded into the
            // preprocessor). A nullopt activeFormat can never match (no selection).
            WhenMatch const wm = matchVariantWhen(
                vdef.at("when"), WhenAxes::FormatOnly, vat + ".when",
                /*activeTarget=*/std::nullopt, activeFormat, activeFormatName,
                /*activeDataModelName=*/std::string_view{}, reporter);
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
                                      "object-format names ("
                                    + allowedList(kSelectableObjectFormatKindNames)
                                    + ")");
        return;
    }
    for (auto const& v : doc.at("availableObjectFormats")) {
        if (!v.is_string()) {
            emitMalformed(reporter, "shipped-lib descriptor '" + pathStr
                                        + "': 'availableObjectFormats' entries must be strings");
            continue;
        }
        std::string fmt = v.get<std::string>();
        auto const fmtKind = objectFormatKindFromName(fmt);
        if (!fmtKind.has_value()) {
            emitMalformed(reporter, "shipped-lib descriptor '" + pathStr
                                        + "': 'availableObjectFormats' has unknown object-format "
                                          "name '" + fmt + "' (expected "
                                        + allowedList(kSelectableObjectFormatKindNames)
                                        + ")");
            continue;
        }
        // The `unknown` sentinel spells correctly, so it survives the lookup and
        // then narrows availability to a format no image can have — the library
        // becomes silently unavailable everywhere, which is what a typo does too.
        if (!isSelectableObjectFormatKind(*fmtKind)) {
            emitMalformed(reporter, "shipped-lib descriptor '" + pathStr
                                        + "': 'availableObjectFormats' names the invalid "
                                          "sentinel — "
                                        + std::string{kObjectFormatKindSentinelRejection});
            continue;
        }
        out.push_back(std::move(fmt));
    }
}

// Decode a per-object-format `library` MAP node ({"pe":"msvcrt.dll",
// "elf":"libc.so.6"}) into `out`. Each KEY must be a known object-format name
// (the `objectFormatKindFromName` vocabulary — a typo like "pee" fails loud
// HERE, not at a user's link); each VALUE must be a string. A NON-OBJECT node is
// a SHAPE error → emits + returns false (the caller ABORTS: the descriptor-level
// map hard-returns nullopt, a per-symbol override skips that symbol). Per-KEY
// errors (unknown format / non-string value) are collect-all (emitted + skipped;
// the overall read still fails via the caller's errorCount delta). The SHARED
// chokepoint for the descriptor-level `library` AND the per-symbol `library`
// override — so the two validations can NEVER drift (the decodeShippedAvailability
// precedent). AGNOSTIC: the key set is the object-format vocabulary, never an
// `if (key == "pe")` identity branch. `ctx` is the caller's already-quoted
// diagnostic context (e.g. "'p'" for the root, "'p' symbols[3]" for a symbol);
// `field` is the map's spelling ("library"). (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC)
[[nodiscard]] bool decodeLibraryMap(json const& node, std::string const& ctx,
                                    std::string const& field,
                                    DiagnosticReporter& reporter,
                                    std::unordered_map<std::string, std::string>& out) {
    if (!node.is_object()) {
        emitMalformed(reporter, "shipped-lib descriptor " + ctx + ": '" + field
            // The example names the MODERN pe C runtime deliberately: this text is
            // what an author copies, and `msvcrt.dll` is the legacy CRT the UCRT
            // migration moved off (only `setjmp.json` still names it, on purpose).
            + "' must be a per-object-format object, e.g. "
              "{\"pe\":\"ucrtbase.dll\",\"elf\":\"libc.so.6\"}");
        return false;
    }
    for (auto const& kv : node.items()) {
        auto const keyKind = objectFormatKindFromName(kv.key());
        if (!keyKind.has_value()) {
            emitMalformed(reporter, "shipped-lib descriptor " + ctx + ": '" + field
                + "' has unknown object-format key '" + kv.key()
                + "' (expected one of the object-format names: "
                + allowedList(kSelectableObjectFormatKindNames) + ")");
            continue;
        }
        // The `unknown` sentinel spells correctly ("pee" does not), so only an
        // explicit check stops it. A library stored under it resolves for no
        // real format — the symbol reaches the link with no import library,
        // which is precisely what this closed vocabulary exists to prevent.
        if (!isSelectableObjectFormatKind(*keyKind)) {
            emitMalformed(reporter, "shipped-lib descriptor " + ctx + ": '" + field
                + "' names the invalid sentinel — "
                + std::string{kObjectFormatKindSentinelRejection});
            continue;
        }
        if (!kv.value().is_string()) {
            emitMalformed(reporter, "shipped-lib descriptor " + ctx + ": '" + field
                + "." + kv.key() + "' must be a string");
            continue;
        }
        out.emplace(kv.key(), kv.value().get<std::string>());
    }
    return true;
}

// ★★★ D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF — decode a per-object-format
// `realization` MAP node (`{"pe": {"unit": "dirent"}}`) into `out`
// (format → shipped source unit NAME).
//
// DELIBERATELY THE SAME SHAPE AND THE SAME CHOKEPOINT DISCIPLINE AS
// `decodeLibraryMap` ABOVE — same closed object-format key vocabulary, same
// sentinel rejection, same "non-object node is a SHAPE error, per-key errors are
// collect-all" contract, and the same single function serving BOTH the
// descriptor-level map and the per-symbol override. That is what makes this an
// EXTENSION of the `library` axis rather than a second, parallel notion of
// "where a body comes from": the two maps cannot drift on key validation,
// because they validate keys the same way, in code written the same way.
//
// The VALUE is an OBJECT with a closed one-key set rather than a bare string,
// and that is not ceremony. A bare string would be indistinguishable from an
// image name — the exact ambiguity between "imported from X" and "provided by
// X" this axis exists to remove — and it would leave no place for a second
// realization KIND to land without a type change. `{"unit": …}` states the kind.
//
// ★ THE UNIT IS NAMED, NEVER PATHED. A path here would make every future
// re-layout of the runtime tree a config migration, and it would turn refusal R2
// from a set difference over NAMES into a path walk. The layout is the loader's
// business (`<tier>/<name>/<name>.c`), exactly as `<stem>.json` is for headers.
[[nodiscard]] bool decodeRealizationMap(
    json const& node, std::string const& ctx, std::string const& field,
    DiagnosticReporter& reporter,
    std::unordered_map<std::string, std::string>& out) {
    if (!node.is_object()) {
        emitMalformed(reporter, "shipped-lib descriptor " + ctx + ": '" + field
            + "' must be a per-object-format object whose values name a shipped "
              "source FILE, e.g. "
              "{\"pe\":{\"source\":\"runtime/platform/pe/dirent.c\"}}");
        return false;
    }
    for (auto const& kv : node.items()) {
        auto const keyKind = objectFormatKindFromName(kv.key());
        if (!keyKind.has_value()) {
            emitMalformed(reporter, "shipped-lib descriptor " + ctx + ": '" + field
                + "' has unknown object-format key '" + kv.key()
                + "' (expected one of the object-format names: "
                + allowedList(kSelectableObjectFormatKindNames) + ")");
            continue;
        }
        // Same reason as `decodeLibraryMap`: the `unknown` sentinel SPELLS
        // correctly, so only an explicit check stops it, and a realization
        // stored under it would select for no real format at all.
        if (!isSelectableObjectFormatKind(*keyKind)) {
            emitMalformed(reporter, "shipped-lib descriptor " + ctx + ": '" + field
                + "' names the invalid sentinel — "
                + std::string{kObjectFormatKindSentinelRejection});
            continue;
        }
        if (!kv.value().is_object()) {
            emitMalformed(reporter, "shipped-lib descriptor " + ctx + ": '" + field
                + "." + kv.key() + "' must be an object, e.g. "
                  "{\"source\":\"runtime/platform/pe/dirent.c\"}");
            continue;
        }
        (void)rejectUnknownKeys(reporter, kv.value(),
                                ctx + " " + field + "." + kv.key(), {"source"});
        if (!kv.value().contains("source") || !kv.value().at("source").is_string()) {
            emitMalformed(reporter, "shipped-lib descriptor " + ctx + ": '" + field
                + "." + kv.key() + "' must declare a string 'source' naming a "
                  "shipped source FILE, relative to src/dss-config/");
            continue;
        }
        std::string src = kv.value().at("source").get<std::string>();
        // The path is RELATIVE to the shipped-config root and must stay inside
        // it. This is the same containment posture `findShippedConfig` takes on a
        // logical name, applied to a path: a `..` component or an absolute/rooted
        // spelling would let a descriptor reach an arbitrary file on the host, so
        // both are refused HERE rather than at the filesystem.
        bool escapes = src.empty();
        {
            std::filesystem::path const asPath{src};
            if (asPath.is_absolute() || asPath.has_root_name()
                || src.find('\\') != std::string::npos)
                escapes = true;
            for (auto const& seg : asPath)
                if (seg == ".." || seg == ".") escapes = true;
        }
        if (escapes) {
            emitMalformed(reporter, "shipped-lib descriptor " + ctx + ": '" + field
                + "." + kv.key() + ".source' must be a non-empty path RELATIVE to "
                  "src/dss-config/, with forward slashes and no '.'/'..' "
                  "components, got '" + src + "'");
            continue;
        }
        out.emplace(kv.key(), std::move(src));
    }
    return true;
}

// True iff `text` contains `name` as a whole PREPROCESSING TOKEN — i.e. not as a
// substring of a longer identifier. `acos` occurs inside `acosf` and inside
// `_Generic`; only a match with a non-identifier character (or nothing) on both
// sides is the shadowed name being CALLED. Identifier characters are C's:
// [A-Za-z0-9_]. Content-blind — no name is special-cased.
[[nodiscard]] bool referencesIdentifierToken(std::string_view text,
                                             std::string_view name) {
    if (name.empty()) return false;
    auto isIdent = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_';
    };
    for (std::size_t at = text.find(name); at != std::string_view::npos;
         at = text.find(name, at + 1)) {
        bool const leftOk  = at == 0 || !isIdent(text[at - 1]);
        std::size_t const end = at + name.size();
        bool const rightOk = end >= text.size() || !isIdent(text[end]);
        if (leftOk && rightOk) return true;
    }
    return false;
}

// ── A MACRO THAT SHADOWS A SYMBOL WITHOUT REFERENCING IT IS A DEFECT ─────────
//
// MEASURED behaviour before this guard: such a descriptor compiled rc=0, emitted no
// diagnostic, the macro silently SHADOWED the symbol at preprocess time, and the
// symbol was STILL eagerly imported (D-FFI-DESCRIPTOR-EAGER-IMPORT) — a wasted
// import of a name nothing could ever call. Loud at LOAD is the only place this can
// be caught: by the time a TU is compiled the macro has already won.
//
// ★★ "SAME NAME + OVERLAPPING FORMAT = ERROR" IS A FALSE RULE. DO NOT SHIP IT.
// It would red 17 in-tree rows that are STANDARD-MANDATED and BENIGN: C 7.25
// <tgmath.h> defines `acos` (and 16 siblings) as a type-generic MACRO over the very
// `acos` symbol <math.h> declares, and the replacement
// `_Generic((x), float: acosf(...), default: acos((x)))` REFERENCES the shadowed
// name — so by C 6.10.3.4p2 (a replacement list is not re-scanned for the macro
// being replaced) the symbol IS the macro's own callee and the import is correct.
//
// ⇒ THE DISCRIMINATOR IS THE REFERENCE, NOT THE NAME. A defect is:
//      formats overlap  AND  NO body of the macro references the shadowed name.
// MEASURED clean tree-wide at the time this landed: 28 same-name macro/symbol
// pairs, 17 overlapping-and-referencing (the tgmath family) and 11 that do NOT
// reference but live on DISJOINT formats — pe `time`→`_time64` while the `time`
// SYMBOL is gated ["elf","macho"], pe `setjmp`→`_setjmp`, `stdin`/`stdout`/`stderr`,
// `atexit`, `strtoll`, `fstat`, … Those 11 are the DESIGN: the macro exists on the
// format precisely BECAUSE the symbol is absent there. Zero defects; this guard is a
// LOCK on a clean corpus, not a repair of a broken one.
//
// ⚠ NOTE THE DIVERGENCE FROM THE PREDEFINED-MACRO COLLISION SCAN
// (`preprocessor.cpp`), which deliberately checks collisions BEFORE the format
// filter because "two configs claim one NAME" is itself the fault there. That
// reasoning does NOT transfer: here per-format divergence is the intended design,
// and following that precedent literally would red the 11 rows above. Stated so the
// inconsistency is not "tidied up" into a build break.
//
// ⚠ READS THE RAW JSON, NOT `out.macros`. `decodeShippedMacros` collapses per-format
// `variants` down to the ONE matching the active format, and under a nullopt
// activeFormat (unit tests, the LSP, and the corpus-wide decode test) it emits NONE
// of them. A guard reading the decoded surface would therefore be blind to exactly
// the per-format cases, and inert in the very test that sweeps all 51 descriptors.
// The raw `doc` gives every body on every format regardless of the active target.
//
// ⚠ EVERY VARIANT BODY IS CHECKED, not the first or the active one: a macro whose pe
// body references the name while its elf body does not is a REAL defect on elf, and
// checking one body would hide it.
//
// AGNOSTIC: availability is compared through the shared `objectFormatInAvailabilitySet`
// contract's own encoding (an EMPTY set means EVERY format), never against a
// hardcoded {pe,elf,macho} universe — the vocabulary has five selectable kinds today
// and this must not need editing when a sixth lands.
void checkMacroSymbolShadowing(json const& doc, ShippedLibDescriptor const& out,
                               std::string const& pathStr,
                               DiagnosticReporter& reporter) {
    auto const mIt = doc.find("macros");
    if (mIt == doc.end() || !mIt->is_array() || out.symbols.empty()) return;

    // "Do these two availability sets share a format?" EMPTY means EVERY format on
    // both sides, so an empty set overlaps everything (including another empty one).
    auto overlaps = [](std::vector<std::string> const& a,
                       std::vector<std::string> const& b) {
        if (a.empty() || b.empty()) return true;
        for (auto const& f : a)
            if (std::find(b.begin(), b.end(), f) != b.end()) return true;
        return false;
    };
    // ONE macro entry decomposes into one BODY PER FORMAT ARM: a flat macro is a
    // single unrestricted body, a `variants` macro is one body per variant (a
    // variant whose `when` names no format is unrestricted). Checking per BODY
    // rather than per ENTRY is what makes a MIXED macro — pe arm references the
    // name, elf arm does not — a defect ON ELF instead of being excused by its pe
    // sibling. An "any body references it" test would hide exactly that.
    struct MacroBody {
        std::vector<std::string> formats;   // EMPTY ⇒ every format
        std::string              replacement;
    };
    auto bodiesOf = [](json const& m) {
        std::vector<MacroBody> bodies;
        auto const vIt = m.find("variants");
        if (vIt == m.end() || !vIt->is_array()) {
            auto const rIt = m.find("replacement");
            bodies.push_back(MacroBody{{}, rIt != m.end() && rIt->is_string()
                                               ? rIt->get<std::string>()
                                               : std::string{}});
            return bodies;
        }
        for (auto const& v : *vIt) {
            if (!v.is_object()) continue;
            MacroBody b;
            auto const rIt = v.find("replacement");
            if (rIt != v.end() && rIt->is_string())
                b.replacement = rIt->get<std::string>();
            auto const wIt = v.find("when");
            if (wIt != v.end() && wIt->is_object()) {
                auto const fIt = wIt->find("format");
                if (fIt != wIt->end() && fIt->is_string())
                    b.formats.push_back(fIt->get<std::string>());
            }
            bodies.push_back(std::move(b));
        }
        return bodies;
    };

    for (std::size_t mi = 0; mi < mIt->size(); ++mi) {
        json const& m = mIt->at(mi);
        if (!m.is_object()) continue;
        auto const nIt = m.find("name");
        if (nIt == m.end() || !nIt->is_string()) continue;   // shape: owned upstream
        auto const name = nIt->get<std::string>();
        bool reported = false;
        for (auto const& body : bodiesOf(m)) {
            // C 6.10.3.4p2 — a replacement list is not re-scanned for the macro
            // being replaced, so a body that NAMES the symbol calls it.
            if (referencesIdentifierToken(body.replacement, name)) continue;
            for (auto const& sym : out.symbols) {
                if (sym.name != name) continue;
                // TIER 1 the symbol's own gate, TIER 2 the document's — the same
                // two-level fallback the injector applies.
                std::vector<std::string> const& symFormats =
                    sym.availableObjectFormats.empty() ? out.availableObjectFormats
                                                       : sym.availableObjectFormats;
                if (!overlaps(body.formats, symFormats)) continue;  // by design
                reported = true;
                break;
            }
            if (reported) break;
        }
        if (reported) {
            emitMalformed(reporter,
                std::string{"shipped-lib descriptor '"} + pathStr + "' macros["
                + std::to_string(mi) + "]: the macro '" + name
                + "' SHADOWS this descriptor's symbol row of the same name on a "
                  "format they SHARE, and its replacement does not reference '"
                + name
                + "' — so the macro wins at preprocess time, nothing can ever call "
                  "the symbol, and the symbol is still eagerly imported (a dead "
                  "import in every binary). Either reference the name in the "
                  "replacement (the C 7.25 <tgmath.h> pattern, which is why a "
                  "same-name macro is NOT itself an error), or restrict the two to "
                  "DISJOINT 'availableObjectFormats' (the pe 'time'->'_time64' "
                  "pattern, where the macro exists because the symbol does not)");
            // COLLECT-ALL across macro entries (the house style for per-entry
            // descriptor validation): a corpus with three such macros should name
            // all three, not make the author re-run the build twice.
        }
    }
}

// Decode the optional `includes` array (the transitive sibling-header NAMES, plan
// D-FFI-DESCRIPTOR-INCLUDES) into `out`, keeping only the edges ACTIVE on
// `activeFormat`. Each entry is EITHER a NON-EMPTY string (an UNCONDITIONAL edge —
// today's shape, always taken) OR an object `{header, when}` whose `when` is
// evaluated by the ONE shared `matchVariantWhen` in its FORMAT-ONLY mode (the same
// vocabulary the `macros` surface uses). A header NAME is later resolved via
// `resolveSystemDescriptor`'s `<stem>.json` convention by the closure walker — this
// decode does NOT resolve or validate existence, only shape.
//
// ★ THE GATE IS ON THE EDGE, NOT ON THE CHILD. An inactive edge is NOT AN EDGE on
// this target: the closure never contains the child, so no tier can form an opinion
// about it and no two tiers can disagree. The alternative (walk every edge, then
// have each tier drop unavailable children) is what produced the drift this gate
// closes — the preprocessor dropped them SILENTLY while the semantic tier reported
// `F_ShippedHeaderUnavailableForTarget` on the user's `#include` line for a header
// the user never wrote.
//
// Absent/empty ⇒ no transitive edges (back-compat). Shared
// chokepoint: the full interned read AND the fast interner-free
// `readShippedLibIncludes` (the preprocessor + import-resolver tiers) both decode
// through this, so they can never drift (the `decodeShippedMacros`/
// `decodeShippedAvailability` lock-step precedent). Content-blind: whether a name
// resolves to a real descriptor is the walker's concern (it alone has systemDirs).
void decodeShippedIncludes(json const& doc, std::string const& pathStr,
                           DiagnosticReporter& reporter,
                           std::vector<std::string>& out,
                           std::optional<ObjectFormatKind> activeFormat) {
    if (!doc.contains("includes")) return;
    if (!doc.at("includes").is_array()) {
        emitMalformed(reporter, "shipped-lib descriptor '" + pathStr
                                    + "': 'includes' must be an array of header-name "
                                      "strings, e.g. [\"stdio.h\"]");
        return;
    }
    std::string const activeFormatName =
        activeFormat.has_value() ? std::string{objectFormatKindName(*activeFormat)}
                                 : std::string{};
    std::size_t iidx = 0;
    for (auto const& v : doc.at("includes")) {
        std::string const at =
            "'" + pathStr + "' includes[" + std::to_string(iidx) + "]";
        ++iidx;
        // FORM 1 — a bare non-empty STRING: the unconditional edge, byte-identical
        // to the pre-gate shape and always taken.
        if (v.is_string()) {
            if (v.get<std::string>().empty()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": an 'includes' header-name string must be "
                                              "non-empty");
                continue;
            }
            out.push_back(v.get<std::string>());
            continue;
        }
        // FORM 2 — `{header, when}`: the CONDITIONAL edge.
        if (!v.is_object()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at
                                        + ": every 'includes' entry must be a non-empty "
                                          "header-name string (e.g. \"stdio.h\") or an "
                                          "object {\"header\":\"h\",\"when\":{...}}");
            continue;
        }
        if (!rejectUnknownKeys(reporter, v, at, {"header", "when"})) continue;
        if (!v.contains("header") || !v.at("header").is_string()
            || v.at("header").get<std::string>().empty()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at
                                        + ": a conditional 'includes' entry requires a "
                                          "non-empty string 'header'");
            continue;
        }
        // `when` is REQUIRED in the object form, and must declare at least one
        // key. Both rules exist for the same reason: an object entry with no
        // condition — omitted `when`, or `when:{}` which matches everything — is
        // a SECOND SPELLING of the string form, and a fact with an owner does not
        // get a second owner. The author who wrote `{"header":"x"}` meant to
        // write a condition and lost it; that must fail loud, not silently
        // degrade into an unconditional edge.
        if (!v.contains("when") || !v.at("when").is_object()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at
                                        + ": a conditional 'includes' entry requires a "
                                          "'when' object (e.g. {\"format\":\"pe\"}); an "
                                          "UNCONDITIONAL edge is spelled as a bare header-"
                                          "name string");
            continue;
        }
        if (v.at("when").empty()) {
            emitMalformed(reporter, "shipped-lib descriptor " + at
                                        + ": 'when' must declare at least one key — an "
                                          "empty 'when' matches every target, which is "
                                          "what the bare header-name string already says");
            continue;
        }
        // THE ONE SHARED `when` EVALUATOR, in the SAME format-only mode the
        // `macros` surface uses (this tier has no arch and no data model — the
        // preprocessor and the import resolver are both keyed on (file x format)
        // alone). Its vocabulary, its closed format-name check and its fail-loud
        // on an unknown key are therefore identical to the macro surface's, by
        // construction rather than by convention.
        //
        // EAGER: the SHAPE of every entry is validated above regardless of
        // whether the edge is active, and `matchVariantWhen` validates the `when`
        // itself on every read — so a malformed INACTIVE edge fails the read on
        // EVERY target (the anti-lurking property the variant surfaces already
        // have). Only the DECISION is per-format.
        WhenMatch const wm =
            matchVariantWhen(v.at("when"), WhenAxes::FormatOnly, at + ".when",
                             /*activeTarget=*/std::nullopt, activeFormat,
                             activeFormatName,
                             /*activeDataModelName=*/std::string_view{}, reporter);
        if (wm == WhenMatch::Error) continue;   // already reported
        // NoMatch (including EVERY conditional edge when no format is active —
        // LSP / direct-API / test callers) ⇒ the edge is NOT TAKEN. It is not an
        // edge on this target at all, so no tier sees it and no tier can disagree
        // about it. Mirrors "a variants-only macro is not injected".
        if (wm == WhenMatch::Match) out.push_back(v.at("header").get<std::string>());
    }
}

// The weakly-canonical descriptor-path KEY shared by the closure walker's visited
// set, the semantic `readDescriptors` dedup, and `cachedDescriptorJson` — so all
// three agree that "the same descriptor" is the same path. Falls back to
// `lexically_normal` when the file can't be canonicalized (mirrors the two
// existing call sites verbatim).
[[nodiscard]] core::PathIdentity descriptorPathKey(
    std::filesystem::path const& path) {
    return core::PathIdentity::of(path);
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

namespace {

// FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER + Cycle-2 D-CSUBSET-C11-THREADS-TRAMPOLINES) +
// D-FFI-PE-CRT-UCRT-MIGRATION Phase 3: the CLOSED pe64 synth-recipe vocabulary — 27
// recipes across TWO families: 21 <threads.h> + 6 <stdio.h>. Each is named for the C
// function it implements (the `synthesize` value MUST equal the symbol name); rows are
// grouped by family for auditability.
//
// <threads.h> (21): Cycle 1 shipped the 18 single-basic-block recipes; Cycle 2 adds
// thrd_create (a branchless SINGLE block — DIRECT-PASS to CreateThread, no closure: the
// C11 int(*)(void*) start routine has the SAME x64 ABI as the Win32 DWORD(*)(void*)),
// call_once (SINGLE block over InitOnceExecuteOnce, via the module-scoped __dss_once_tramp
// adapter the synth pass emits once + address-takes), and thrd_join (the first MULTI-block
// recipe — `WaitForSingleObject; if(res) GetExitCodeThread; CloseHandle`, its canonical
// StructCfMarkers rederived module-wide after finish()). STILL deferred: thrd_equal · the
// timed-waits AND thrd_sleep (a pe timespec read has an unverified time_t/long-width
// layout — a wrong offset is a silent miscompile → elf-FFI-only, D-CSUBSET-C11-THREADS-TIMED).
//
// <stdio.h> (6): the WHOLE printf/scanf family the UCRT leaves undefined — `printf`,
// `fprintf`, `sprintf`, `snprintf`, `vfprintf`, `sscanf`. `ucrtbase.dll` exports NOT ONE of
// those six names (MEASURED, `objdump -p C:/Windows/System32/ucrtbase.dll`; msvcrt.dll
// exports all but `snprintf`, which is exactly why the other five only became shims when
// the pe CRT flipped — and why `snprintf` was NEVER importable on pe under either CRT): in
// a real MSVC build each is a HEADER INLINE over one of the `__stdio_common_v*` cores, so a
// compiler that binds the CRT by export table finds nothing to import and must synthesize
// the body. This table is the loader's ADVERTISED vocabulary, so it lists what actually
// ships: a row here with no descriptor row and no synth arm would advertise a recipe that
// cannot be used. Each FURTHER printf-family recipe (the `_s` family, the wide twins) lands
// together with its stdio.json row, its `__stdio_common_v*` core's symbol row, and a
// runtime witness — never ahead of them.
//
// A closed `contains`-check — never an `if (id == ...)` chain that could silently drift;
// MUST stay in lock-step with each family's synth-pass switch (a vocab id with no arm
// fails loud there).
//
// The ONE recipe table, tagged by family. Both `isKnownSynthesizeRecipe` (the loader's
// read-time guard) and `shimFamilyOf` (the seam's pass partition) read it, so a recipe
// can never be admitted by one and invisible to the other.
struct RecipeRow {
    std::string_view id;
    ShimFamily       family;
};
constexpr RecipeRow kRecipes[] = {
    // <threads.h> — synthesized over kernel32 (pe) / libSystem pthread (macho).
    {"mtx_init", ShimFamily::Threads},      {"mtx_lock", ShimFamily::Threads},
    {"mtx_unlock", ShimFamily::Threads},    {"mtx_trylock", ShimFamily::Threads},
    {"mtx_destroy", ShimFamily::Threads},   {"cnd_init", ShimFamily::Threads},
    {"cnd_signal", ShimFamily::Threads},    {"cnd_broadcast", ShimFamily::Threads},
    {"cnd_wait", ShimFamily::Threads},      {"cnd_destroy", ShimFamily::Threads},
    {"tss_create", ShimFamily::Threads},    {"tss_get", ShimFamily::Threads},
    {"tss_set", ShimFamily::Threads},       {"tss_delete", ShimFamily::Threads},
    {"thrd_current", ShimFamily::Threads},  {"thrd_yield", ShimFamily::Threads},
    {"thrd_exit", ShimFamily::Threads},     {"thrd_detach", ShimFamily::Threads},
    // Cycle 2 (direct-pass / trampoline / multi-block)
    {"thrd_create", ShimFamily::Threads},   {"thrd_join", ShimFamily::Threads},
    {"call_once", ShimFamily::Threads},
    // <stdio.h> printf/scanf family — synthesized over the UCRT __stdio_common_v* cores,
    // which ucrtbase exports in place of any concrete printf/sprintf/…
    // (D-FFI-PE-CRT-UCRT-MIGRATION Phase 3). See the note above for why these six and no more.
    {"printf", ShimFamily::Stdio},          {"fprintf", ShimFamily::Stdio},
    {"sprintf", ShimFamily::Stdio},         {"snprintf", ShimFamily::Stdio},
    {"vfprintf", ShimFamily::Stdio},        {"sscanf", ShimFamily::Stdio},
};

} // namespace

bool isKnownSynthesizeRecipe(std::string_view id) {
    for (auto const& r : kRecipes) if (r.id == id) return true;
    return false;
}

std::optional<ShimFamily> shimFamilyOf(std::string_view id) {
    for (auto const& r : kRecipes) if (r.id == id) return r.family;
    return std::nullopt;
}

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

    // D-LANG-TYPE-IDENTITY-VOCABULARY: the ACTIVE data model's canonical JSON
    // spelling — the third `when` selector axis, alongside arch + format. Unlike
    // those two the data model is never "unknown" (it is a non-optional
    // parameter with an LP64 default), so a `when:{dataModel}` always resolves.
    std::string const activeDataModelName{dataModelName(dataModel)};

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
        // SHARED chokepoint with the per-symbol `library` override (in the symbol
        // loop below) so the two decodes can NEVER drift. A non-object node hard-
        // fails the whole read (a malformed descriptor-level map is unrecoverable —
        // there is nothing left to bind); per-key errors ride the errorCount delta.
        if (!decodeLibraryMap(doc.at("library"),
                              std::string{"'"} + path.generic_string() + "'",
                              "library", reporter, out.library))
            return std::nullopt;
    }

    // (2.4) D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF: the SIBLING axis to `library`.
    // `library` answers "which IMAGE, per format"; this answers "IMPORTED at all,
    // per format". Absent (every descriptor but dirent.json today) ⇒ every symbol
    // imports, byte-identical to the pre-ruling image. Same shared-chokepoint
    // discipline and the same hard-fail-on-shape contract as `library` above.
    if (doc.contains("realization")) {
        if (!decodeRealizationMap(doc.at("realization"),
                                  std::string{"'"} + path.generic_string() + "'",
                                  "realization", reporter, out.realization))
            return std::nullopt;
    }

    // (2.5) Optional `availableObjectFormats` — the per-target AVAILABILITY set
    // (which object-formats this header EXISTS on). Absent/empty ⇒ available on
    // every format (back-compat). Decoded through the SHARED chokepoint so the
    // full read + the fast front-end reader never drift.
    decodeShippedAvailability(doc, path.generic_string(), reporter,
                              out.availableObjectFormats);

    // (2.6) Optional `includes` — the transitive sibling-header NAMES
    // (D-FFI-DESCRIPTOR-INCLUDES). Decoded through the SHARED chokepoint so this
    // interned read + the interner-free `readShippedLibIncludes` (the closure
    // walker's source) can never drift. Resolution/existence of each name is the
    // walker's concern (it alone carries `systemDirs`) — HERE we only validate
    // shape (a non-empty string, or a `{header, when}` object). A malformed
    // `includes` field fails the read via the errorCount delta below (never a
    // partial import). `activeFormat` gates which edges the decode KEEPS, so the
    // descriptor this read returns carries exactly the transitive set the closure
    // walker builds for the same format.
    decodeShippedIncludes(doc, path.generic_string(), reporter, out.includes,
                          activeFormat);

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

    // Reject unknown top-level keys (closed key set). `$comment` is NOT in
    // this list any more and its absence is the fix, not an omission: the
    // repo-wide config-documentation convention is a `$` PREFIX (a human note
    // such as the LP64-vs-LLP64 deferral rationale in stdio/stdlib, under any
    // `$…` spelling), and it is now applied by the shared check to this object
    // AND to every nested one. Listing the literal here made the convention
    // true of the root only, and true of one spelling only.
    (void)rejectUnknownKeys(reporter, doc, "(root)",
                            {"header", "standard", "library", "realization",
                             "availableObjectFormats",
                             "includes", "symbols", "constants", "floatConstants",
                             "typedefs", "structs", "unions", "macros"});

    // (3.pre) TYPEDEFS resolved FIRST — Option C (D-FFI-DESCRIPTOR-TYPEDEF-NAME-RESOLUTION).
    // A descriptor's own typedefs are decoded BEFORE its symbols /
    // constants / structs, and each resolved `name -> TypeId` is threaded into the
    // working `mergedNamedTypes` so a later signature, struct field, or typedef can
    // spell an earlier descriptor typedef BY NAME (`ptr<Tcl_Obj>`) instead of
    // re-inlining its full `struct "Tcl_Obj" {…}` body — collapsing the ~45-site
    // ripple a body change would otherwise force (the Tcl_Obj layout arc). Seeded
    // with the CALLER's bindings (the c82 `va_list` alias), then EACH typedef is
    // appended as it resolves, so a typedef may reference an EARLIER typedef (array
    // order IS the dependency order; a genuinely-unknown name still fails loud at
    // the identifier fallback — fail-loud preserved). Content-blind + agnostic: NO
    // name is special-cased and there is NO source-language / CPU / object-format
    // branch — the same generic merge every descriptor gets. `typedefNameStore`
    // (a std::deque, whose elements never move) gives each appended binding's
    // `name` view STABLE backing, so growing `mergedNamedTypes` never dangles.
    std::deque<std::string>       typedefNameStore;
    std::vector<NamedTypeBinding> mergedNamedTypes(namedTypes.begin(), namedTypes.end());
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
            // type — scalar, pointer, struct ref, fn ptr, OR an EARLIER descriptor
            // typedef spelled by name via `mergedNamedTypes`) through the ONE codec;
            // fail loud on a missing/undecodable type. Returns the TypeId, or
            // InvalidType (the caller skips on invalid). `ctx` is the diag context.
            auto decodeTypedefType = [&](json const& obj, std::string const& ctx) -> TypeId {
                if (!obj.contains("type") || !obj.at("type").is_string()) {
                    emitMalformed(reporter, "shipped-lib descriptor " + ctx
                                                + ": missing or non-string 'type'");
                    return InvalidType;
                }
                std::string const typeText = obj.at("type").get<std::string>();
                TypeId const ty = parseTypeFromText(typeText, interner, typeReg, reporter,
                                                    mergedNamedTypes);
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
                        vdef.at("when"), WhenAxes::FullTarget, vat + ".when",
                        activeTarget, activeFormat, activeFormatName,
                        activeDataModelName, reporter);
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
            // Option C: PUBLISH this typedef as a NAME binding for the REST of this
            // descriptor's parses (symbols / constants / structs + later typedefs).
            // The name lives in the address-stable `typedefNameStore` so the view
            // stays valid as `mergedNamedTypes` grows; the binding is appended
            // BEFORE `tname` is moved into `out.typedefs`.
            typedefNameStore.push_back(tname);
            mergedNamedTypes.push_back(
                NamedTypeBinding{std::string_view{typedefNameStore.back()}, selType});
            out.typedefs.push_back(ShippedTypedef{std::move(tname), selType});
        }
    }

    // (3.union) UNIONS — the named-member sibling of `structs`, resolved right
    // after typedefs (so a member may spell an earlier typedef by name, e.g.
    // `ptr<Tcl_Obj>`) and BEFORE symbols/structs (so a later signature or struct
    // FIELD may spell a union BY NAME, e.g. `Tcl_HashEntry.key : "Tcl_HashKey"`).
    // Each union interns as `TypeKind::Union` — every member overlaid at OFFSET 0
    // (C 6.7.2.1) — and the semantic phase injects a member field scope +
    // `compositeScopeByType` entry so `unionValue.member` resolves (the NEW
    // mechanism the real `Tcl_GetHashKey` macro's `h->key.oneWordValue` needs).
    // The name is PUBLISHED into `mergedNamedTypes` (Option C, mirroring the
    // typedef loop) so this surface alone suffices to reference a union by name.
    // (D-FFI-DESCRIPTOR-UNION-MEMBER-INJECTION)
    if (doc.contains("unions")) {
        if (!doc.at("unions").is_array()) {
            emitMalformed(reporter, "shipped-lib descriptor '" + path.generic_string()
                                        + "': 'unions' must be an array");
        } else {
            std::size_t uidx = 0;
            for (auto const& udef : doc.at("unions")) {
                std::string const at =
                    "'" + path.generic_string() + "' unions[" + std::to_string(uidx) + "]";
                ++uidx;
                if (!udef.is_object()) {
                    emitMalformed(reporter, "shipped-lib descriptor " + at + ": must be an object");
                    continue;
                }
                (void)rejectUnknownKeys(reporter, udef,
                                        "unions[" + std::to_string(uidx - 1) + "]",
                                        {"name", "fields"});
                if (!udef.contains("name") || !udef.at("name").is_string()
                    || udef.at("name").get<std::string>().empty()) {
                    emitMalformed(reporter, "shipped-lib descriptor " + at
                                                + ": missing or empty 'name'");
                    continue;
                }
                std::string const uname = udef.at("name").get<std::string>();
                if (!udef.contains("fields") || !udef.at("fields").is_array()
                    || udef.at("fields").empty()) {
                    emitMalformed(reporter, "shipped-lib descriptor " + at
                                                + ": 'fields' must be a non-empty array");
                    continue;
                }
                ShippedUnion ust;
                ust.name = uname;
                std::vector<TypeId> memberTypes;
                if (!decodeStructFieldList(udef.at("fields"), at, interner, typeReg,
                                           reporter, ust.fields, memberTypes,
                                           mergedNamedTypes)) {
                    continue;
                }
                // A union member has NO explicit byte offset — every member overlays
                // at 0 by union semantics. An explicit `offset` here is a config
                // confusion with the c107 explicit-offset STRUCT overlay channel;
                // fail loud rather than silently drop it.
                bool badOffset = false;
                for (auto const& fld : ust.fields) {
                    if (fld.offset.has_value()) {
                        emitMalformed(reporter, "shipped-lib descriptor " + at
                                                    + ": a union member must not declare an "
                                                      "'offset' (every member overlays at 0; an "
                                                      "explicit-offset overlapping layout is the "
                                                      "'structs' channel)");
                        badOffset = true;
                        break;
                    }
                }
                if (badOffset) continue;
                ust.typeId = interner.unionType(uname, memberTypes);
                // Option C: publish the union NAME so a later surface (structs
                // field, signature, typedef) can spell it by name. Address-stable
                // backing via `typedefNameStore` (the deque never moves an element).
                typedefNameStore.push_back(uname);
                mergedNamedTypes.push_back(
                    NamedTypeBinding{std::string_view{typedefNameStore.back()}, ust.typeId});
                out.unions.push_back(std::move(ust));
            }
        }
    }

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
                    + "' (expected "
                    + allowedList(allNames(kShippedSymbolKindTable), " or ")
                    + ")");
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
                    + "' (expected "
                    + allowedList(allNames(kShippedSymbolLinkageTable), " or ")
                    + ")");
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

        // FC17.9(c) (D-CSUBSET-SETJMP): optional `returnsTwice` bool (default false) —
        // TRUE for setjmp/_setjmp. Threaded onto the injected symbol's returnsTwice, then
        // onto a per-Call MirInstFlags::ReturnsTwice at HIR->MIR (the isVolatile->Volatile
        // mirror) so the optimizer's returns-twice-aware passes read it. Same decode shape
        // as `noreturn` above.
        bool returnsTwice = false;
        if (sym.contains("returnsTwice")) {
            if (!sym.at("returnsTwice").is_boolean()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": 'returnsTwice' must be a boolean");
                continue;
            }
            returnsTwice = sym.at("returnsTwice").get<bool>();
        }

        // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER): optional `synthesize` recipe tag
        // (default empty) — marks a pe64 <threads.h> shim symbol (mtx_lock etc.) whose
        // body the synth pass emits over kernel32, rather than a plain FFI import.
        // Closed-vocab + name-invariant fail-loud: a non-empty value MUST be a known
        // recipe id (`isKnownSynthesizeRecipe`) AND equal this symbol's own `name` (the
        // synth pass keys the body on the symbol name — a mismatch would emit the wrong
        // recipe). A typo in either fails the read (never a silent wrong shim). The
        // `linkageFromName`/closed-enum precedent, applied to a config verb.
        std::string synthesize;
        if (sym.contains("synthesize")) {
            if (!sym.at("synthesize").is_string()) {
                emitMalformed(reporter, "shipped-lib descriptor " + at
                                            + ": 'synthesize' must be a string");
                continue;
            }
            synthesize = sym.at("synthesize").get<std::string>();
            if (!synthesize.empty()) {
                if (!isKnownSynthesizeRecipe(synthesize)) {
                    emitMalformed(reporter, "shipped-lib descriptor " + at
                        + ": unknown 'synthesize' recipe id '" + synthesize
                        + "' (closed vocabulary — see isKnownSynthesizeRecipe)");
                    continue;
                }
                if (synthesize != name) {
                    emitMalformed(reporter, "shipped-lib descriptor " + at
                        + ": 'synthesize' recipe id '" + synthesize
                        + "' must equal the symbol name '" + name
                        + "' (the synth pass identifies the recipe by symbol name)");
                    continue;
                }
            }
        }

        // c156 (D-LK-ELF-SYMBOL-VERSIONING): optional per-symbol REQUIRED ELF
        // version string. Absent ⇒ unversioned (the default). A flat string
        // applies to every target; a `{ "variants": [ {when:{arch?,format?},
        // value}, … ] }` object selects PER-TARGET — glibc `realpath` is
        // `GLIBC_2.3` on x86_64 but the single baseline `GLIBC_2.17` on
        // aarch64, so a flat string would wrongly require GLIBC_2.3 on the
        // arm64 leg (whose libc has no such version node). The reader resolves
        // the version to a SINGLE string for the ACTIVE target HERE, so the
        // whole downstream pipeline + the ELF writer carry a plain resolved
        // string (the writer emits `.gnu.version_r` from it with NO arch/format
        // branch — the agnostic law). 0 matching variants ⇒ empty ⇒ unversioned
        // on that target (LEGAL — the aarch64 realpath case). EAGER: every
        // variant's shape is validated regardless of which is active (a
        // malformed INACTIVE variant fails the read on EVERY target —
        // anti-lurking, mirrors `signatureByDataModel` / the struct variants).
        std::string version;
        if (!decodePerTargetSymbolString(sym, "version", at, idx - 1,
                                         activeTarget, activeFormat,
                                         activeDataModelName, reporter, version))
            continue;

        // TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME): the optional
        // per-symbol LINK BASE NAME — the UNDECORATED name the shipped library
        // exports for this C identifier ON THIS TARGET. Absent ⇒ empty ⇒ the
        // canonical `name` (every symbol until opted in; byte-identical image).
        // SAME per-target `variants` shape as `version` one line up, through the
        // SAME decoder, because it is the same kind of fact: a string whose
        // correct value is a function of (arch, format). Darwin's 64-bit-inode
        // ABI is `fstat$INODE64` on x86_64 and the plain name on arm64, so the
        // x86_64 arm is a `when:{format:"macho",arch:"x86_64"}` variant and every
        // other target matches nothing and keeps the identifier.
        //
        // ★ THE VALUE IS UNDECORATED ON PURPOSE. The leading `_` Mach-O puts on
        // every C symbol is composed downstream by `ffi::linkNameFor` ->
        // `applyCMangling` — the SAME single call the un-overridden path takes.
        // Config never spells a per-FORMAT fact per-symbol.
        std::string linkName;
        if (!decodePerTargetSymbolString(sym, "linkName", at, idx - 1,
                                         activeTarget, activeFormat,
                                         activeDataModelName, reporter, linkName))
            continue;

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

        // Optional per-SYMBOL `library` OVERRIDE — the per-object-format runtime
        // image for THIS symbol alone, SAME shape as the descriptor-level `library`
        // map. Absent ⇒ empty (the symbol inherits the descriptor's map). Present ⇒
        // the RAW override, which the semantic injector MERGES over the descriptor
        // map (symbol keys win; an omitted format inherits the descriptor's) — so a
        // single symbol can bind a different image than its header default (pe
        // `strftime`→ucrtbase while the rest of <time.h> stays on msvcrt). Validated
        // through the SAME `decodeLibraryMap` chokepoint as the descriptor-level map
        // (unknown format key / non-string value fail loud); a non-object node skips
        // this symbol (the symbol-loop collect-all pattern). AGNOSTIC: a generic
        // per-format map, no name/arch/format identity branch.
        std::unordered_map<std::string, std::string> symLibrary;
        if (sym.contains("library")
            && !decodeLibraryMap(sym.at("library"), at, "library", reporter, symLibrary))
            continue;

        // D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF: the per-SYMBOL `realization`
        // override, the exact sibling of the `library` override just above and
        // merged over the descriptor's map by the SAME rule (symbol keys win, an
        // omitted format inherits). It exists at symbol granularity for the same
        // reason `library` does: one row of a header can diverge from the rest —
        // a header may be importable on a format for most of its surface while
        // one symbol has no platform implementation there at all.
        std::unordered_map<std::string, std::string> symRealization;
        if (sym.contains("realization")
            && !decodeRealizationMap(sym.at("realization"), at, "realization",
                                     reporter, symRealization))
            continue;

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
                        + kv.key() + "' (expected one of "
                        + allowedList(allNames(kDataModelTable)) + ")");
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
                    parseTypeFromText(ovText, interner, typeReg, reporter, mergedNamedTypes);
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
                                 "noreturn", "returnsTwice", "synthesize", "version",
                                 "linkName", "library", "realization"});

        // Decode the signature via the ONE type-text decoder. A decode failure
        // is the CRITICAL fail-loud: F_ShippedLibUnsupportedType, and the
        // symbol is NEVER appended with InvalidType (it is dropped from `out`,
        // and the whole read fails via the errorCount delta below). The BASE
        // text is decoded even when an override is active (both must be
        // valid); the EFFECTIVE signature is the active model's.
        TypeId const baseSig = parseTypeFromText(sigText, interner, typeReg, reporter, mergedNamedTypes);
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
            sig = parseTypeFromText(effectiveSigText, interner, typeReg, reporter, mergedNamedTypes);
            // Already validated above; a second-parse failure here would be
            // interner drift — covered by the errorCount delta either way.
            if (!sig.valid() || sig == InvalidType) continue;
        }

        out.symbols.push_back(
            ShippedSymbol{std::move(name), sig, kind, linkage, std::move(symAvail),
                          noreturn, returnsTwice, std::move(synthesize),
                          std::move(version), std::move(linkName),
                          std::move(symLibrary), std::move(symRealization)});
    }

    // ══ D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF — R1 + R3, AT LOAD ==========
    //
    // Both are checked on the descriptor BEING READ, over EVERY declared format
    // rather than the active one, so an arm no current target selects cannot rot.
    // Cost is one `is_regular_file` per realization entry and one map lookup per
    // format — 39 of the 40 descriptors declare no `realization` at all and pay
    // literally nothing.
    {
        auto effective = [&](std::unordered_map<std::string, std::string> const& base,
                             std::unordered_map<std::string, std::string> const& ov) {
            auto m = base;
            for (auto const& [k, v] : ov) m.insert_or_assign(k, v);
            return m;
        };
        auto checkOwner =
            [&](std::unordered_map<std::string, std::string> const& lib,
                std::unordered_map<std::string, std::string> const& real,
                std::string const& ctx) {
                for (auto const& [fmt, src] : real) {
                    // R3 — an IMAGE and a SOURCE for one format. Two owners for one
                    // body is the defect, not a fallback: silently preferring
                    // either is how a program links against an image that does not
                    // export the symbol and then dies at LOAD, with no diagnostic
                    // at any compile stage.
                    if (auto const libIt = lib.find(fmt); libIt != lib.end()) {
                        emitMalformed(reporter,
                            "shipped-lib descriptor '" + path.generic_string() + "' "
                            + ctx + " declares BOTH an import ('library." + fmt
                            + "' = '" + libIt->second
                            + "') AND a shipped-source realization ('realization."
                            + fmt + ".source' = '" + src + "') for the object format '"
                            + fmt + "' — two owners for one body "
                            "(D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF R3)");
                    }
                    // R1 — the named source is not there. Deliberately NOT reported
                    // as a bare "file not found": the message names BOTH sides,
                    // because either half alone leaves the reader guessing which
                    // one is wrong.
                    if (!resolveShippedSourcePath(src).has_value()) {
                        emitMalformed(reporter,
                            "shipped-lib descriptor '" + path.generic_string() + "' "
                            + ctx + " declares a shipped-source realization naming '"
                            + src + "' for the object format '" + fmt
                            + "', but no readable file exists there (resolved "
                              "against src/dss-config/) — that format would carry a "
                              "DECLARED symbol with no body "
                              "(D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF R1)");
                    }
                }
            };
        checkOwner(out.library, out.realization, "(root)");
        for (std::size_t i = 0; i < out.symbols.size(); ++i) {
            auto const& sym = out.symbols[i];
            if (sym.realization.empty() && out.realization.empty()) continue;
            checkOwner(effective(out.library, sym.library),
                       effective(out.realization, sym.realization),
                       "symbols[" + std::to_string(i) + "] ('" + sym.name + "')");
        }
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
                                                mergedNamedTypes)) {
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
                                                    mergedNamedTypes)) {
                        okVariants = false; break;
                    }
                    WhenMatch const wm = matchVariantWhen(
                        vdef.at("when"), WhenAxes::FullTarget, vat + ".when",
                        activeTarget, activeFormat, activeFormatName,
                        activeDataModelName, reporter);
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
            TypeId const cty = parseTypeFromText(typeText, interner, typeReg, reporter, mergedNamedTypes);
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

    // (6) `typedefs` — resolved EARLY, BEFORE symbols/constants/structs (see the
    // Option C block just before the `symbols` loop above,
    // D-FFI-DESCRIPTOR-TYPEDEF-NAME-RESOLUTION). Relocated so each typedef's
    // `name -> TypeId` is threaded into `mergedNamedTypes` and can be spelled BY
    // NAME (`ptr<Tcl_Obj>`) by every later `parseTypeFromText` call, instead of
    // re-inlining the full `struct "Tcl_Obj" {…}` body at ~45 sites.

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
    //
    // (6.5.pre) THE BY-NAME DEPENDENCY GATE. The loop below PUBLISHES each
    // injected struct's tag name into `mergedNamedTypes` (the typedef/union
    // Option C mirror), so a LATER entry may spell an EARLIER one as a field type
    // BY NAME — `{"name":"it_interval","type":"timeval"}` — instead of restating
    // the inner body once per format (the layout-disagreement failure class).
    // But an earlier entry carrying per-target `variants` is NOT published when
    // ZERO of them match: `struct timeval` genuinely does not exist on a
    // pe/unknown-format read, so neither can a struct that embeds one. Skipping
    // the DEPENDENT — exactly as `matchCount == 0` skips the dependency — is the
    // honest answer. Letting the EAGER field decode fail instead would take the
    // WHOLE descriptor down on every target lacking the inner struct (both
    // all-descriptor sweeps read every shipped file on every format, and the
    // nullopt direct-API/LSP read selects no variant at all).
    //
    // SOURCE-ORDER-EXACT, so fail-loud survives intact: only a name this
    // descriptor declared BEFORE the referring entry can suppress it. A FORWARD
    // name (declared later), a SELF reference, and a TYPO (declared nowhere) all
    // fall through to the ordinary decode, where `parseTypeFromText` reports
    // `unknown type '<name>'` and `decodeStructFieldList` adds
    // F_ShippedLibUnsupportedType — two loud diagnostics, never a silent empty
    // type. Content-blind: a membership test over the descriptor's OWN declared
    // vocabulary, never an `if (name == "timeval")`.
    // (D-CSUBSET-DARWIN-BSD-STRUCT-BY-NAME)
    auto declaredNamesOf = [&](char const* key) {
        std::vector<std::string> names;
        if (doc.contains(key) && doc.at(key).is_array()) {
            for (auto const& e : doc.at(key)) {
                if (e.is_object() && e.contains("name") && e.at("name").is_string())
                    names.push_back(e.at("name").get<std::string>());
            }
        }
        return names;
    };
    // Declared before the loop starts: every `typedefs` + `unions` name (both
    // surfaces decode ABOVE). Struct names join as the loop walks past them.
    std::vector<std::string> declaredEarlier = declaredNamesOf("typedefs");
    {
        auto un = declaredNamesOf("unions");
        declaredEarlier.insert(declaredEarlier.end(), un.begin(), un.end());
    }
    auto publishedByName = [&](std::string const& n) {
        return std::any_of(mergedNamedTypes.begin(), mergedNamedTypes.end(),
                           [&](NamedTypeBinding const& nb) { return nb.name == n; });
    };
    // Does this entry name an EARLIER-declared type that is NOT published for the
    // active target? Scans the flat `fields` AND every variant's `fields` — the
    // eager decode covers them all, so ONE unavailable reference anywhere in the
    // entry makes the whole entry unavailable here. The test is on the WHOLE
    // `type` text (the by-name spelling); a compound spelling that merely embeds
    // an unavailable name (`ptr<timeval>`) is NOT admitted and still fails loud —
    // the gate accepts only what it can prove.
    auto referencesUnavailable = [&](json const& sdef) {
        auto scan = [&](json const& fields) {
            if (!fields.is_array()) return false;
            for (auto const& f : fields) {
                if (!f.is_object() || !f.contains("type") || !f.at("type").is_string())
                    continue;
                std::string const t = f.at("type").get<std::string>();
                if (std::find(declaredEarlier.begin(), declaredEarlier.end(), t)
                        != declaredEarlier.end()
                    && !publishedByName(t))
                    return true;
            }
            return false;
        };
        if (sdef.contains("fields") && scan(sdef.at("fields"))) return true;
        if (sdef.contains("variants") && sdef.at("variants").is_array()) {
            for (auto const& v : sdef.at("variants"))
                if (v.is_object() && v.contains("fields") && scan(v.at("fields")))
                    return true;
        }
        return false;
    };
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

                // (6.5.pre) THE BY-NAME DEPENDENCY GATE — see the note above the
                // loop. Evaluated BEFORE `sname` joins `declaredEarlier`, so a
                // SELF reference is a forward name and still fails loud; recorded
                // right after, so every entry BELOW may name this one.
                bool const dependencyUnavailable = referencesUnavailable(sdef);
                declaredEarlier.push_back(sname);
                if (dependencyUnavailable) continue;   // unavailable here → inject nothing

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
                                               mergedNamedTypes)) {
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
                                                   reporter, vFields, vFieldTypes, mergedNamedTypes)) {
                            okVariants = false; break;
                        }

                        // Does this variant's `when` MATCH the active target? (the
                        // SHARED selector — typed surfaces allow {arch,format}.)
                        WhenMatch const wm = matchVariantWhen(
                            vdef.at("when"), WhenAxes::FullTarget, vat + ".when",
                            activeTarget, activeFormat, activeFormatName,
                            activeDataModelName, reporter);
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
                // Option C: publish this struct's TAG name so a LATER surface —
                // above all another struct's FIELD — can spell it BY NAME
                // (`{"name":"it_value","type":"timeval"}`), which is what keeps an
                // inner struct's per-format widths in ONE place instead of being
                // restated inside every outer struct. Byte-identical to the
                // typedef/union publications (address-stable `typedefNameStore`
                // backing, appended only for a SELECTED entry so an unselected
                // struct stays unnameable). Publication is strictly ADDITIVE: the
                // scan is first-match, structs are published LAST, so no name that
                // already resolved changes meaning.
                // (D-CSUBSET-DARWIN-BSD-STRUCT-BY-NAME)
                typedefNameStore.push_back(sname);
                mergedNamedTypes.push_back(
                    NamedTypeBinding{std::string_view{typedefNameStore.back()}, sst.typeId});
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

    // (7.5) A macro that SHADOWS one of this descriptor's own symbol rows on a
    // format they share, WITHOUT referencing it, is a defect — see
    // `checkMacroSymbolShadowing` for why "same name" alone is a false rule and why
    // this reads the raw JSON instead of the decoded `out.macros`. Runs here, after
    // both surfaces exist, and its diagnostics fail the read through the errBefore
    // delta below (never a partial surface).
    checkMacroSymbolShadowing(doc, out, path.generic_string(), reporter);

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
    bool const declaredUnions         = declaresArray("unions");
    bool const declaredConstants      = declaresArray("constants");
    bool const declaredTypedefs       = declaresArray("typedefs");
    bool const declaredMacroVariants  = declaresArray("macros");
    if (out.symbols.empty() && out.constants.empty() && out.floatConstants.empty()
        && out.typedefs.empty() && out.structs.empty() && out.unions.empty()
        && out.macros.empty()
        && !declaredStructs && !declaredUnions && !declaredConstants
        && !declaredTypedefs && !declaredMacroVariants) {
        emitMalformed(reporter,
            std::string{"shipped-lib descriptor '"} + path.generic_string()
                + "': declares nothing — needs at least one of 'symbols', "
                  "'constants', 'floatConstants', 'typedefs', 'structs', 'unions', "
                  "or 'macros'");
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

std::optional<std::vector<std::string>>
readShippedLibIncludes(std::filesystem::path const&    path,
                       DiagnosticReporter&             reporter,
                       std::optional<ObjectFormatKind> activeFormat) {
    // Interner-FREE `includes` read for the two `systemDirs`-bearing tiers (the
    // preprocessor macro-splice + the import resolver), which have no interner.
    // Decodes through the SAME `decodeShippedIncludes` chokepoint as the full read,
    // so the interner-free and interned reads validate identically (the
    // `readShippedLibMacros`/`readShippedLibAvailability` lock-step precedent). No
    // `header`/typed-surface gate — the semantic read owns those (this stays no
    // STRICTER than the full read). std::nullopt on a broken JSON / malformed
    // `includes`; EMPTY ⇒ the descriptor declares no `includes` (the common case).
    // `activeFormat` selects which CONDITIONAL edges are active; every entry's
    // SHAPE is validated regardless, so this is no stricter and no laxer per
    // target than the full read on the same document.
    std::size_t const errBefore = reporter.errorCount();
    json const* const docPtr = cachedDescriptorJson(path, reporter);
    if (!docPtr) return std::nullopt;
    json const& doc = *docPtr;
    std::vector<std::string> out;
    decodeShippedIncludes(doc, path.generic_string(), reporter, out, activeFormat);
    if (reporter.errorCount() != errBefore) return std::nullopt;
    return out;  // empty ⇒ no transitive edges
}

void forEachDescriptorInClosure(
    std::filesystem::path const&                             startPath,
    std::span<std::filesystem::path const>                   systemDirs,
    HeaderNameMatching                                       matching,
    std::optional<ObjectFormatKind>                          activeFormat,
    std::unordered_set<core::PathIdentity>&                  visited,
    std::function<void(std::filesystem::path const&)> const& visit,
    std::function<void(std::string const&,
                       HeaderSearchResult const&)> const&    onUnresolvedInclude,
    std::function<void(std::string const&,
                       std::filesystem::path const&)> const& onUnavailableChild) {
    // ★ CYCLE / DIAMOND GUARD (correctness must): a single DFS keyed on the
    // weakly-canonical descriptor path (the SAME key the semantic readDescriptors
    // dedup + cachedDescriptorJson use). A path is visited AT MOST ONCE, so a cycle
    // A→B→A stops at the second A and a diamond's shared leaf is visited once. The
    // recursion is bounded by the finite shipped-descriptor count — no fixpoint
    // iteration, a single DFS is complete + terminating.
    auto const key = descriptorPathKey(startPath);
    if (!visited.insert(key).second) return;   // already in the closure

    // PARENT FIRST — visit this descriptor before the siblings its `includes`
    // declares (so the caller records/splices parent-before-transitive-child; the
    // semantic first-wins dedup then lets the named parent's surface beat a
    // transitively-included sibling's on any name collision).
    visit(startPath);

    // ★ A DESCRIPTOR THAT DOES NOT EXIST ON THIS FORMAT DECLARES NOTHING ON IT —
    // ITS EDGES INCLUDED. Only ever reached for the ROOT (a child's availability is
    // tested in the loop below, BEFORE it is visited), and the root is the header
    // the USER named: the caller owns that verdict (the semantic tier's
    // `F_ShippedHeaderUnavailableForTarget`, positioned on the `#include` line), so
    // the root is still VISITED and only the DESCENT stops.
    // ⚠ THIS FIXED A REAL LEAK, not a hypothetical one: before the gate, a root
    // refused as unavailable still had its whole `includes` closure recorded, so
    // the semantic tier injected every SIBLING's surface for an `#include` that had
    // just been rejected. It was unobservable only because the rejection also
    // errored the compile — a coincidence, not a design.
    if (activeFormat.has_value()
        && !shippedHeaderAvailableForFormat(startPath, *activeFormat)) {
        return;
    }

    // Read this descriptor's `includes` interner-free with a THROWAWAY reporter: a
    // malformed `includes` FIELD is surfaced by the semantic readShippedLibDescriptor
    // that reads the SAME descriptor (the import resolver records a ref per closure
    // descriptor) — never silent, never double-reported here (the
    // shippedHeaderAvailableForFormat throwaway-reporter precedent). A malformed
    // field ⇒ nullopt ⇒ no children traversed (the loud report comes from semantic).
    // `activeFormat` selects the ACTIVE edges: an entry whose `when` does not match
    // is not an edge on this target and never appears here.
    DiagnosticReporter throwaway;
    auto const includes = readShippedLibIncludes(startPath, throwaway, activeFormat);
    if (!includes) return;
    for (std::string const& headerName : *includes) {
        // Resolve each sibling by the SAME `<stem>.json` funnel a source
        // `#include <h>` uses (so the transitive edge and a direct include agree
        // byte-for-byte). An entry that resolves to NO descriptor is a config error
        // — the caller surfaces it LOUD (this is the ONLY tier that can, since the
        // interner-less semantic tier has no systemDirs). Continue past it so one
        // typo does not swallow the rest of the closure.
        // D-PP-HEADER-CASE-INSENSITIVE-PE: the SAME case policy the source
        // spelling gets — a `includes:["Windows.h"]` edge must reach
        // `windows.json` on a pe build from ANY host, and must NOT on an elf one.
        HeaderSearchResult child =
            resolveSystemDescriptor(headerName, systemDirs, matching);
        if (child.status != HeaderSearchStatus::Found) {
            onUnresolvedInclude(headerName, child);
            continue;
        }
        // ★★ AN ACTIVE EDGE WHOSE CHILD DOES NOT EXIST ON THIS FORMAT IS A CONFIG
        // CONTRADICTION, AND IT IS LOUD. The config said "on this format, including
        // me also includes X" and, in the same breath, "X does not exist on this
        // format". There is no correct silent answer: dropping the child hides a
        // surface the parent PROMISED, and recording it makes the semantic tier
        // report a header the user never wrote. So the walker reports it and each
        // tier renders it in its own voice, exactly as `onUnresolvedInclude` already
        // works — the resolver (which alone owns a positioned `#include` span) is
        // loud; the preprocessor stays silent so nothing double-reports.
        // ⚠ Invariant (i) in `validateShippedIncludeClosure` refuses this STATICALLY
        // over the whole corpus, including arms no current target selects. This arm
        // is the belt to that invariant's braces: the invariant is a sweep, and a
        // sweep only covers the corpus it was run over.
        if (activeFormat.has_value()
            && !shippedHeaderAvailableForFormat(child.path, *activeFormat)) {
            onUnavailableChild(headerName, child.path);
            continue;
        }
        forEachDescriptorInClosure(child.path, systemDirs, matching, activeFormat,
                                   visited, visit, onUnresolvedInclude,
                                   onUnavailableChild);   // DFS recurse
    }
}

void forEachDescriptorInClosure(
    std::filesystem::path const&                             startPath,
    std::span<std::filesystem::path const>                   systemDirs,
    HeaderNameMatching                                       matching,
    std::unordered_set<core::PathIdentity>&                  visited,
    std::function<void(std::filesystem::path const&)> const& visit,
    std::function<void(std::string const&,
                       HeaderSearchResult const&)> const&    onUnresolvedInclude) {
    // The FORMAT-BLIND walk. `onUnavailableChild` is an empty lambda and that is
    // SAFE BY CONSTRUCTION, not by hope: the only site that invokes it is guarded
    // on `activeFormat.has_value()`, and this overload passes nullopt. See the
    // header for why this is an overload rather than a default argument.
    forEachDescriptorInClosure(startPath, systemDirs, matching,
                               /*activeFormat=*/std::nullopt, visited, visit,
                               onUnresolvedInclude,
                               [](std::string const&,
                                  std::filesystem::path const&) {});
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

namespace {

// Is a `variants`-bearing typed entry REACHABLE on object format `fmt`? True iff
// the entry declares no `variants` (a flat entry is present on every format its
// descriptor is) or at least ONE variant's `when` is format-compatible.
//
// ★ THIS IS A NAME-PRESENCE QUESTION, WHICH IS STRICTLY WEAKER THAN THE
// SELECTION QUESTION, AND THAT IS DELIBERATE. `readShippedLibDescriptor` asks
// "which arm is ACTIVE for this (arch, format, dataModel)?" and needs all three
// axes. A surface SCAN has no arch and no data model, and it does not need
// them: a variant set exists to give a name a different TYPE or LAYOUT per
// arch, never to make the name exist on one arch and not another. So the honest
// question here is "could this arm be selected on THIS FORMAT, for some target?"
// — `WhenAxes::FormatReachability`. The two wrong answers were both available
// and both silent: asking the full question with no arch says NoMatch for every
// arch-keyed arm and UNDER-reports the surface; ignoring `variants` entirely
// OVER-reports it. Two fail-loud invariants are built on this answer.
//
// LENIENT on shape: a malformed `variants` node is reported for real by the
// semantic read of the same descriptor; here it simply yields "not reachable",
// and the caller's error-count delta turns a malformed descriptor into a refusal
// to answer at all rather than a wrong answer.
[[nodiscard]] bool
typedEntryReachableOnFormat(json const& entry, std::string const& at,
                            ObjectFormatKind fmt,
                            std::string const& fmtName,
                            DiagnosticReporter& reporter) {
    auto const it = entry.find("variants");
    if (it == entry.end()) return true;             // flat ⇒ always present
    if (!it->is_array() || it->empty()) return false;
    std::size_t vidx = 0;
    for (auto const& vdef : *it) {
        std::string const vat = at + " variants[" + std::to_string(vidx) + "]";
        ++vidx;
        if (!vdef.is_object() || !vdef.contains("when")
            || !vdef.at("when").is_object()) {
            continue;
        }
        WhenMatch const wm = matchVariantWhen(
            vdef.at("when"), WhenAxes::FormatReachability, vat + ".when",
            /*activeTarget=*/std::nullopt, fmt, fmtName,
            /*activeDataModelName=*/std::string_view{}, reporter);
        if (wm == WhenMatch::Match) return true;
    }
    return false;
}

// Harvest every NAME an entry array contributes, applying the per-entry
// reachability gate. `key` is the surface array's name; `hasVariants` says
// whether that surface admits `variants` at all (the flat surfaces skip the
// reachability probe entirely rather than probe a key that cannot be there).
void harvestNamedSurface(json const& doc, std::string const& pathStr,
                         char const* key, bool surfaceAdmitsVariants,
                         ObjectFormatKind fmt, std::string const& fmtName,
                         DiagnosticReporter& reporter,
                         std::vector<std::string>& out) {
    auto const it = doc.find(key);
    if (it == doc.end() || !it->is_array()) return;
    std::size_t idx = 0;
    for (auto const& e : *it) {
        std::string const at = "'" + pathStr + "' " + key + "["
                               + std::to_string(idx) + "]";
        ++idx;
        if (!e.is_object() || !e.contains("name") || !e.at("name").is_string())
            continue;                       // shape is the full read's to refuse
        std::string name = e.at("name").get<std::string>();
        if (name.empty()) continue;
        if (surfaceAdmitsVariants
            && !typedEntryReachableOnFormat(e, at, fmt, fmtName, reporter)) {
            continue;
        }
        out.push_back(std::move(name));
    }
}

}  // namespace

std::optional<std::vector<std::string>>
shippedSurfaceNamesForFormat(std::filesystem::path const& path,
                             ObjectFormatKind             fmt,
                             DiagnosticReporter&          reporter) {
    std::size_t const errBefore = reporter.errorCount();
    json const* const docPtr = cachedDescriptorJson(path, reporter);
    if (!docPtr) return std::nullopt;
    json const&       doc      = *docPtr;
    std::string const pathStr  = path.generic_string();
    std::string const fmtName{objectFormatKindName(fmt)};

    std::vector<std::string> out;

    // The DOCUMENT-level availability set is the TIER-2 fallback for a symbol
    // that declares none of its own — the same two-tier resolution the corpus
    // symbol index uses, so "which symbols exist on F" has ONE answer.
    std::vector<std::string> docAvail;
    decodeShippedAvailability(doc, pathStr, reporter, docAvail);

    // symbols — the one surface with a PER-ENTRY availability set.
    if (auto const symsIt = doc.find("symbols");
        symsIt != doc.end() && symsIt->is_array()) {
        std::size_t idx = 0;
        for (auto const& sym : *symsIt) {
            std::string const at =
                "'" + pathStr + "' symbols[" + std::to_string(idx) + "]";
            ++idx;
            if (!sym.is_object() || !sym.contains("name")
                || !sym.at("name").is_string())
                continue;
            std::string name = sym.at("name").get<std::string>();
            if (name.empty()) continue;
            std::vector<std::string> symAvail;
            decodeShippedAvailability(sym, at, reporter, symAvail);
            if (!objectFormatInAvailabilitySet(
                    symAvail.empty() ? docAvail : symAvail, fmt)) {
                continue;
            }
            out.push_back(std::move(name));
        }
    }

    // The typed surfaces. `constants` / `typedefs` / `structs` admit per-target
    // `variants`; `floatConstants` / `unions` are flat. The flags are read off
    // the SAME facts the full read's key vocabularies declare — a surface that
    // grows `variants` later must flip its flag here, and the surface-name test
    // is what makes that visible.
    harvestNamedSurface(doc, pathStr, "constants",      true,  fmt, fmtName,
                        reporter, out);
    harvestNamedSurface(doc, pathStr, "floatConstants", false, fmt, fmtName,
                        reporter, out);
    harvestNamedSurface(doc, pathStr, "typedefs",       true,  fmt, fmtName,
                        reporter, out);
    harvestNamedSurface(doc, pathStr, "structs",        true,  fmt, fmtName,
                        reporter, out);
    harvestNamedSurface(doc, pathStr, "unions",         false, fmt, fmtName,
                        reporter, out);

    // macros — decoded through the REAL macro decoder with the real format, so a
    // variants-only macro that selects no arm on `fmt` contributes no name here
    // for exactly the reason it contributes no `#define` to a compile.
    {
        std::vector<ShippedMacro> macros;
        decodeShippedMacros(doc, pathStr, reporter, macros, fmt);
        for (auto& m : macros) out.push_back(std::move(m.name));
    }

    if (reporter.errorCount() != errBefore) return std::nullopt;
    return out;
}

namespace {

// ── THE SHIPPED-CORPUS SYMBOL INDEX ──────────────────────────────────────────
// ONE walk of the corpus, TWO public views: `collectShippedExternSymbolFormats`
// (availability only, the `--resolve-library` oracle) and
// `realizeShippedExternSymbols` (the full platform REALIZATION). They were two
// walks; sharing the index is what makes "is X a known system symbol here" and
// "how does the platform realize X here" structurally incapable of disagreeing.
struct CorpusSymbolRow {
    // The declaring descriptor, RELATIVE to the corpus root and in generic form
    // — the sort key, so candidate order is identical on every host (a
    // `recursive_directory_iterator` order is filesystem-dependent).
    std::string relPath;
    // The row's resolved availability: TIER 1 the symbol's own
    // `availableObjectFormats`, else TIER 2 the document's, else unrestricted.
    // EMPTY ⇒ available on EVERY format (the `objectFormatInAvailabilitySet`
    // encoding, verbatim).
    std::vector<std::string> formats;
    bool                     isObject = false;   // `kind: "object"` (vs function)
};

struct CorpusIndex {
    std::filesystem::path                                          root;
    std::unordered_map<std::string, std::vector<CorpusSymbolRow>>  byName;
};

// Decode ONE `availableObjectFormats` array into `out`, generically: every entry
// must name a real format in the `objectFormatKindFromName` vocabulary AND must
// not be the `unknown` sentinel (which spells correctly but selects nothing). A
// bad entry is SKIPPED rather than errored — this scan is lenient by contract
// (the strict validation lives in the semantic read on the same descriptor), and
// skipping can only WIDEN a row's set, never narrow it into a false fail-loud.
// Returns false iff the key is absent / not an array, so the caller can apply the
// next fallback tier.
[[nodiscard]] bool scanAvailability(json const& obj,
                                    std::vector<std::string>& out) {
    auto const it = obj.find("availableObjectFormats");
    if (it == obj.end() || !it->is_array()) return false;
    for (auto const& v : *it) {
        if (!v.is_string()) continue;
        auto const name = v.get<std::string>();
        auto const kind = objectFormatKindFromName(name);
        if (!kind || !isSelectableObjectFormatKind(*kind)) continue;
        out.push_back(name);
    }
    return true;
}

// Build the index by walking `root`. Names + availability only (no signature
// decode, no interner) — a lightweight scan distinct from the full
// `readShippedLibDescriptor`. Lenient per-file: an unreadable / malformed
// descriptor is SKIPPED (its symbols are simply not "known"; the malformedness is
// caught for real by the semantic reader on `#include` plus the corpus-wide
// AllShippedDescriptorsDecode test).
[[nodiscard]] CorpusIndex buildCorpusIndex(std::filesystem::path root) {
    namespace fs = std::filesystem;
    CorpusIndex idx;
    idx.root = std::move(root);
    std::error_code ec;
    for (auto const& entry : fs::recursive_directory_iterator(idx.root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
            continue;
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in) continue;  // lenient: unreadable descriptor skipped
        json j;
        try {
            in >> j;
        } catch (...) {
            continue;
        }
        if (!j.is_object() || !j.contains("symbols")
            || !j.at("symbols").is_array()) {
            continue;
        }
        std::error_code relEc;
        auto rel = fs::relative(entry.path(), idx.root, relEc);
        std::string const relPath =
            relEc ? entry.path().generic_string() : rel.generic_string();
        // DOCUMENT-level availability — the fallback a symbol row with no
        // `availableObjectFormats` key of its own inherits (mirrors the semantic
        // injector's two-level gate). `hasDoc == false` ⇒ the document itself is
        // unrestricted, so such a row is available on every format.
        std::vector<std::string> docFormats;
        bool const hasDoc = scanAvailability(j, docFormats);
        for (auto const& sym : j.at("symbols")) {
            if (!sym.is_object() || !sym.contains("name")
                || !sym.at("name").is_string()) {
                continue;
            }
            CorpusSymbolRow row;
            row.relPath = relPath;
            // TIER 1 per-symbol gate, TIER 2 the document gate, else everywhere
            // (an unrestricted row leaves `formats` EMPTY, which every consumer
            // reads through `objectFormatInAvailabilitySet` as "every format").
            if (!scanAvailability(sym, row.formats) && hasDoc)
                row.formats = docFormats;
            auto const kindIt = sym.find("kind");
            row.isObject = kindIt != sym.end() && kindIt->is_string()
                        && kindIt->get<std::string>() == "object";
            idx.byName[sym.at("name").get<std::string>()]
                .push_back(std::move(row));
        }
    }
    // DETERMINISTIC candidate order, independent of directory-iteration order.
    for (auto& [name, rows] : idx.byName) {
        std::stable_sort(rows.begin(), rows.end(),
                         [](CorpusSymbolRow const& a, CorpusSymbolRow const& b) {
                             return a.relPath < b.relPath;
                         });
    }
    return idx;
}

// The memoized index for the CURRENTLY-RESOLVED corpus root.
//
// ⚠ THE MEMO KEY IS THE RESOLVED CONFIG-DIR PATH, AND THAT IS LOAD-BEARING, NOT
// tidiness. `DSS_CONFIG_ROOT` is mutated IN-PROCESS by tests (the config-path-walk
// and system-dirs suites each re-point it at a scratch tree several times in one
// process, one of them literally named `stale`), and the examples runner is
// in-process too. A memo keyed on anything else — or a process-wide one-shot —
// would answer the FIRST tree's corpus for every later invocation and make the
// answers invocation-ORDER dependent (D-PROGRAM-CONFIG-DIR-WALK-RESOLVES-A-FOREIGN-TREE).
//
// ⚠ A DISCOVERY FAILURE IS NEVER MEMOIZED. `findShippedConfigDir` returning
// nullopt means "we could not locate the corpus RIGHT NOW" — a statement about the
// environment, not about the corpus. Caching it as a positive ("there are no
// shipped symbols") would poison every later lookup in the same process once one
// invocation ran with the env unset.
//
// Realize ONE decoded symbol row of `desc` for the active format. The SINGLE
// kernel both public entry points use, so a name's realization cannot differ
// between "I asked about this name" and "I asked about its descriptor's surface".
// Availability is tested with the ONE shared predicate — never an `if (format ==)`.
[[nodiscard]] ShippedSymbolRealization
realizeRow(ShippedLibDescriptor const& desc, ShippedSymbol const& sym,
           ObjectFormatKind activeFormat, std::string const& formatKey,
           bool docAvailableHere) {
    ShippedSymbolRealization real;
    if (!docAvailableHere
        || !objectFormatInAvailabilitySet(sym.availableObjectFormats,
                                         activeFormat)) {
        real.status = ShippedRealizationStatus::UnavailableForFormat;
        return real;
    }
    // Per-SYMBOL `library` override MERGED OVER the descriptor map (symbol keys
    // win; an omitted format inherits) — the identical merge the semantic injector
    // performs, so the two produce the same image for the same row.
    real.library = desc.library;
    for (auto const& [ovFmt, ovImage] : sym.library)
        real.library.insert_or_assign(ovFmt, ovImage);
    real.version    = sym.version;
    real.recipeId   = sym.synthesize;
    real.linkName   = sym.linkName;
    real.signature  = sym.signature;
    real.isFunction = sym.kind == ShippedSymbolKind::Function;
    // D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF: the per-SYMBOL `realization` override
    // MERGED OVER the descriptor's map by the SAME rule as `library` directly
    // above — one merge shape for one axis pair, so the two cannot answer
    // differently about the same row.
    std::unordered_map<std::string, std::string> realizationMap = desc.realization;
    for (auto const& [ovFmt, ovUnit] : sym.realization)
        realizationMap.insert_or_assign(ovFmt, ovUnit);
    if (auto const it = realizationMap.find(formatKey); it != realizationMap.end()) {
        // ★ THE SHIPPED-SOURCE ARM. The platform declares this symbol exists here
        // and states that its BODY is SHIPPED, not imported. It is a terminal
        // answer, tested BEFORE the library/no-library split below and never a
        // fallback from it: R3 already refused a row that named both, so reaching
        // here means the descriptor named exactly one owner for this body.
        real.shippedSourcePath = it->second;
        real.status = ShippedRealizationStatus::ProvidedByShippedSource;
        return real;
    }
    // ★ THE ENUMERATED NO-LIBRARY ARM. Available here, yet the row names no image
    // FOR this format: the platform says the symbol exists but not where it lives,
    // so there is nothing to bind. A synthesized shim needs no image at all, so a
    // `synthesize` row is REALIZED regardless of the library map.
    real.status = (real.recipeId.empty() && !real.library.contains(formatKey))
                      ? ShippedRealizationStatus::NoLibraryForFormat
                      : ShippedRealizationStatus::Realized;
    return real;
}

// Returns nullptr iff discovery failed. The pointer stays valid for the process:
// the map is node-based and entries are never erased, so an insertion for another
// root cannot move an already-returned index, and the index is IMMUTABLE once
// published (built to completion, then moved in under the lock).
[[nodiscard]] CorpusIndex const* corpusIndex() {
    auto const rootOpt = findShippedConfigDir("shippedLibs");
    if (!rootOpt) return nullptr;   // discovery failed — NOT memoized
    std::string key = rootOpt->generic_string();
    static std::mutex                              mu;
    static std::unordered_map<std::string, CorpusIndex> cache;
    std::lock_guard<std::mutex> const lock{mu};
    if (auto const it = cache.find(key); it != cache.end()) return &it->second;
    CorpusIndex built = buildCorpusIndex(*rootOpt);
    return &cache.emplace(std::move(key), std::move(built)).first->second;
}

} // namespace

std::optional<std::unordered_map<std::string, std::vector<std::string>>>
collectShippedExternSymbolFormats() {
    // ── D-FFI-SHIPPED-SYMBOL-ORACLE-IGNORES-OBJECT-FORMATS ────────────────────
    // name -> the UNION of the object-format names every declaring row makes it
    // available on. EMPTY vector ⇒ available on EVERY format (the
    // `objectFormatInAvailabilitySet` encoding, verbatim), so the union
    // SATURATES: once any row is unrestricted the name is unrestricted and no
    // later restricted row may narrow it back.
    //
    // Derived from the SHARED corpus index rather than from its own walk, so this
    // availability view and the REALIZATION view below read the identical rows.
    CorpusIndex const* const idx = corpusIndex();
    if (idx == nullptr) return std::nullopt;  // discovery failed -> caller falls through
    std::unordered_map<std::string, std::vector<std::string>> byName;
    byName.reserve(idx->byName.size());
    for (auto const& [name, rows] : idx->byName) {
        std::vector<std::string> acc;
        bool unrestricted = false;
        for (auto const& row : rows) {
            if (row.formats.empty()) {   // this row is available everywhere
                unrestricted = true;     // SATURATE — no later row may narrow it
                break;
            }
            for (auto const& f : row.formats) {
                if (std::find(acc.begin(), acc.end(), f) == acc.end())
                    acc.push_back(f);
            }
        }
        byName.emplace(name, unrestricted ? std::vector<std::string>{}
                                          : std::move(acc));
    }
    return byName;
}

std::optional<std::unordered_map<std::string, ShippedSymbolRealization>>
realizeShippedExternSymbols(std::span<std::string const>      names,
                            TypeInterner&                     interner,
                            TypeRegistry&                     typeReg,
                            DataModel                         dataModel,
                            std::optional<std::string_view>   activeTarget,
                            std::optional<ObjectFormatKind>   activeFormat,
                            std::span<NamedTypeBinding const> namedTypes) {
    CorpusIndex const* const idx = corpusIndex();
    if (idx == nullptr) return std::nullopt;   // discovery failed — route unbound
    std::unordered_map<std::string, ShippedSymbolRealization> out;
    if (names.empty()) return out;
    // No active format ⇒ no realization to state. Availability AND the library map
    // are both per-format facts; answering without a format would be exactly the
    // per-name guess this oracle exists to delete.
    if (!activeFormat.has_value()) return out;
    std::string const formatKey{objectFormatKindName(*activeFormat)};

    // (1) Which descriptors do the requested names live in? Only those are read —
    // a TU that hand-declares nothing reads nothing. Candidate order is the
    // index's deterministic relPath order, and the FIRST row available on this
    // format wins (the same first-wins rule cross-descriptor duplicate rows
    // already carry; the corpus ships byte-identical duplicates by convention).
    std::vector<std::string> wantedDescriptors;
    std::unordered_map<std::string, std::vector<CorpusSymbolRow> const*> wantedRows;
    for (auto const& n : names) {
        auto const it = idx->byName.find(n);
        if (it == idx->byName.end()) continue;   // Unknown — absent from `out`
        wantedRows.emplace(n, &it->second);
        for (auto const& row : it->second) {
            if (std::find(wantedDescriptors.begin(), wantedDescriptors.end(),
                          row.relPath) == wantedDescriptors.end())
                wantedDescriptors.push_back(row.relPath);
        }
    }
    if (wantedRows.empty()) return out;

    // (2) Read each candidate descriptor ONCE, through the SAME reader the
    // `#include` path uses — so `variants`, `signatureByDataModel` and per-symbol
    // `library` overrides cannot resolve one way here and another way there.
    //
    // THROWAWAY reporter, and a failed read is SKIPPED: this oracle is consulted
    // for names the user never `#include`d, so an unrelated descriptor's
    // malformedness must not become this program's build failure. Its names then
    // stay `Unknown` and route unbound, where the link tier judges the reference
    // LOUD. (The `shippedHeaderAvailableForFormat` precedent.)
    std::unordered_map<std::string, ShippedLibDescriptor> decoded;
    for (auto const& rel : wantedDescriptors) {
        DiagnosticReporter throwaway;
        auto desc = readShippedLibDescriptor(idx->root / rel, interner, typeReg,
                                             throwaway, dataModel, activeTarget,
                                             activeFormat, namedTypes);
        if (!desc) continue;
        decoded.emplace(rel, std::move(*desc));
    }

    // (3) Per requested name: walk its candidate rows in order and take the first
    // that is AVAILABLE here, then state its outcome. Availability is tested with
    // the ONE shared predicate — never an `if (format == …)`.
    for (auto const& [name, rows] : wantedRows) {
        ShippedSymbolRealization real;
        bool sawRow = false;
        for (auto const& row : *rows) {
            auto const dIt = decoded.find(row.relPath);
            if (dIt == decoded.end()) continue;   // unreadable descriptor
            ShippedLibDescriptor const& desc = dIt->second;
            // The DOCUMENT gate first (a header that does not exist here declares
            // nothing here), then the SYMBOL gate inside `realizeRow` — the same
            // two gates, in the same order, the semantic injector applies.
            bool const docHere = objectFormatInAvailabilitySet(
                desc.availableObjectFormats, *activeFormat);
            // ★ SCAN EVERY ROW OF THIS NAME, NOT JUST THE FIRST. One descriptor
            // routinely declares the SAME name several times with DIFFERENT
            // availability gates — `printf` has an ["elf","macho"] IMPORT row AND a
            // ["pe"] `synthesize` row in stdio.json, and the C11 threads surface
            // carries three rows per name. Stopping at the first match makes the
            // answer depend on DECLARATION ORDER inside the file: MEASURED, it made
            // pe `printf` resolve `UnavailableForFormat` off the elf row and the
            // shim was never claimed, so a hand-declared printf reached the linker
            // as an undefined symbol. A non-realized row is only ever a FALLBACK
            // status — it must never shadow a realizable sibling.
            for (auto const& sym : desc.symbols) {
                if (sym.name != name) continue;
                sawRow = true;
                auto candidate =
                    realizeRow(desc, sym, *activeFormat, formatKey, docHere);
                bool const usable =
                    candidate.status == ShippedRealizationStatus::Realized
                    || candidate.status == ShippedRealizationStatus::NoLibraryForFormat;
                if (usable) { real = std::move(candidate); break; }
                if (real.status == ShippedRealizationStatus::Unknown)
                    real = std::move(candidate);   // remember "declared, not here"
            }
            if (real.status == ShippedRealizationStatus::Realized
                || real.status == ShippedRealizationStatus::NoLibraryForFormat)
                break;   // first descriptor that realizes the name wins
        }
        if (sawRow) out.emplace(name, std::move(real));
    }
    return out;
}

std::optional<std::unordered_map<std::string, ShippedSymbolRealization>>
realizeShippedDescriptorSurfaceFor(std::string_view                  name,
                                   TypeInterner&                     interner,
                                   TypeRegistry&                     typeReg,
                                   DataModel                         dataModel,
                                   std::optional<std::string_view>   activeTarget,
                                   std::optional<ObjectFormatKind>   activeFormat,
                                   std::span<NamedTypeBinding const> namedTypes) {
    CorpusIndex const* const idx = corpusIndex();
    if (idx == nullptr) return std::nullopt;
    std::unordered_map<std::string, ShippedSymbolRealization> out;
    if (!activeFormat.has_value()) return out;   // no format ⇒ no realization
    auto const nameIt = idx->byName.find(std::string{name});
    if (nameIt == idx->byName.end()) return out;
    std::string const formatKey{objectFormatKindName(*activeFormat)};

    // Walk the SAME deterministic candidate order and take the FIRST descriptor
    // that realizes `name` here — so the surface returned belongs to exactly the
    // descriptor `realizeShippedExternSymbols` chose for that name, never a
    // different one that happens to declare the name too.
    for (auto const& row : nameIt->second) {
        DiagnosticReporter throwaway;   // see the skip rationale in the header
        auto desc = readShippedLibDescriptor(idx->root / row.relPath, interner,
                                            typeReg, throwaway, dataModel,
                                            activeTarget, activeFormat, namedTypes);
        if (!desc) continue;
        bool const docHere = objectFormatInAvailabilitySet(
            desc->availableObjectFormats, *activeFormat);
        // Does THIS descriptor realize `name` here? If not, keep looking — the
        // surface of a descriptor that cannot realize the name is not the surface
        // whose cores that name's recipe would call.
        // EVERY row of the name, for the same reason the sibling entry point scans
        // them all: one descriptor declares `printf` twice with different gates,
        // and the elf row must not answer for the pe one.
        bool realizesName = false;
        for (auto const& sym : desc->symbols) {
            if (sym.name != name) continue;
            if (realizeRow(*desc, sym, *activeFormat, formatKey, docHere).status
                == ShippedRealizationStatus::Realized) {
                realizesName = true;
                break;
            }
        }
        if (!realizesName) continue;
        for (auto const& sym : desc->symbols) {
            auto real = realizeRow(*desc, sym, *activeFormat, formatKey, docHere);
            if (real.status != ShippedRealizationStatus::Realized) continue;
            out.emplace(sym.name, std::move(real));   // first row of a name wins
        }
        return out;
    }
    return out;
}

// ═══ D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF — THE SHIPPED SOURCE TREE ═════════════

namespace {

// One descriptor claim: which (format, config-root-relative source path) pair a
// `realization` entry names, plus a human locator for the diagnostic
// ("dirent.json symbols[0] realization").
struct ShippedSourceClaim {
    std::string format;
    std::string source;
    std::string where;
};

// EVERY shipped-source claim any descriptor under `descriptorDir` makes, on ANY
// object format. FORMAT-INDEPENDENT on purpose: the refusals must hold for an arm
// no current target selects, or an inactive arm rots silently — the bidirectional
// half of the bar.
[[nodiscard]] std::vector<ShippedSourceClaim>
claimedShippedSources(std::filesystem::path const& descriptorDir,
                      DiagnosticReporter&          reporter) {
    std::vector<ShippedSourceClaim> claims;
    std::error_code ec;
    if (!std::filesystem::is_directory(descriptorDir, ec)) return claims;
    for (std::filesystem::recursive_directory_iterator
             it{descriptorDir,
                std::filesystem::directory_options::skip_permission_denied, ec},
             end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".json") continue;
        json const* const docPtr = cachedDescriptorJson(it->path(), reporter);
        if (!docPtr) continue;   // a broken descriptor is the full read's business
        std::string const where = it->path().filename().generic_string();
        auto harvest = [&](json const& node, std::string const& ctx) {
            if (!node.is_object()) return;
            for (auto const& kv : node.items()) {
                if (!kv.value().is_object()) continue;
                if (!kv.value().contains("source")) continue;
                if (!kv.value().at("source").is_string()) continue;
                claims.push_back(ShippedSourceClaim{
                    kv.key(), kv.value().at("source").get<std::string>(),
                    where + " " + ctx});
            }
        };
        if (docPtr->contains("realization"))
            harvest(docPtr->at("realization"), "(root) realization");
        if (docPtr->contains("symbols") && docPtr->at("symbols").is_array()) {
            std::size_t i = 0;
            for (auto const& sym : docPtr->at("symbols")) {
                std::string const ctx =
                    "symbols[" + std::to_string(i++) + "] realization";
                if (!sym.is_object() || !sym.contains("realization")) continue;
                harvest(sym.at("realization"), ctx);
            }
        }
    }
    return claims;
}

} // namespace

std::optional<std::filesystem::path> findShippedConfigRootDir() {
    // `findShippedConfigDir` composes `<root>/src/dss-config/<subdir>`; an EMPTY
    // subdir therefore resolves the config root itself, by the SAME precedence
    // ($DSS_CONFIG_ROOT first, then the bounded cwd walk) every other shipped
    // thing uses. Reusing that one function rather than opening a fresh walk is
    // the whole point — the header's own note records that seventeen private
    // cwd-walks under `tests/` drifted from it exactly once each.
    return findShippedConfigDir("");
}

std::optional<std::filesystem::path>
resolveShippedSourcePath(std::string_view configRelativePath) {
    auto const root = findShippedConfigRootDir();
    if (!root) return std::nullopt;
    std::filesystem::path const p =
        (*root / std::filesystem::path{std::string{configRelativePath}})
            .lexically_normal();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return std::nullopt;
    return p;
}

std::vector<std::string>
readShippedSourcesForFormat(std::filesystem::path const& path,
                            std::string_view             formatName) {
    // INTERNER-FREE, DIAGNOSTIC-FREE fast read — the `readShippedLibAvailability`
    // precedent. The driver asks this once per resolved descriptor per build, and
    // the answer is "none" for 39 of the 40 descriptors, so it must not pay for a
    // full typed read. Descriptor HEALTH is owned by the tier that reads it for
    // real (the `#include` path) plus the corpus-wide decode test; a malformed
    // descriptor here simply contributes nothing.
    DiagnosticReporter throwaway{};
    json const* const  docPtr = cachedDescriptorJson(path, throwaway);
    if (!docPtr) return {};
    std::vector<std::string> out;
    auto take = [&](json const& node) {
        if (!node.is_object()) return;
        auto const it = node.find(std::string{formatName});
        if (it == node.end() || !it->is_object()) return;
        if (!it->contains("source") || !it->at("source").is_string()) return;
        std::string src = it->at("source").get<std::string>();
        if (src.empty()) return;
        // The per-symbol override wins over the descriptor default, and a source
        // named twice is ONE build-graph edge — the merge and the dedup are the
        // same operation over a set of PATHS.
        if (std::find(out.begin(), out.end(), src) == out.end())
            out.push_back(std::move(src));
    };
    if (docPtr->contains("realization")) take(docPtr->at("realization"));
    if (docPtr->contains("symbols") && docPtr->at("symbols").is_array())
        for (auto const& sym : docPtr->at("symbols"))
            if (sym.is_object() && sym.contains("realization"))
                take(sym.at("realization"));
    return out;
}

std::vector<std::string>
allShippedSourcesForFormat(std::filesystem::path const& descriptorDir,
                           std::string_view             formatName) {
    // Shares `claimedShippedSources` with the refusals, so "which sources does
    // this format realize" has ONE answer for the driver and the validator. A
    // second walk here is exactly the drift surface this axis exists to remove.
    DiagnosticReporter throwaway{};
    std::vector<std::string> out;
    for (auto const& c : claimedShippedSources(descriptorDir, throwaway)) {
        if (c.format != formatName) continue;
        if (std::find(out.begin(), out.end(), c.source) == out.end())
            out.push_back(c.source);
    }
    std::sort(out.begin(), out.end());   // deterministic across filesystems
    return out;
}

bool validateShippedSourceTree(std::filesystem::path const& descriptorDir,
                               std::filesystem::path const& runtimeRootDir,
                               DiagnosticReporter&          reporter) {
    std::size_t const errBefore = reporter.errorCount();
    auto const        claims    = claimedShippedSources(descriptorDir, reporter);

    // R1 lives at DESCRIPTOR READ TIME (`readShippedLibDescriptor`), where it is a
    // single `is_regular_file` per declared entry and therefore free. It is
    // repeated here so the corpus sweep is a TOTAL statement about the tree rather
    // than a partial one that assumes every descriptor has been read.
    std::unordered_set<core::PathIdentity> claimedPaths;
    for (auto const& c : claims) {
        auto const resolved = resolveShippedSourcePath(c.source);
        if (resolved) {
            claimedPaths.insert(core::PathIdentity::of(*resolved));
            continue;
        }
        emitMalformed(reporter,
            "shipped-lib descriptor '" + c.where + "." + c.format
            + "' declares a shipped-source realization naming '" + c.source
            + "', but no readable file exists there (resolved against "
              "src/dss-config/) — the object format '" + c.format
            + "' would be left with a DECLARED symbol and no body "
              "(D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF R1)");
    }

    // ★★ R2 + R4 ARE ONE RULE, AND FOLDING THEM IS STRICTLY STRONGER THAN
    // KEEPING THEM APART: **every regular file under the runtime tree must be
    // named by some descriptor's `realization` map.**
    //
    // R2 (a source file no descriptor names) is that rule read forwards: nothing
    // can ever add such a file to a build graph, because the ONLY thing that adds
    // one is a descriptor naming it. Inert config, and the direction R1
    // structurally cannot see.
    //
    // R4 (no headers in this tree) is that SAME rule read backwards, and it needed
    // no extension vocabulary at all. The layout mirrors the include namespace, so
    // a private header for the dirent unit would land at `<format>/dirent.h` —
    // which IS the include path `dirent.h` — and would SHADOW the descriptor the
    // unit exists to consume, silently, producing exactly the struct-layout
    // disagreement the compile-time check exists to catch. A header is never a
    // translation unit, so no `realization` can name one, so the unclaimed-file
    // rule refuses it BY CONSTRUCTION. Enumerating `.h` as a special case would
    // have been the glyph-enumerating check this repo keeps deleting — define the
    // complement, never the members. (The other half of R4 — that this tree is
    // never added to an include-search root — holds by construction on the driver
    // side: a shipped source CU is built with the SYSTEM dirs only, and the user's
    // `-I` list is never extended with it.)
    std::error_code ec;
    // ★★★ THE SUBJECT IS DERIVED, NOT PASSED — SO THE WRONG SUBJECT IS
    // UNREPRESENTABLE. `runtimeRootDir` is the runtime ROOT (`.../runtime`); this
    // function walks `<root>/<tier>/src/` and nothing else.
    //
    // ⚠ THE TRAP THIS CLOSES, and it would have looked like a config error
    // rather than a wrong argument: a tier also contains `dist/`, the GENERATED
    // object cache. Handed a tier root, an unfiltered walk refuses every cached
    // object as "named by no descriptor" and EVERY WARM BUILD GOES RED. Deriving
    // `src/` per tier means `dist/` is not merely filtered out, it is never a
    // candidate — and a future `runtime/managed/<lang>/src/` is covered with no
    // edit here.
    //
    // ⚠⚠ AND IT IS NOT CLOSED BY FILTERING TO `.c`. An extension filter would
    // make this function silently ignore a stray `.txt` or `.o` sitting in the
    // AUTHORED tree, which is exactly the inert-config case R2 exists to catch.
    // The problem was the SUBJECT DIRECTORY, so the subject is what got fixed.
    if (std::filesystem::is_directory(runtimeRootDir / "src", ec)) {
        // The caller handed a TIER (or a `src/`) instead of the root. Refusing is
        // the whole point: silently scanning the wrong tree is how a guard passes
        // while guarding nothing.
        emitMalformed(reporter,
            "shipped-source tree: '" + runtimeRootDir.generic_string()
            + "' looks like a TIER (it has a 'src' child), but this check takes the "
              "runtime ROOT and derives '<root>/<tier>/src' itself — passing a tier "
              "would put the generated 'dist/' cache in scope and red every warm "
              "build (D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF R2/R4)");
        return false;
    }
    for (std::filesystem::directory_iterator tierIt{runtimeRootDir, ec}, tierEnd;
         tierIt != tierEnd; tierIt.increment(ec)) {
        if (ec) break;
        if (!tierIt->is_directory(ec)) continue;
        std::filesystem::path const authored = tierIt->path() / "src";
        if (!std::filesystem::is_directory(authored, ec)) continue;
        for (std::filesystem::recursive_directory_iterator
                 it{authored,
                    std::filesystem::directory_options::skip_permission_denied, ec},
                 end;
             it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::filesystem::path const p = it->path();
            if (claimedPaths.contains(core::PathIdentity::of(p))) continue;
            emitMalformed(reporter,
                "shipped-source tree: '" + p.generic_string() + "' is named by NO "
                "shipped-lib descriptor under '" + descriptorDir.generic_string()
                + "' in a 'realization' map, so nothing can ever add it to a build "
                  "graph. Only files a descriptor names may live under '"
                + authored.generic_string()
                + "' — in particular a HEADER here would sit at an INCLUDE PATH and "
                  "silently shadow the descriptor a unit exists to consume "
                  "(D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF R2/R4)");
        }
    }

    return reporter.errorCount() == errBefore;
}

namespace {

// What the ACTIVE closure of one descriptor looks like on ONE object format.
// Built by the SAME `forEachDescriptorInClosure` a compile uses, with the SAME
// edge gate — so nothing here can be true of the sweep and false of a build.
struct ClosureSurfaceOnFormat {
    std::vector<std::string> names;             // union over every closure member
    std::vector<std::string> unresolvedEdges;   // header names resolving to nothing
    std::vector<std::string> unavailableEdges;  // ACTIVE edge, child absent on fmt
    std::vector<std::string> undecodable;       // members whose surface would not decode
};

[[nodiscard]] ClosureSurfaceOnFormat
closureSurfaceOnFormat(std::filesystem::path const&           startPath,
                       std::span<std::filesystem::path const> systemDirs,
                       ObjectFormatKind                       fmt) {
    ClosureSurfaceOnFormat outS;
    std::unordered_set<core::PathIdentity> visited;
    // Case-SENSITIVE resolution, and the choice is load-bearing rather than
    // incidental: this sweep is FORMAT-INDEPENDENT by design (it evaluates arms
    // no current target selects), while `headerNameMatching` is a per-FORMAT
    // policy. Using one format's policy to answer a question about all of them
    // would make the answer depend on which format asked. The conservative POSIX
    // rule is the one every format's own rule is at least as permissive as, so a
    // claim or an edge that resolves here resolves on every format — and one that
    // does not is refused with a spelling the author can fix in one place.
    forEachDescriptorInClosure(
        startPath, systemDirs, kDefaultHeaderNameMatching, fmt, visited,
        [&](std::filesystem::path const& p) {
            DiagnosticReporter throwaway;
            auto names = shippedSurfaceNamesForFormat(p, fmt, throwaway);
            if (!names) {
                outS.undecodable.push_back(p.generic_string());
                return;
            }
            for (auto& n : *names) outS.names.push_back(std::move(n));
        },
        [&](std::string const& headerName, HeaderSearchResult const&) {
            outS.unresolvedEdges.push_back(headerName);
        },
        [&](std::string const& headerName, std::filesystem::path const&) {
            outS.unavailableEdges.push_back(headerName);
        });
    return outS;
}

// The availability set a descriptor DECLARES, as format KINDS, intersected with
// `servedFormats`. An EMPTY declared set means "every format", so it yields the
// whole served set — which is why the served set has to be an INPUT: "every
// format" is not a fact this file can know, and enumerating the whole
// `ObjectFormatKind` table would evaluate `wasm`/`spirv` arms that no shipped
// object-format document declares and refuse the corpus for failing to serve a
// platform nobody targets.
[[nodiscard]] std::vector<ObjectFormatKind>
declaredFormatsOf(std::span<std::string const>     declared,
                  std::span<ObjectFormatKind const> servedFormats) {
    std::vector<ObjectFormatKind> out;
    for (ObjectFormatKind const f : servedFormats) {
        if (objectFormatInAvailabilitySet(declared, f)) out.push_back(f);
    }
    return out;
}

// The corpus-sweep emitter. A DISTINCT code from `emitMalformed`'s
// `F_ShippedLibDescriptorMalformed` on purpose: every descriptor involved here
// is individually well-formed and decodes cleanly, and telling an author their
// JSON is malformed when it is not sends them to the wrong file.
void emitCorpusInvariant(DiagnosticReporter& reporter, std::string msg) {
    dss::report(reporter, DiagnosticCode::F_ShippedCorpusInvariantBroken,
                DiagnosticSeverity::Error, std::move(msg));
}

// The `requires` emitter. Its subject is a CONFIG ROW in a language/target/
// format document, not a descriptor, so it carries the C_ (configuration) code.
void emitUnbackedPredefine(DiagnosticReporter& reporter, std::string msg) {
    dss::report(reporter, DiagnosticCode::C_UnbackedPredefinedMacro,
                DiagnosticSeverity::Error, std::move(msg));
}

}  // namespace

bool validateShippedIncludeClosure(
    std::filesystem::path const&           descriptorDir,
    std::span<ObjectFormatKind const>      servedFormats,
    DiagnosticReporter&                    reporter) {
    // ═══ THE TWO INVARIANTS THAT SHIP WITH THE `includes` EDGE GATE ═══
    //
    // ★★★ WHY THEY ARE PART OF THE GATE AND NOT A SEPARATE NICETY. A conditional
    // edge turns a LOUD failure into a QUIET one for free: before the gate, an
    // `includes` edge to a header that does not exist on the active format
    // produced a diagnostic (the wrong one, on the wrong subject — see the import
    // resolver — but a diagnostic); after the gate, an author can silence that by
    // writing a `when` that simply never fires, and get an empty surface instead
    // of a complaint. A mechanism that lets a config author buy silence must ship
    // with the checks that make silence expensive.
    //
    //   (i)  EDGE FIRES ⇒ CHILD AVAILABLE. For every descriptor D and every
    //        object format F in D's availability set, every `includes` edge of D
    //        ACTIVE on F must resolve to a child descriptor that is itself
    //        available on F. The config may not promise, on F, a surface it
    //        declares absent on F.
    //   (ii) NO EMPTY SURFACE ON A SERVED FORMAT. A descriptor available on F
    //        must contribute AT LEAST ONE name on F — from its own surfaces or
    //        from its active closure. A header that exists and declares nothing
    //        is a header whose `#include` compiles and whose contents silently
    //        are not there, which is the shape of the defect this whole cycle
    //        exists to close.
    //
    // Both are STATIC over the WHOLE corpus and INDEPENDENT OF ANY BUILD'S ACTIVE
    // FORMAT — the `validateShippedSourceTree` posture, and for its reason: an
    // arm no current target selects must not be allowed to rot.
    //
    // ── WHAT THEY HONESTLY DO NOT COVER ────────────────────────────────────
    //  * PARTIAL OMISSION. (ii) is an EXISTENCE claim, not a COMPLETENESS one. A
    //    descriptor that declares one of the forty names its real header
    //    declares passes (ii) exactly as a complete one does. Nothing here can
    //    know the real header's full surface, so nothing here can measure
    //    completeness; the `requires` mechanism is how a CONSUMER states the
    //    specific names it depends on, and that is the only completeness check
    //    in the system.
    //  * SEMANTIC CORRECTNESS. A name being present says nothing about its
    //    signature, its layout, or whether the platform actually behaves that
    //    way. `ShippedTypeConsistency` and the corpus tests own those.
    //  * ARCH AND DATA MODEL. Both invariants are FORMAT-keyed. A variant arm
    //    keyed on an arch this sweep does not enumerate is treated as PRESENT on
    //    every format it is format-compatible with (see
    //    `WhenAxes::FormatReachability`), so an arch-only hole is not visible
    //    here. That is a deliberate weakening: name presence is not an arch
    //    property in any descriptor written so far, and asking the arch question
    //    would require this sweep to enumerate targets it has no business
    //    knowing.
    //  * FORMATS NOT IN `servedFormats`. The caller supplies the served set;
    //    a format left out of it is simply not asked about.
    //  * QUOTE-INCLUDED or NON-SHIPPED headers. Only the shipped descriptor
    //    corpus under `descriptorDir` is in scope.
    std::size_t const errBefore = reporter.errorCount();
    if (servedFormats.empty()) {
        emitCorpusInvariant(reporter,
            "shipped-lib corpus sweep: no served object formats were supplied, so "
            "the include-closure invariants would pass VACUOUSLY over every "
            "descriptor. A sweep that cannot fail is not a sweep — pass the "
            "object-format kinds this build actually serves");
        return false;
    }

    std::error_code ec;
    std::vector<std::filesystem::path> descriptors;
    for (std::filesystem::recursive_directory_iterator
             it{descriptorDir,
                std::filesystem::directory_options::skip_permission_denied, ec},
             end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".json") continue;
        descriptors.push_back(it->path());
    }
    // Deterministic order on every host (a directory iteration order is the
    // filesystem's, and a sweep that reports its failures in a host-dependent
    // order is a sweep whose output cannot be diffed).
    std::sort(descriptors.begin(), descriptors.end());

    std::span<std::filesystem::path const> const searchDirs{&descriptorDir, 1};

    for (std::filesystem::path const& p : descriptors) {
        std::string const rel = p.generic_string();
        DiagnosticReporter availRep;
        auto const avail = readShippedLibAvailability(p, availRep);
        if (!avail) {
            // A descriptor that does not even yield an availability set is
            // malformed; the corpus decode test owns that diagnostic. Reported
            // here too rather than skipped, because a SWEEP that quietly steps
            // over the files it cannot read is a sweep whose green means nothing.
            emitCorpusInvariant(reporter,
                "shipped-lib corpus sweep: '" + rel + "' does not decode far "
                "enough to read its 'availableObjectFormats', so neither "
                "include-closure invariant can be evaluated for it");
            continue;
        }
        for (ObjectFormatKind const fmt : declaredFormatsOf(*avail, servedFormats)) {
            std::string const fmtName{objectFormatKindName(fmt)};
            ClosureSurfaceOnFormat const cs =
                closureSurfaceOnFormat(p, searchDirs, fmt);

            // (i) — the two ways an active edge fails to land.
            for (std::string const& h : cs.unresolvedEdges) {
                emitCorpusInvariant(reporter,
                    "shipped-lib corpus invariant (i): '" + rel + "' declares an "
                    "'includes' edge to '" + h + "' that is ACTIVE on object "
                    "format '" + fmtName + "', but that header resolves to no "
                    "descriptor at all");
            }
            for (std::string const& h : cs.unavailableEdges) {
                emitCorpusInvariant(reporter,
                    "shipped-lib corpus invariant (i): '" + rel + "' is available "
                    "on object format '" + fmtName + "' and its 'includes' edge to "
                    "'" + h + "' is ACTIVE there, but '" + h + "'\'s descriptor "
                    "declares it does NOT exist on '" + fmtName + "'. The config "
                    "promises a surface it also declares absent — gate the edge "
                    "with a 'when', or widen the child's "
                    "'availableObjectFormats'");
            }
            for (std::string const& d : cs.undecodable) {
                emitCorpusInvariant(reporter,
                    "shipped-lib corpus invariant (ii): the closure of '" + rel
                    + "' on object format '" + fmtName + "' contains '" + d
                    + "', whose surface does not decode — the invariant cannot be "
                      "evaluated, and an unevaluated invariant is reported, never "
                      "assumed");
            }

            // (ii) — the surface must not be empty on a format this descriptor
            // claims to serve.
            if (cs.names.empty() && cs.undecodable.empty()) {
                emitCorpusInvariant(reporter,
                    "shipped-lib corpus invariant (ii): '" + rel + "' is available "
                    "on object format '" + fmtName + "' but contributes NO name "
                    "there — not from its own surfaces and not from its active "
                    "'includes' closure. An '#include' of it would compile and "
                    "declare nothing, which is a header that silently is not "
                    "there. Either declare a surface for '" + fmtName
                    + "', or remove '" + fmtName
                    + "' from its 'availableObjectFormats'");
            }
        }
    }
    return reporter.errorCount() == errBefore;
}

bool validateShippedSurfaceRequirements(
    std::span<PredefinedMacroDef const>    macros,
    std::string_view                       declaringDocument,
    std::span<std::filesystem::path const> systemDirs,
    std::optional<ObjectFormatKind>        activeFormat,
    DiagnosticReporter&                    reporter) {
    // ═══ D-LANG-PREDEFINED-MACRO-REQUIRES-REALIZED-SURFACE — SATISFACTION ═══
    //
    // The SHAPE of the predicate is core's (`ShippedSurfaceClaim`); this
    // is the half that can see the shipped corpus, which is why it lives here.
    //
    // ★★★ THE CLAIM IS CHECKED PER FORMAT, AND THE FORMAT SET IS THE MACRO'S OWN.
    // A macro gated `["pe"]` is checked on pe — on EVERY leg, including the elf
    // one, because an arm no current build selects is exactly the arm that rots.
    // An UNGATED macro is effective on every format, but this function cannot
    // enumerate "every format" (see `validateShippedIncludeClosure`'s note), so
    // it checks the ACTIVE one. That is weaker per invocation and equal in
    // practice: the driver builds a CU per (target, object-format), so a
    // multi-format build checks each format's arm on that format's own leg.
    //
    // ★ IT IS AN ASSERTION, NEVER A SUPPRESSION. A macro whose backing is missing
    // does NOT get quietly withdrawn — the build fails. A silently-withdrawn
    // identity macro flips `#ifdef` branches under the user with no diagnostic,
    // which is the same species of quiet wrongness as the silently-PRESENT one
    // this whole mechanism exists to end.
    std::size_t const errBefore = reporter.errorCount();
    // "<file family> <array pointer>[<index>]" — the declaring document and the
    // exact row. `declaredAt` is a full JSON pointer whose ARRAY PREFIX is
    // already the tail of `declaringDocument`, so printing both verbatim
    // repeats it ("…/predefinedMacros/preprocess/predefinedMacros/0"); the
    // index is the only part that is not already said.
    auto const where = [&](PredefinedMacroDef const& pm) {
        std::string_view idx{pm.declaredAt};
        if (auto const slash = idx.rfind('/'); slash != std::string_view::npos) {
            idx = idx.substr(slash + 1);
        }
        return std::string{declaringDocument} + "[" + std::string{idx} + "]";
    };
    for (PredefinedMacroDef const& pm : macros) {
        // Only a `surface` claim is EVALUATED. `claims-nothing` states a
        // declared absence, and `not-expressible` states that the macro DOES
        // imply something this predicate cannot express — recorded by ruling
        // B', deliberately NOT evaluated here (a language-feature predicate
        // is an operator-deferred build, and silently treating the tag as
        // "nothing" would be the conflation the third state exists to stop).
        if (pm.impliedSurface.kind != ImpliedSurfaceKind::Surface) continue;

        // The formats this claim must hold on.
        std::vector<ObjectFormatKind> formats;
        for (std::string const& fn : pm.availableObjectFormats) {
            auto const k = objectFormatKindFromName(fn);
            // The name vocabulary is validated at config load; a value that got
            // here unknown would be a loader hole, so it is reported rather than
            // skipped.
            if (!k || !isSelectableObjectFormatKind(*k)) {
                emitUnbackedPredefine(reporter,
                    std::string{"predefined macro '"} + pm.name + "' ("
                    + where(pm) + ") declares 'impliedSurface' but its 'availableObjectFormats' "
                      "entry '" + fn + "' does not name a selectable object "
                      "format, so the requirement has no format to hold on");
                continue;
            }
            formats.push_back(*k);
        }
        if (pm.availableObjectFormats.empty()) {
            if (!activeFormat.has_value()) {
                // No gate and no active format: there is no platform in scope, so
                // the claim has no subject. NOT a skipped check — a check whose
                // subject does not exist. Every driver path sets the active
                // format (the CU is built once per (target, object-format) pair);
                // the callers that do not are the LSP, the direct API and the
                // FFI header parser, which deliberately have no target at all.
                continue;
            }
            formats.push_back(*activeFormat);
        }

        for (ObjectFormatKind const fmt : formats) {
            std::string const fmtName{objectFormatKindName(fmt)};
            for (ShippedSurfaceClaim const& claim :
                 pm.impliedSurface.headers) {
                // Resolve the claimed header by the SAME `<stem>.json` funnel a
                // source `#include <h>` uses. Case-SENSITIVE for the reason the
                // corpus sweep is: the claim is checked on formats other than the
                // active one, and a per-format case policy cannot answer a
                // cross-format question.
                HeaderSearchResult const hit = resolveSystemDescriptor(
                    claim.header, systemDirs, kDefaultHeaderNameMatching);
                if (hit.status != HeaderSearchStatus::Found) {
                    emitUnbackedPredefine(reporter,
                        std::string{"predefined macro '"} + pm.name + "' ("
                        + where(pm) + ") requires shipped header '" + claim.header
                        + "' on object format '" + fmtName
                        + "', but no descriptor for it is on the shipped-library "
                          "search path. The macro asserts a platform surface that "
                          "is not there — build the surface, or state honestly "
                          "that the macro requires nothing by removing the claim");
                    continue;
                }
                if (!shippedHeaderAvailableForFormat(hit.path, fmt)) {
                    emitUnbackedPredefine(reporter,
                        std::string{"predefined macro '"} + pm.name + "' ("
                        + where(pm) + ") requires shipped header '" + claim.header
                        + "' on object format '" + fmtName
                        + "', but that header's descriptor declares it does NOT "
                          "exist on '" + fmtName
                        + "'. A macro may not promise, on a format, a header the "
                          "corpus says is absent from it");
                    continue;
                }
                ClosureSurfaceOnFormat const cs =
                    closureSurfaceOnFormat(hit.path, systemDirs, fmt);
                if (!cs.undecodable.empty()) {
                    emitUnbackedPredefine(reporter,
                        std::string{"predefined macro '"} + pm.name + "' ("
                        + where(pm) + ") requires shipped header '" + claim.header
                        + "' on object format '" + fmtName + "', but '"
                        + cs.undecodable.front()
                        + "' in its include closure does not decode — the "
                          "requirement cannot be verified, and an unverified "
                          "requirement is reported, never assumed satisfied");
                    continue;
                }
                std::vector<std::string> missing;
                for (std::string const& want : claim.names) {
                    if (std::find(cs.names.begin(), cs.names.end(), want)
                        == cs.names.end()) {
                        missing.push_back(want);
                    }
                }
                if (missing.empty()) continue;
                std::string list;
                for (std::string const& m : missing) {
                    if (!list.empty()) list += "', '";
                    list += m;
                }
                emitUnbackedPredefine(reporter,
                    std::string{"predefined macro '"} + pm.name + "' ("
                    + where(pm) + ") requires '" + claim.header + "' to declare '" + list
                    + "' on object format '" + fmtName
                    + "', and the shipped surface reachable from that header does "
                      "not. The macro's presence must be a CHECKED CONSEQUENCE of "
                      "the realized surface, not a second declaration of the same "
                      "fact sitting next to it — so either the surface ships, or "
                      "the macro does not");
            }
        }
    }
    return reporter.errorCount() == errBefore;
}

} // namespace dss::ffi
