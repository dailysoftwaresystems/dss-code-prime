// THE AMBIGUITY RULE BELONGS TO THE BLOCK (P69 round 4, lane lm; the review's NIT 14).
//
// A `variants` block whose two arms can both match one pair is ambiguous on every pair
// where both do. Every variant loop used to refuse it only when a READ matched two arms,
// so a read missing the fact one arm names — a format-less read (the LSP's language-only
// mode, the direct API), a direct-API read without the long-double axis — could not
// decide that arm (`Undecided`), and selected the OTHER, where a read carrying the fact
// refused both. The question needs no pair (`whensCanCoMatch`, core/types/variant_when),
// so every read now refuses such a block the same way.
//
// ✔MEASURED before the change: no shipped document holds such a block (533 `variants`
// arrays across src/dss-config, 201 with more than one guarded arm, 0 co-matching arm
// pairs) — the rule refuses nothing shipped; these pins hand the readers the block.
//
// RED-ON-DISABLE: `whensCanCoMatch` answering false (no two arms ever co-match) reds the
// refusal arms here — each reader goes back to selecting, skipping or refusing per read —
// while the disjoint control stays green.

#include "core/types/data_model.hpp"             // DataModel, LongDoubleFormat
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "core/types/variant_when.hpp"
#include "diagnostic_count.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
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

[[nodiscard]] fs::path writeTemp(ScratchDir const& dir, std::string const& name,
                                 std::string const& content) {
    fs::path const p = dir.path() / name;
    std::ofstream(p, std::ios::binary) << content;
    return p;
}

[[nodiscard]] bool anyDiagMentions(DiagnosticReporter const& rep, std::string_view needle) {
    for (auto const& d : rep.all())
        if (d.actual.find(needle) != std::string::npos) return true;
    return false;
}

[[nodiscard]] std::string diagText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) { out += "\n  "; out += d.actual; }
    return out.empty() ? std::string{"\n  <none>"} : out;
}

constexpr std::string_view kCoMatch = "can both match one pair";

WhenSpec spec(std::optional<std::string> arch, std::optional<ObjectFormatKind> format,
              std::optional<std::string> dataModel, std::optional<std::string> ldf) {
    WhenSpec s;
    s.arch             = std::move(arch);
    s.format           = format;
    s.dataModel        = std::move(dataModel);
    s.longDoubleFormat = std::move(ldf);
    return s;
}

// The four reads a descriptor meets: a full pair (x86_64 ELF with its long double), a
// full pair whose long double differs, the direct API's read without the long-double
// axis, and the language-only read with no format at all.
struct Read {
    char const*                     label;
    std::optional<ObjectFormatKind> format;
    std::optional<LongDoubleFormat> ldf;
};
std::array<Read, 4> const kReads{{
    {"x86_64 elf, x87-80", ObjectFormatKind::Elf, LongDoubleFormat::X87_80},
    {"x86_64 elf, ieee128", ObjectFormatKind::Elf, LongDoubleFormat::Ieee128},
    {"x86_64 elf, no long-double axis", ObjectFormatKind::Elf, std::nullopt},
    {"no format, no axis", std::nullopt, std::nullopt},
}};

struct ReadResult {
    bool                     clean = false;
    std::vector<std::string> symbols;
    std::vector<std::string> typedefs;
};

ReadResult readOn(fs::path const& path, Read const& r, DiagnosticReporter& rep) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    ShippedPairFacts facts;
    facts.dataModel = DataModel::Lp64;
    if (r.ldf.has_value()) facts.longDoubleFormat = *r.ldf;
    auto d = readShippedLibDescriptor(
        path, interner, typeReg, rep, DataModel::Lp64,
        r.format.has_value() ? std::optional<std::string_view>{"x86_64"} : std::nullopt,
        r.format, {}, nullptr, r.ldf.has_value() ? &facts : nullptr);
    ReadResult out;
    out.clean = d.has_value() && !rep.hasErrors();
    if (d.has_value()) {
        for (auto const& s : d->symbols) out.symbols.push_back(s.name);
        for (auto const& t : d->typedefs) out.typedefs.push_back(t.name);
    }
    return out;
}

} // namespace

// ── The predicate ─────────────────────────────────────────────────────────────

TEST(WhenArmsCoMatch, TheKeysBothArmsNameDecide) {
    auto const x86 = spec("x86_64", std::nullopt, std::nullopt, std::nullopt);
    auto const a64 = spec("aarch64", std::nullopt, std::nullopt, std::nullopt);
    auto const pe  = spec(std::nullopt, ObjectFormatKind::Pe, std::nullopt, std::nullopt);
    auto const x86x87 = spec("x86_64", std::nullopt, std::nullopt, "x87-80");
    auto const elfX87 = spec(std::nullopt, ObjectFormatKind::Elf, std::nullopt, "x87-80");
    auto const elfQ   = spec(std::nullopt, ObjectFormatKind::Elf, std::nullopt, "ieee128");

    EXPECT_FALSE(whensCanCoMatch(x86, a64, WhenAxes::FullTarget)) << "a shared key that differs";
    EXPECT_TRUE(whensCanCoMatch(x86, pe, WhenAxes::FullTarget))
        << "no shared key: x86_64 pe matches both";
    EXPECT_TRUE(whensCanCoMatch(x86, x86x87, WhenAxes::FullTarget))
        << "one arm names a key the other does not, and they agree on the one both name";
    EXPECT_FALSE(whensCanCoMatch(elfX87, elfQ, WhenAxes::FullTarget));
    EXPECT_TRUE(whensCanCoMatch(x86, x86, WhenAxes::FullTarget)) << "a duplicated arm";
    // Participation follows the mode: only `format` takes part outside FullTarget.
    auto const elfX86 = spec("x86_64", ObjectFormatKind::Elf, std::nullopt, std::nullopt);
    auto const elfA64 = spec("aarch64", ObjectFormatKind::Elf, std::nullopt, std::nullopt);
    EXPECT_FALSE(whensCanCoMatch(elfX86, elfA64, WhenAxes::FullTarget));
    EXPECT_TRUE(whensCanCoMatch(elfX86, elfA64, WhenAxes::FormatReachability))
        << "the arch does not participate in a format-only question";
}

// ── The review's case: a per-pair symbol string ───────────────────────────────

// Arm 0 names the format alone; arm 1 the format AND the long double. They co-match on
// every ELF pair whose long double is x87-80. Before the rule: the first read refused
// (two matches), the second and third SELECTED arm 0 (the third because it could not
// decide arm 1), the fourth left the symbol absent. Now all four refuse the same way.
TEST(WhenArmsCoMatch, ASymbolSignatureBlockIsRefusedOnEveryRead) {
    ScratchDir dir{Location::Temp, "variant-co-match-symbol"};
    auto const path = writeTemp(dir, "co.json", R"JSON({ "header": "co.h", "symbols": [
        { "name": "wide",
          "signature": { "variants": [
              { "when": { "format": "elf" }, "value": "fn() -> i64" },
              { "when": { "format": "elf", "longDoubleFormat": "x87-80" }, "value": "fn() -> f64" } ] } },
        { "name": "plain", "signature": "fn() -> i32" } ] })JSON");
    for (auto const& r : kReads) {
        SCOPED_TRACE(r.label);
        DiagnosticReporter rep;
        auto const res = readOn(path, r, rep);
        EXPECT_FALSE(res.clean) << "the block is ambiguous on some pair: every read refuses it";
        EXPECT_TRUE(anyDiagMentions(rep, kCoMatch)) << diagText(rep);
        EXPECT_TRUE(anyDiagMentions(rep, "symbols[0].signature.variants[0].when")) << diagText(rep);
    }
}

// CONTROL: the same two arms made disjoint on the key both name read clean, and each
// pair selects its own arm — the rule refuses an ambiguity, not a variants block.
TEST(WhenArmsCoMatch, ADisjointSymbolBlockReadsCleanOnEveryRead) {
    ScratchDir dir{Location::Temp, "variant-co-match-symbol-control"};
    auto const path = writeTemp(dir, "dis.json", R"JSON({ "header": "dis.h", "symbols": [
        { "name": "wide",
          "signature": { "variants": [
              { "when": { "format": "elf", "longDoubleFormat": "ieee128" }, "value": "fn() -> i64" },
              { "when": { "format": "elf", "longDoubleFormat": "x87-80" }, "value": "fn() -> f64" } ] } },
        { "name": "plain", "signature": "fn() -> i32" } ] })JSON");
    for (auto const& r : kReads) {
        SCOPED_TRACE(r.label);
        DiagnosticReporter rep;
        auto const res = readOn(path, r, rep);
        EXPECT_TRUE(res.clean) << diagText(rep);
        bool const decidable = r.format.has_value() && r.ldf.has_value();
        EXPECT_EQ(res.symbols, decidable ? (std::vector<std::string>{"wide", "plain"})
                                         : (std::vector<std::string>{"plain"}))
            << "a read that can decide selects; one that cannot leaves the symbol absent";
    }
}

// ── The other loops: a typedef block, and a macro block read without a format ─────

TEST(WhenArmsCoMatch, ATypedefBlockIsRefusedOnEveryRead) {
    ScratchDir dir{Location::Temp, "variant-co-match-typedef"};
    auto const path = writeTemp(dir, "td.json", R"JSON({ "header": "td.h", "typedefs": [
        { "name": "wide_t", "variants": [
            { "when": { "format": "elf" }, "type": "i64" },
            { "when": { "format": "elf", "longDoubleFormat": "x87-80" }, "type": "f64" } ] } ] })JSON");
    for (auto const& r : kReads) {
        SCOPED_TRACE(r.label);
        DiagnosticReporter rep;
        auto const res = readOn(path, r, rep);
        EXPECT_FALSE(res.clean) << diagText(rep);
        EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::F_ShippedTypedefVariantAmbiguous), 1u)
            << diagText(rep);
        EXPECT_TRUE(res.typedefs.empty()) << "an ambiguous typedef is never published";
    }
}

TEST(WhenArmsCoMatch, AConstantAndAStructBlockAreRefusedOnEveryRead) {
    ScratchDir dir{Location::Temp, "variant-co-match-constant-struct"};
    auto const path = writeTemp(dir, "cs.json", R"JSON({ "header": "cs.h",
        "constants": [
            { "name": "K", "variants": [
                { "when": { "arch": "x86_64" }, "value": 1, "type": "i32" },
                { "when": { "format": "elf" }, "value": 2, "type": "i32" } ] } ],
        "structs": [
            { "name": "S", "variants": [
                { "when": { "format": "elf" }, "fields": [ { "name": "x", "type": "i32" } ] },
                { "when": { "format": "elf", "longDoubleFormat": "ieee128" },
                  "fields": [ { "name": "y", "type": "i64" } ] } ] } ] })JSON");
    for (auto const& r : kReads) {
        SCOPED_TRACE(r.label);
        DiagnosticReporter rep;
        auto const res = readOn(path, r, rep);
        EXPECT_FALSE(res.clean) << diagText(rep);
        EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::F_ShippedConstantVariantAmbiguous), 1u)
            << "x86_64 ELF matches both constant arms; no read may select one" << diagText(rep);
        EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::F_ShippedStructVariantAmbiguous), 1u)
            << "an ieee128 ELF pair matches both struct arms" << diagText(rep);
    }
}

TEST(WhenArmsCoMatch, AMacroBlockIsRefusedWithoutAFormatToo) {
    ScratchDir dir{Location::Temp, "variant-co-match-macro"};
    auto const path = writeTemp(dir, "m.json", R"JSON({ "header": "m.h", "macros": [
        { "name": "X", "variants": [
            { "when": { "format": "elf" }, "replacement": "1" },
            { "when": { "format": "elf" }, "replacement": "2" } ] } ] })JSON");
    for (std::optional<ObjectFormatKind> const fmt :
         {std::optional<ObjectFormatKind>{ObjectFormatKind::Elf},
          std::optional<ObjectFormatKind>{ObjectFormatKind::Pe},
          std::optional<ObjectFormatKind>{}}) {
        SCOPED_TRACE(fmt.has_value() ? std::string{objectFormatKindName(*fmt)} : "no format");
        DiagnosticReporter rep;
        auto const macros = readShippedLibMacros(path, rep, fmt);
        EXPECT_FALSE(macros.has_value()) << "refused on a pe read and a format-less one too";
        EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::F_ShippedMacroVariantAmbiguous), 1u)
            << diagText(rep);
    }
}
