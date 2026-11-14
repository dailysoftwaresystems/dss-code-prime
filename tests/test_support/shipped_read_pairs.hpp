#pragma once

// ══ READ A SHIPPED DESCRIPTOR AS A COMPILE READS IT: ON A REAL PAIR ══════════
// (P68 round 12, S2a-1 of D-C-STDLIB-H-LACKS-THIRTY-FIVE-ISO-NAMES)
//
// A shipped descriptor is PER PAIR. A typedef may be a `variants` set keyed on
// the format, the data model or the arch (`size_t` is `unsigned long` on ELF and
// Mach-O and `unsigned long long` on pe); a typedef may name the target's ABI
// typedef (`wchar_t`); a symbol's `signature` may carry `when` arms (the data
// model, the long-double format). The compiler reads a descriptor only WITH a
// pair — the analyzer and the preprocessor both pass one — so a test that reads
// a REAL descriptor with no pair reads a document no compile reads: the day a
// prototype names a per-pair typedef, that read stops decoding while every
// build still succeeds.
//
// These are the pairs to read under and the facts to read them with, derived
// from the shipped documents by the computation the compile itself uses
// (`predefinedTypeFactsFor`, the one owner of a pair's type facts), so a test
// cannot read a descriptor under facts no build has.
//
// WHICH PAIRS. Every REAL pair — a format document and the shipped target it
// names (`targetArch`) — collapsed to one representative per DISTINCT set of the
// facts the reader consumes (arch, format kind, data model, long-double format,
// plain-`char` signedness, ABI typedefs); the flavours of one family (exec, dyn,
// pie, staticlib) agree on all of them. ✔MEASURED at S2a-1 over the 24 format
// documents: 22 real pairs, 5 distinct reads — x86_64 × {elf, macho, pe} and
// arm64 × {elf, macho}; spirv and wasm32 name no shipped target. The floors
// below fail the calling test when the enumeration collapses: a sweep over an
// empty list passes in silence, which is the one failure a sweep must not have.
//
// NOT HERE: a runtime-library role resolver. Which image a role binds is a fact
// of the individual format document (an executable and a DLL flavour may
// differ), so a caller that binds imports builds its resolver from the flavour
// it means, and reads on that flavour's own pair.

#include "analysis/compilation_unit/compilation_unit.hpp"   // predefinedTypeFactsFor
#include "core/types/data_model.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/preprocess_config.hpp"                 // PredefinedTypeFacts
#include "core/types/target_schema.hpp"
#include "ffi/shipped_lib_descriptor.hpp"                   // ffi::ShippedPairFacts
#include "link/object_format_schema.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dss::test_support {

// One distinct read: the pair's identity and the facts the reader is given.
struct ShippedReadPair {
    std::string         arch;        // the shipped target's name (the format's `targetArch`)
    std::string         formatDoc;   // the first format document (by name) with these facts
    ObjectFormatKind    kind = ObjectFormatKind::Unknown;
    PredefinedTypeFacts facts;       // predefinedTypeFactsFor(target, format)

    [[nodiscard]] DataModel dataModel() const noexcept { return facts.dataModel; }
    [[nodiscard]] std::optional<std::string_view> activeTarget() const noexcept {
        return std::string_view{arch};
    }
    [[nodiscard]] std::optional<ObjectFormatKind> activeFormat() const noexcept { return kind; }
    [[nodiscard]] std::string label() const { return arch + ":" + formatDoc; }

    // The reader's pair facts, field for field as the preprocessor's splice
    // builds them from the same `PredefinedTypeFacts`. `language` is the
    // CONSUMING language; nullptr (the default) realizes no lattice-derived
    // constant — every other fact (the `when` axes, the ABI typedefs) is the
    // pair's — which is what a read that is not about derived constants wants:
    // it validates each such row without realizing it, exactly as before.
    [[nodiscard]] ffi::ShippedPairFacts pairFacts(GrammarSchema const* language = nullptr) const {
        return ffi::ShippedPairFacts{language, facts.dataModel, facts.charIsUnsigned,
                                     facts.abiTypedefs, facts.longDoubleFormat};
    }
};

namespace shipped_read_pairs_detail {

// The stems of `<configRoot>/<dir>/*<suffix>`, sorted — never a hand list.
[[nodiscard]] inline std::vector<std::string> stems(std::string_view dir, std::string_view suffix) {
    std::vector<std::string> out;
    for (auto const& e : std::filesystem::directory_iterator{dss::test::configRoot() / dir}) {
        if (!e.is_regular_file()) continue;
        std::string const fn = e.path().filename().string();
        if (fn.size() <= suffix.size() || !fn.ends_with(suffix)) continue;
        out.push_back(fn.substr(0, fn.size() - suffix.size()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

[[nodiscard]] inline bool sameReadFacts(ShippedReadPair const& a, ShippedReadPair const& b) {
    return a.arch == b.arch && a.kind == b.kind && a.facts.dataModel == b.facts.dataModel
        && a.facts.longDoubleFormat == b.facts.longDoubleFormat
        && a.facts.charIsUnsigned == b.facts.charIsUnsigned
        && a.facts.abiTypedefs == b.facts.abiTypedefs;
}

struct Enumeration {
    std::vector<ShippedReadPair> distinct;
    std::size_t                  realPairs = 0;
    std::string                  failure;   // empty ⇔ every document loaded
};

[[nodiscard]] inline Enumeration enumerate() {
    Enumeration out;
    std::vector<std::shared_ptr<TargetSchema>>       targets;
    std::vector<std::shared_ptr<ObjectFormatSchema>> formats;
    for (std::string const& name : stems("targets", ".target.json")) {
        auto r = TargetSchema::loadShipped(name);
        if (!r.has_value()) {
            out.failure += "target '" + name + "' does not load; ";
            continue;
        }
        targets.push_back(*r);
    }
    std::vector<std::string> formatNames = stems("object-formats", ".format.json");
    for (std::string const& name : formatNames) {
        auto r = ObjectFormatSchema::loadShipped(name);
        if (!r.has_value()) {
            out.failure += "object format '" + name + "' does not load; ";
            continue;
        }
        formats.push_back(*r);
    }
    for (std::size_t fi = 0; fi < formats.size(); ++fi) {
        ObjectFormatSchema const& format = *formats[fi];
        for (auto const& target : targets) {
            if (format.targetArch() != target->name()) continue;   // not a real pair
            ++out.realPairs;
            ShippedReadPair pair;
            pair.arch      = std::string{target->name()};
            pair.formatDoc = std::string{format.name()};
            pair.kind      = format.kind();
            pair.facts     = predefinedTypeFactsFor(*target, format);
            bool const seen = std::any_of(out.distinct.begin(), out.distinct.end(),
                                          [&](ShippedReadPair const& p) { return sameReadFacts(p, pair); });
            if (!seen) out.distinct.push_back(std::move(pair));
        }
    }
    return out;
}

}  // namespace shipped_read_pairs_detail

// Every distinct read of the shipped real pairs. The enumeration runs once per
// test binary; the checks run on EVERY call, so each consuming test fails on its
// own when the documents stop loading or the enumeration collapses.
[[nodiscard]] inline std::vector<ShippedReadPair> const& shippedReadPairs() {
    static shipped_read_pairs_detail::Enumeration const e = shipped_read_pairs_detail::enumerate();
    EXPECT_TRUE(e.failure.empty()) << "the shipped pair documents do not all load: " << e.failure;
    // FLOORS, ✔MEASURED at S2a-1: 22 real pairs, 5 distinct reads (see the header).
    EXPECT_GE(e.realPairs, 22u) << "the real-pair enumeration collapsed";
    EXPECT_GE(e.distinct.size(), 5u) << "the distinct-read enumeration collapsed";
    return e.distinct;
}

// ══ EVERY C CALLING CONVENTION A SHIPPED FORMAT NAMES ═══════════════════════
// (P69, lane `cs`, fold f26c2.) A convention is per FORMAT (`cCallingConvention`
// names a row of the paired target's `callingConventions[]`), so a test that means
// "on every convention" walks the real pairs and keeps one per (target,
// convention) -- its first format document by name -- with the pair's own type
// facts (the data model, the `long double` format). Never a hand list: a format
// or a convention row added later is covered the day it lands. The facts are NOT
// collapsed (unlike `shippedReadPairs`): two conventions with equal facts are two
// entries. ✔READ at f26c2 over the 24 format documents: five conventions --
// sysv_amd64 (ELF x86_64), sysv_amd64_darwin (Mach-O x86_64), ms_x64 (pe64),
// aapcs64 (ELF arm64), apple_arm64 (Mach-O arm64); spirv and wasm32 name `none`.
struct ShippedConventionPair {
    std::string         arch;               // the shipped target's name
    std::string         convention;         // the format's `cCallingConvention`
    std::uint16_t       conventionIndex = 0;   // its row in the target's callingConventions[]
    std::string         formatDoc;          // the first format document (by name) naming it
    ObjectFormatKind    kind = ObjectFormatKind::Unknown;
    PredefinedTypeFacts facts;              // predefinedTypeFactsFor(target, format)

    [[nodiscard]] std::string label() const { return arch + ":" + formatDoc + " (" + convention + ")"; }
};

namespace shipped_read_pairs_detail {

struct ConventionEnumeration {
    std::vector<ShippedConventionPair> pairs;
    std::string                        failure;   // empty ⇔ every document loaded and every name resolved
};

[[nodiscard]] inline ConventionEnumeration enumerateConventions() {
    ConventionEnumeration out;
    std::vector<std::shared_ptr<TargetSchema>> targets;
    for (std::string const& name : stems("targets", ".target.json")) {
        auto r = TargetSchema::loadShipped(name);
        if (!r.has_value()) {
            out.failure += "target '" + name + "' does not load; ";
            continue;
        }
        targets.push_back(*r);
    }
    for (std::string const& name : stems("object-formats", ".format.json")) {
        auto r = ObjectFormatSchema::loadShipped(name);
        if (!r.has_value()) {
            out.failure += "object format '" + name + "' does not load; ";
            continue;
        }
        ObjectFormatSchema const& format = **r;
        if (format.cCallingConvention().declaresNoConvention()) continue;
        for (auto const& target : targets) {
            if (format.targetArch() != target->name()) continue;   // not a real pair
            std::string const& cc = format.cCallingConvention().convention;
            bool const seen = std::any_of(out.pairs.begin(), out.pairs.end(), [&](ShippedConventionPair const& p) {
                return p.arch == target->name() && p.convention == cc;
            });
            if (seen) continue;
            std::optional<std::uint16_t> index;
            for (std::uint16_t i = 0; i < target->callingConventionCount(); ++i)
                if (target->callingConvention(i)->name == cc) index = i;
            if (!index.has_value()) {
                out.failure += "format '" + name + "' names convention '" + cc + "', which target '"
                             + std::string{target->name()} + "' does not declare; ";
                continue;
            }
            ShippedConventionPair pair;
            pair.arch            = std::string{target->name()};
            pair.convention      = cc;
            pair.conventionIndex = *index;
            pair.formatDoc       = std::string{format.name()};
            pair.kind            = format.kind();
            pair.facts           = predefinedTypeFactsFor(*target, format);
            out.pairs.push_back(std::move(pair));
        }
    }
    return out;
}

}  // namespace shipped_read_pairs_detail

// Every (target, convention) a shipped format names. The checks run on EVERY call,
// so each consuming test fails on its own when the enumeration collapses.
[[nodiscard]] inline std::vector<ShippedConventionPair> const& shippedConventionPairs() {
    static shipped_read_pairs_detail::ConventionEnumeration const e =
        shipped_read_pairs_detail::enumerateConventions();
    EXPECT_TRUE(e.failure.empty()) << "the shipped convention pairs do not all resolve: " << e.failure;
    // FLOOR, ✔READ at f26c2: five conventions (see above).
    EXPECT_GE(e.pairs.size(), 5u) << "the convention enumeration collapsed";
    return e.pairs;
}

// Every shipped format document's name, in name order -- the `object-formats` directory read, never a hand
// list. For a test that walks a property of the formats themselves (an executable's container, say).
[[nodiscard]] inline std::vector<std::string> shippedFormatNames() {
    return shipped_read_pairs_detail::stems("object-formats", ".format.json");
}

// The one read for (arch, format kind), or nullptr with a test failure naming it.
[[nodiscard]] inline ShippedReadPair const* shippedReadPair(std::string_view arch, ObjectFormatKind kind) {
    for (ShippedReadPair const& p : shippedReadPairs()) {
        if (p.arch == arch && p.kind == kind) return &p;
    }
    ADD_FAILURE() << "no shipped real pair for " << arch << " × " << objectFormatKindName(kind);
    return nullptr;
}

}  // namespace dss::test_support
