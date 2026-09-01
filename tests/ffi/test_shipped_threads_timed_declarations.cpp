// D-CSUBSET-C11-THREADS-TIMED (P49 lane tw) — the FOUR C11 <threads.h> functions
// that were declared on NO object format, and how each of the three formats
// declares them now.
//
// ── THE DEFECT ──────────────────────────────────────────────────────────────
// `thrd_sleep`, `mtx_timedlock`, `cnd_timedwait` and `thrd_equal` were absent
// from `threads.json` on every format but one (`thrd_sleep` carried an elf entry
// and nothing else), while `__STDC_NO_THREADS__` is deliberately defined NOWHERE
// — c.lang.json removed it when the header was completed. A conforming
// implementation that does not define that macro promises the WHOLE header
// (C11 6.10.8.3 / C23 6.10.9.3), so every leg was short four functions with
// nothing saying so.
//
// ── WHY THIS PIN IS NOT REDUNDANT WITH THE CORPUS WITNESSES ─────────────────
// `examples/c/c11_threads_timed` and `examples/c/c11_thrd_equal` prove the
// BEHAVIOUR, and they are the stronger instrument — a timed wait that returns
// immediately is invisible to any static check. But an example only RUNS on the
// host whose format it targets: the pe arms run on Windows, the macho arms on a
// Mac, the elf arms under WSL or qemu. This pin asserts all THREE formats'
// declarations from ANY host, so a config edit that drops the macho arms reds on
// the Windows leg instead of waiting for someone to reach a Mac.
//
// ── WHAT IS ASSERTED, AND WHY EACH PART EXISTS ──────────────────────────────
//  (1) All four names are declared on elf, pe AND macho, for both arches.
//  (2) The pe and macho rows carry a `synthesize` tag EQUAL to the name, and the
//      elf rows carry NONE. That split is the whole architecture: neither
//      kernel32/ucrtbase nor libSystem exports a C11 `thrd_*`, so those two
//      formats get a compiler-synthesized body, while glibc exports all four and
//      elf binds them directly. ⚠ AND THE TAG IS LOAD-BEARING FOR LOADABILITY,
//      not merely for correctness: DSS eagerly imports every UNTAGGED shipped
//      extern, so dropping the tag turns the row into an import of a name
//      kernel32 does not export and the produced .exe dies at LOAD with
//      STATUS_ENTRYPOINT_NOT_FOUND — ✔MEASURED as this row's mutant B below.
//  (3) The SIGNATURES, by interned identity against a reference vocabulary this
//      file owns. `thrd_equal` takes thrd_t BY VALUE and thrd_t is `ptr<void>` (a
//      HANDLE) on pe against `u64` on elf/macho, so its pe row MUST differ — the
//      same per-format signature divergence thrd_current/thrd_detach/thrd_join
//      already carry, and getting it wrong is an ABI mismatch rather than a
//      compile error.
//  (4) Every `synthesize` id is in the CLOSED recipe vocabulary and is partitioned
//      to the Threads family — the loader's guard and the synth pass's switch read
//      the same table, so an id admitted by one and invisible to the other is
//      inexpressible.
//
// ── RED-ON-DISABLE, AND WHY THE MUTANTS DELETE JSON ─────────────────────────
// A config change has a specific trap: an ADD-direction fixture stays GREEN when
// the real config LOSES the feature. Both mutants here therefore REMOVE from a
// COPY of the shipped tree — one deletes the pe ROWS, the other deletes only the
// `synthesize` KEY — and each reports how many arms it touched, so a mutant that
// silently matched nothing cannot pass for a mutant that worked. `src/dss-config`
// is never written.
//
// ⚠ EVERY MUTANT GETS ITS OWN SCRATCH PATH — the per-root corpus index is
// memoized process-wide with no staleness check, so mutating one tree in place
// and re-asking is answered from the PRE-mutation index (the trap recorded in
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

// The four functions this row shipped, in the order the registry names them.
constexpr std::array<std::string_view, 4> kTimedNames{
    "thrd_sleep", "mtx_timedlock", "cnd_timedwait", "thrd_equal"};

// The three shipped signature SHAPES, spelled INDEPENDENTLY of threads.json so an
// assertion compares the shipped answer against something this file owns rather
// than against itself. Pointer-wrapped because a `typedefs` entry names a TYPE and
// a bare function type is not one — `interner.pointer(symbol.signature)` is what
// each is compared against.
constexpr char kReferenceVocabulary[] = R"({
  "$comment": "P49 lane tw — reference signature vocabulary for the C11 timed-wait declarations. Not shipped; written to a scratch directory by the test.",
  "header": "dss-test-threads-timed-reference-vocabulary.h",
  "typedefs": [
    { "name": "ref_fn_two_pointers",   "type": "ptr<fn(ptr<void>, ptr<void>) -> i32>" },
    { "name": "ref_fn_three_pointers", "type": "ptr<fn(ptr<void>, ptr<void>, ptr<void>) -> i32>" },
    { "name": "ref_fn_two_u64",        "type": "ptr<fn(u64, u64) -> i32>" }
  ]
})";

// The data model a format really is: pe is LLP64 on every shipped pe target,
// elf and macho LP64 on every shipped one. Passing the wrong pair asks about a
// target that does not exist and reads an arm no compile could select.
[[nodiscard]] DataModel modelFor(ObjectFormatKind fmt) {
    return fmt == ObjectFormatKind::Pe ? DataModel::Llp64 : DataModel::Lp64;
}

// One (arch, format) view of `threads.json`, read into an interner shared with the
// reference vocabulary so every TypeId is comparable.
class Reading {
public:
    Reading(fs::path const& cfgRoot, fs::path const& scratch, std::string_view arch,
            ObjectFormatKind fmt)
        : interner_{CompilationUnitId{1}} {
        fs::path const refPath = scratch / "dss_threads_timed_reference_vocabulary.json";
        std::ofstream(refPath, std::ios::binary) << kReferenceVocabulary;
        add(refPath, arch, fmt);
        add(cfgRoot / "shippedLibs" / fs::path{std::string{kThreadsJson}}, arch, fmt);
    }

    [[nodiscard]] bool clean() const { return ok_ && !reporter_.hasErrors(); }
    [[nodiscard]] TypeInterner& interner() { return interner_; }

    // Every row in the SHIPPED descriptor (index 1) carrying `name`, whatever
    // format it is gated to.
    //
    // ⚠ THE READER DOES NOT FILTER BY THE ACTIVE FORMAT, AND THIS TEST WAS WRITTEN
    // BELIEVING IT DID — measured when the first run reported three rows where the
    // assertion expected one. `readShippedLibDescriptor` returns EVERY arm of a
    // same-name triple and the per-format gate is applied later, at semantic
    // injection (see `ShippedSymbol::availableObjectFormats`, which says so). That
    // makes the honest pin STRONGER than the one first written: assert the triple
    // is COMPLETE (three arms) and that EXACTLY ONE of them claims each format.
    [[nodiscard]] std::vector<ffi::ShippedSymbol const*> rowsNamed(std::string_view n) const {
        std::vector<ffi::ShippedSymbol const*> out;
        if (descs_.size() < 2) return out;
        for (auto const& s : descs_[1].symbols)
            if (s.name == n) out.push_back(&s);
        return out;
    }

    // The rows named `n` that DECLARE themselves available on `formatName`. Exactly
    // one must, on every format, for every one of the four functions.
    [[nodiscard]] std::vector<ffi::ShippedSymbol const*>
    rowsFor(std::string_view n, std::string_view formatName) const {
        std::vector<ffi::ShippedSymbol const*> out;
        for (auto const* s : rowsNamed(n))
            for (auto const& f : s->availableObjectFormats)
                if (f == formatName) { out.push_back(s); break; }
        return out;
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

// Assert one function's whole declaration for one format: present exactly once,
// tagged (or not) as the format's mechanism requires, and carrying the signature
// that format's thrd_t width demands.
void expectDeclared(Reading& r, std::string_view name, std::string_view formatName,
                    bool expectSynthesized, std::string_view expectedShape,
                    std::size_t expectedArms, char const* where) {
    EXPECT_EQ(r.rowsNamed(name).size(), expectedArms)
        << where << ": `" << name << "` should carry " << expectedArms << " arm(s). "
        << "The shipped tree carries a COMPLETE TRIPLE — one per object format — "
        << "because __STDC_NO_THREADS__ is defined nowhere, which promises the whole "
        << "header on every leg; a row-deleting mutant leaves two, and asserting the "
        << "count under the mutant is what keeps its blast radius honest";
    auto const rows = r.rowsFor(name, formatName);
    ASSERT_EQ(rows.size(), 1u)
        << where << ": exactly one arm of `" << name << "` must claim this format — "
        << "0 leaves the leg short the function, and 2+ would inject two "
        << "declarations of one name";

    if (expectSynthesized) {
        EXPECT_EQ(rows[0]->synthesize, std::string{name})
            << where << ": `" << name << "` must carry a `synthesize` tag equal to "
            << "its own name. Losing it does not merely lose the recipe — the row "
            << "becomes an eager import of a name the platform library does not "
            << "export, and EVERY binary built against this descriptor fails to "
            << "LOAD (D-FFI-DESCRIPTOR-EAGER-IMPORT)";
        EXPECT_TRUE(ffi::isKnownSynthesizeRecipe(rows[0]->synthesize))
            << where << ": `" << name << "`'s recipe id is not in the closed "
            << "vocabulary the loader guards with";
        auto const family = ffi::shimFamilyOf(rows[0]->synthesize);
        ASSERT_TRUE(family.has_value()) << where << ": `" << name << "` has no family";
        EXPECT_EQ(*family, ffi::ShimFamily::Threads)
            << where << ": `" << name << "` must partition to the Threads synth pass";
    } else {
        EXPECT_TRUE(rows[0]->synthesize.empty())
            << where << ": `" << name << "` is a DIRECT libc import on this format "
            << "(glibc exports all four; thrd_sleep is a WEAK definition, which the "
            << "eager-import law permits) — a tag here would suppress the import and "
            << "synthesize a body over the wrong primitive family";
    }

    auto const shape = r.reference(expectedShape);
    ASSERT_TRUE(shape.has_value()) << "reference vocabulary lost '" << expectedShape << "'";
    EXPECT_EQ(r.interner().pointer(rows[0]->signature), *shape)
        << where << ": `" << name << "` should have the shape '" << expectedShape
        << "'. thrd_t is passed BY VALUE and is ptr<void> (a HANDLE) on pe against "
        << "u64 on elf/macho, so a wrong shape here is an ABI mismatch that compiles";
}

// The shape each function has on a given format. Only `thrd_equal` diverges.
[[nodiscard]] std::string_view shapeOf(std::string_view name, ObjectFormatKind fmt) {
    if (name == "cnd_timedwait") return "ref_fn_three_pointers";
    if (name == "thrd_equal")
        return fmt == ObjectFormatKind::Pe ? "ref_fn_two_pointers" : "ref_fn_two_u64";
    return "ref_fn_two_pointers";
}

// The descriptor's own spelling of a format, as `availableObjectFormats` carries it.
[[nodiscard]] std::string_view formatNameOf(ObjectFormatKind fmt) {
    switch (fmt) {
        case ObjectFormatKind::Pe:    return "pe";
        case ObjectFormatKind::MachO: return "macho";
        default:                      return "elf";
    }
}

void expectWholeFormat(fs::path const& cfgRoot, ObjectFormatKind fmt, char const* where,
                       std::size_t expectedArms = 3u) {
    bool const             synthesized = fmt != ObjectFormatKind::Elf;
    std::string_view const formatName  = formatNameOf(fmt);
    for (std::string_view arch : {"x86_64", "arm64"}) {
        SCOPED_TRACE(std::string{where} + "/" + std::string{arch});
        ScratchDir dir{Location::Temp, "threads-timed"};
        Reading    r{cfgRoot, dir.path(), arch, fmt};
        ASSERT_TRUE(r.clean()) << where << ": the descriptor did not read cleanly";
        for (auto const name : kTimedNames)
            expectDeclared(r, name, formatName, synthesized, shapeOf(name, fmt),
                           expectedArms, where);
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

[[nodiscard]] bool isTimedName(nlohmann::json const& row) {
    if (!row.contains("name")) return false;
    for (auto const n : kTimedNames)
        if (row.at("name") == n) return true;
    return false;
}

[[nodiscard]] bool gatedTo(nlohmann::json const& row, char const* fmt) {
    if (!row.contains("availableObjectFormats")) return false;
    for (auto const& f : row.at("availableObjectFormats"))
        if (f == fmt) return true;
    return false;
}

// MUTANT A — DELETE the four rows gated to `fmt` from a COPIED tree. Returns how
// many rows it removed.
[[nodiscard]] std::size_t dropTimedRowsFor(fs::path const& treeRoot, char const* fmt) {
    fs::path const p = treeRoot / "shippedLibs" / fs::path{std::string{kThreadsJson}};
    auto            doc = readJson(p);
    auto            kept = nlohmann::json::array();
    std::size_t     removed = 0;
    for (auto const& row : doc.at("symbols")) {
        if (isTimedName(row) && gatedTo(row, fmt)) { ++removed; continue; }
        kept.push_back(row);
    }
    doc["symbols"] = kept;
    writeJson(p, doc);
    return removed;
}

// MUTANT B — DELETE only the `synthesize` KEY from the four rows gated to `fmt`,
// leaving the rows themselves. This is the eager-import breach: the row survives
// as a plain external import of a name the platform library does not export.
[[nodiscard]] std::size_t dropSynthesizeKeyFor(fs::path const& treeRoot, char const* fmt) {
    fs::path const p = treeRoot / "shippedLibs" / fs::path{std::string{kThreadsJson}};
    auto            doc = readJson(p);
    std::size_t     removed = 0;
    for (auto& row : doc.at("symbols")) {
        if (!isTimedName(row) || !gatedTo(row, fmt)) continue;
        if (row.erase("synthesize") != 0) ++removed;
    }
    writeJson(p, doc);
    return removed;
}

} // namespace

// ── elf — THE CONTROL THAT MATTERS MOST ────────────────────────────────────
//
// glibc genuinely exports all four (`nm -D --defined-only libc.so.6`, glibc 2.39,
// on BOTH elf legs — x86_64 under WSL and aarch64 on the native arm64 VPS:
// thrd_equal@@GLIBC_2.28 and thrd_sleep@@GLIBC_2.28 (WEAK), mtx_timedlock and
// cnd_timedwait @@GLIBC_2.34). ⚠ `readelf --dyn-syms` TRUNCATES long names and
// answered NOT FOUND for `mtx_timedlock` on a libc that exports it; `nm -D` is the
// instrument that does not lie here. The elf arms are plain FFI and must stay so.
TEST(ShippedThreadsTimed, ElfDeclaresAllFourAsDirectImports) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    expectWholeFormat(*cfg, ObjectFormatKind::Elf, "elf");
}

// pe — kernel32/ucrtbase export no `thrd_*` at all, so all four are synthesized.
// `thrd_equal` is the one whose SIGNATURE diverges: pe's thrd_t is a HANDLE.
TEST(ShippedThreadsTimed, PeDeclaresAllFourAsSynthesizedRecipes) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    expectWholeFormat(*cfg, ObjectFormatKind::Pe, "pe");
}

// macho — libSystem exports no C11 `thrd_*` either. ✔MEASURED by LINK PROBE on
// macOS 26.6.2 / Apple clang 21, because `nm` and `dyld_info` CANNOT answer this:
// /usr/lib/libSystem.B.dylib is not a file on disk there but a dyld-shared-cache
// entry, and both instruments report ZERO for every symbol including `_nanosleep`,
// which certainly exists. The linker is the oracle.
TEST(ShippedThreadsTimed, MachoDeclaresAllFourAsSynthesizedRecipes) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    expectWholeFormat(*cfg, ObjectFormatKind::MachO, "macho");
}

// The CONTROL for both mutants: an UNMUTATED copy of the shipped tree reads
// exactly like the shipped tree. Without it a mutant's red could be the COPY
// failing rather than the mutation biting.
TEST(ShippedThreadsTimed, AnUnmutatedCopyOfTheTreeReadsIdentically) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir     dir{Location::Temp, "threads-timed-copy"};
    fs::path const copy = copyConfigTree(dir, *cfg);
    expectWholeFormat(copy, ObjectFormatKind::Pe, "pe/unmutated-copy");
    expectWholeFormat(copy, ObjectFormatKind::Elf, "elf/unmutated-copy");
}

// MUTANT A, pe — the rows are GONE. Every one of the four must vanish from the pe
// reading, and the elf reading must be untouched: this row is additive per format,
// so a change that "fixed" pe by moving elf would be worse than the gap.
TEST(ShippedThreadsTimed, DroppingThePeRowsIsSeen) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir     dir{Location::Temp, "threads-timed-mutant-pe-rows"};
    fs::path const copy = copyConfigTree(dir, *cfg);
    ASSERT_EQ(dropTimedRowsFor(copy, "pe"), 4u)
        << "the mutant matched nothing — it would have passed for a working one";

    ScratchDir readDir{Location::Temp, "threads-timed-mutant-pe-rows-read"};
    Reading    pe{copy, readDir.path(), "x86_64", ObjectFormatKind::Pe};
    ASSERT_TRUE(pe.clean());
    for (auto const name : kTimedNames) {
        EXPECT_TRUE(pe.rowsFor(name, "pe").empty())
            << "`" << name << "` survived a mutant that deleted its pe row";
        EXPECT_EQ(pe.rowsNamed(name).size(), 2u)
            << "`" << name << "`: the mutant must remove ONE arm of the triple, "
            << "not more — a mutant with a wider blast radius proves less";
    }

    // The elf arm is the control and must be UNMOVED.
    expectWholeFormat(copy, ObjectFormatKind::Elf, "elf/control-under-pe-mutant",
                      /*expectedArms=*/2u);
}

// MUTANT B, pe — the rows SURVIVE but lose their `synthesize` key. This is the
// exact state that produced STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139) from a
// cleanly-compiled .exe while this row was being written: the descriptor still
// declares the name, DSS still eagerly imports it, and kernel32 does not export it.
TEST(ShippedThreadsTimed, DroppingOnlyTheSynthesizeKeyIsSeen) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir     dir{Location::Temp, "threads-timed-mutant-pe-key"};
    fs::path const copy = copyConfigTree(dir, *cfg);
    ASSERT_EQ(dropSynthesizeKeyFor(copy, "pe"), 4u)
        << "the mutant matched nothing — it would have passed for a working one";

    ScratchDir readDir{Location::Temp, "threads-timed-mutant-pe-key-read"};
    Reading    pe{copy, readDir.path(), "x86_64", ObjectFormatKind::Pe};
    ASSERT_TRUE(pe.clean());
    for (auto const name : kTimedNames) {
        auto const rows = pe.rowsFor(name, "pe");
        ASSERT_EQ(rows.size(), 1u) << name << ": mutant B must keep the row itself";
        EXPECT_TRUE(rows[0]->synthesize.empty())
            << "`" << name << "` still claims a recipe after its key was deleted — "
            << "the assertion in the positive tests would then be vacuous";
    }
}

// MUTANT A, macho — the same deletion on the OTHER synthesized format. Written
// separately rather than folded into the pe case because the two formats reach
// the synth pass through different `librarySynthesis` vehicles, and a mutant that
// only ever exercised pe would leave the macho arms pinned by nothing.
TEST(ShippedThreadsTimed, DroppingTheMachoRowsIsSeen) {
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value());
    ScratchDir     dir{Location::Temp, "threads-timed-mutant-macho-rows"};
    fs::path const copy = copyConfigTree(dir, *cfg);
    ASSERT_EQ(dropTimedRowsFor(copy, "macho"), 4u)
        << "the mutant matched nothing — it would have passed for a working one";

    ScratchDir readDir{Location::Temp, "threads-timed-mutant-macho-rows-read"};
    Reading    macho{copy, readDir.path(), "arm64", ObjectFormatKind::MachO};
    ASSERT_TRUE(macho.clean());
    for (auto const name : kTimedNames) {
        EXPECT_TRUE(macho.rowsFor(name, "macho").empty())
            << "`" << name << "` survived a mutant that deleted its macho row";
        EXPECT_EQ(macho.rowsNamed(name).size(), 2u)
            << "`" << name << "`: the mutant must remove ONE arm of the triple";
    }

    expectWholeFormat(copy, ObjectFormatKind::Pe, "pe/control-under-macho-mutant",
                      /*expectedArms=*/2u);
}
