// D-CONFIG-DESCRIPTOR-LIBRARY-LITERAL-DUPLICATES-THE-FORMAT-ROLE-TABLE — THE
// DECODER'S HALF: a `library` entry that names a runtime ROLE instead of an
// image, resolved at descriptor decode through `RuntimeLibraryRoleResolver`.
//
// WHAT IS PINNED HERE, AND WHY IT IS PINNED WITH A HAND-BUILT TABLE. The role
// form exists so the image is written ONCE, in the object format's
// `runtimeLibraries` table, and every consumer of the decoded `library` map
// keeps reading plain strings. The seam between the two tiers is a core
// interface (`RuntimeLibraryRoleResolver`), and this file drives it with a
// `RuntimeLibraryTable` built by hand — a CORE type, no format document loaded —
// so every verdict below is about the DECODER and nothing else: the shipped
// corpus, the format loader and the driver each have their own witness
// (`tests/link/test_descriptor_library_role_agreement.cpp`,
// `tests/program/test_descriptor_role_follows_the_table.cpp`).
//
// The refusals split into two classes, and the split is load-bearing:
//   * FORMAT-INDEPENDENT (an unknown role, the `none` sentinel, an image beside
//     a role, an unknown key, a missing or non-string `role`, a value that is
//     neither a string nor an object) — refused with NO resolver in hand, so an
//     arm no current target selects cannot rot.
//   * RESOLUTION (a role no document of the family declares, a role the family
//     REALIZES from a shipped source, a family that cannot be assembled) —
//     refused only with a resolver in hand, because without one the caller has
//     declared it binds no import and a recorded, unresolved entry IS the
//     stated UNBOUND arm (✔MEASURED: ≥40 test call sites in 17 files pass an
//     `activeFormat` with no table, most reading the shipped corpus).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"

#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace dss;
using dss::ffi::readShippedLibDescriptor;
using dss::ffi::realizeShippedExternSymbols;
using dss::ffi::ShippedLibDescriptor;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] fs::path writeTemp(ScratchDir const& dir, std::string const& name,
                                 std::string const& content) {
    fs::path const p = dir.path() / name;
    std::ofstream(p, std::ios::binary) << content;
    return p;
}

// True iff SOME reported diagnostic's text contains `needle` — the reader packs
// its message into `ParseDiagnostic::actual`, and every refusal here shares ONE
// code (`F_ShippedLibDescriptorMalformed`), so `hasErrors()` alone cannot tell
// the intended rejection from an unrelated one in the same fixture.
[[nodiscard]] bool anyDiagMentions(DiagnosticReporter const& rep,
                                   std::string_view needle) {
    for (auto const& d : rep.all()) {
        if (d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
}

[[nodiscard]] std::string joinDiags(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) { out += d.actual; out += "\n"; }
    return out;
}

// A resolver over ONE hand-built table for ONE kind. `refusal` non-empty makes
// every answer a refusal — the "family could not be assembled" arm.
class TableRoleResolver final : public RuntimeLibraryRoleResolver {
public:
    TableRoleResolver(std::string kind, RuntimeLibraryTable table,
                      std::string refusal = {})
        : kind_(std::move(kind)), table_(std::move(table)),
          refusal_(std::move(refusal)) {}

    [[nodiscard]] std::string_view formatKindName() const noexcept override {
        return kind_;
    }
    [[nodiscard]] RuntimeLibraryBinding const*
    rowForRole(RuntimeLibraryRole role, std::string& refusal) const override {
        if (!refusal_.empty()) {
            refusal = refusal_;
            return nullptr;
        }
        return table_.rowForRole(role);
    }

private:
    std::string         kind_;
    RuntimeLibraryTable table_;
    std::string         refusal_;
};

[[nodiscard]] RuntimeLibraryTable peTable(std::string cLibrary = "ucrtbase.dll") {
    RuntimeLibraryTable t;
    t.bindings.push_back({RuntimeLibraryRole::CLibrary, std::move(cLibrary), {}});
    t.bindings.push_back({RuntimeLibraryRole::SystemPrimitives, "kernel32.dll", {}});
    t.bindings.push_back({RuntimeLibraryRole::AtomicsRuntime, {},
                          "runtime/platform/src/atomic.c"});
    return t;
}

struct Fixture {
    ScratchDir         dir{Location::Temp, "shipped-lib-role"};
    TypeInterner       interner{CompilationUnitId{1}};
    TypeRegistry       typeReg;
    DiagnosticReporter rep;

    [[nodiscard]] std::optional<ShippedLibDescriptor>
    read(std::string const& name, std::string const& json,
         RuntimeLibraryRoleResolver const* resolver) {
        auto const path = writeTemp(dir, name, json);
        return readShippedLibDescriptor(path, interner, typeReg, rep,
                                        DataModel::Lp64, std::nullopt,
                                        std::nullopt, {}, resolver);
    }
};

constexpr std::string_view kMathShape = R"JSON({
    "header": "m.h",
    "library": { "pe": { "role": "cLibrary" }, "elf": "libm.so.6" },
    "symbols": [ { "name": "sin", "signature": "fn(f64) -> f64" } ]
})JSON";

}  // namespace

// ── RESOLUTION ───────────────────────────────────────────────────────────────

// The role form resolves to the table's image and lands in `library` as a plain
// string beside the literal, which is untouched; the declared role is recorded.
// Then the table is REPOINTED: the role-bound entry FOLLOWS and the literal does
// not — the decoder-level statement of the whole row.
TEST(ShippedLibraryRole, RoleEntryResolvesToTheTablesImageAndFollowsARepoint) {
    Fixture f;
    {
        TableRoleResolver const resolver{"pe", peTable()};
        auto desc = f.read("math_shape.json", std::string{kMathShape}, &resolver);
        ASSERT_TRUE(desc.has_value()) << joinDiags(f.rep);
        EXPECT_EQ(f.rep.errorCount(), 0u) << joinDiags(f.rep);
        EXPECT_EQ(desc->library.size(), 2u);
        EXPECT_EQ(desc->library.at("pe"), "ucrtbase.dll");
        EXPECT_EQ(desc->library.at("elf"), "libm.so.6");
        ASSERT_EQ(desc->libraryRoles.size(), 1u);
        EXPECT_EQ(desc->libraryRoles.at("pe"), RuntimeLibraryRole::CLibrary);
    }
    {
        TableRoleResolver const repointed{"pe", peTable("vcruntime140.dll")};
        auto desc = f.read("math_shape.json", std::string{kMathShape}, &repointed);
        ASSERT_TRUE(desc.has_value()) << joinDiags(f.rep);
        EXPECT_EQ(desc->library.at("pe"), "vcruntime140.dll")
            << "the role-bound entry must FOLLOW the table";
        EXPECT_EQ(desc->library.at("elf"), "libm.so.6")
            << "the literal must NOT follow — it names no role";
    }
}

// The per-symbol override goes through the SAME chokepoint: it resolves its own
// role (a different one from the descriptor's) and records it on the symbol.
TEST(ShippedLibraryRole, SymbolOverrideResolvesThroughTheSameChokepoint) {
    Fixture f;
    TableRoleResolver const resolver{"pe", peTable()};
    auto desc = f.read("sym_override.json", R"JSON({
        "header": "w.h",
        "library": { "pe": { "role": "cLibrary" } },
        "symbols": [
            { "name": "Sleep", "signature": "fn(u32) -> void",
              "library": { "pe": { "role": "systemPrimitives" } } },
            { "name": "puts",  "signature": "fn(ptr<char>) -> i32" }
        ]
    })JSON", &resolver);
    ASSERT_TRUE(desc.has_value()) << joinDiags(f.rep);
    EXPECT_EQ(desc->library.at("pe"), "ucrtbase.dll");
    ASSERT_EQ(desc->symbols.size(), 2u);
    EXPECT_EQ(desc->symbols[0].library.at("pe"), "kernel32.dll");
    EXPECT_EQ(desc->symbols[0].libraryRoles.at("pe"),
              RuntimeLibraryRole::SystemPrimitives);
    EXPECT_TRUE(desc->symbols[1].library.empty());
    EXPECT_TRUE(desc->symbols[1].libraryRoles.empty());
}

// A `$`-prefixed note inside the role object is documentation, exactly as it is
// on every other object the reader accepts (the shared prefix carve-out).
TEST(ShippedLibraryRole, ADocumentationKeyBesideTheRoleIsAccepted) {
    Fixture f;
    TableRoleResolver const resolver{"pe", peTable()};
    auto desc = f.read("commented.json", R"JSON({
        "header": "c.h",
        "library": { "pe": { "role": "cLibrary", "$comment": "the UCRT" } },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })JSON", &resolver);
    ASSERT_TRUE(desc.has_value()) << joinDiags(f.rep);
    EXPECT_EQ(desc->library.at("pe"), "ucrtbase.dll");
}

// NO RESOLVER: the entry is validated and RECORDED but binds nothing — the
// caller declared it produces no import, and an absent key is the UNBOUND arm.
TEST(ShippedLibraryRole, WithoutAResolverARoleEntryIsRecordedAndBindsNothing) {
    Fixture f;
    auto desc = f.read("no_resolver.json", std::string{kMathShape}, nullptr);
    ASSERT_TRUE(desc.has_value()) << joinDiags(f.rep);
    EXPECT_EQ(f.rep.errorCount(), 0u);
    EXPECT_EQ(desc->library.size(), 1u) << "only the literal is an image";
    EXPECT_EQ(desc->library.at("elf"), "libm.so.6");
    EXPECT_EQ(desc->library.count("pe"), 0u) << "no image was invented";
    ASSERT_EQ(desc->libraryRoles.size(), 1u);
    EXPECT_EQ(desc->libraryRoles.at("pe"), RuntimeLibraryRole::CLibrary);
}

// A resolver answers for ONE kind. A role entry under any other key is recorded,
// never resolved: a `pe` role has no answer inside an elf build.
TEST(ShippedLibraryRole, ARoleEntryForAnotherKindIsRecordedNotResolved) {
    Fixture f;
    RuntimeLibraryTable elf;
    elf.bindings.push_back({RuntimeLibraryRole::CLibrary, "libc.so.6", {}});
    TableRoleResolver const resolver{"elf", elf};
    auto desc = f.read("other_kind.json", R"JSON({
        "header": "o.h",
        "library": { "pe": { "role": "cLibrary" }, "elf": { "role": "cLibrary" } },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })JSON", &resolver);
    ASSERT_TRUE(desc.has_value()) << joinDiags(f.rep);
    EXPECT_EQ(desc->library.size(), 1u);
    EXPECT_EQ(desc->library.at("elf"), "libc.so.6");
    EXPECT_EQ(desc->library.count("pe"), 0u);
    EXPECT_EQ(desc->libraryRoles.size(), 2u);
}

// ── FORMAT-INDEPENDENT REFUSALS — with NO resolver, so they cannot rot ────────

TEST(ShippedLibraryRole, AnUnknownRoleIsRefusedWithoutAResolver) {
    Fixture f;
    auto desc = f.read("unknown_role.json", R"JSON({
        "header": "u.h",
        "library": { "pe": { "role": "cLibrar" } },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })JSON", nullptr);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(f.rep.hasErrors());
    EXPECT_TRUE(anyDiagMentions(f.rep, "unknown runtime-library role 'cLibrar'"))
        << joinDiags(f.rep);
    EXPECT_TRUE(anyDiagMentions(f.rep, "systemPrimitives"))
        << "the refusal must list the accepted roles: " << joinDiags(f.rep);
}

// `none` spells correctly — it is a row of the role table — so only an explicit
// selectability check stops it, exactly as with the `unknown` format sentinel.
TEST(ShippedLibraryRole, TheNoneSentinelIsRefusedWithoutAResolver) {
    Fixture f;
    auto desc = f.read("none_role.json", R"JSON({
        "header": "n.h",
        "library": { "pe": { "role": "none" } },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })JSON", nullptr);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(anyDiagMentions(f.rep, "sentinel")) << joinDiags(f.rep);
}

// Both spellings in one entry: an image beside a role is refused BY NAME, not as
// an unknown key — the author meant the other spelling.
TEST(ShippedLibraryRole, AnImageBesideARoleIsRefusedAsBothSpellings) {
    Fixture f;
    auto desc = f.read("both.json", R"JSON({
        "header": "b.h",
        "library": { "pe": { "role": "cLibrary", "image": "ucrtbase.dll" } },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })JSON", nullptr);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(anyDiagMentions(f.rep, "BOTH a runtime role and an image"))
        << joinDiags(f.rep);
}

TEST(ShippedLibraryRole, AnUnknownKeyInTheRoleObjectIsRefused) {
    Fixture f;
    auto desc = f.read("unknown_key.json", R"JSON({
        "header": "k.h",
        "library": { "pe": { "role": "cLibrary", "bogus": 1 } },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })JSON", nullptr);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(anyDiagMentions(f.rep, "bogus")) << joinDiags(f.rep);
}

TEST(ShippedLibraryRole, AMissingOrNonStringRoleIsRefused) {
    {
        Fixture f;
        auto desc = f.read("missing_role.json", R"JSON({
            "header": "m.h",
            "library": { "pe": { } },
            "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
        })JSON", nullptr);
        EXPECT_FALSE(desc.has_value());
        EXPECT_TRUE(anyDiagMentions(f.rep, "must declare a string 'role'"))
            << joinDiags(f.rep);
    }
    {
        Fixture f;
        auto desc = f.read("numeric_role.json", R"JSON({
            "header": "m.h",
            "library": { "pe": { "role": 7 } },
            "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
        })JSON", nullptr);
        EXPECT_FALSE(desc.has_value());
        EXPECT_TRUE(anyDiagMentions(f.rep, "must declare a string 'role'"))
            << joinDiags(f.rep);
    }
}

TEST(ShippedLibraryRole, AValueThatIsNeitherStringNorObjectIsRefused) {
    Fixture f;
    auto desc = f.read("numeric_value.json", R"JSON({
        "header": "v.h",
        "library": { "pe": 7 },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })JSON", nullptr);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(anyDiagMentions(f.rep, "must be a string naming the image"))
        << joinDiags(f.rep);
    EXPECT_TRUE(anyDiagMentions(f.rep, "{\"role\": ...}"))
        << "the refusal must name the other spelling: " << joinDiags(f.rep);
}

// ── RESOLUTION REFUSALS — with a resolver in hand ────────────────────────────

// A role no document of the family declares: nullptr with an empty refusal.
TEST(ShippedLibraryRole, ARoleTheFamilyDoesNotDeclareIsRefused) {
    Fixture f;
    RuntimeLibraryTable table;
    table.bindings.push_back({RuntimeLibraryRole::CLibrary, "ucrtbase.dll", {}});
    TableRoleResolver const resolver{"pe", table};
    auto desc = f.read("undeclared.json", R"JSON({
        "header": "d.h",
        "library": { "pe": { "role": "unwindPersonality" } },
        "symbols": [ { "name": "puts", "signature": "fn(ptr<char>) -> i32" } ]
    })JSON", &resolver);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(anyDiagMentions(f.rep, "no shipped 'pe' object-format document declares"))
        << joinDiags(f.rep);
    EXPECT_TRUE(anyDiagMentions(f.rep, "unwindPersonality")) << joinDiags(f.rep);
}

// A REALIZED role has no image to import from: the body belongs under
// `realization`, and importing it would be a binary the loader refuses at start.
TEST(ShippedLibraryRole, ARoleTheFamilyRealizesFromSourceIsRefused) {
    Fixture f;
    TableRoleResolver const resolver{"pe", peTable()};
    auto desc = f.read("realized.json", R"JSON({
        "header": "a.h",
        "library": { "pe": { "role": "atomicsRuntime" } },
        "symbols": [ { "name": "__atomic_load", "signature": "fn(u64, ptr<void>, ptr<void>, i32) -> void" } ]
    })JSON", &resolver);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(anyDiagMentions(f.rep, "REALIZES")) << joinDiags(f.rep);
    EXPECT_TRUE(anyDiagMentions(f.rep, "runtime/platform/src/atomic.c"))
        << "the refusal must name the shipped source: " << joinDiags(f.rep);
    EXPECT_TRUE(anyDiagMentions(f.rep, "'realization'")) << joinDiags(f.rep);
}

// A family that cannot be assembled: the resolver's refusal travels verbatim.
TEST(ShippedLibraryRole, AFamilyRefusalIsReportedVerbatim) {
    Fixture f;
    TableRoleResolver const resolver{
        "pe", {}, "kind 'pe' declares role 'cLibrary' with two different providers"};
    auto desc = f.read("family_refusal.json", std::string{kMathShape}, &resolver);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(anyDiagMentions(f.rep, "cannot be answered")) << joinDiags(f.rep);
    EXPECT_TRUE(anyDiagMentions(f.rep, "two different providers")) << joinDiags(f.rep);
}

// ── THE REALIZATION ORACLE THREADS THE RESOLVER ─────────────────────────────
//
// The bare-prototype / archive-member / assembly binders reach descriptors
// through `realizeShippedExternSymbols`, not through the `#include` read. Over
// the SHIPPED corpus (whose `stdio.json` now names `cLibrary`), the oracle's
// row for `puts` must carry the resolver's image — and follow a repoint.
TEST(ShippedLibraryRole, TheRealizationOracleResolvesRolesThroughTheResolver) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    std::array<NamedTypeBinding, 1> const named{
        NamedTypeBinding{"va_list",
                         interner.pointer(interner.primitive(TypeKind::Void))}};
    std::array<std::string, 1> const names{"puts"};

    auto realizeWith = [&](TableRoleResolver const& resolver) {
        DiagnosticReporter rep;
        auto realized = realizeShippedExternSymbols(
            names, interner, typeReg, rep, DataModel::Llp64,
            std::optional<std::string_view>{"x86_64"}, ObjectFormatKind::Pe,
            named, &resolver);
        EXPECT_EQ(rep.errorCount(), 0u) << joinDiags(rep);
        return realized;
    };

    TableRoleResolver const shipped{"pe", peTable()};
    auto const witness = realizeWith(shipped);
    ASSERT_TRUE(witness.has_value()) << "the shipped corpus must be locatable";
    auto const w = witness->find("puts");
    ASSERT_NE(w, witness->end()) << "stdio.json declares puts";
    ASSERT_EQ(w->second.library.count("pe"), 1u)
        << "the oracle's row must carry a pe image for puts";
    EXPECT_EQ(w->second.library.at("pe"), "ucrtbase.dll");

    TableRoleResolver const repointed{"pe", peTable("vcruntime140.dll")};
    auto const mutant = realizeWith(repointed);
    ASSERT_TRUE(mutant.has_value());
    auto const m = mutant->find("puts");
    ASSERT_NE(m, mutant->end());
    EXPECT_EQ(m->second.library.at("pe"), "vcruntime140.dll")
        << "the oracle's row must FOLLOW the resolver, or the archive-member and "
           "assembly binders bind a different image than the #include path";
}

// ── R3: TWO OWNERS FOR ONE BODY, IN EITHER SPELLING ─────────────────────────
//
// D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF R3 refuses a descriptor that
// declares BOTH an import and a shipped-source realization for one object
// format. Its subject is "an import is declared here", and after this row a
// `library` entry declares one in EITHER spelling — a literal image, or a role.
//
// ⚠ THE RULE IS FORMAT-INDEPENDENT AND MUST STAY SO. The check reads the
// DECODED image map plus the DECLARED role map, deliberately: a role entry
// lands in the image map only when a resolver answers for that exact kind, so a
// check keyed on the image map alone would fire for at most the ONE kind a
// build happens to select and NEVER at all for the many readers that carry no
// resolver (the LSP, the header parser, the corpus sweeps, ~40 test call
// sites). The arms below therefore read with NO resolver and NO active format
// — the widest blind spot — and use `macho`, a kind no such reader answers for.
// ✔MEASURED at the time this pin was written: the shipped corpus contains no
// descriptor in this shape, so the defect was latent, not live; a pin is what
// keeps it that way.
TEST(ShippedLibraryRole, ARoleBesideARealizationIsTwoOwnersEvenWithNoResolver) {
    Fixture f;
    auto desc = f.read("role_vs_realization.json", R"JSON({
        "header": "rr.h",
        "library": { "macho": { "role": "cLibrary" } },
        "realization": { "macho": { "source": "runtime/platform/src/atomic.c" } },
        "symbols": [ { "name": "rr_fn", "signature": "fn() -> i32" } ]
    })JSON", nullptr);
    EXPECT_FALSE(desc.has_value())
        << "a descriptor naming BOTH an import and a shipped body for one format "
           "must not read";
    EXPECT_TRUE(anyDiagMentions(f.rep, "D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF R3"))
        << joinDiags(f.rep);
    EXPECT_TRUE(anyDiagMentions(f.rep, "two owners for one body")) << joinDiags(f.rep);
    EXPECT_TRUE(anyDiagMentions(f.rep, "{\"role\": \"cLibrary\"}"))
        << "the message must name the import in the spelling the author used: "
        << joinDiags(f.rep);
    EXPECT_TRUE(anyDiagMentions(f.rep, "runtime/platform/src/atomic.c"))
        << "and the other owner: " << joinDiags(f.rep);
}

// The per-symbol half of the same rule: the symbol's own role overrides the
// descriptor's map for that format key, and R3 compares the EFFECTIVE maps.
TEST(ShippedLibraryRole, APerSymbolRoleBesideARealizationIsAlsoTwoOwners) {
    Fixture f;
    auto desc = f.read("sym_role_vs_realization.json", R"JSON({
        "header": "sr.h",
        "realization": { "macho": { "source": "runtime/platform/src/atomic.c" } },
        "symbols": [
            { "name": "sr_fn", "signature": "fn() -> i32",
              "library": { "macho": { "role": "cLibrary" } } }
        ]
    })JSON", nullptr);
    EXPECT_FALSE(desc.has_value());
    EXPECT_TRUE(anyDiagMentions(f.rep, "D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF R3"))
        << joinDiags(f.rep);
    EXPECT_TRUE(anyDiagMentions(f.rep, "symbols[0] ('sr_fn')")) << joinDiags(f.rep);
}

// CONTROL, in the OPPOSITE direction so the arms above cannot widen into
// "anything with a role is refused": the SAME role entry, with the realization
// on a DIFFERENT format, is two owners of NOTHING and must read clean.
TEST(ShippedLibraryRole, ARoleAndARealizationOnDifferentFormatsIsNotTwoOwners) {
    Fixture f;
    auto desc = f.read("role_other_format.json", R"JSON({
        "header": "ro.h",
        "library": { "macho": { "role": "cLibrary" } },
        "realization": { "pe": { "source": "runtime/platform/src/atomic.c" } },
        "symbols": [ { "name": "ro_fn", "signature": "fn() -> i32" } ]
    })JSON", nullptr);
    ASSERT_TRUE(desc.has_value()) << joinDiags(f.rep);
    EXPECT_EQ(f.rep.errorCount(), 0u) << joinDiags(f.rep);
    EXPECT_TRUE(desc->library.empty())
        << "no resolver answers for macho here, so the role binds no image — the "
           "stated UNBOUND arm";
    ASSERT_EQ(desc->libraryRoles.size(), 1u);
    EXPECT_EQ(desc->libraryRoles.at("macho"), RuntimeLibraryRole::CLibrary);
}

// ── S2: A ROLE THIS BUILD CANNOT ANSWER REACHES THE BUILD'S REPORTER ────────
//
// `realizeShippedExternSymbols` reads candidate descriptors on a THROWAWAY
// reporter and skips a failed read, deliberately: it is consulted for names the
// user never `#include`d, so an unrelated malformed descriptor must not become
// this program's build failure. The role channel routes a NEW failure class down
// that same drain — a role the active format family cannot answer — and there the
// skip is wrong: the descriptor is well-formed, the corpus and the format
// documents simply disagree, and the consequence is that every name in it routes
// unbound and the link tier blames the USER's program for an undefined symbol.
// This is the binder that a `.s` writing `call puts` and an archive member both
// reach through.
TEST(ShippedLibraryRole, AnUnanswerableRoleReachesTheBuildsReporter) {
    ScratchDir dir{Location::Temp, "role-oracle-refusal"};
    fs::path const libs = dir.path() / "src" / "dss-config" / "shippedLibs";
    fs::create_directories(libs);
    std::ofstream(libs / "witness.json", std::ios::binary) << R"JSON({
        "header": "witness.h",
        "library": { "pe": { "role": "unwindPersonality" } },
        "symbols": [ { "name": "witness_fn", "signature": "fn() -> i32" } ]
    })JSON";
    // A SECOND descriptor, malformed on its own terms and declaring a DIFFERENT
    // name — the control: the swallow must still swallow it.
    std::ofstream(libs / "broken.json", std::ios::binary) << R"JSON({
        "header": "broken.h",
        "symbols": [ { "name": "broken_fn", "signature": "fn(NOT_A_TYPE) -> i32" } ]
    })JSON";

    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    // The table declares `cLibrary` and `systemPrimitives` — NOT
    // `unwindPersonality` — so the witness names a role nothing answers.
    auto const askFor = [&](std::string_view name, DiagnosticReporter& rep) {
        ScopedEnv const env{"DSS_CONFIG_ROOT", dir.path().string()};
        TableRoleResolver const resolver{"pe", peTable()};
        std::array<std::string, 1> const names{std::string{name}};
        return realizeShippedExternSymbols(
            names, interner, typeReg, rep, DataModel::Llp64,
            std::optional<std::string_view>{"x86_64"}, ObjectFormatKind::Pe,
            std::span<NamedTypeBinding const>{}, &resolver);
    };

    DiagnosticReporter loud;
    auto const realized = askFor("witness_fn", loud);
    ASSERT_TRUE(realized.has_value()) << "the scratch corpus must be locatable";
    EXPECT_GT(loud.errorCount(), 0u)
        << "a role the active family cannot answer is a fact about THIS BUILD, "
           "not about an unrelated file — swallowing it routes the name unbound "
           "and the link tier then blames the user's program";
    EXPECT_TRUE(anyDiagMentions(loud, "unwindPersonality")) << joinDiags(loud);
    EXPECT_TRUE(anyDiagMentions(loud, "witness.json")) << joinDiags(loud);

    // CONTROL, in the opposite direction: an unrelated descriptor that is
    // malformed ON ITS OWN TERMS is still skipped in silence. Without this the
    // arm above could pass by simply un-swallowing everything.
    DiagnosticReporter quiet;
    auto const other = askFor("broken_fn", quiet);
    ASSERT_TRUE(other.has_value());
    EXPECT_EQ(quiet.errorCount(), 0u)
        << "an unrelated malformed descriptor must NOT become this build's "
           "failure: " << joinDiags(quiet);
    EXPECT_EQ(other->count("broken_fn"), 0u)
        << "and its names stay Unknown, routing unbound as before";
}
