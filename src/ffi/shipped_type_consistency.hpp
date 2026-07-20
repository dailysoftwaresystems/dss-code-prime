#pragma once

#include "core/export.hpp"
#include "core/types/object_format_kind.hpp"          // ObjectFormatKind
#include "core/types/strong_ids.hpp"                 // TypeId
#include "core/types/type_lattice/core_type.hpp"     // TypeKind

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

// ── CROSS-DESCRIPTOR TYPE-IDENTITY CONSISTENCY (fail-loud) ──────────────────
//
// D-LANG-TYPE-IDENTITY-VOCABULARY. Shipped descriptors are AUTHORED INDEPENDENTLY
// but interned into ONE lattice, and two of the injection paths are FIRST-WINS BY
// NAME:
//
//   * a struct/union TAG (`semantic_analyzer.cpp`, the `injectedTags` set) — only
//     the WINNER gets a `compositeScopeByType` field scope, so a second, DIVERGENT
//     declaration of the same tag interns a SECOND TypeId whose members are
//     unreachable. The user sees an INCLUDE-ORDER-DEPENDENT `S000D member access
//     '.' requires a composite-typed operand`;
//   * a typedef NAME (the `injectedNames` set) — the loser silently vanishes, so
//     which WIDTH/IDENTITY `off_t` has depends on include order.
//
// Neither is diagnosed by the per-file reader: `readShippedLibDescriptor` sees ONE
// descriptor at a time and cannot know that a sibling spells the same tag
// differently. Three consecutive fix rounds on this feature ALL failed on exactly
// this class (`struct timeval` declared with `i64` in `sys/resource.json` and
// `i64 "long"` in `sys/time.json`), so it is machine-checked here rather than
// re-asserted in a `$comment`.
//
// TWO invariants, both enforced over the types a descriptor ACTUALLY SELECTED for
// the active target (arch × format × dataModel) — the reader has already collapsed
// `variants` by the time this runs, so "selected for the same target" is exactly
// what is compared:
//
//   (A) CROSS-DECLARATION IDENTITY. Every declaration of a given struct/union TAG
//       NAME — whether a `structs` entry, an INLINE `struct "N" {…}` inside another
//       type's text, or a repeat in a second descriptor — must intern to the SAME
//       TypeId. Likewise every declaration of a given typedef NAME. Interned-TypeId
//       equality IS byte-identical-text equality after resolution, and it is
//       STRICTLY stronger: it also catches two spellings that differ only in a
//       vocabulary tag (`i64` vs `i64 "long"`), which is precisely the defect.
//
//   (B) VOCABULARY-WIDTH AGREEMENT. A vocabulary TAG names a type whose WIDTH is a
//       property of the DATA MODEL, so `i64 "long"` is a PHANTOM on LLP64 (where
//       the language mints `long` as I32): it matches no vocabulary entry at all,
//       and every `_Generic` / pointer-compatibility test against it fails. Each
//       tag a descriptor spells must therefore resolve to the SAME core the ACTIVE
//       LANGUAGE gives that name under the ACTIVE data model.
//
// AGNOSTIC. This checker knows NO spelling and NO data model: the vocabulary is
// passed in as opaque (name → core) rows the CALLER resolved from its own language
// config, and a name the language does not declare is skipped (a descriptor may
// legitimately model a type this language has no word for). Invariant (A) is pure
// TypeId comparison.

namespace dss {

class DiagnosticReporter;
class TypeInterner;

namespace ffi {

struct ShippedLibDescriptor;

// One row of the ACTIVE LANGUAGE's primitive type VOCABULARY, already resolved for
// the ACTIVE data model (and long-double axis) by the caller. `name` is borrowed —
// it must outlive the checker.
struct DSS_EXPORT VocabularyCore {
    std::string_view name;
    TypeKind         core = TypeKind::Void;
};

// Accumulates the NAMED types declared by a set of descriptors resolved for ONE
// target and reports every violation of (A)/(B) above as
// `F_ShippedTypeIdentityConflict` (unsuppressable — a silent first-wins here is a
// wrong-layout / unreachable-member miscompile).
//
// USAGE: one instance per (compilation unit × target). Feed every descriptor the
// unit resolved via `add`; the checker is stateful because (A) is inherently
// cross-file. `add` returns false iff THIS descriptor violated something (earlier
// descriptors' violations are not re-reported).
class DSS_EXPORT ShippedTypeConsistency {
public:
    // `activeFormat` reproduces the analyzer's PER-SYMBOL availability gate: a
    // symbol whose `availableObjectFormats` excludes the active format is never
    // injected, so its signature is not a declaration ON THIS TARGET and must
    // not be compared against one that is (`threads.json` declares three
    // per-format `tss_get` rows, only one of which ships). nullopt = no gate,
    // the reader's own convention for direct-API/unit callers.
    ShippedTypeConsistency(TypeInterner const&             interner,
                           std::span<VocabularyCore const> vocabulary,
                           std::optional<ObjectFormatKind> activeFormat = std::nullopt)
        : in_(&interner), vocabulary_(vocabulary), activeFormat_(activeFormat) {}

    // `origin` names WHERE this descriptor came from, for the diagnostic (the
    // analyzer passes the header spelling, the exhaustive test the file path).
    [[nodiscard]] bool add(std::string_view              origin,
                           ShippedLibDescriptor const&   desc,
                           DiagnosticReporter&           reporter);

private:
    // The first declaration of a name — the one every later declaration is
    // compared against. `origin` is copied (a descriptor is a loop temporary).
    struct Decl {
        TypeId      type;
        std::string origin;
    };

    // Recursively record/verify every named type REACHABLE from `t`. `operands()`
    // is the universal child accessor (and is qualifier-transparent), so no kind
    // switch is needed; `visited_` makes a self-referential type terminate.
    void walk(TypeId t, std::string_view origin, DiagnosticReporter& reporter,
              bool& ok);

    void recordNamed(std::unordered_map<std::string, Decl>& into,
                     char const* what, std::string name, TypeId t,
                     std::string_view origin, DiagnosticReporter& reporter,
                     bool& ok);

    // The hir-text rendering of `t` — the SAME spelling a descriptor author
    // writes, so the diagnostic is directly actionable.
    [[nodiscard]] std::string render(TypeId t, int depth = 0) const;

    TypeInterner const*             in_;
    std::span<VocabularyCore const> vocabulary_;
    std::optional<ObjectFormatKind> activeFormat_;
    std::unordered_map<std::string, Decl> tags_;      // struct/union/enum TAG ns
    std::unordered_map<std::string, Decl> typedefs_;  // typedef NAME ns
    std::unordered_set<std::uint32_t>     visited_;   // TypeId.v, walk memo
};

} // namespace ffi
} // namespace dss
