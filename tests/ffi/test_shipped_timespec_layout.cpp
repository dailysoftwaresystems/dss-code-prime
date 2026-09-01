// D-CSUBSET-C11-THREADS-TIMED (P49 lane th) — the per-format `struct timespec`
// body, in BOTH descriptors that declare it.
//
// ── THE DEFECT ──────────────────────────────────────────────────────────────
// `time.json` and `sys/stat.json` each declared `timespec` FLAT —
// {i64 tv_sec, i64 tv_nsec} — with no variant selector, while BOTH files list
// `pe` in `availableObjectFormats`. On LLP64 the nanoseconds field is `long`,
// which is FOUR bytes there. So every Windows TU that spelled the tag received
// an LP64 body: an 8-byte store through a 4-byte field, and an 8-byte read that
// folds four bytes of trailing pad into the high half.
//
// ── WHY `sizeof` CANNOT SEE IT ──────────────────────────────────────────────
// The same-size trap `struct timeval` already taught this project
// (D-FFI-MACHO-TIMEVAL-TV-USEC-WIDTH): the struct is 16 bytes on BOTH sides and
// tv_nsec is at offset 8 on BOTH. LP64 spends the 16 bytes on two 8-byte fields;
// LLP64 on 8 + 4 + 4 of trailing pad. A size assertion and an offset assertion
// are BOTH satisfied by the wrong answer. Only the FIELD WIDTH moves, so every
// test below asserts the width and the interned IDENTITY, never the total alone.
//
// ── THE REFERENCE COMPILERS, PROBED SEPARATELY ──────────────────────────────
// ✔MEASURED 2026-09-01 with a compile-only `_Static_assert` battery over
// `sizeof` / `offsetof` / `_Alignof` plus `_Generic` for the exact C spelling —
// so the answer read is the COMPILER's, after the whole typedef chain, not a
// grep of a header. Each reference was ALSO run with a CONTROL arm carrying one
// deliberately-false assertion, and that arm failed to compile on all four, so
// the probe is known to be capable of failing:
//
//   mingw-w64 gcc 13.2.0 (x86_64-w64-mingw32, ucrt)  {long long @0 (8), long @8 (4)}
//   MSVC cl.exe 19.51.36252                          {long long @0 (8), long @8 (4)}
//   WSL gcc 13.3.0 (glibc)                           {long      @0 (8), long @8 (8)}
//   clang 18.1.3 (glibc)                             {long      @0 (8), long @8 (8)}
//
// The two pe references AGREE on the width, so no disjunction question arises
// about it. (They DO split on whether <sys/stat.h> alone makes the tag visible —
// mingw yes, MSVC no — and the disjunction settles that in favour of declaring
// it, which is why `sys/stat.json` keeps a pe arm at all. That half is recorded
// in `sys/stat.json`'s own `$timespecVariantsComment`.)
//
// ── WHY THE ELF AND MACHO ARMS ARE ASSERTED AT LEAST AS HARD ────────────────
// The fix had to be ADDITIVE. glibc's and Darwin's tv_nsec genuinely ARE 8
// bytes, so a change that "corrected" pe by moving elf or macho would be
// strictly worse than the defect. Those arms are byte-identical to the former
// flat body ON PURPOSE, and their bare `i64` spellings were deliberately left
// untagged: `RealTimeAndSysStatShareOneTimespecTypeId` in
// test_shipped_lib_descriptor.cpp derives its declSiteKey from FIELD CONTENT, so
// re-spelling one file's `i64` as `i64 "long"` splits an identity the macho
// `st_mtimespec` by-name member depends on. The tag residuals are tracked with
// their own trigger at D-FFI-DESCRIPTOR-VOCABULARY-TAG-RESIDUALS.
//
// ── RED-ON-DISABLE, AND WHY THE MUTANTS DELETE JSON ─────────────────────────
// A config change has a specific trap: an ADD-direction fixture stays GREEN when
// the real config LOSES the feature. Every mutant here therefore REMOVES the pe
// arm from a COPY of the shipped tree and asserts the loss is SEEN — including
// the ONE-SIDED removal, which is exactly the half-fix the cross-file identity
// guard caught while this row was being written. `src/dss-config` is never
// touched.
//
// ⚠ EVERY MUTANT GETS ITS OWN SCRATCH PATH — the per-root corpus index is
// memoized process-wide with no staleness check, so mutating one tree in place
// and re-asking is answered from the PRE-mutation index (the trap recorded in
// `test_shipped_realization_oracle.cpp`'s header).

#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::ffi::readShippedLibDescriptor;
using dss::test_support::Location;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

// The shipped-target aggregate-layout params — natural alignment, 16-byte ISA
// cap; the same context every other descriptor layout pin asserts under.
constexpr AggregateLayoutParams kNatural16{ScalarAlignmentRule::Natural, 16};

// The two named C types tv_sec / tv_nsec can BE, plus the ANONYMOUS cores.
// Spelled INDEPENDENTLY of the shipped descriptors, so an assertion compares the
// shipped answer against something this file owns rather than against itself.
// The anonymous rows are load-bearing in the NEGATIVE direction on pe: a bare
// `i32` there would be a THIRD type matching neither `long` nor `int`.
constexpr char kReferenceVocabulary[] = R"({
  "$comment": "P49 lane th — reference vocabulary for the timespec layout pin. Not shipped; written to a scratch directory by the test.",
  "header": "dss-test-timespec-reference-vocabulary.h",
  "typedefs": [
    { "name": "ref_long_64",       "type": "i64 \"long\"" },
    { "name": "ref_long_long_64",  "type": "i64 \"long long\"" },
    { "name": "ref_long_32",       "type": "i32 \"long\"" },
    { "name": "ref_anonymous_i64", "type": "i64" },
    { "name": "ref_anonymous_i32", "type": "i32" }
  ]
})";

// The data model a format really is. pe is LLP64 on every shipped pe target and
// elf/macho are LP64 on every shipped one; passing the wrong pair is how P48's
// `decodeShippedFor` ended up asking about a `(Pe, LP64)` target that does not
// exist, and reading an arm no compile could select.
[[nodiscard]] DataModel modelFor(ObjectFormatKind fmt) {
    return fmt == ObjectFormatKind::Pe ? DataModel::Llp64 : DataModel::Lp64;
}

// One target's view of `timespec` as declared by ONE descriptor, read into an
// interner shared with the reference vocabulary so every TypeId is comparable.
class Reading {
public:
    Reading(fs::path const& cfgRoot, fs::path const& scratch,
            std::string_view relative, std::string_view arch, ObjectFormatKind fmt)
        : interner_{CompilationUnitId{1}} {
        fs::path const refPath = scratch / "dss_timespec_reference_vocabulary.json";
        std::ofstream(refPath, std::ios::binary) << kReferenceVocabulary;
        add(refPath, arch, fmt);
        add(cfgRoot / "shippedLibs" / fs::path{std::string{relative}}, arch, fmt);
    }

    void alsoRead(fs::path const& cfgRoot, std::string_view relative,
                  std::string_view arch, ObjectFormatKind fmt) {
        add(cfgRoot / "shippedLibs" / fs::path{std::string{relative}}, arch, fmt);
    }

    [[nodiscard]] bool clean() const { return ok_ && !reporter_.hasErrors(); }
    [[nodiscard]] TypeInterner const& interner() const { return interner_; }

    // `timespec` as the descriptor at `descIndex` (0 = the reference vocabulary)
    // declares it, or nullptr when NO variant matched — the reader injects
    // nothing in that case, which is the fail-loud path an unmeasured platform
    // must keep.
    [[nodiscard]] ffi::ShippedStruct const* timespecFrom(std::size_t descIndex) const {
        if (descIndex >= descs_.size()) return nullptr;
        for (auto const& s : descs_[descIndex].structs)
            if (s.name == "timespec") return &s;
        return nullptr;
    }

    [[nodiscard]] std::optional<TypeId> reference(std::string_view name) const {
        for (auto const& d : descs_)
            for (auto const& t : d.typedefs)
                if (t.name == name) return t.type;
        return std::nullopt;
    }

    [[nodiscard]] std::size_t descriptorCount() const { return descs_.size(); }

private:
    void add(fs::path const& p, std::string_view arch, ObjectFormatKind fmt) {
        auto d = readShippedLibDescriptor(p, interner_, typeReg_, reporter_,
                                          modelFor(fmt), arch, fmt);
        if (!d) { ok_ = false; return; }
        descs_.push_back(std::move(*d));
    }

    TypeInterner                           interner_;
    TypeRegistry                           typeReg_;
    DiagnosticReporter                     reporter_;
    std::vector<ffi::ShippedLibDescriptor> descs_;
    bool                                   ok_ = true;
};

// Assert the WHOLE body: field names, field WIDTHS, the interned identity of
// each field, and the derived offsets and total. `expectedNsecReference` is the
// name in the reference vocabulary the nanoseconds field must BE, or empty when
// the arm is deliberately left untagged (elf/macho) — in which case identity is
// asserted against the ANONYMOUS core instead, so "untagged" is a positive
// statement rather than an absence nobody checked.
void expectTimespecBody(Reading const& r, std::size_t descIndex,
                        std::uint64_t expectedNsecSize,
                        std::string_view expectedSecReference,
                        std::string_view expectedNsecReference,
                        DataModel model, char const* where) {
    auto const* ts = r.timespecFrom(descIndex);
    ASSERT_NE(ts, nullptr) << where << ": `timespec` is not injected at all";
    ASSERT_EQ(ts->fields.size(), 2u) << where;
    EXPECT_EQ(ts->fields[0].name, "tv_sec") << where;
    EXPECT_EQ(ts->fields[1].name, "tv_nsec") << where;

    auto const secRef  = r.reference(expectedSecReference);
    auto const nsecRef = r.reference(expectedNsecReference);
    ASSERT_TRUE(secRef.has_value())
        << "reference vocabulary lost '" << expectedSecReference << "'";
    ASSERT_TRUE(nsecRef.has_value())
        << "reference vocabulary lost '" << expectedNsecReference << "'";

    // IDENTITY, not merely width — `sizeof` cannot tell `long` from `long long`
    // at 8 bytes, and cannot tell a tagged `i32 "long"` from a bare `i32`.
    EXPECT_EQ(ts->fields[0].type, *secRef)
        << where << ": tv_sec should BE '" << expectedSecReference << "'";
    EXPECT_EQ(ts->fields[1].type, *nsecRef)
        << where << ": tv_nsec should BE '" << expectedNsecReference << "'";

    // WIDTH — the one fact that actually moves between the worlds.
    auto const nsecLayout = computeLayout(ts->fields[1].type, r.interner(),
                                          kNatural16, model);
    ASSERT_TRUE(nsecLayout.has_value()) << where;
    EXPECT_EQ(nsecLayout->size, expectedNsecSize)
        << where << ": tv_nsec width — THE assertion this row exists for";

    auto const secLayout = computeLayout(ts->fields[0].type, r.interner(),
                                         kNatural16, model);
    ASSERT_TRUE(secLayout.has_value()) << where;
    EXPECT_EQ(secLayout->size, 8u)
        << where << ": tv_sec is 8 bytes on every current target";

    // TOTAL + OFFSETS — deliberately asserted even though they are IDENTICAL on
    // both worlds. That is the point: recorded here, the same-size trap is a
    // documented property rather than a coincidence a later reader might lean on.
    auto const whole = computeLayout(ts->typeId, r.interner(), kNatural16, model);
    ASSERT_TRUE(whole.has_value()) << where;
    EXPECT_EQ(whole->size, 16u) << where << ": sizeof agrees on BOTH worlds";
    ASSERT_EQ(whole->fieldOffsets.size(), 2u) << where;
    EXPECT_EQ(whole->fieldOffsets[0], 0u) << where;
    EXPECT_EQ(whole->fieldOffsets[1], 8u)
        << where << ": tv_nsec is at 8 on BOTH worlds — an offset check cannot "
                    "discriminate, which is why the width check above exists";
}

[[nodiscard]] fs::path copyConfigTree(ScratchDir const& dir, fs::path const& cfgRoot) {
    fs::path const dst = dir.path() / "src" / "dss-config";
    fs::create_directories(dst.parent_path());
    fs::copy(cfgRoot, dst,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    return dst;
}

[[nodiscard]] nlohmann::json readJson(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    EXPECT_TRUE(in.good()) << p.generic_string();
    auto doc = nlohmann::json::parse(in, nullptr, false);
    EXPECT_FALSE(doc.is_discarded()) << p.generic_string();
    return doc;
}

void writeJson(fs::path const& p, nlohmann::json const& doc) {
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    out << doc.dump(2);
    EXPECT_TRUE(out.good())
        << "the mutation did not reach disk: " << p.generic_string();
}

// DELETE the `pe` arm of `timespec` from one descriptor of a COPIED tree.
// Returns how many arms it removed, so a mutant that silently matched nothing
// cannot pass for a mutant that worked.
[[nodiscard]] std::size_t dropPeTimespecArm(fs::path const& treeRoot,
                                            std::string_view relative) {
    fs::path const p = treeRoot / "shippedLibs" / fs::path{std::string{relative}};
    auto doc = readJson(p);
    std::size_t removed = 0;
    for (auto& s : doc.at("structs")) {
        if (!s.contains("name") || s.at("name") != "timespec") continue;
        if (!s.contains("variants")) continue;
        auto kept = nlohmann::json::array();
        for (auto const& v : s.at("variants")) {
            bool const isPe = v.contains("when") && v.at("when").contains("format")
                              && v.at("when").at("format") == "pe";
            if (isPe) { ++removed; continue; }
            kept.push_back(v);
        }
        s["variants"] = kept;
    }
    writeJson(p, doc);
    return removed;
}

constexpr std::string_view kTimeJson = "time.json";
constexpr std::string_view kStatJson = "sys/stat.json";

} // namespace

// ── elf / LP64 — THE CONTROL THAT MATTERS MOST ─────────────────────────────
//
// glibc's tv_nsec genuinely IS 8 bytes. This row is ADDITIVE and must not move
// elf at all, so the elf arm is asserted first and in full. `ref_anonymous_i64`
// is the expected identity on purpose: the elf arm was deliberately left with
// its bare `i64` spelling so the cross-file `timespec` identity does not split.
TEST(ShippedTimespecLayout, ElfLp64NanosecondsIsEightBytes) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    for (std::string_view arch : {"x86_64", "arm64"}) {
        SCOPED_TRACE(std::string{"elf/"} + std::string{arch});
        ScratchDir dir{Location::Temp, "timespec-elf"};
        Reading r{*cfg, dir.path(), kTimeJson, arch, ObjectFormatKind::Elf};
        ASSERT_TRUE(r.clean());
        expectTimespecBody(r, 1, /*nsec=*/8u, "ref_anonymous_i64",
                           "ref_anonymous_i64", DataModel::Lp64, "elf time.json");
    }
}

// macho is LP64 too and its tv_nsec is also 8 bytes — the SECOND control. Darwin
// is where this project has been surprised before (`struct timeval`'s tv_usec is
// 32-bit there while glibc's is 64-bit), so "macho follows elf here" is measured
// rather than assumed.
TEST(ShippedTimespecLayout, MachoLp64AgreesWithElf) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    for (std::string_view arch : {"x86_64", "arm64"}) {
        SCOPED_TRACE(std::string{"macho/"} + std::string{arch});
        ScratchDir dir{Location::Temp, "timespec-macho"};
        Reading r{*cfg, dir.path(), kTimeJson, arch, ObjectFormatKind::MachO};
        ASSERT_TRUE(r.clean());
        expectTimespecBody(r, 1, /*nsec=*/8u, "ref_anonymous_i64",
                           "ref_anonymous_i64", DataModel::Lp64, "macho time.json");
    }
}

// ── pe / LLP64 — THE ROW ───────────────────────────────────────────────────
//
// tv_nsec is FOUR bytes and is spelled `long`; tv_sec is eight and is spelled
// `long long`. Both spellings are TAGGED here, unlike the elf/macho arms: Win32
// spells these over NAMED vocabulary entries (D-LANG-TYPE-IDENTITY-VOCABULARY),
// this file's own pe `time_t` / `clock_t` / CLOCKS_PER_SEC arms already are, and
// nothing on pe consumed `timespec` before this row, so tagging costs no
// existing identity.
TEST(ShippedTimespecLayout, PeLlp64NanosecondsIsFourBytesAndSpelledLong) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "timespec-pe"};
    Reading r{*cfg, dir.path(), kTimeJson, "x86_64", ObjectFormatKind::Pe};
    ASSERT_TRUE(r.clean());
    expectTimespecBody(r, 1, /*nsec=*/4u, "ref_long_long_64", "ref_long_32",
                       DataModel::Llp64, "pe time.json");

    // The NEGATIVE half of the identity claim: a bare `i32` would be a third type
    // matching neither `long` nor `int`, and it has the RIGHT WIDTH, so only an
    // identity assertion can rule it out.
    auto const* ts = r.timespecFrom(1);
    ASSERT_NE(ts, nullptr);
    auto const anonymous = r.reference("ref_anonymous_i32");
    ASSERT_TRUE(anonymous.has_value());
    EXPECT_NE(ts->fields[1].type, *anonymous)
        << "pe tv_nsec interned as the ANONYMOUS 32-bit core — the `long` "
           "vocabulary tag is missing, so it matches NO named C type";
}

// ── THE CROSS-FILE INVARIANT, ON EVERY FORMAT ──────────────────────────────
//
// `time.json` and `sys/stat.json` BOTH declare `timespec` and both are available
// on all three formats. The complete-at-once composite path derives its
// declSiteKey from FIELD CONTENT, so the two rows intern ONE TypeId only while
// they stay byte-identical PER FORMAT. `RealTimeAndSysStatShareOneTimespecTypeId`
// already pins that on macho; this extends it to elf and — the reason the row
// needed it — to pe, where both files carried the same wrong flat body and
// fixing only one would have SPLIT the identity instead of fixing anything.
TEST(ShippedTimespecLayout, TimeAndSysStatAgreeOnEveryFormat) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    for (auto fmt : {ObjectFormatKind::Elf, ObjectFormatKind::MachO,
                     ObjectFormatKind::Pe}) {
        SCOPED_TRACE(objectFormatKindName(fmt));
        ScratchDir dir{Location::Temp, "timespec-cross-file"};
        Reading r{*cfg, dir.path(), kTimeJson, "x86_64", fmt};
        r.alsoRead(*cfg, kStatJson, "x86_64", fmt);
        ASSERT_TRUE(r.clean());
        ASSERT_EQ(r.descriptorCount(), 3u) << "both descriptors must have read";

        auto const* fromTime = r.timespecFrom(1);
        auto const* fromStat = r.timespecFrom(2);
        ASSERT_NE(fromTime, nullptr) << "time.json must declare timespec here";
        ASSERT_NE(fromStat, nullptr) << "sys/stat.json must declare timespec here";
        EXPECT_EQ(fromTime->typeId, fromStat->typeId)
            << "the two descriptors must intern ONE timespec on this format — a "
               "split here is the cross-file identity defect at the descriptor "
               "tier (D-FFI-DESCRIPTOR-CROSS-FILE-TYPE-IDENTITY)";
        ASSERT_EQ(fromStat->fields.size(), 2u);
        EXPECT_EQ(fromTime->fields[1].type, fromStat->fields[1].type)
            << "and the nanoseconds MEMBER must be the same type in both";
    }
}

// ── RED-ON-DISABLE — REMOVE-direction mutants on a COPY ────────────────────
//
// Deleting the pe arm from BOTH descriptors is the config LOSING the feature.
// pe then matches no variant and `timespec` is not injected there at all, while
// elf is untouched — the control that proves the mutant was the pe arm and not
// the copy.
TEST(ShippedTimespecLayout, MutantDroppingBothPeArmsUninjectsTimespecOnPe) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "timespec-mutant-drop-pe"};
    fs::path const tree = copyConfigTree(dir, *cfg);

    ASSERT_EQ(dropPeTimespecArm(tree, kTimeJson), 1u)
        << "the mutant must remove exactly one pe arm from time.json";
    ASSERT_EQ(dropPeTimespecArm(tree, kStatJson), 1u)
        << "the mutant must remove exactly one pe arm from sys/stat.json";

    {
        ScratchDir readDir{Location::Temp, "timespec-mutant-drop-pe-read"};
        Reading r{tree, readDir.path(), kTimeJson, "x86_64", ObjectFormatKind::Pe};
        EXPECT_EQ(r.timespecFrom(1), nullptr)
            << "with its pe arm deleted, `timespec` must be ABSENT on pe — the "
               "reader must not fall back to another format's body";
    }
    {
        ScratchDir ctlDir{Location::Temp, "timespec-mutant-drop-pe-control"};
        Reading r{tree, ctlDir.path(), kTimeJson, "x86_64", ObjectFormatKind::Elf};
        ASSERT_TRUE(r.clean()) << "elf CONTROL under the pe mutant";
        expectTimespecBody(r, 1, /*nsec=*/8u, "ref_anonymous_i64",
                           "ref_anonymous_i64", DataModel::Lp64,
                           "elf control under the pe mutant");
    }
}

// THE HALF-FIX. Removing the pe arm from ONE of the two files is the mistake
// this row actually made while it was being written, and the assertion is that
// the asymmetry is SEEN rather than tolerated: one descriptor declares the tag
// on pe and the other does not, so a pe TU including both headers gets a
// `timespec` from exactly one of them.
TEST(ShippedTimespecLayout, MutantDroppingOnlySysStatPeArmIsSeenAsAsymmetry) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "timespec-mutant-half-fix"};
    fs::path const tree = copyConfigTree(dir, *cfg);

    ASSERT_EQ(dropPeTimespecArm(tree, kStatJson), 1u);

    ScratchDir readDir{Location::Temp, "timespec-mutant-half-fix-read"};
    Reading r{tree, readDir.path(), kTimeJson, "x86_64", ObjectFormatKind::Pe};
    r.alsoRead(tree, kStatJson, "x86_64", ObjectFormatKind::Pe);
    ASSERT_EQ(r.descriptorCount(), 3u);

    EXPECT_NE(r.timespecFrom(1), nullptr)
        << "positive control: time.json still declares timespec on pe";
    EXPECT_EQ(r.timespecFrom(2), nullptr)
        << "the one-sided removal must be visible — sys/stat.json no longer "
           "declares `timespec` on pe while time.json still does";
}

// The mutant machinery's own control: an UNMUTATED copy of the tree answers
// exactly as the live tree does, so a red above is the mutation and not the copy.
TEST(ShippedTimespecLayout, UnmutatedCopyAnswersLikeTheLiveTree) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "timespec-copy-control"};
    fs::path const tree = copyConfigTree(dir, *cfg);

    ScratchDir readDir{Location::Temp, "timespec-copy-control-read"};
    Reading r{tree, readDir.path(), kTimeJson, "x86_64", ObjectFormatKind::Pe};
    ASSERT_TRUE(r.clean());
    expectTimespecBody(r, 1, /*nsec=*/4u, "ref_long_long_64", "ref_long_32",
                       DataModel::Llp64, "unmutated copy, pe");
}
