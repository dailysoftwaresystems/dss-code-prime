// The OBJECT-vs-IMAGE relocation-vocabulary census --
// D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-THE-OBJECT-FORMAT (OPEN,
// HIGH).
//
// ⚠⚠ THIS HEADER WAS WRITTEN WHILE THE ROW WAS OPEN AND HAS BEEN REWRITTEN NOW
// THAT IT IS FIXED. It used to say the census "PINS A KNOWN DEFECT'S BLAST
// RADIUS. IT DOES NOT ASSERT DESIRED BEHAVIOUR", and that the divergence below
// was "expected to stay wrong until that row closes". BOTH HALVES TURNED OUT TO
// BE THE WRONG PREDICTION, and the correction is the point worth keeping: the
// divergence was never the defect. It is a CORRECT and PERMANENT fact about the
// two vocabularies, and the defect was reading a member through the wrong one of
// them.
//
// THE DEFECT, AND WHAT FIXED IT. `compile_pipeline.cpp::readArchiveMemberModule`
// used to hand an archive MEMBER to a reader together with the FINAL IMAGE's
// `ObjectFormatSchema` -- the `-exec` / `-dylib` / `-dll` variant the link is
// producing -- rather than the member's own relocatable format. Every reader
// builds a reverse map from the schema's `nativeId` back to a universal
// `RelocationKind`, so the member's wire values were decoded against a
// vocabulary that was never promised to describe them. It now resolves the
// member's OWN format (the `container: "archive"` document for the same kind and
// machine) and reads through that, refusing loud if it cannot.
//
// ★ SO WHAT DOES THIS CENSUS STILL ASSERT, now that the read is correct? The
// SHAPE of the shipped configuration, and it is still worth asserting. On most
// families the object-side and image-side vocabularies declare the same
// `nativeId` for the same `kind` -- an AGREEMENT BY COINCIDENCE that no rule
// enforces. While the reader was reading with the wrong document, that
// coincidence was load-bearing for correctness. It no longer is; what it is now
// is a claim about the corpus that a maintainer would otherwise have to
// re-derive by hand, and the ONE family that breaks it is the one that proves
// the two vocabularies are genuinely different documents rather than duplicates
// -- i.e. the reason "declare the relocation vocabulary once per lineage" is not
// an available design.
//
// ⚠⚠ THE ONE FAMILY WHERE THE COINCIDENCE FAILED WAS `macho64-x86_64-darwin`,
// on two kinds, and THIS HEADER USED TO CALL THAT SPLIT "structurally
// meaningful rather than a typo". THAT WAS WRONG, and the correction is worth
// more than the original claim: it WAS a typo, in two documents, and each of
// the two values contradicted its own row's name, comment and stated packing
// (D-CONFIG-MACHO-X86_64-EXEC-DYLIB-RELOC-NATIVEID-CONTRADICTS-ITS-OWN-ROW,
// 2026-08-20). The set below is now EMPTY. The lesson the old text drew from
// the divergence -- that object and image vocabularies are genuinely different
// documents -- is still TRUE, but this census is no longer its evidence; see
// the note at the expectation.
//
// ⚠⚠ A RED HERE IS A CONFIGURATION CHANGE.
//   * If a family appears in the divergent set, its variants have drifted
//     apart. That is no longer a correctness alarm -- the member read resolves
//     its own document now -- but it IS news, and widening the list without
//     understanding why the family drifted throws the news away. The FIRST
//     question to ask is the one that settled the last occurrence: does the new
//     value contradict its own row?
//   * ✔MEASURED 2026-08-20, both directions, so the expectation is not vacuous:
//     restoring `macho64-x86_64-darwin-exec`'s kind-1 `nativeId` to the old
//     369098752 reds this case (the set gains a row), and with the corrected
//     value it is green.
//
// ⓘ MEASURED, NOT PINNED: several families also declare a `kind` in one variant
// and not another (e.g. an ELF exec-only PLT kind absent from the relocatable
// arm). That asymmetry is an ordinary consequence of image-only relocation types
// existing at all, and pinning it would red every time a schema legitimately
// grows a kind. The CONFLICT set below is the part that cannot be explained that
// way: the same kind, two different wire values.

#include "core/types/strong_ids.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
namespace fs = std::filesystem;

namespace {

// One divergence: a family, a relocation kind, and the exact partition of that
// family's variants by the `nativeId` each declares.
struct Divergence {
    std::string family;
    std::uint32_t kind = 0;
    // Rendered `"<value> = <variant>/<variant>"` groups, sorted -- the whole
    // fact as one comparable string, so gtest prints the real difference rather
    // than "vectors differ in element 3".
    std::vector<std::string> groups;

    [[nodiscard]] std::string render() const {
        std::string out = family + " kind " + std::to_string(kind) + ": ";
        for (std::size_t i = 0; i < groups.size(); ++i) {
            if (i != 0) out += " | ";
            out += groups[i];
        }
        return out;
    }
};

[[nodiscard]] fs::path objectFormatsDir() {
    auto const root = dss::test::findConfigRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *root / "object-formats";
}

// Every shipped format's NAME, enumerated from disk. A format added tomorrow is
// in the census the day it lands -- a hard-coded list would make this guard
// blind to exactly the new-family drift it is watching for.
[[nodiscard]] std::vector<std::string> shippedFormatNames() {
    std::vector<std::string> names;
    auto const dir = objectFormatsDir();
    if (dir.empty()) return names;
    constexpr std::string_view kSuffix = ".format.json";
    std::error_code ec;
    for (auto const& entry : fs::directory_iterator{dir, ec}) {
        std::string const filename = entry.path().filename().string();
        if (filename.size() <= kSuffix.size()) continue;
        if (filename.compare(filename.size() - kSuffix.size(), kSuffix.size(),
                             kSuffix) != 0) {
            continue;
        }
        names.push_back(filename.substr(0, filename.size() - kSuffix.size()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

// The family grouping, DERIVED from the corpus rather than from a list of
// suffixes: a format whose name is another shipped format's name plus `-<tail>`
// is a VARIANT of that one, and the bare name is the family -- which is also
// exactly what the bare name MEANS (the relocatable/object format; the suffixed
// ones are the images built from it). Deriving it this way means a new variant
// suffix (`-pie`, `-dyn`, `-dll` all arrived this way) is grouped correctly with
// no edit here, and a genuinely new family is a new group rather than a
// mis-attached variant.
//
// Returns family -> (variant label -> format name). The bare format's variant
// label is "relocatable"; a suffixed one's label is the suffix.
[[nodiscard]] std::map<std::string, std::map<std::string, std::string>>
groupIntoFamilies(std::vector<std::string> const& names) {
    std::set<std::string> const all{names.begin(), names.end()};
    std::map<std::string, std::map<std::string, std::string>> families;
    for (auto const& name : names) {
        std::string base = name;
        for (auto const& candidate : all) {
            if (candidate == name) continue;
            if (name.size() <= candidate.size() + 1) continue;
            if (name.compare(0, candidate.size(), candidate) != 0) continue;
            if (name[candidate.size()] != '-') continue;
            if (base == name || candidate.size() > base.size()) base = candidate;
        }
        std::string const label =
            (base == name) ? std::string{"relocatable"} : name.substr(base.size() + 1);
        families[base][label] = name;
    }
    return families;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
// The census, and the exact divergent set.
// ─────────────────────────────────────────────────────────────────────────
TEST(ArchiveMemberFormatVocabulary, ObjectAndImageRelocationTablesDivergeExactlyHere) {
    auto const names = shippedFormatNames();
    // Non-vacuity first: every assertion below is a loop over this, and an empty
    // loop reports success. A moved config tree or a renamed suffix must red
    // here rather than pass a census of nothing.
    ASSERT_GE(names.size(), 20u)
        << "only " << names.size() << " shipped formats were enumerated -- the "
           "census collapsed (moved tree? renamed suffix?), it did not pass";

    auto const families = groupIntoFamilies(names);
    ASSERT_GE(families.size(), 5u)
        << "the family grouping collapsed: " << families.size() << " families";

    std::vector<std::string> observed;
    std::size_t comparedKinds = 0;

    for (auto const& [family, variants] : families) {
        // variant label -> (kind -> nativeId), read through the SAME loader the
        // compiler uses, so the census is over the vocabulary a reader is
        // actually handed rather than over the raw JSON beside it.
        std::map<std::string, std::map<std::uint32_t, std::uint32_t>> tables;
        for (auto const& [label, formatName] : variants) {
            auto loaded = ObjectFormatSchema::loadShipped(formatName);
            ASSERT_TRUE(loaded.has_value()) << "loadShipped(" << formatName << ")";
            auto& table = tables[label];
            for (auto const& row : (*loaded)->relocations()) {
                // An `emitOnly` alias row shares its wire value with a real row
                // and exists only so the EMITTER can reach it through a second
                // kind; it is not part of the reverse map a reader builds, so it
                // is not part of the vocabulary under comparison.
                if (row.emitOnly) continue;
                table.emplace(row.kind.v, row.nativeId);
            }
        }

        std::set<std::uint32_t> kinds;
        for (auto const& [label, table] : tables) {
            for (auto const& [kind, native] : table) kinds.insert(kind);
        }
        for (std::uint32_t kind : kinds) {
            std::map<std::uint32_t, std::vector<std::string>> byValue;
            for (auto const& [label, table] : tables) {
                auto const it = table.find(kind);
                // A kind DECLARED BY ONE VARIANT AND NOT ANOTHER is the
                // "missing key" case, deliberately outside this census (see the
                // file header) -- only variants that declare the kind take part.
                if (it != table.end()) byValue[it->second].push_back(label);
            }
            if (byValue.size() < 2u) { ++comparedKinds; continue; }
            Divergence d;
            d.family = family;
            d.kind   = kind;
            for (auto& [value, labels] : byValue) {
                std::sort(labels.begin(), labels.end());
                std::string group = std::to_string(value) + " = ";
                for (std::size_t i = 0; i < labels.size(); ++i) {
                    if (i != 0) group += '/';
                    group += labels[i];
                }
                d.groups.push_back(std::move(group));
            }
            observed.push_back(d.render());
            ++comparedKinds;
        }
    }
    std::sort(observed.begin(), observed.end());

    EXPECT_GT(comparedKinds, 20u)
        << "only " << comparedKinds << " (family, kind) pairs were compared -- "
           "the relocation tables did not load, so agreement here means nothing";

    // ★ THE EXACT SET, re-derived from the shipped configuration on this run.
    // Not "at most N": EXACTLY this, so the test reds when any family JOINS.
    //
    // ⚠⚠ THE SET IS EMPTY, AND IT WAS NOT ALWAYS. It held two rows for
    // `macho64-x86_64-darwin` (kind 1: 369098752 dylib/exec vs 620756992
    // relocatable/staticlib; kind 3: 33554432 vs 67108864) until 2026-08-20,
    // and this guard fired exactly as it was built to when they left. THE
    // SCHEMA EDIT WAS RE-ARGUED, which is what the failure message demands,
    // and the argument is
    // `D-CONFIG-MACHO-X86_64-EXEC-DYLIB-RELOC-NATIVEID-CONTRADICTS-ITS-OWN-ROW`:
    // the two image documents were not a second
    // honest encoding, they were SELF-REFUTING. Under the packing their own
    // comments state -- (r_type<<28)|(r_length<<25)|(r_pcrel<<24) -- 369098752
    // decodes to r_type=1 (SIGNED), 8 bytes, NOT pc-relative, for a row named
    // X86_64_RELOC_BRANCH, and 33554432 decodes to a TWO-byte slot for a row
    // named `_4`. No external authority was needed to settle it; the documents
    // refute themselves, and their `-darwin`/`-staticlib` siblings and the
    // entire arm64 family always carried the corrected values.
    //
    // ⓘ WHAT THE EMPTINESS DOES **NOT** MEAN. This file used to argue that the
    // one divergent family was "the reason `declare the relocation vocabulary
    // once per lineage` is not an available design". That argument is now
    // GONE and must not be quietly inherited: on the same-kind-different-value
    // axis the shipped corpus is unanimous. What still differs between an
    // object and an image vocabulary is KIND MEMBERSHIP -- an image-only PLT
    // or TLS kind absent from the relocatable arm, an `emitOnly` alias present
    // only in it -- which this census deliberately excludes (see the header).
    // A future cycle that wants to collapse the two documents has to argue
    // against THAT, not against this now-empty set.
    std::vector<std::string> const expected = {};
    EXPECT_EQ(observed, expected)
        << "the object-vs-image relocation-vocabulary divergence set changed.\n"
           "  A NEW family in the set: a family's object and image variants "
           "have drifted apart, i.e. one variant declares a wire value for a "
           "kind that its sibling spells differently. Investigate that family "
           "-- widening this list throws the news away. Check first whether "
           "the new value CONTRADICTS its own row (name, comment, declared "
           "packing), which is how the last two entries got here.";
}
