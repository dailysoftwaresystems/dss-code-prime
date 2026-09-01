// D-FFI-DESCRIPTOR-TYPE-ALIAS-SPELLING-KEYED-ON-DATA-MODEL-ALONE (P48 lane st) —
// WHICH NAMED C TYPE each shipped `<stdint.h>` / `<stddef.h>` / `<time.h>` /
// `<sys/types.h>` 64-bit alias IS, per (data model, object format).
//
// ── THE DEFECT THIS PINS, AND WHY IT IS AN IDENTITY DEFECT ──────────────────
// Every one of these aliases carried a `when:{dataModel}` variant selector, on the
// assumption — written into `stdint.json`'s own `$comment` — that `uint64_t` /
// `uintptr_t` / `uintmax_t` / `size_t` ARE `unsigned long` on LP64 and
// `unsigned long long` on LLP64. That is a TWO-WORLD assumption and there is a
// THIRD world. ✔MEASURED 2026-09-01 on the operator's Mac (macOS 26.6.2, build
// 25G83, MacOSX26.5.sdk, Apple clang 21.0.0) with a `_Generic` compile-only probe
// — every alias tested against every candidate named type, so the answer read is
// the COMPILER's after the whole typedef chain, not a grep of the SDK — under BOTH
// `-arch arm64` AND `-arch x86_64`, answers byte-identical:
//
//     DARWIN IS LP64 AND ITS `uint64_t` IS `unsigned long long`,
//     WHILE ITS `size_t` STAYS `unsigned long`.
//
// So the aliases that one key held together SPLIT, and no single `dataModel` axis
// can express it. Every alias is 8 bytes on all three platforms, so NOTHING
// miscompiled by size — what Darwin recorded was a false ABI SPELLING. Since type
// identity was split off representation, a `uint64_t` interned as `unsigned long`
// is a DIFFERENT TYPE from `unsigned long long`: `_Generic` takes no arm and
// `unsigned long long *p = &u64var;` is a bare `S_TypeMismatch`.
//
// ⚠ THIS FILE THEREFORE ASSERTS IDENTITY, NOT WIDTH. `sizeof` cannot see the
// defect — both candidates are 8 bytes — so every spelling assertion here is a
// `TypeId` EQUALITY against an independently spelled reference typedef read into
// the SAME interner. `TypeId`s are interner-local, so a second interner would be
// answering a different question; `Vocabulary` below exists to keep them one.
// The width is asserted too, separately, precisely to record that it never moved.
//
// ── THE REFERENCE COMPILERS, PROBED SEPARATELY ─────────────────────────────
//   elf   LP64 : WSL gcc 13.3.0, clang 18.1.3, aarch64-linux-gnu-gcc 13.3.0
//   macho LP64 : Apple clang 21.0.0, `-arch arm64` and `-arch x86_64`
//   pe    LLP64: mingw-w64 gcc 13.2.0 (ucrt), MSVC cl.exe 19.51.36252
// The two pe references agree with each other and the three elf references agree
// with each other; macho is the leg that disagrees with elf despite sharing LP64.
//
// ── RED-ON-DISABLE, AND WHY THE MUTANTS DELETE JSON KEYS ────────────────────
// A config change has a specific trap: an ADD-direction fixture stays GREEN when
// the real config LOSES the feature. So every mutant here REMOVES something from a
// COPY of the shipped tree — a whole variant arm, or just the `"format"` key that
// this row ADDED — and asserts the loss is SEEN. Deleting only the `"format"` keys
// restores the exact pre-row shape, and the assertion is that the restored shape
// now fails LOUD (two arms match on one target) rather than silently answering
// `unsigned long` on Darwin as it used to. `src/dss-config` is never touched.
//
// ⚠ EVERY MUTANT GETS ITS OWN SCRATCH PATH — the per-root corpus index is memoized
// process-wide with no staleness check, so mutating one tree in place and re-asking
// is answered from the PRE-mutation index (the trap recorded in
// `test_shipped_realization_oracle.cpp`'s header).

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

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

// The four named C types every alias here can BE, plus the ANONYMOUS 8-byte core.
// Spelled INDEPENDENTLY of the shipped descriptors so an assertion below compares
// the shipped answer against something this test owns, not against itself. The
// anonymous row is load-bearing: a bare `u64` is a THIRD type matching NEITHER
// named entry, which is the failure mode the vocabulary tag exists to prevent, and
// asserting `!=` against it is how this file proves a tag is present at all.
constexpr char kReferenceVocabulary[] = R"({
  "$comment": "P48 lane st — reference vocabulary for the stdint alias identity pin. Not shipped; written to a scratch directory by the test.",
  "header": "dss-test-stdint-reference-vocabulary.h",
  "typedefs": [
    { "name": "ref_long",               "type": "i64 \"long\"" },
    { "name": "ref_long_long",          "type": "i64 \"long long\"" },
    { "name": "ref_unsigned_long",      "type": "u64 \"unsigned long\"" },
    { "name": "ref_unsigned_long_long", "type": "u64 \"unsigned long long\"" },
    { "name": "ref_anonymous_i64",      "type": "i64" },
    { "name": "ref_anonymous_u64",      "type": "u64" }
  ]
})";

// One target's view of the shipped aliases, read into ONE interner alongside the
// reference vocabulary so every TypeId below is comparable. `arch` participates in
// the `when` match, so it is passed for real rather than defaulted — the point of
// `TheTwoDarwinArchesAgree` is that it does NOT change the answer.
class Vocabulary {
public:
    Vocabulary(fs::path const& cfgRoot, fs::path const& scratch,
               std::string_view arch, ObjectFormatKind fmt, DataModel model)
        : interner_{CompilationUnitId{1}} {
        fs::path const refPath = scratch / "dss_stdint_reference_vocabulary.json";
        std::ofstream(refPath, std::ios::binary) << kReferenceVocabulary;
        add(refPath, arch, fmt, model);
        add(cfgRoot / "shippedLibs" / "stdint.json", arch, fmt, model);
    }

    // A descriptor read AFTER construction, into the same interner — used by the
    // sibling-header controls (`stddef.json`, `time.json`, `sys/types.json`).
    void alsoRead(fs::path const& p, std::string_view arch, ObjectFormatKind fmt,
                  DataModel model) {
        add(p, arch, fmt, model);
    }

    [[nodiscard]] bool clean() const { return ok_ && !reporter_.hasErrors(); }
    [[nodiscard]] DiagnosticReporter const& reporter() const { return reporter_; }
    [[nodiscard]] TypeInterner const& interner() const { return interner_; }

    // The TypeId a name resolved to, or nullopt when NO variant matched (the
    // reader injects nothing in that case — the fail-loud path this row preserves
    // for ILP32 and for any unmeasured platform).
    [[nodiscard]] std::optional<TypeId> operator[](std::string_view name) const {
        for (auto const& d : descs_)
            for (auto const& t : d.typedefs)
                if (t.name == name) return t.type;
        return std::nullopt;
    }

private:
    void add(fs::path const& p, std::string_view arch, ObjectFormatKind fmt,
             DataModel model) {
        auto d = readShippedLibDescriptor(p, interner_, typeReg_, reporter_, model,
                                          arch, fmt);
        if (!d) { ok_ = false; return; }
        descs_.push_back(std::move(*d));
    }

    TypeInterner                        interner_;
    TypeRegistry                        typeReg_;
    DiagnosticReporter                  reporter_;
    std::vector<ffi::ShippedLibDescriptor> descs_;
    bool                                ok_ = true;
};

// `EXPECT` that `alias` resolved AND is the SAME interned type as `reference`.
// Written as a helper so every failure names both sides — a bare TypeId pair in a
// gtest message is two integers and tells the reader nothing.
void expectSpelledAs(Vocabulary const& v, std::string_view alias,
                     std::string_view reference, char const* where) {
    auto const a = v[alias];
    auto const r = v[reference];
    ASSERT_TRUE(r.has_value()) << "reference vocabulary lost '" << reference << "'";
    ASSERT_TRUE(a.has_value())
        << where << ": no variant matched for '" << alias
        << "' — the alias is not injected at all on this target";
    EXPECT_EQ(*a, *r) << where << ": '" << alias << "' should BE '" << reference
                      << "' but interned as a different type";
}

// The alias must NOT be the anonymous representative — i.e. it carries a
// vocabulary tag at all. `sizeof` cannot see this; `_Generic` can.
void expectNotAnonymous(Vocabulary const& v, std::string_view alias,
                        std::string_view anonymousRef, char const* where) {
    auto const a = v[alias];
    auto const r = v[anonymousRef];
    ASSERT_TRUE(a.has_value()) << where << ": '" << alias << "' not injected";
    ASSERT_TRUE(r.has_value());
    EXPECT_NE(*a, *r) << where << ": '" << alias
                      << "' interned as the ANONYMOUS core — the vocabulary tag is "
                         "missing, so it matches NEITHER named C type";
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
    EXPECT_TRUE(out.good()) << "the mutation did not reach disk: " << p.generic_string();
}

// The six aliases whose spelling DIVERGES between elf-LP64 and macho-LP64, and the
// four whose spelling does not. Both lists are MEASURED, and keeping them apart is
// the whole content of the row: a fix that moved all ten would be as wrong as the
// key that moved none of them.
constexpr std::string_view kSignedExactWidthSix[] = {
    "int64_t", "int_least64_t", "int_fast64_t"};
constexpr std::string_view kUnsignedExactWidthSix[] = {
    "uint64_t", "uint_least64_t", "uint_fast64_t"};
constexpr std::string_view kSignedPointerAndMax[]   = {"intptr_t", "intmax_t"};
constexpr std::string_view kUnsignedPointerAndMax[] = {"uintptr_t", "uintmax_t"};

} // namespace

// ── elf / LP64 — THE CONTROL THAT MATTERS MOST ─────────────────────────────
//
// glibc's LP64 `uint64_t` genuinely IS `unsigned long`. This row is ADDITIVE: it
// adds a Darwin arm and must not move ELF at all. A change that "fixed" Darwin by
// breaking glibc would be strictly worse than the defect, so this test is written
// FIRST and asserts the whole table, not a sample.
TEST(StdintAliasIdentity, ElfLp64SpellsEveryAliasWithLong) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "stdint-alias-elf"};
    Vocabulary v{*cfg, dir.path(), "x86_64", ObjectFormatKind::Elf, DataModel::Lp64};
    ASSERT_TRUE(v.clean());

    for (auto n : kSignedExactWidthSix)   expectSpelledAs(v, n, "ref_long", "elf");
    for (auto n : kSignedPointerAndMax)   expectSpelledAs(v, n, "ref_long", "elf");
    for (auto n : kUnsignedExactWidthSix) expectSpelledAs(v, n, "ref_unsigned_long", "elf");
    for (auto n : kUnsignedPointerAndMax) expectSpelledAs(v, n, "ref_unsigned_long", "elf");
}

// The aarch64 elf leg answers identically — the same three glibc references were
// probed and agree, so a per-arch divergence here would be a config typo, not an ABI.
TEST(StdintAliasIdentity, ElfArm64AgreesWithElfX86_64) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "stdint-alias-elf-arm64"};
    Vocabulary v{*cfg, dir.path(), "arm64", ObjectFormatKind::Elf, DataModel::Lp64};
    ASSERT_TRUE(v.clean());

    expectSpelledAs(v, "uint64_t",  "ref_unsigned_long", "elf/arm64");
    expectSpelledAs(v, "uintmax_t", "ref_unsigned_long", "elf/arm64");
    expectSpelledAs(v, "int64_t",   "ref_long",          "elf/arm64");
}

// ── macho / LP64 — THE DEFECT ──────────────────────────────────────────────
//
// The SPLIT, asserted as a split: the exact/least/fast-width six are `long long`
// while the pointer/greatest-width four stay `long`, on the SAME data model. Under
// the defect every one of these read `long` / `unsigned long`, so the first loop
// is the direct regression witness and the second is what proves the fix did not
// simply flip the whole file.
TEST(StdintAliasIdentity, MachoLp64SplitsTheExactWidthSixFromThePointerAndMaxFour) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "stdint-alias-macho"};
    Vocabulary v{*cfg, dir.path(), "arm64", ObjectFormatKind::MachO, DataModel::Lp64};
    ASSERT_TRUE(v.clean());

    for (auto n : kSignedExactWidthSix)   expectSpelledAs(v, n, "ref_long_long", "macho");
    for (auto n : kUnsignedExactWidthSix) expectSpelledAs(v, n, "ref_unsigned_long_long", "macho");
    for (auto n : kSignedPointerAndMax)   expectSpelledAs(v, n, "ref_long", "macho");
    for (auto n : kUnsignedPointerAndMax) expectSpelledAs(v, n, "ref_unsigned_long", "macho");
}

// The same fact stated WITHOUT the reference vocabulary, so it survives even if the
// reference spellings were themselves wrong: on Darwin `uint64_t` and `uintmax_t`
// are DIFFERENT TYPES. They are both 8 bytes and both unsigned, so nothing but
// identity separates them — which is exactly what the defect erased.
TEST(StdintAliasIdentity, OnDarwinUint64AndUintmaxAreDifferentTypesAtTheSameWidth) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "stdint-alias-macho-split"};
    Vocabulary v{*cfg, dir.path(), "arm64", ObjectFormatKind::MachO, DataModel::Lp64};
    ASSERT_TRUE(v.clean());

    auto const u64  = v["uint64_t"];
    auto const umax = v["uintmax_t"];
    auto const i64  = v["int64_t"];
    auto const imax = v["intmax_t"];
    ASSERT_TRUE(u64.has_value());
    ASSERT_TRUE(umax.has_value());
    ASSERT_TRUE(i64.has_value());
    ASSERT_TRUE(imax.has_value());
    EXPECT_NE(*u64, *umax) << "Darwin's uint64_t is `unsigned long long` and its "
                              "uintmax_t is `unsigned long` — folding them is the defect";
    EXPECT_NE(*i64, *imax);
    EXPECT_EQ(v.interner().kind(*u64), TypeKind::U64);
    EXPECT_EQ(v.interner().kind(*umax), TypeKind::U64);
    EXPECT_EQ(v.interner().kind(*i64), TypeKind::I64);
    EXPECT_EQ(v.interner().kind(*imax), TypeKind::I64);
}

// ...and on elf they are the SAME type. The identical assertion with the opposite
// verdict, so neither direction can be satisfied by a blanket answer.
TEST(StdintAliasIdentity, OnElfUint64AndUintmaxAreTheSameType) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "stdint-alias-elf-same"};
    Vocabulary v{*cfg, dir.path(), "x86_64", ObjectFormatKind::Elf, DataModel::Lp64};
    ASSERT_TRUE(v.clean());

    auto const u64  = v["uint64_t"];
    auto const umax = v["uintmax_t"];
    ASSERT_TRUE(u64.has_value());
    ASSERT_TRUE(umax.has_value());
    EXPECT_EQ(*u64, *umax);
}

// Both Darwin arches answer identically — MEASURED with `-arch arm64` and
// `-arch x86_64` against the same SDK, byte-identical output. The arch axis
// participates in the `when` match, so this is a real question, not a tautology.
TEST(StdintAliasIdentity, TheTwoDarwinArchesAgree) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dirA{Location::Temp, "stdint-alias-macho-a"};
    ScratchDir dirX{Location::Temp, "stdint-alias-macho-x"};
    Vocabulary arm{*cfg, dirA.path(), "arm64", ObjectFormatKind::MachO, DataModel::Lp64};
    Vocabulary x86{*cfg, dirX.path(), "x86_64", ObjectFormatKind::MachO, DataModel::Lp64};
    ASSERT_TRUE(arm.clean());
    ASSERT_TRUE(x86.clean());

    // TypeIds are interner-local, so the two are compared through the reference
    // vocabulary each read into its OWN interner rather than against each other.
    for (auto n : kUnsignedExactWidthSix) {
        expectSpelledAs(arm, n, "ref_unsigned_long_long", "macho/arm64");
        expectSpelledAs(x86, n, "ref_unsigned_long_long", "macho/x86_64");
    }
    for (auto n : kUnsignedPointerAndMax) {
        expectSpelledAs(arm, n, "ref_unsigned_long", "macho/arm64");
        expectSpelledAs(x86, n, "ref_unsigned_long", "macho/x86_64");
    }
}

// ── pe / LLP64 — the second control ────────────────────────────────────────
TEST(StdintAliasIdentity, PeLlp64SpellsEveryAliasWithLongLong) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "stdint-alias-pe"};
    Vocabulary v{*cfg, dir.path(), "x86_64", ObjectFormatKind::Pe, DataModel::Llp64};
    ASSERT_TRUE(v.clean());

    for (auto n : kSignedExactWidthSix)   expectSpelledAs(v, n, "ref_long_long", "pe");
    for (auto n : kSignedPointerAndMax)   expectSpelledAs(v, n, "ref_long_long", "pe");
    for (auto n : kUnsignedExactWidthSix) expectSpelledAs(v, n, "ref_unsigned_long_long", "pe");
    for (auto n : kUnsignedPointerAndMax) expectSpelledAs(v, n, "ref_unsigned_long_long", "pe");
}

// ── the width never moved — this is an IDENTITY fix ────────────────────────
//
// Recorded as an assertion rather than as prose because it is the reason nothing
// miscompiled: every alias is an 8-byte core on all three platforms, before and
// after. A future edit that "fixes" a spelling by changing a core would trip here.
TEST(StdintAliasIdentity, EveryAliasIsAnEightByteCoreOnEveryPlatform) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    struct Leg { char const* arch; ObjectFormatKind fmt; DataModel model; char const* label; };
    Leg const legs[] = {
        {"x86_64", ObjectFormatKind::Elf,   DataModel::Lp64,  "elf/x86_64"},
        {"arm64",  ObjectFormatKind::Elf,   DataModel::Lp64,  "elf/arm64"},
        {"arm64",  ObjectFormatKind::MachO, DataModel::Lp64,  "macho/arm64"},
        {"x86_64", ObjectFormatKind::MachO, DataModel::Lp64,  "macho/x86_64"},
        {"x86_64", ObjectFormatKind::Pe,    DataModel::Llp64, "pe/x86_64"},
    };
    int leg = 0;
    for (auto const& l : legs) {
        ScratchDir dir{Location::Temp, std::string{"stdint-alias-width-"}
                                           + std::to_string(leg++)};
        Vocabulary v{*cfg, dir.path(), l.arch, l.fmt, l.model};
        ASSERT_TRUE(v.clean()) << l.label;
        for (auto n : kSignedExactWidthSix) {
            auto const t = v[n];
            ASSERT_TRUE(t.has_value()) << l.label << " " << n;
            EXPECT_EQ(v.interner().kind(*t), TypeKind::I64) << l.label << " " << n;
        }
        for (auto n : kSignedPointerAndMax) {
            auto const t = v[n];
            ASSERT_TRUE(t.has_value()) << l.label << " " << n;
            EXPECT_EQ(v.interner().kind(*t), TypeKind::I64) << l.label << " " << n;
        }
        for (auto n : kUnsignedExactWidthSix) {
            auto const t = v[n];
            ASSERT_TRUE(t.has_value()) << l.label << " " << n;
            EXPECT_EQ(v.interner().kind(*t), TypeKind::U64) << l.label << " " << n;
        }
        for (auto n : kUnsignedPointerAndMax) {
            auto const t = v[n];
            ASSERT_TRUE(t.has_value()) << l.label << " " << n;
            EXPECT_EQ(v.interner().kind(*t), TypeKind::U64) << l.label << " " << n;
        }
    }
}

// Every alias carries a vocabulary TAG on every platform — none of them is the
// anonymous 8-byte core, which is a THIRD type matching neither named C type.
TEST(StdintAliasIdentity, NoAliasInternsAsTheAnonymousCore) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dirE{Location::Temp, "stdint-alias-anon-elf"};
    ScratchDir dirM{Location::Temp, "stdint-alias-anon-macho"};
    ScratchDir dirP{Location::Temp, "stdint-alias-anon-pe"};
    Vocabulary e{*cfg, dirE.path(), "x86_64", ObjectFormatKind::Elf, DataModel::Lp64};
    Vocabulary m{*cfg, dirM.path(), "arm64", ObjectFormatKind::MachO, DataModel::Lp64};
    Vocabulary p{*cfg, dirP.path(), "x86_64", ObjectFormatKind::Pe, DataModel::Llp64};
    ASSERT_TRUE(e.clean());
    ASSERT_TRUE(m.clean());
    ASSERT_TRUE(p.clean());
    for (auto n : kUnsignedExactWidthSix) {
        expectNotAnonymous(e, n, "ref_anonymous_u64", "elf");
        expectNotAnonymous(m, n, "ref_anonymous_u64", "macho");
        expectNotAnonymous(p, n, "ref_anonymous_u64", "pe");
    }
    for (auto n : kSignedPointerAndMax) {
        expectNotAnonymous(e, n, "ref_anonymous_i64", "elf");
        expectNotAnonymous(m, n, "ref_anonymous_i64", "macho");
        expectNotAnonymous(p, n, "ref_anonymous_i64", "pe");
    }
}

// ── the sibling headers that carried the same key ──────────────────────────
//
// `size_t` / `ptrdiff_t` / `time_t` were on the SAME `dataModel`-only selector and
// are the reason the fix is not "flip stdint on macho": MEASURED, all three keep
// the elf spelling on Darwin. So the KEY moved for them and the VALUE did not,
// and that has to be asserted or the next reader will assume they moved too.
TEST(StdintAliasIdentity, SizeAndPtrdiffAndTimeKeepTheElfSpellingOnDarwin) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "stdint-alias-siblings-macho"};
    Vocabulary v{*cfg, dir.path(), "arm64", ObjectFormatKind::MachO, DataModel::Lp64};
    v.alsoRead(*cfg / "shippedLibs" / "stddef.json", "arm64", ObjectFormatKind::MachO,
               DataModel::Lp64);
    v.alsoRead(*cfg / "shippedLibs" / "time.json", "arm64", ObjectFormatKind::MachO,
               DataModel::Lp64);
    v.alsoRead(*cfg / "shippedLibs" / "sys" / "types.json", "arm64",
               ObjectFormatKind::MachO, DataModel::Lp64);
    ASSERT_TRUE(v.clean());

    expectSpelledAs(v, "size_t",    "ref_unsigned_long", "macho");
    expectSpelledAs(v, "ptrdiff_t", "ref_long",          "macho");
    expectSpelledAs(v, "time_t",    "ref_long",          "macho");
    // ...and `size_t` is therefore NOT the same type as `uint64_t` on Darwin,
    // which is the single sentence this whole row is about.
    auto const sz  = v["size_t"];
    auto const u64 = v["uint64_t"];
    ASSERT_TRUE(sz.has_value());
    ASSERT_TRUE(u64.has_value());
    EXPECT_NE(*sz, *u64);
}

TEST(StdintAliasIdentity, SizeAndPtrdiffAndTimeAreUnchangedOnElfAndPe) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dirE{Location::Temp, "stdint-alias-siblings-elf"};
    Vocabulary e{*cfg, dirE.path(), "x86_64", ObjectFormatKind::Elf, DataModel::Lp64};
    e.alsoRead(*cfg / "shippedLibs" / "stddef.json", "x86_64", ObjectFormatKind::Elf,
               DataModel::Lp64);
    e.alsoRead(*cfg / "shippedLibs" / "time.json", "x86_64", ObjectFormatKind::Elf,
               DataModel::Lp64);
    ASSERT_TRUE(e.clean());
    expectSpelledAs(e, "size_t",    "ref_unsigned_long", "elf");
    expectSpelledAs(e, "ptrdiff_t", "ref_long",          "elf");
    expectSpelledAs(e, "time_t",    "ref_long",          "elf");
    // On elf `size_t` and `uint64_t` ARE the same type — the opposite verdict to
    // the Darwin leg above, from the same two lookups.
    auto const sz  = e["size_t"];
    auto const u64 = e["uint64_t"];
    ASSERT_TRUE(sz.has_value());
    ASSERT_TRUE(u64.has_value());
    EXPECT_EQ(*sz, *u64);

    ScratchDir dirP{Location::Temp, "stdint-alias-siblings-pe"};
    Vocabulary p{*cfg, dirP.path(), "x86_64", ObjectFormatKind::Pe, DataModel::Llp64};
    p.alsoRead(*cfg / "shippedLibs" / "stddef.json", "x86_64", ObjectFormatKind::Pe,
               DataModel::Llp64);
    p.alsoRead(*cfg / "shippedLibs" / "time.json", "x86_64", ObjectFormatKind::Pe,
               DataModel::Llp64);
    ASSERT_TRUE(p.clean());
    expectSpelledAs(p, "size_t",    "ref_unsigned_long_long", "pe");
    expectSpelledAs(p, "ptrdiff_t", "ref_long_long",          "pe");
    expectSpelledAs(p, "time_t",    "ref_long_long",          "pe");
}

// ── the fail-loud properties this row had to PRESERVE ──────────────────────
//
// D-FFI-STDINT-PTR-WIDTH-ILP32 stays open, and the reason no ILP32 arm is declared
// is that an ILP32 target needs 4-byte intptr/ptrdiff/size widths: 0 variants match
// there and a reference fails LOUD as an undefined type. Adding the `format` axis
// must not have introduced a catch-all that turns that loud failure into a silent
// wrong width — so it is asserted, on every format.
TEST(StdintAliasIdentity, NoArmMatchesIlp32OnAnyFormat) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ObjectFormatKind const formats[] = {ObjectFormatKind::Elf, ObjectFormatKind::Pe,
                                        ObjectFormatKind::MachO};
    int i = 0;
    for (auto fmt : formats) {
        ScratchDir dir{Location::Temp,
                       std::string{"stdint-alias-ilp32-"} + std::to_string(i++)};
        Vocabulary v{*cfg, dir.path(), "x86_64", fmt, DataModel::Ilp32};
        ASSERT_TRUE(v.clean()) << "an ILP32 read must be CLEAN and EMPTY, not an error";
        for (auto n : kUnsignedExactWidthSix)
            EXPECT_FALSE(v[n].has_value())
                << "ILP32 must match 0 variants for '" << n
                << "' so the reference fails loud as an undefined type";
        for (auto n : kUnsignedPointerAndMax)
            EXPECT_FALSE(v[n].has_value()) << n;
    }
}

// A format DSS declares but has never measured these spellings for must also match
// nothing, rather than inheriting glibc's answer. `wasm` is ILP32 today, so `spirv`
// is not a second spelling of the test above only because the pair (LP64, wasm) is
// asked for here deliberately — the question is whether the FORMAT axis is
// enumerated, not whether the model is.
TEST(StdintAliasIdentity, AnUnmeasuredFormatInheritsNothing) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "stdint-alias-unmeasured-format"};
    Vocabulary v{*cfg, dir.path(), "x86_64", ObjectFormatKind::Wasm, DataModel::Lp64};
    ASSERT_TRUE(v.clean());
    for (auto n : kUnsignedExactWidthSix)
        EXPECT_FALSE(v[n].has_value())
            << "'" << n << "' must not silently inherit an elf spelling on an "
               "unenumerated object format";
    for (auto n : kSignedPointerAndMax) EXPECT_FALSE(v[n].has_value()) << n;
}

// ── RED-ON-DISABLE — REMOVE-direction mutants on a COPY ────────────────────

// Deleting the whole macho arm makes the alias VANISH on Darwin — the reader
// injects nothing when 0 variants match — while elf is untouched. The elf half is
// the control: it proves the mutant was the macho arm and not the file.
TEST(StdintAliasIdentity, DeletingTheMachoArmLeavesUint64Uninjected) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "stdint-alias-mutant-drop-macho"};
    fs::path const tree = copyConfigTree(dir, *cfg);
    fs::path const stdintJson = tree / "shippedLibs" / "stdint.json";

    nlohmann::json doc = readJson(stdintJson);
    std::size_t removed = 0;
    for (auto& td : doc.at("typedefs")) {
        if (!td.contains("variants")) continue;
        auto& arms = td.at("variants");
        for (auto it = arms.begin(); it != arms.end();) {
            auto const& w = it->at("when");
            if (w.contains("format") && w.at("format") == "macho") {
                it = arms.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
    }
    ASSERT_EQ(removed, 10u) << "the mutant must remove exactly the ten macho arms — "
                               "a different count means the file moved under it";
    writeJson(stdintJson, doc);

    ScratchDir dirM{Location::Temp, "stdint-alias-mutant-drop-macho-read"};
    Vocabulary m{tree, dirM.path(), "arm64", ObjectFormatKind::MachO, DataModel::Lp64};
    ASSERT_TRUE(m.clean());
    for (auto n : kUnsignedExactWidthSix)
        EXPECT_FALSE(m[n].has_value())
            << "'" << n << "' survived the deletion of its only macho arm";

    ScratchDir dirE{Location::Temp, "stdint-alias-mutant-drop-macho-control"};
    Vocabulary e{tree, dirE.path(), "x86_64", ObjectFormatKind::Elf, DataModel::Lp64};
    ASSERT_TRUE(e.clean());
    expectSpelledAs(e, "uint64_t", "ref_unsigned_long",
                    "elf CONTROL under the macho mutant");
}

// ★ THE PIN ON THE ROW ITSELF. Deleting only the `"format"` keys restores the exact
// pre-row selector shape — `{dataModel:LP64}` twice over — and the two LP64 arms
// then BOTH match on elf and on macho. The reader refuses an ambiguous per-target
// typedef, so the reverted shape now fails LOUD where it used to silently answer
// `unsigned long` on Darwin. If this test ever passes without the diagnostic, the
// `format` axis has stopped participating in the match.
TEST(StdintAliasIdentity, DeletingTheFormatKeysRestoresTheAmbiguityTheRowRemoved) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "stdint-alias-mutant-drop-format-key"};
    fs::path const tree = copyConfigTree(dir, *cfg);
    fs::path const stdintJson = tree / "shippedLibs" / "stdint.json";

    nlohmann::json doc = readJson(stdintJson);
    std::size_t stripped = 0;
    for (auto& td : doc.at("typedefs")) {
        if (!td.contains("variants")) continue;
        for (auto& arm : td.at("variants")) {
            if (arm.at("when").erase("format") != 0) ++stripped;
        }
    }
    ASSERT_EQ(stripped, 30u) << "ten typedefs x three arms carry the key this row added";
    writeJson(stdintJson, doc);

    TypeInterner       interner{CompilationUnitId{1}};
    TypeRegistry       typeReg;
    DiagnosticReporter rep;
    auto const desc = readShippedLibDescriptor(stdintJson, interner, typeReg, rep,
                                               DataModel::Lp64, std::string_view{"arm64"},
                                               ObjectFormatKind::MachO);
    EXPECT_TRUE(rep.hasErrors())
        << "with the format keys gone, two LP64 arms match on Darwin and the read "
           "must be REFUSED — a clean read means the ambiguity is being resolved "
           "silently, which is the defect this row closed";
}

// The mutant machinery's own control: an UNMUTATED copy of the tree answers exactly
// as the live tree does, so a red above is the mutation and not the copy.
TEST(StdintAliasIdentity, AnUnmutatedCopyOfTheTreeAnswersIdentically) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir dir{Location::Temp, "stdint-alias-copy-control"};
    fs::path const tree = copyConfigTree(dir, *cfg);

    ScratchDir dirM{Location::Temp, "stdint-alias-copy-control-read"};
    Vocabulary m{tree, dirM.path(), "arm64", ObjectFormatKind::MachO, DataModel::Lp64};
    ASSERT_TRUE(m.clean());
    expectSpelledAs(m, "uint64_t",  "ref_unsigned_long_long", "macho COPY");
    expectSpelledAs(m, "uintmax_t", "ref_unsigned_long",      "macho COPY");
}
