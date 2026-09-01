// D-FFI-DARWIN-FSID-T-SHAPE + D-FFI-DARWIN-MALLOC-ZONE-TAIL-OPAQUE (P48) — the
// two Darwin FFI struct SHAPES, pinned by OFFSET and TOTAL SIZE rather than by
// "the file parses".
//
// WHAT WAS MEASURED, AND WHERE. Both rows were opened on 2026-08-03 against an SDK
// SLICE, with the tag/member spellings INFERRED from Darwin convention and the
// `malloc_zone_t` tail left as one opaque 176-byte array because its members'
// signatures had never been read on a real Mac. On 2026-09-01 they were read on the
// operator's physical macOS host — macOS 26.6.2, build 25G83, arm64 — against
// MacOSX26.5.sdk, reached through BOTH SDK roots the host carries
// (`/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk` and Xcode's
// `.../MacOSX.platform/Developer/SDKs/MacOSX.sdk`; each is a symlink to
// `MacOSX26.5.sdk`, and `sys/_types/_fsid_t.h` has the same md5
// `fc7d0cdacbb156c7da8342b1eecc800b` under both, so they are one file, not two
// agreeing sources).
//
//   `usr/include/sys/_types/_fsid_t.h`:
//       typedef struct fsid { int32_t val[2]; } fsid_t; /* file system id type */
//
// so the INFERRED tag `fsid` and member `val` were BOTH CORRECT and `sys/mount.json`
// needed no edit — the row closed on the measurement, not on a change. That is
// exactly why this pin exists: with nothing in the tree to diff, the only durable
// record of the measurement is an assertion that goes red if a later edit moves it.
//
//   `usr/include/malloc/malloc.h`: 25 members, `sizeof == 200`, `_Alignof == 8`.
// A probe TU asserting the total, the alignment and EVERY ONE of the 25 `offsetof`
// values as `_Static_assert` compiles rc=0 under `/usr/bin/cc -arch arm64` AND
// `-arch x86_64`; each member's exact SIGNATURE is pinned there by assigning it to
// an independently spelled function-pointer variable, so a wrong parameter list or
// return type is a compile error rather than a passing size check. A CONTROL arm
// asserting `sizeof == 199` FAILS to compile, so that probe can go red.
//
// THE ONE PER-ARCH DIVERGENCE, and why `_malloc_zone_t` is now `variants`:
// `claimed_address` returns mach `boolean_t`, which Darwin defines per arch —
// `mach/arm/boolean.h` is `typedef int boolean_t;`, `mach/i386/boolean.h` is
// `typedef unsigned int boolean_t;` under `#if defined(__x86_64__) &&
// !defined(KERNEL)`. Both are 4 bytes, so NO OFFSET MOVES; only that member's
// interned TypeId differs (#47 / D-LANG-TYPE-IDENTITY-VOCABULARY makes identity,
// not width, decide). ⇒ `i32` on arm64, `u32` on x86_64, and the OTHER 24 cells
// must stay identical — which `TheTwoArchArmsDifferInExactlyOneMember` asserts
// cell by cell, so a typo in one arm cannot hide on the arch nobody ran.
//
// ── RED-ON-DISABLE, AND WHY THE MUTANTS DELETE JSON KEYS ────────────────────
// A config-driven change has a specific trap: an ADD-direction fixture stays GREEN
// when the real config LOSES the feature. So every mutant here REMOVES something
// from a COPY of the shipped tree — a variant arm, a struct entry, a field — and
// the assertion is that the loss is SEEN. `src/dss-config` is never touched.
//
// ⚠ EVERY MUTANT GETS ITS OWN SCRATCH PATH. The per-root corpus index is memoized
// process-wide with no staleness check, so mutating one tree in place and re-asking
// is answered from the PRE-mutation index (the trap recorded in
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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::Location;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

// The layout parameters every Mach-O target declares. The CRUX recorded on
// `ShippedStruct`: `x86_64.target.json` and `arm64.target.json` carry BYTE-IDENTICAL
// `AggregateLayoutParams`, so one value serves both arches here. These two structs
// carry EXPLICIT offsets on every field (the all-or-none channel,
// D-FFI-DESCRIPTOR-UNION-OVERLAY), so the total is `alignUp(max field extent, align)`
// and the scalar-alignment rule cannot move it — which is what makes this pin an
// assertion about the DESCRIPTOR rather than about the layout engine's defaults.
constexpr AggregateLayoutParams kDarwinLayout{
    ScalarAlignmentRule::Natural, 16, BitFieldStrategy::GnuPacked,
    UnnamedBitFieldAlignment::Ignored};

// The 25 members of `struct _malloc_zone_t`, in declaration order, with the byte
// offset MEASURED by `offsetof` on real Darwin for BOTH arches. Written out rather
// than computed from `8 * i`, because `version` is 4 bytes at 104 and the 4 bytes of
// tail padding at 108 are exactly the thing a generated sequence would paper over.
struct ZoneMember { std::string_view name; std::uint64_t offset; };
constexpr ZoneMember kZoneMembers[] = {
    {"reserved1", 0},                        {"reserved2", 8},
    {"size", 16},                            {"malloc", 24},
    {"calloc", 32},                          {"valloc", 40},
    {"free", 48},                            {"realloc", 56},
    {"destroy", 64},                         {"zone_name", 72},
    {"batch_malloc", 80},                    {"batch_free", 88},
    {"introspect", 96},                      {"version", 104},
    {"memalign", 112},                       {"free_definite_size", 120},
    {"pressure_relief", 128},                {"claimed_address", 136},
    {"try_free_default", 144},               {"malloc_with_options", 152},
    {"malloc_type_malloc", 160},             {"malloc_type_calloc", 168},
    {"malloc_type_realloc", 176},            {"malloc_type_memalign", 184},
    {"malloc_type_malloc_with_options", 192},
};
constexpr std::uint64_t kZoneSize = 200;
constexpr std::uint64_t kZoneAlign = 8;

// `sizeof(struct statfs)` and the offset of its `f_fsid` member, both MEASURED by
// `offsetof`/`sizeof` on real Darwin (arm64 run, x86_64 compile).
constexpr std::uint64_t kStatfsSize = 2168;
constexpr std::uint64_t kFsidOffset = 48;
constexpr std::uint64_t kFsidSize = 8;

// One descriptor read, kept alive with the interner that owns its TypeIds.
class Read {
public:
    Read(fs::path const& path, std::string_view arch, ObjectFormatKind fmt)
        : interner_{CompilationUnitId{1}} {
        desc_ = readShippedLibDescriptor(path, interner_, typeReg_, reporter_,
                                         DataModel::Lp64, arch, fmt);
    }

    [[nodiscard]] bool ok() const { return desc_.has_value() && !reporter_.hasErrors(); }
    [[nodiscard]] TypeInterner const& interner() const { return interner_; }

    [[nodiscard]] ShippedStruct const* structNamed(std::string_view n) const {
        if (!desc_) return nullptr;
        for (auto const& s : desc_->structs)
            if (s.name == n) return &s;
        return nullptr;
    }
    [[nodiscard]] std::optional<TypeId> typedefNamed(std::string_view n) const {
        if (!desc_) return std::nullopt;
        for (auto const& t : desc_->typedefs)
            if (t.name == n) return t.type;
        return std::nullopt;
    }

private:
    TypeInterner                       interner_;
    TypeRegistry                       typeReg_;
    DiagnosticReporter                 reporter_;
    std::optional<ShippedLibDescriptor> desc_;
};

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
    EXPECT_TRUE(out.good()) << "the mutation did not reach disk: " << p.generic_string();
}

} // namespace

// ── D-FFI-DARWIN-FSID-T-SHAPE — the CONTROL ────────────────────────────────
//
// The tag, the member name, the element type, the size, and the offset inside
// `struct statfs`. The member NAME is the half the row could not measure in 2026-08,
// so it is asserted by name here and not merely by position.
TEST(DarwinStructShapes, FsidTagAndMemberSpellingMatchTheSdk) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    fs::path const mount = *cfg / "shippedLibs" / "sys" / "mount.json";

    for (std::string_view const arch : {"arm64", "x86_64"}) {
        SCOPED_TRACE(arch);
        Read const r{mount, arch, ObjectFormatKind::MachO};
        ASSERT_TRUE(r.ok()) << "sys/mount.json did not read cleanly";

        ShippedStruct const* fsid = r.structNamed("fsid");
        ASSERT_NE(fsid, nullptr)
            << "the SDK spells the tag `fsid` (sys/_types/_fsid_t.h): "
               "typedef struct fsid { int32_t val[2]; } fsid_t;";
        ASSERT_EQ(fsid->fields.size(), 1u);
        EXPECT_EQ(fsid->fields[0].name, "val")
            << "the SDK names the member `val` — a different spelling makes every "
               "`x.val` miss, and the bytes stay right either way, so nothing else "
               "would catch it";

        auto const layout = computeLayout(fsid->typeId, r.interner(), kDarwinLayout,
                                          DataModel::Lp64);
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->size, kFsidSize) << "int32_t[2] is 8 bytes";
        ASSERT_EQ(layout->fieldOffsets.size(), 1u);
        EXPECT_EQ(layout->fieldOffsets[0], 0u);

        // THE INVARIANT THE ROW NAMES: the `fsid_t` typedef's inline body and the
        // `fsid` tag must intern to the SAME TypeId, or `fsid_t x = sb.f_fsid;`
        // stops type-checking even though both spellings describe the same bytes.
        auto const fsidT = r.typedefNamed("fsid_t");
        ASSERT_TRUE(fsidT.has_value());
        EXPECT_EQ(*fsidT, fsid->typeId)
            << "fsid_t's inline struct body and the `fsid` tag interned to "
               "DIFFERENT TypeIds — they must stay textually identical";

        // …and `struct statfs`'s member must BE that type, at the measured offset.
        ShippedStruct const* statfs = r.structNamed("statfs");
        ASSERT_NE(statfs, nullptr);
        bool seen = false;
        for (auto const& f : statfs->fields) {
            if (f.name != "f_fsid") continue;
            seen = true;
            EXPECT_EQ(f.type, fsid->typeId) << "statfs.f_fsid is not the fsid type";
            ASSERT_TRUE(f.offset.has_value());
            EXPECT_EQ(*f.offset, kFsidOffset);
        }
        EXPECT_TRUE(seen) << "struct statfs has no f_fsid member";

        auto const sl = computeLayout(statfs->typeId, r.interner(), kDarwinLayout,
                                      DataModel::Lp64);
        ASSERT_TRUE(sl.has_value());
        EXPECT_EQ(sl->size, kStatfsSize) << "sizeof(struct statfs) moved";
    }
}

// ── D-FFI-DARWIN-MALLOC-ZONE-TAIL-OPAQUE — the CONTROL ─────────────────────
TEST(DarwinStructShapes, MallocZoneIsFullyTypedAtTheMeasuredOffsets) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    fs::path const mallocJson = *cfg / "shippedLibs" / "malloc" / "malloc.json";

    for (std::string_view const arch : {"arm64", "x86_64"}) {
        SCOPED_TRACE(arch);
        Read const r{mallocJson, arch, ObjectFormatKind::MachO};
        ASSERT_TRUE(r.ok()) << "malloc/malloc.json did not read cleanly";

        ShippedStruct const* zone = r.structNamed("_malloc_zone_t");
        ASSERT_NE(zone, nullptr) << "no `_malloc_zone_t` variant matched " << arch
                                 << " — a Darwin arch with no arm injects NOTHING";
        ASSERT_EQ(zone->fields.size(), std::size(kZoneMembers))
            << "the zone must carry all 25 members — no `__opaque_tail`";

        for (std::size_t i = 0; i < std::size(kZoneMembers); ++i) {
            EXPECT_EQ(zone->fields[i].name, kZoneMembers[i].name)
                << "member " << i << " is misnamed or out of order";
            ASSERT_TRUE(zone->fields[i].offset.has_value())
                << kZoneMembers[i].name << " lost its explicit offset";
            EXPECT_EQ(*zone->fields[i].offset, kZoneMembers[i].offset)
                << kZoneMembers[i].name << " moved";
        }

        auto const layout = computeLayout(zone->typeId, r.interner(), kDarwinLayout,
                                          DataModel::Lp64);
        ASSERT_TRUE(layout.has_value());
        EXPECT_EQ(layout->size, kZoneSize)
            << "sizeof(malloc_zone_t) must stay exactly 200 — a consumer that "
               "embeds one by value gets whatever this file declares, silently";
        EXPECT_EQ(layout->align.bytes(), kZoneAlign);
        ASSERT_EQ(layout->fieldOffsets.size(), std::size(kZoneMembers));
        for (std::size_t i = 0; i < std::size(kZoneMembers); ++i)
            EXPECT_EQ(layout->fieldOffsets[i], kZoneMembers[i].offset)
                << "laid-out offset of " << kZoneMembers[i].name
                << " disagrees with the declared one";

        // The bare `malloc_zone_t` typedef must intern to the SAME type, or `.size`
        // stops resolving on a value spelled with the typedef name.
        auto const zoneT = r.typedefNamed("malloc_zone_t");
        ASSERT_TRUE(zoneT.has_value())
            << "no `malloc_zone_t` typedef variant matched " << arch;
        EXPECT_EQ(*zoneT, zone->typeId)
            << "the typedef's inline body and the `structs` entry interned apart";
    }
}

// The two arms exist for ONE cell. Anything else differing is a typo that would
// only ever be seen on the arch nobody happened to build.
TEST(DarwinStructShapes, TheTwoArchArmsDifferInExactlyOneMember) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    nlohmann::json const doc = readJson(*cfg / "shippedLibs" / "malloc" / "malloc.json");

    ASSERT_TRUE(doc.contains("structs"));
    auto const& variants = doc.at("structs").at(0).at("variants");
    ASSERT_EQ(variants.size(), 2u) << "expected exactly the arm64 and x86_64 arms";

    nlohmann::json const* arm = nullptr;
    nlohmann::json const* x86 = nullptr;
    for (auto const& v : variants) {
        auto const a = v.at("when").at("arch").get<std::string>();
        EXPECT_EQ(v.at("when").at("format").get<std::string>(), "macho");
        if (a == "arm64") arm = &v;
        else if (a == "x86_64") x86 = &v;
    }
    ASSERT_NE(arm, nullptr);
    ASSERT_NE(x86, nullptr);

    auto const& af = arm->at("fields");
    auto const& xf = x86->at("fields");
    ASSERT_EQ(af.size(), xf.size());

    std::vector<std::string> differing;
    for (std::size_t i = 0; i < af.size(); ++i) {
        EXPECT_EQ(af[i].at("name"), xf[i].at("name")) << "member " << i;
        EXPECT_EQ(af[i].at("offset"), xf[i].at("offset")) << "member " << i;
        if (af[i].at("type") != xf[i].at("type"))
            differing.push_back(af[i].at("name").get<std::string>());
    }
    ASSERT_EQ(differing.size(), 1u)
        << "the arms may differ in exactly ONE member — mach boolean_t. A second "
           "divergence needs its own measurement and its own note, not a silent "
           "second arm";
    EXPECT_EQ(differing[0], "claimed_address");
    EXPECT_NE(af[17].at("type").get<std::string>().find("-> i32>"), std::string::npos)
        << "arm64 boolean_t is `int`";
    EXPECT_NE(xf[17].at("type").get<std::string>().find("-> u32>"), std::string::npos)
        << "x86_64 boolean_t is `unsigned int`";
}

// ── REMOVE-direction mutants ───────────────────────────────────────────────

// Delete the x86_64 arm. The selector requires EXACTLY ONE match and has no
// catch-all, so losing an arm does not fall back — the tag injects NOTHING on that
// arch, silently, while arm64 stays perfectly green. This is the failure mode the
// two-arm shape buys, so it is the one that must be pinned.
TEST(DarwinStructShapes, DeletingTheX86ArmSilencesTheZoneOnThatArchOnly) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    ScratchDir dir{Location::Temp, "darwin-shapes-drop-arm"};
    fs::path const tree = copyConfigTree(dir, *cfg);
    fs::path const mallocJson = tree / "shippedLibs" / "malloc" / "malloc.json";

    nlohmann::json doc = readJson(mallocJson);
    auto& variants = doc.at("structs").at(0).at("variants");
    ASSERT_EQ(variants.size(), 2u);
    for (std::size_t i = 0; i < variants.size(); ++i) {
        if (variants[i].at("when").at("arch") == "x86_64") { variants.erase(i); break; }
    }
    ASSERT_EQ(variants.size(), 1u) << "the mutation removed nothing";
    writeJson(mallocJson, doc);

    Read const gone{mallocJson, "x86_64", ObjectFormatKind::MachO};
    ASSERT_TRUE(gone.ok()) << "the mutant must still READ — the loss is silent, "
                              "which is exactly why it needs a pin";
    EXPECT_EQ(gone.structNamed("_malloc_zone_t"), nullptr)
        << "the x86_64 arm was deleted and the tag still injected — this test can "
           "no longer tell arm coverage from arm absence";

    Read const kept{mallocJson, "arm64", ObjectFormatKind::MachO};
    ASSERT_TRUE(kept.ok());
    EXPECT_NE(kept.structNamed("_malloc_zone_t"), nullptr)
        << "arm64 must be untouched, or this proves only that the file broke";
}

// Delete ONE typed tail member from both arms. Its 8 bytes sit between neighbours
// that keep their explicit offsets, so the TOTAL stays 200 and the member simply
// vanishes — a size-only pin would call that green. This is the ADD/REMOVE
// asymmetry the brief names, made concrete.
TEST(DarwinStructShapes, DeletingATypedTailMemberIsSeenThoughTheSizeIsUnchanged) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    ScratchDir dir{Location::Temp, "darwin-shapes-drop-member"};
    fs::path const tree = copyConfigTree(dir, *cfg);
    fs::path const mallocJson = tree / "shippedLibs" / "malloc" / "malloc.json";

    nlohmann::json doc = readJson(mallocJson);
    std::size_t removed = 0;
    for (auto& v : doc.at("structs").at(0).at("variants")) {
        auto& fields = v.at("fields");
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].at("name") == "pressure_relief") {
                fields.erase(i);
                ++removed;
                break;
            }
        }
    }
    ASSERT_EQ(removed, 2u) << "expected `pressure_relief` in both arms";
    writeJson(mallocJson, doc);

    Read const r{mallocJson, "arm64", ObjectFormatKind::MachO};
    ASSERT_TRUE(r.ok());
    ShippedStruct const* zone = r.structNamed("_malloc_zone_t");
    ASSERT_NE(zone, nullptr);
    EXPECT_EQ(zone->fields.size(), std::size(kZoneMembers) - 1)
        << "the deleted member is still there";

    auto const layout = computeLayout(zone->typeId, r.interner(), kDarwinLayout,
                                      DataModel::Lp64);
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(layout->size, kZoneSize)
        << "the CONTROL half of this mutant: the total is UNCHANGED by the deletion, "
           "so a pin that only checked the size would have stayed green";
}

// Delete the `fsid` struct entry from sys/mount.json. `f_fsid` is spelled BY NAME
// (`"type": "fsid"`), so losing the tag is not a parse error waiting to happen in
// some other file — it is this descriptor losing the member spelling the row is
// about.
TEST(DarwinStructShapes, DeletingTheFsidStructEntryIsSeen) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    ScratchDir dir{Location::Temp, "darwin-shapes-drop-fsid"};
    fs::path const tree = copyConfigTree(dir, *cfg);
    fs::path const mount = tree / "shippedLibs" / "sys" / "mount.json";

    nlohmann::json doc = readJson(mount);
    auto& structs = doc.at("structs");
    bool erased = false;
    for (std::size_t i = 0; i < structs.size(); ++i) {
        if (structs[i].at("name") == "fsid") { structs.erase(i); erased = true; break; }
    }
    ASSERT_TRUE(erased) << "sys/mount.json no longer declares the `fsid` tag — "
                           "re-point this mutant, do not delete the test";
    writeJson(mount, doc);

    Read const r{mount, "arm64", ObjectFormatKind::MachO};
    EXPECT_EQ(r.structNamed("fsid"), nullptr)
        << "the `fsid` entry was deleted and the tag still injected";
}
