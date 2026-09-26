#pragma once

#include "core/export.hpp"
#include "core/types/object_format_kind.hpp"   // ObjectFormatKind

#include <optional>
#include <string>
#include <string_view>

// ══ THE ONE `when` SELECTOR ══════════════════════════════════════════════════
// (P68 round 12, S2a-1 of D-C-STDLIB-H-LACKS-THIRTY-FIVE-ISO-NAMES)
//
// A config row that is different on different build pairs carries `variants`,
// each arm guarded by a `when` object — `{ "format": "pe" }`,
// `{ "dataModel": "LLP64" }`, `{ "arch": "arm64", "format": "elf" }`,
// `{ "format": "elf", "longDoubleFormat": "ieee128" }`. This is the ONE place a
// `when` is DECODED (its keys and values checked against their closed
// vocabularies) and the ONE place it is MATCHED against a pair.
//
// ★ ONE OWNER, TWO KINDS OF CALLER — the `type_name_resolve` precedent. The
// evaluator was private to the shipped-descriptor reader, which decodes and
// matches in one breath at READ time. The C language document's builtin
// functions carry the same `signature` variants, but that document is loaded
// interner-free at LOAD, long before any pair exists, and the analyzer selects
// an arm only at INJECTION. A second evaluator for the builtins would be a
// `when` two readers can disagree about, so both now call this: the reader
// decodes and matches at once; the language loader decodes at LOAD (every
// defect reported where it is written) and keeps the `WhenSpec`, which the
// analyzer matches at injection with the pair it has.
//
// WHICH AXES OF THE SELECTOR PARTICIPATE — one evaluator, three modes:
//
//  • `FormatOnly`     — `{format}` is the WHOLE legal key vocabulary; any other
//                       key FAILS LOUD. The preprocessor-facing surfaces
//                       (`macros`, the `includes` edge gate) live here: neither
//                       arch, the data model nor the long-double format is
//                       threaded into preprocess, so a key naming them could
//                       only ever be a config author's mistake.
//  • `FullTarget`     — `{arch, format, dataModel, longDoubleFormat}` are all
//                       legal and all participate. The TYPED surfaces (structs,
//                       constants, typedefs, per-target `value` variants and
//                       `signature` variants), which select a LAYOUT or a TYPE
//                       and therefore need every axis.
//  • `FormatReachability` — every `FullTarget` key is legal and VALIDATED, but
//                       only `format` participates: "could this arm be selected
//                       on object format F, for SOME pair?", the question a
//                       NAME-PRESENCE scan asks, because a variant set changes a
//                       name's TYPE or LAYOUT per pair, never whether it exists.
//
// THE CONTRACT IS MATCH-ALL-SPECIFIED: every key a `when` names must equal the
// pair's value (generic equality — never an `if (arch == "x86_64")` here); an
// unnamed key is a wildcard; a key tested against an ABSENT fact never matches.
// An EMPTY `when` is refused: it would match every pair, including one with no
// active format — the flat, non-variant form is the target-invariant spelling.
//
// ★ `longDoubleFormat` (S2a-1): the representation of C's `long double` is a
// fact of the pair's FORMAT DOCUMENT (x87-80 on SysV and Darwin x86_64, ieee128
// on AAPCS64 Linux, f64 on MSVC x64 and Apple arm64) — not of arch, format kind
// or data model, which is why a `{arch, format}`-keyed `max_align_t` conflicted
// with the consistency sweep (the sweep pairs every format document's axes with
// every arch). It rides the SAME selector, so every typed surface gains it at
// once.
namespace dss {

enum class WhenAxes { FormatOnly, FullTarget, FormatReachability };

// A DECODED `when`: every key the object named, each value already checked
// against its closed vocabulary (the arch name is OPEN — it lives only in the
// target schemas — so an unknown arch simply never matches).
struct DSS_EXPORT WhenSpec {
    std::optional<std::string>      arch;
    std::optional<ObjectFormatKind> format;
    std::optional<std::string>      dataModel;          // a kDataModelTable spelling
    std::optional<std::string>      longDoubleFormat;   // a kLongDoubleFormatTable spelling
};

// The pair a `when` is tested against. `dataModelName` is never empty for a
// real read (every reader has a data model); every other absent fact makes a
// key that names it NoMatch.
struct DSS_EXPORT WhenFacts {
    std::optional<std::string_view> arch;
    std::optional<ObjectFormatKind> format;
    std::string_view                dataModelName;
    std::optional<std::string_view> longDoubleFormatName;
};

// DECODING a `when` needs the parsed document, so it lives in `variant_when_json.hpp` — the sanctioned
// `_json` shape (src/core/CMakeLists.txt): this header stays JSON-free for the builtin row that stores a
// `WhenSpec` in `semantic_config.hpp`, which half the tree includes.

// Match a decoded `when` against the pair, by the mode's participation rule.
[[nodiscard]] DSS_EXPORT bool
whenMatches(WhenSpec const& spec, WhenAxes axes, WhenFacts const& facts) noexcept;

}  // namespace dss
