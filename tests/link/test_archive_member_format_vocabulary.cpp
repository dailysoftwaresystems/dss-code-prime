// The OBJECT-vs-IMAGE relocation-vocabulary census --
// D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-THE-OBJECT-FORMAT (OPEN,
// HIGH).
//
// ★★★ THIS TEST PINS A KNOWN DEFECT'S BLAST RADIUS. IT DOES NOT ASSERT DESIRED
// BEHAVIOUR. Everything below describes something that is WRONG and is expected
// to stay wrong until that row closes; the assertion exists so the wrongness
// cannot quietly spread or quietly change shape.
//
// THE DEFECT. `compile_pipeline.cpp::readArchiveMemberModule` hands an archive
// MEMBER to a reader together with the FINAL IMAGE's `ObjectFormatSchema` -- the
// `-exec` / `-dylib` / `-dll` variant the link is producing -- rather than the
// member's own relocatable format. Every reader builds a reverse map from the
// schema's `nativeId` back to a universal `RelocationKind`, so the member's wire
// values are decoded against a vocabulary that was never promised to describe
// them.
//
// WHY IT HAS NOT BURNED EVERY LEG. On most families the object-side and
// image-side vocabularies happen to declare the same `nativeId` for the same
// `kind`, so reading a member through the wrong one is harmless BY COINCIDENCE.
// That coincidence is load-bearing and entirely undocumented, which is exactly
// the thing worth pinning: if a family's variants drift apart, the leg that
// stops working gives no hint that a schema pairing was the cause.
//
// ★ THE ONE FAMILY WHERE THE COINCIDENCE ALREADY FAILS is `macho64-x86_64-
// darwin`, on two kinds, and the split is structurally meaningful rather than a
// typo: `{relocatable, staticlib}` on one side against `{exec, dylib}` on the
// other -- the OBJECT vocabulary disagreeing with the IMAGE vocabulary, which is
// precisely the axis the defect crosses. A member read there does not merely
// miss a key; it resolves the same wire value to a different meaning, or fails
// to resolve it at all.
//
// ⚠⚠ A RED HERE MAY BE GOOD NEWS, AND THE NEXT READER SHOULD CHECK WHICH.
//   * If a SECOND family appears in the divergent set, the coincidence the other
//     legs rely on has started to erode -- that is the alarm this test exists to
//     raise, and it must not be silenced by widening the expectation without
//     understanding why the new family drifted.
//   * If `macho64-x86_64-darwin` DISAPPEARS from the set, the defect has been
//     REPAIRED. The expectation below must then be updated IN THE SAME COMMIT
//     that repairs it, and this file's premise re-read: once no family diverges,
//     the census pins nothing and the row is closable.
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
    auto const root = dss::test::findRepoRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::repoRootDiagnostic();
        return {};
    }
    return *root / "src" / "dss-config" / "object-formats";
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
    // Not "at least one", not "no more than N": exactly these, so the test reds
    // when a family JOINS the set and reds again when this family LEAVES it.
    // MEASURED from `src/dss-config/object-formats` at the commit that added
    // this test.
    std::vector<std::string> const expected = {
        "macho64-x86_64-darwin kind 1: "
        "369098752 = dylib/exec | 620756992 = relocatable/staticlib",
        "macho64-x86_64-darwin kind 3: "
        "33554432 = dylib/exec | 67108864 = relocatable/staticlib",
    };
    EXPECT_EQ(observed, expected)
        << "the object-vs-image relocation-vocabulary divergence set changed.\n"
           "  A NEW family in the set: the coincidence every other leg relies on "
           "is eroding -- investigate that family, do not widen this list.\n"
           "  macho64-x86_64-darwin GONE from the set: "
           "D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-THE-OBJECT-FORMAT "
           "was repaired -- update this expectation in the repairing commit.";
}
