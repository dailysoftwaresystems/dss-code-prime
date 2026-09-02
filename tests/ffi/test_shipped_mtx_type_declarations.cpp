// D-CSUBSET-C11-THREADS-MACHO-MTX-PLAIN-RECURSIVE +
// D-CSUBSET-C11-THREADS-MTX-PLAIN-RECURSIVE — the DECLARED half of `mtx_init`'s
// `type` argument: that the parameter exists at all, on every format, and that the
// three `mtx_*` constants the synth pass masks against are what it believes.
//
// ── WHY THE DECLARATION IS LOAD-BEARING AND NOT MERELY TIDY ─────────────────
// The macho recipe now READS `mtx_init`'s second parameter (`builder.addArg(1,
// i32Ty)`) to choose a pthread mutex kind. If threads.json ever lost that `i32`
// from a row — narrowed it, or dropped it to `fn(ptr<void>) -> i32` — the shim
// would materialize an `Arg` for a parameter the declaration does not promise, and
// the mutex kind would come out of whatever happened to be in the second argument
// register. That compiles, links, loads and runs; it just builds the wrong kind of
// mutex, which on the recursive path is a HANG. Nothing else in the tree asserts
// this signature: the descriptor suite pins only that `mtx_init` partitions to the
// Threads recipe family.
//
// ── AND WHY THE CONSTANTS ARE PINNED HERE, WITH A BIT-NESS ASSERTION ────────
// `synth_threads_shim.cpp` masks the incoming type with the literal 1 and calls
// that "mtx_recursive". threads.json separately DECLARES `mtx_recursive = 1`. Two
// owners of one fact, and only one of them is config — so this asserts the shipped
// value equals what the pass masks with, and that it is a single BIT. The bit-ness
// is the assertion that carries the C11 semantics: 7.26.4.2 / C23 7.28.4.2 name
// FOUR legal type values, two of which (`mtx_plain | mtx_recursive` and
// `mtx_timed | mtx_recursive`) are ORs, so a recursive request can arrive as 1 OR
// as 3. A renumbering that made mtx_recursive non-single-bit would silently break
// the mask while every equality-shaped test stayed green.
// ⚠ The expected values are restated here from C11 and from the two reference
// implementations rather than read back out of the file under test — ✔MEASURED
// identical in glibc 2.39's <threads.h> (mtx_plain 0, mtx_recursive 1, mtx_timed 2,
// printed by a program built with gcc 13.3.0 and again with clang 18.1.3) and in
// MSVC 19.51's own <threads.h> (`mtx_plain = 0, mtx_recursive = 1 << 0,
// mtx_timed = 1 << 1`). A test that read the shipped number and compared it to
// itself would pass over any value at all.
// ⓘ NOTE the `thrd_*` status codes are NOT pinned against a reference, and that is
// deliberate rather than an omission: ✔MEASURED, glibc and MSVC DISAGREE on them
// (MSVC's thrd_error is 4, glibc's 2), C11 leaves the values implementation-
// defined, and DSS's shims return values consistent with DSS's own declarations.
// The `mtx_*` values are the ones all three agree on, so they are the ones an
// external reference can adjudicate.
//
// ── WHY THIS PIN IS NOT REDUNDANT WITH THE OTHER TWO INSTRUMENTS ────────────
// `examples/c/c11_mtx_recursive` proves the BEHAVIOUR but its macho arm runs only
// on a Mac. `tests/mir/test_synth_threads_mtx_type` proves the emitted MIR from any
// host but drives the pass with a hand-built caller, so it never touches
// threads.json. This is the only instrument that reads the SHIPPED declaration.
//
// ── RED-ON-DISABLE, AND WHY THE MUTANTS DELETE JSON ─────────────────────────
// A config change has a specific trap: an ADD-direction fixture stays GREEN when
// the real config LOSES the feature. Both mutants here therefore REMOVE from a
// COPY of the shipped tree — one strips the `i32` parameter from `mtx_init`'s
// signature, the other deletes the `mtx_recursive` constant row — and each reports
// how many arms it touched, so a mutant that silently matched nothing cannot pass
// for a mutant that worked. `src/dss-config` is never written.
//
// ⚠ EVERY MUTANT GETS ITS OWN SCRATCH PATH — the per-root corpus index is memoized
// process-wide with no staleness check, so mutating one tree in place and re-asking
// is answered from the PRE-mutation index (the trap recorded in
// `test_shipped_realization_oracle.cpp`'s header).

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
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

constexpr std::string_view kThreadsJson = "threads.json";

// The value the synth pass masks with, restated from C11 + both references.
constexpr std::int64_t kMtxPlain     = 0;
constexpr std::int64_t kMtxRecursive = 1;
constexpr std::int64_t kMtxTimed     = 2;

// `mtx_init`'s shape is the SAME on all three formats — unlike thrd_equal, whose
// pe arm diverges because thrd_t is a HANDLE there. `mtx_t` is passed by POINTER,
// so its per-format width (48/40/64) never reaches the signature; only the `type`
// does, and it is `int` everywhere. Spelled independently of threads.json so the
// comparison is against something this file owns.
constexpr char kReferenceVocabulary[] = R"({
  "$comment": "P51 lane mx — reference signature vocabulary for mtx_init. Not shipped; written to a scratch directory by the test.",
  "header": "dss-test-mtx-type-reference-vocabulary.h",
  "typedefs": [
    { "name": "ref_mtx_init",        "type": "ptr<fn(ptr<void>, i32) -> i32>" },
    { "name": "ref_mtx_init_no_type", "type": "ptr<fn(ptr<void>) -> i32>" }
  ]
})";

[[nodiscard]] DataModel modelFor(ObjectFormatKind fmt) {
    return fmt == ObjectFormatKind::Pe ? DataModel::Llp64 : DataModel::Lp64;
}

[[nodiscard]] std::string_view formatNameOf(ObjectFormatKind fmt) {
    switch (fmt) {
        case ObjectFormatKind::Pe:    return "pe";
        case ObjectFormatKind::MachO: return "macho";
        default:                      return "elf";
    }
}

// One (arch, format) view of threads.json, read into an interner shared with the
// reference vocabulary so every TypeId is comparable.
class Reading {
public:
    Reading(fs::path const& cfgRoot, fs::path const& scratch, std::string_view arch,
            ObjectFormatKind fmt)
        : interner_{CompilationUnitId{1}} {
        fs::path const refPath = scratch / "dss_mtx_type_reference_vocabulary.json";
        std::ofstream(refPath, std::ios::binary) << kReferenceVocabulary;
        add(refPath, arch, fmt);
        add(cfgRoot / "shippedLibs" / fs::path{std::string{kThreadsJson}}, arch, fmt);
    }

    [[nodiscard]] bool clean() const { return ok_ && !reporter_.hasErrors(); }
    [[nodiscard]] TypeInterner& interner() { return interner_; }

    // ⚠ The reader returns EVERY arm of a same-name triple; the per-format gate is
    // applied later, at semantic injection. So the honest pin asserts the triple is
    // COMPLETE and that exactly one arm claims each format.
    [[nodiscard]] std::vector<ffi::ShippedSymbol const*> rowsNamed(std::string_view n) const {
        std::vector<ffi::ShippedSymbol const*> out;
        if (descs_.size() < 2) return out;
        for (auto const& s : descs_[1].symbols)
            if (s.name == n) out.push_back(&s);
        return out;
    }

    [[nodiscard]] std::vector<ffi::ShippedSymbol const*>
    rowsFor(std::string_view n, std::string_view formatName) const {
        std::vector<ffi::ShippedSymbol const*> out;
        for (auto const* s : rowsNamed(n))
            for (auto const& f : s->availableObjectFormats)
                if (f == formatName) { out.push_back(s); break; }
        return out;
    }

    [[nodiscard]] std::optional<std::int64_t> constantNamed(std::string_view n) const {
        if (descs_.size() < 2) return std::nullopt;
        for (auto const& c : descs_[1].constants)
            if (c.name == n) return c.value;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<TypeId> reference(std::string_view name) const {
        for (auto const& d : descs_)
            for (auto const& t : d.typedefs)
                if (t.name == name) return t.type;
        return std::nullopt;
    }

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

void expectMtxInitOn(fs::path const& cfgRoot, ObjectFormatKind fmt, char const* where) {
    std::string_view const formatName  = formatNameOf(fmt);
    bool const             synthesized = fmt != ObjectFormatKind::Elf;
    for (std::string_view arch : {"x86_64", "arm64"}) {
        SCOPED_TRACE(std::string{where} + "/" + std::string{arch});
        ScratchDir dir{Location::Temp, "mtx-type"};
        Reading    r{cfgRoot, dir.path(), arch, fmt};
        ASSERT_TRUE(r.clean()) << where << ": the descriptor did not read cleanly";

        EXPECT_EQ(r.rowsNamed("mtx_init").size(), 3u)
            << where << ": `mtx_init` carries a COMPLETE TRIPLE — one arm per object "
            << "format — because __STDC_NO_THREADS__ is defined nowhere, which "
            << "promises the whole header on every leg";
        auto const rows = r.rowsFor("mtx_init", formatName);
        ASSERT_EQ(rows.size(), 1u)
            << where << ": exactly one arm of `mtx_init` must claim this format";

        // ★ THE TYPE PARAMETER EXISTS. This is the assertion the macho recipe now
        //   depends on: it materializes Arg 1 to pick the pthread mutex kind.
        auto const shape = r.reference("ref_mtx_init");
        ASSERT_TRUE(shape.has_value()) << "reference vocabulary lost 'ref_mtx_init'";
        EXPECT_EQ(r.interner().pointer(rows[0]->signature), *shape)
            << where << ": `mtx_init` must be `fn(ptr<void>, i32) -> i32` on every "
            << "format. Losing the `i32` leaves the synth pass reading a parameter "
            << "the declaration does not promise, and the mutex kind then comes out "
            << "of whatever occupies the second argument register — a wrong-kind "
            << "mutex that compiles, links and hangs";

        if (synthesized) {
            EXPECT_EQ(rows[0]->synthesize, std::string{"mtx_init"})
                << where << ": neither kernel32/ucrtbase nor libSystem exports a C11 "
                << "`mtx_init`, so this arm must be synthesized. Losing the tag turns "
                << "the row into an eager import of an absent name and EVERY binary "
                << "built against this descriptor fails to LOAD "
                << "(D-FFI-DESCRIPTOR-EAGER-IMPORT)";
            auto const family = ffi::shimFamilyOf(rows[0]->synthesize);
            ASSERT_TRUE(family.has_value()) << where << ": `mtx_init` has no family";
            EXPECT_EQ(*family, ffi::ShimFamily::Threads);
        } else {
            EXPECT_TRUE(rows[0]->synthesize.empty())
                << where << ": glibc exports mtx_init from libc.so.6 (and HONOURS "
                << "mtx_recursive — ✔MEASURED), so the elf arm is a direct import; a "
                << "tag here would synthesize a body over the wrong primitive family";
        }

        // ★ AND THE THREE CONSTANTS THE PASS MASKS AGAINST.
        EXPECT_EQ(r.constantNamed("mtx_plain"), std::optional<std::int64_t>{kMtxPlain})
            << where;
        EXPECT_EQ(r.constantNamed("mtx_recursive"),
                  std::optional<std::int64_t>{kMtxRecursive})
            << where << ": the macho recipe MASKS the incoming type with this exact "
            << "value; glibc and MSVC both declare it 1 (✔MEASURED)";
        EXPECT_EQ(r.constantNamed("mtx_timed"), std::optional<std::int64_t>{kMtxTimed})
            << where;
        // The bit-ness, which is the assertion that carries the C11 semantics: a
        // recursive request may arrive as `mtx_plain|mtx_recursive` (1) or as
        // `mtx_timed|mtx_recursive` (3), so the pass must mask and the value must be
        // one bit for masking to mean anything.
        auto const rec = r.constantNamed("mtx_recursive");
        ASSERT_TRUE(rec.has_value());
        EXPECT_NE(*rec, 0) << where;
        EXPECT_EQ(*rec & (*rec - 1), 0)
            << where << ": mtx_recursive must be a SINGLE BIT — C11 spells a "
            << "recursive mutex as an OR, so the synth pass masks with it rather "
            << "than comparing, and a non-single-bit value breaks the mask while "
            << "every equality-shaped assertion stays green";
    }
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

// MUTANT A — REMOVE the `type` parameter from every `mtx_init` row, on a COPY.
// The narrowing a careless edit would make, and the one the synth pass cannot
// survive. Returns how many rows it touched.
[[nodiscard]] std::size_t dropTypeParameter(fs::path const& treeRoot) {
    fs::path const p = treeRoot / "shippedLibs" / fs::path{std::string{kThreadsJson}};
    auto           doc = readJson(p);
    std::size_t    touched = 0;
    for (auto& row : doc.at("symbols")) {
        if (!row.contains("name") || row.at("name") != "mtx_init") continue;
        row["signature"] = "fn(ptr<void>) -> i32";
        ++touched;
    }
    writeJson(p, doc);
    return touched;
}

// MUTANT B — DELETE the `mtx_recursive` constant row entirely, on a COPY. The
// REMOVE direction: an ADD-direction fixture would stay green through exactly this.
[[nodiscard]] std::size_t dropRecursiveConstant(fs::path const& treeRoot) {
    fs::path const p = treeRoot / "shippedLibs" / fs::path{std::string{kThreadsJson}};
    auto           doc = readJson(p);
    auto           kept = nlohmann::json::array();
    std::size_t    removed = 0;
    for (auto const& row : doc.at("constants")) {
        if (row.contains("name") && row.at("name") == "mtx_recursive") { ++removed; continue; }
        kept.push_back(row);
    }
    doc["constants"] = kept;
    writeJson(p, doc);
    return removed;
}

} // namespace

// elf — a DIRECT libc import, and the arm that witnesses the reference behaviour:
// glibc 2.39 honours mtx_recursive (✔MEASURED through gcc 13.3.0 and clang 18.1.3,
// probed separately; the same-thread relock proceeds, and an mtx_plain one blocks).
TEST(ShippedMtxType, ElfDeclaresMtxInitWithItsTypeParameter) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    expectMtxInitOn(*cfg, ObjectFormatKind::Elf, "elf");
}

// pe — synthesized over kernel32. The type is accepted and deliberately not read
// (a CRITICAL_SECTION is always recursion-capable), but the PARAMETER must stay
// declared: it is part of the C11 interface a user program calls through.
TEST(ShippedMtxType, PeDeclaresMtxInitWithItsTypeParameter) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    expectMtxInitOn(*cfg, ObjectFormatKind::Pe, "pe");
}

// macho — synthesized over libSystem, and the arm that READS the parameter to pick
// a pthread mutex kind. This is the declaration the fix depends on.
TEST(ShippedMtxType, MachoDeclaresMtxInitWithItsTypeParameter) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    expectMtxInitOn(*cfg, ObjectFormatKind::MachO, "macho");
}

// The CONTROL for both mutants: an UNMUTATED copy reads exactly like the shipped
// tree. Without it a mutant's red could be the COPY failing rather than the
// mutation biting.
TEST(ShippedMtxType, AnUnmutatedCopyOfTheTreeReadsIdentically) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir     dir{Location::Temp, "mtx-type-copy"};
    fs::path const copy = copyConfigTree(dir, *cfg);
    expectMtxInitOn(copy, ObjectFormatKind::MachO, "macho/unmutated-copy");
    expectMtxInitOn(copy, ObjectFormatKind::Elf, "elf/unmutated-copy");
}

// MUTANT A — the `type` parameter is gone from every arm.
TEST(ShippedMtxType, NarrowingMtxInitAwayFromItsTypeParameterIsSeen) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir     dir{Location::Temp, "mtx-type-mutant-signature"};
    fs::path const copy = copyConfigTree(dir, *cfg);
    ASSERT_EQ(dropTypeParameter(copy), 3u)
        << "the mutant matched nothing — it would have passed for a working one";

    ScratchDir readDir{Location::Temp, "mtx-type-mutant-signature-read"};
    Reading    macho{copy, readDir.path(), "arm64", ObjectFormatKind::MachO};
    ASSERT_TRUE(macho.clean());
    auto const rows = macho.rowsFor("mtx_init", "macho");
    ASSERT_EQ(rows.size(), 1u);
    auto const withType = macho.reference("ref_mtx_init");
    auto const without  = macho.reference("ref_mtx_init_no_type");
    ASSERT_TRUE(withType.has_value() && without.has_value());
    EXPECT_NE(macho.interner().pointer(rows[0]->signature), *withType)
        << "the mutant did not move the signature the shipped tree declares";
    EXPECT_EQ(macho.interner().pointer(rows[0]->signature), *without)
        << "the mutant should leave exactly the narrowed shape, so its blast radius "
        << "is known rather than merely nonzero";
}

// MUTANT B — the constant the synth pass masks against is gone.
TEST(ShippedMtxType, DroppingTheMtxRecursiveConstantIsSeen) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir     dir{Location::Temp, "mtx-type-mutant-constant"};
    fs::path const copy = copyConfigTree(dir, *cfg);
    ASSERT_EQ(dropRecursiveConstant(copy), 1u)
        << "the mutant matched nothing — it would have passed for a working one";

    ScratchDir readDir{Location::Temp, "mtx-type-mutant-constant-read"};
    Reading    macho{copy, readDir.path(), "arm64", ObjectFormatKind::MachO};
    ASSERT_TRUE(macho.clean());
    EXPECT_FALSE(macho.constantNamed("mtx_recursive").has_value())
        << "`mtx_recursive` survived a mutant that deleted its row";
    // The siblings are the control: this mutant must remove ONE constant, not the
    // block. A wider blast radius would prove less.
    EXPECT_EQ(macho.constantNamed("mtx_plain"), std::optional<std::int64_t>{kMtxPlain});
    EXPECT_EQ(macho.constantNamed("mtx_timed"), std::optional<std::int64_t>{kMtxTimed});
}
