// Plan 11 FF3 (ABI resolution) tests — `dss::ffi::resolveAbi`.
//
// D-FFI-ABI-CATALOG-SELECTS-CALLING-CONVENTION-BY-FORMAT-IDENTITY CLOSED (P44).
// This file used to assert a closed C++ table, `kAbiCatalog`, against itself —
// six rows keyed on (target name, `ObjectFormatKind`) — which is the weakest
// shape a test of a two-owner fact can take: it pinned that the engine agreed
// with the engine, and would have passed unchanged while the shipped
// descriptors said something else entirely.
//
// The pins are now aimed at the property that actually matters: THE ANSWER
// LIVES IN THE `.format.json`, AND EDITING THAT FILE CHANGES WHAT RESOLVES.
// The two load-bearing arms are both REMOVE-direction mutants of a shipped
// document (`stripKey` / `retargetConvention`) — an ADD-direction fixture that
// injected the key into a document that already has it would stay green on the
// day the real config LOST the feature.
//
// Pins:
//   * Every shipped (target, format) pair resolves to the cc its own descriptor
//     names — swept across all 24 shipped formats, not a hand-listed six.
//   * REMOVE the key from a shipped format ⇒ the format is REFUSED AT LOAD.
//   * RE-POINT the key at a different convention ⇒ `resolveAbi` returns the
//     DIFFERENT cc, with no engine rebuild. This is the whole anchor.
//   * The reserved `none` spelling, both arms of its coherence rule.
//   * A `.target.json` may not name a cc row `none` (the sentinel collision).
//   * Diagnostic-code name round-trips.

#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/abi/abi_catalog.hpp"
#include "link/object_format_schema.hpp"
#include "diagnostic_count.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::countCode;

namespace {

// ── THE SHIPPED-FORMAT MUTATION SUBSTRATE ───────────────────────────────────
//
// The format-schema analogue of `tests/test_support/mutate_target_schema.hpp`,
// and it carries that header's fail-CLOSED contract for the same reason: a
// mutation that mutated nothing produces a "mutant" that IS the shipped
// document, and every pin downstream of it then asserts nothing at all. Each
// helper below `ADD_FAILURE`s rather than silently returning the original.
//
// It lives here rather than in `tests/test_support/` because it has exactly one
// consumer; promote it the day a second file needs it.

[[nodiscard]] std::string shippedFormatText(std::string_view name) {
    auto pathR = findShippedConfig(
        ShippedConfigLocator{name, "object-formats", ".format.json",
                             "object format",
                             DiagnosticCode::C_InvalidFormatName});
    if (!pathR.has_value()) {
        ADD_FAILURE() << "cannot locate shipped format '" << name << '\'';
        return {};
    }
    std::ifstream in{*pathR};
    if (!in.is_open()) {
        ADD_FAILURE() << "cannot open " << pathR->string();
        return {};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

[[nodiscard]] nlohmann::json shippedFormatDoc(std::string_view name) {
    auto const text = shippedFormatText(name);
    if (text.empty()) return {};
    return nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
}

// Serialize a mutated document and load it, asserting the mutation was REAL.
// `before` is the pre-mutation dump; a byte-identical `after` is the fail-open
// this whole substrate exists to refuse.
[[nodiscard]] LoadResult<std::shared_ptr<ObjectFormatSchema>>
loadMutated(std::string_view name, std::string const& before,
            nlohmann::json const& doc) {
    std::string const after = doc.dump();
    if (after == before) {
        ADD_FAILURE()
            << "the mutation of '" << name << "' was a NO-OP — the document is "
               "byte-identical (" << before.size() << " bytes) before and "
               "after, so the \"mutant\" IS the shipped schema and the pin "
               "consuming it asserts nothing at all";
    }
    return ObjectFormatSchema::loadFromText(
        after, std::string{"<mutated "} + std::string{name} + ">");
}

// REMOVE-direction mutant: the shipped format WITHOUT a top-level key.
[[nodiscard]] LoadResult<std::shared_ptr<ObjectFormatSchema>>
formatWithoutKey(std::string_view name, char const* key) {
    auto doc = shippedFormatDoc(name);
    if (!doc.is_object()) {
        ADD_FAILURE() << "shipped format '" << name << "' did not parse";
        return std::unexpected(std::vector<ConfigDiagnostic>{});
    }
    if (!doc.contains(key)) {
        ADD_FAILURE()
            << "shipped format '" << name << "' does not declare '" << key
            << "' — the REMOVE mutant would remove nothing, which is exactly "
               "the vacuous pin this helper refuses to build";
    }
    std::string const before = doc.dump();
    doc.erase(key);
    return loadMutated(name, before, doc);
}

// RE-POINT mutant: the shipped format with its declared convention replaced.
[[nodiscard]] LoadResult<std::shared_ptr<ObjectFormatSchema>>
formatWithConvention(std::string_view name, std::string const& convention) {
    auto doc = shippedFormatDoc(name);
    if (!doc.is_object()) {
        ADD_FAILURE() << "shipped format '" << name << "' did not parse";
        return std::unexpected(std::vector<ConfigDiagnostic>{});
    }
    if (!doc.contains("cCallingConvention")) {
        ADD_FAILURE() << "shipped format '" << name
                      << "' declares no cCallingConvention to re-point";
    }
    std::string const before = doc.dump();
    doc["cCallingConvention"]["convention"] = convention;
    return loadMutated(name, before, doc);
}

// Every shipped format name, read off the shipped directory rather than
// hand-listed — a hand-listed set silently stops covering the format added
// after it was written.
[[nodiscard]] std::vector<std::string> shippedFormatNames() {
    std::vector<std::string> out;
    auto dirR = findShippedConfigDir("object-formats");
    if (!dirR.has_value()) {
        ADD_FAILURE() << "cannot locate the shipped object-formats directory";
        return out;
    }
    for (auto const& e : std::filesystem::directory_iterator{*dirR}) {
        auto const fn = e.path().filename().string();
        constexpr std::string_view kSuffix = ".format.json";
        if (fn.size() > kSuffix.size()
            && fn.compare(fn.size() - kSuffix.size(), kSuffix.size(), kSuffix)
                   == 0) {
            out.push_back(fn.substr(0, fn.size() - kSuffix.size()));
        }
    }
    return out;
}

} // namespace

// ── (A) THE SWEEP: EVERY SHIPPED FORMAT DECLARES A CONVENTION, AND IT
//        RESOLVES AGAINST THE TARGET IT CAN BE PAIRED WITH ─────────────────

TEST(FfiAbiResolve, EveryShippedFormatDeclaresAConventionThatResolves) {
    // The `kAbiCatalog`-era version of this test hand-listed six rows and
    // asserted the table contained them. This asks the shipped corpus instead:
    // for every format on disk, the name it declares must either be the
    // reserved `none` or resolve in the target whose arch that format pins.
    auto const names = shippedFormatNames();
    // ⚠ A directory sweep passes VACUOUSLY on an empty directory, which is
    // exactly what a moved config root or a broken locator produces. 24 formats
    // ship today; demanding at least that many makes the green mean something.
    ASSERT_GE(names.size(), 24u)
        << "the shipped object-formats directory yielded only " << names.size()
        << " format(s) — a sweep over an empty or truncated set passes while "
           "proving nothing";

    auto x86 = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(x86.has_value());
    auto arm = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(arm.has_value());

    std::size_t named = 0;
    std::size_t none  = 0;
    for (auto const& name : names) {
        auto format = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(format.has_value())
            << name << " failed to load — a format missing the REQUIRED "
                       "cCallingConvention is refused at load, which is the "
                       "intended behaviour but breaks this sweep loudly";
        auto const& declared = (*format)->cCallingConvention();
        EXPECT_TRUE(declared.declared())
            << name << " declares no cCallingConvention";
        if (declared.declaresNoConvention()) {
            ++none;
            continue;
        }
        ++named;
        // The arch a format can be paired with is pinned by its own machine
        // code (crossValidateTargetFormat refuses the rest), and the shipped
        // names spell it. Resolve against the target that name implies.
        bool const isArm = name.find("aarch64") != std::string::npos
                        || name.find("arm64")   != std::string::npos;
        TargetSchema const& target = isArm ? **arm : **x86;
        EXPECT_NE(target.callingConventionByName(declared.convention), nullptr)
            << name << " declares cCallingConvention '" << declared.convention
            << "' but target '" << target.name()
            << "' ships no callingConventions[] row with that name";
    }
    EXPECT_GE(named, 22u) << "only " << named << " shipped format(s) name a "
                             "real convention";
    EXPECT_EQ(none, 2u) << "exactly the two declared-only skeletons (wasm, "
                           "spirv) should declare the reserved spelling";
}

TEST(FfiAbiResolve, TheDeclaredConventionAgreesWithEveryIndependentDeclarationOfIt) {
    // ★★ THE PIN THAT MAKES A *WRONG* VALUE RED, not merely a missing one.
    // Requiring the key catches an omission; nothing above catches
    // `macho64-arm64-darwin-exec` quietly saying `aapcs64` instead of
    // `apple_arm64` — which would silently revert Apple's stacked-argument
    // packing, its always-stack variadics AND its x29 reservation, since all
    // three live on the cc row this name selects.
    //
    // ★ AND IT IS A CROSS-CHECK, NOT A C++ TABLE. Writing the expected answer
    // per format in this file would rebuild `kAbiCatalog` inside its own
    // regression test — the same fact, the same second owner, one directory
    // over. Instead the corpus is checked against ITSELF along two axes that
    // were declared independently and long before this key existed:
    //   1. an exec-flavored format's `entryCallingConvention` — the convention
    //      its entry trampoline resolves — must be the one its C code uses;
    //   2. every format in a family (`macho64-arm64-darwin`, `-exec`, `-dylib`,
    //      `-staticlib`) is the same platform, so all four must agree.
    // A typo in one file therefore contradicts a sibling that has no reason to
    // change with it. (This is `tests/link/test_c_symbol_decoration.cpp`'s
    // cross-check against `processExit.importMangledName`, one axis over.)
    auto const names = shippedFormatNames();
    ASSERT_GE(names.size(), 24u);

    std::size_t entryChecks  = 0;
    std::size_t familyChecks = 0;
    // family prefix -> (convention, the file that first declared it)
    std::map<std::string, std::pair<std::string, std::string>> family;

    for (auto const& name : names) {
        auto format = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(format.has_value()) << name;
        std::string const declared{(*format)->cCallingConvention().convention};
        ASSERT_FALSE(declared.empty()) << name;

        // (1) against this document's OWN entry-trampoline convention.
        auto const entry = (*format)->entryCallingConvention();
        if (!entry.empty()) {
            ++entryChecks;
            EXPECT_EQ(declared, std::string{entry})
                << name << " declares cCallingConvention '" << declared
                << "' but entryCallingConvention '" << entry
                << "'. One image cannot run its entry trampoline under one ABI "
                   "and its C code under another.";
        }

        // (2) against the rest of its own format family.
        std::string prefix = name;
        for (char const* suffix : {"-exec", "-dyn", "-pie", "-dylib",
                                   "-staticlib", "-dll"}) {
            std::string const s{suffix};
            if (prefix.size() > s.size()
                && prefix.compare(prefix.size() - s.size(), s.size(), s) == 0) {
                prefix.resize(prefix.size() - s.size());
                break;
            }
        }
        auto const [it, fresh] =
            family.try_emplace(prefix, std::pair{declared, name});
        if (!fresh) {
            ++familyChecks;
            EXPECT_EQ(declared, it->second.first)
                << name << " declares '" << declared << "' but its family "
                << "sibling " << it->second.second << " declares '"
                << it->second.first
                << "'. Both are the same (ISA x OS) platform, so both have the "
                   "same platform ABI — a flavor is not an ABI axis.";
        }
    }

    // ⚠ A CROSS-CHECK THAT CROSSES NOTHING PASSES. Both axes are `if`-guarded,
    // so a rename that emptied either set would leave this test green while
    // checking zero pairs.
    EXPECT_GE(entryChecks, 7u)
        << "only " << entryChecks << " format(s) were checked against their own "
           "entryCallingConvention — seven exec-flavored formats ship one";
    EXPECT_GE(familyChecks, 15u)
        << "only " << familyChecks << " format(s) were checked against a family "
           "sibling — a filter that pairs nothing proves nothing";
}

// ── (B) THE SHIPPED HAPPY PATHS, BY NAME ────────────────────────────────────
//
// One per (arch × OS) the corpus actually builds. They assert the RESOLVED cc's
// NAME — the thing every caller reads — and never an enum the deleted table
// carried, which nothing in `src/` ever read.

namespace {
void expectResolves(char const* targetName, char const* formatName,
                    char const* expectedCc) {
    auto target = TargetSchema::loadShipped(targetName);
    ASSERT_TRUE(target.has_value()) << targetName;
    auto format = ObjectFormatSchema::loadShipped(formatName);
    ASSERT_TRUE(format.has_value()) << formatName;
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_TRUE(r.has_value())
        << abiResolveErrorKindName(r.error().kind) << ": " << r.error().detail;
    ASSERT_NE(r->cc, nullptr);
    EXPECT_EQ(r->cc->name, expectedCc);
    EXPECT_EQ(rep.errorCount(), 0u);
}
} // namespace

TEST(FfiAbiResolve, X86_64ElfResolvesToSysV) {
    expectResolves("x86_64", "elf64-x86_64-linux", "sysv_amd64");
}

TEST(FfiAbiResolve, X86_64PeResolvesToMS64) {
    // The SAME processor, a DIFFERENT convention — which is why the fact is
    // per-format and could never have lived on the target.
    expectResolves("x86_64", "pe64-x86_64-windows", "ms_x64");
}

TEST(FfiAbiResolve, X86_64MachOResolvesToSysV) {
    // Apple's x86_64 ABI is SysV-with-quirks: the same cc row as Linux ELF.
    expectResolves("x86_64", "macho64-x86_64-darwin", "sysv_amd64");
}

TEST(FfiAbiResolve, Arm64ElfResolvesToAAPCS64) {
    expectResolves("arm64", "elf64-aarch64-linux", "aapcs64");
}

TEST(FfiAbiResolve, Arm64MachOResolvesToAppleArm64) {
    // ⚠ THIS ASSERTION MOVED WHEN THE TABLE DIED, AND THE MOVE IS THE FIX. The
    // pre-P44 version of this test loaded the x86_64 Mach-O format and said so
    // in a comment — "resolveAbi reads only the format KIND (MachO) for the
    // catalog lookup, so the (arm64, MachO) tuple is exercised regardless of
    // the format's CPU". That sentence is a precise description of the defect:
    // a format's declared ABI was being ignored in favour of its KIND. Under
    // the declared verb it is no longer true, and pairing an arm64 target with
    // an x86_64 descriptor now correctly resolves what THAT descriptor says.
    // The arm64 Mach-O descriptor is the one that names `apple_arm64`.
    expectResolves("arm64", "macho64-arm64-darwin", "apple_arm64");
}

TEST(FfiAbiResolve, AnArm64TargetPairedWithAnX86DescriptorGetsTheDescriptorsAnswer) {
    // The negative face of the test above, stated so the property cannot be
    // mistaken for an accident of naming: the resolution follows the DOCUMENT,
    // not the target's identity. (The pairing itself is refused upstream by
    // `crossValidateTargetFormat` on the machine code — FF3 is not that gate,
    // and pretending to be one is what the deleted table did.)
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    // arm64 ships no `sysv_amd64` row, so the x86_64 descriptor's declared
    // answer fails LOUD against it rather than silently yielding apple_arm64.
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, AbiResolveErrorKind::NoMatchingCcInTarget);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_AbiNoMatchingCcInTarget), 1u);
}

// ── (C) THE ANCHOR ITSELF: THE ANSWER LIVES IN THE FILE ─────────────────────

TEST(FfiAbiResolve, ConventionIsReadFromTheShippedSchemaNotACppTable) {
    // ★★ THIS IS THE PIN THE ANCHOR EXISTS FOR, and it is a REMOVE-direction
    // mutant in the sense that matters: it takes the shipped document's OWN
    // answer away and substitutes another, then shows the engine follows the
    // document. Before P44 this test could not have been written — the value in
    // the file was inert and `kAbiCatalog` decided, so re-pointing the
    // descriptor would have changed nothing and this would have gone green
    // while proving the opposite of what it claims.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());

    // Control: the shipped pe64 descriptor resolves ms_x64.
    {
        auto shipped = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
        ASSERT_TRUE(shipped.has_value());
        DiagnosticReporter rep;
        auto r = resolveAbi(**target, **shipped, rep);
        ASSERT_TRUE(r.has_value());
        ASSERT_NE(r->cc, nullptr);
        ASSERT_EQ(r->cc->name, "ms_x64")
            << "the CONTROL arm did not reproduce the shipped answer, so the "
               "mutant arm below would prove nothing";
    }

    // Mutant: the SAME format, re-pointed at the other convention the same
    // processor declares. Nothing is recompiled.
    auto mutated = formatWithConvention("pe64-x86_64-windows", "sysv_amd64");
    ASSERT_TRUE(mutated.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **mutated, rep);
    ASSERT_TRUE(r.has_value())
        << abiResolveErrorKindName(r.error().kind) << ": " << r.error().detail;
    ASSERT_NE(r->cc, nullptr);
    EXPECT_EQ(r->cc->name, "sysv_amd64")
        << "re-pointing the descriptor's cCallingConvention did not change what "
           "resolveAbi returns — the selection is still being derived in C++, "
           "which is exactly the defect "
           "D-FFI-ABI-CATALOG-SELECTS-CALLING-CONVENTION-BY-FORMAT-IDENTITY "
           "names";
    // And the two answers really are different ABIs, not two spellings of one.
    EXPECT_NE(r->cc->argGprs, (*target)->callingConventionByName("ms_x64")->argGprs)
        << "the mutant resolved a convention with the SAME argument registers, "
           "so the assertion above could not have distinguished them";
}

TEST(FfiAbiResolve, ADescriptorNamingAnUnknownConventionFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto mutated =
        formatWithConvention("elf64-x86_64-linux", "riscv_lp64d_not_shipped");
    ASSERT_TRUE(mutated.has_value())
        << "the format itself is well-formed — an unresolvable NAME is a "
           "(target, format) PAIR question and must not be judged at format "
           "load, where no target is in scope";
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **mutated, rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, AbiResolveErrorKind::NoMatchingCcInTarget);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_AbiNoMatchingCcInTarget), 1u);
}

// ── (D) THE KEY IS REQUIRED — REMOVE-DIRECTION MUTANTS ──────────────────────

TEST(FfiAbiResolve, RemovingTheKeyFromAShippedFormatRefusesItAtLoad) {
    // Swept, not sampled: the rule's whole content is that it is
    // UNCONDITIONAL, so testing it on one format would leave exactly the gap
    // an exec-flavor gate would have left. Every shipped format must refuse.
    auto const names = shippedFormatNames();
    ASSERT_GE(names.size(), 24u);
    for (auto const& name : names) {
        auto mutated = formatWithoutKey(name, "cCallingConvention");
        EXPECT_FALSE(mutated.has_value())
            << name << " loaded WITHOUT a cCallingConvention — the key is "
                       "required on every format unconditionally, and a format "
                       "that loads without it has silently inherited whatever "
                       "the engine defaults to";
        if (!mutated.has_value()) {
            bool sawMissingField = false;
            for (auto const& d : mutated.error()) {
                if (d.code == DiagnosticCode::C_MissingField
                    && d.path.find("cCallingConvention") != std::string::npos) {
                    sawMissingField = true;
                }
            }
            EXPECT_TRUE(sawMissingField)
                << name << " was refused, but not by the missing-key rule — "
                           "the refusal must name the absent key, or a future "
                           "unrelated breakage would keep this test green";
        }
    }
}

TEST(FfiAbiResolve, AnEmptyConventionStringIsRefusedAtLoad) {
    // `""` is the in-memory INVALID sentinel, so it must not be reachable from
    // a document — otherwise a truncated edit produces a schema `validate()`
    // then rejects with the MISSING-key wording while the key is present.
    auto mutated = formatWithConvention("elf64-x86_64-linux", "");
    EXPECT_FALSE(mutated.has_value())
        << "an empty cCallingConvention.convention loaded — the empty string is "
           "the invalid sentinel and must be refused at the document boundary";
}

TEST(FfiAbiResolve, AHandBuiltSchemaWithNoConventionIsRefusedAtResolve) {
    // `ObjectFormatSchema{ObjectFormatData}` is a public constructor running NO
    // validation, so an in-memory producer reaches FF3 without passing the
    // loader. That path must not be treated as "declares none".
    detail::ObjectFormatData data;
    data.name = "<hand-built>";
    ObjectFormatSchema const format{std::move(data)};
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, format, rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, AbiResolveErrorKind::UnknownTuple);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_AbiUnknownTuple), 1u);
}

// ── (E) THE RESERVED `none` SPELLING, BOTH COHERENCE ARMS ───────────────────

TEST(FfiAbiResolve, OperandStackTargetWithWasmFormatResolvesNullCc) {
    auto target = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"wasm32","version":"0.0","abiModel":"operand-stack"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("wasm32-v1");
    ASSERT_TRUE(format.has_value());
    EXPECT_TRUE((*format)->cCallingConvention().declaresNoConvention());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_TRUE(r.has_value()) << abiResolveErrorKindName(r.error().kind);
    EXPECT_EQ(r->cc, nullptr);
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(FfiAbiResolve, ResultIdTargetWithSpirvFormatResolvesNullCc) {
    auto target = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"spirv","version":"0.0","abiModel":"result-id"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("spirv-1.6");
    ASSERT_TRUE(format.has_value());
    EXPECT_TRUE((*format)->cCallingConvention().declaresNoConvention());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_TRUE(r.has_value()) << abiResolveErrorKindName(r.error().kind);
    EXPECT_EQ(r->cc, nullptr);
}

TEST(FfiAbiResolve, ARegisterMachineTargetWithANoneFormatFailsLoud) {
    // The arm that replaces the old `UnknownTuple` (a (target, format) pair
    // absent from the deleted table). A register machine paired with a format
    // that declares NO register convention is the same statement: the pairing
    // has no declared ABI. Silently returning a null cc here would send a
    // register-machine pipeline downstream with nothing to allocate against.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto mutated = formatWithConvention("elf64-x86_64-linux", "none");
    ASSERT_TRUE(mutated.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **mutated, rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, AbiResolveErrorKind::UnknownTuple);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_AbiUnknownTuple), 1u);
}

TEST(FfiAbiResolve, AnOperandStackTargetWithANamedConventionFailsLoud) {
    // The inverse coherence arm — and note that NEITHER arm names a format
    // KIND. Both compare two DECLARATIONS: the target's `abiModel` against the
    // format's `cCallingConvention`. The pre-P44 code reached the same verdict
    // by comparing `format.kind()` against `Wasm`/`Spirv` literals, which were
    // two more identity branches in the file the anchor is about.
    auto target = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"wasm32","version":"0.0","abiModel":"operand-stack"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, AbiResolveErrorKind::FormatAbiModelMismatch);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_AbiFormatAbiModelMismatch), 1u);
}

TEST(FfiAbiResolve, ResultIdTargetWithPeFormatFailsLoud) {
    auto target = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"spirv","version":"0.0","abiModel":"result-id"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, AbiResolveErrorKind::FormatAbiModelMismatch);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_AbiFormatAbiModelMismatch), 1u);
}

// ── (F) THE SENTINEL COLLISION IS CLOSED FROM BOTH SIDES ────────────────────

TEST(FfiAbiResolve, ATargetMayNotNameACallingConventionNone) {
    // `none` is an in-band sentinel inside a NAME space — the hazard
    // `objectFormatKindName`'s "★ THE SENTINEL SPELLS CORRECTLY" note describes.
    // The format loader refuses an empty convention; this refuses the other
    // side, so the spelling can never denote both "no convention" and a real
    // one. Refused at LOAD, once, rather than defended against at every use.
    auto bad = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"x86_64","version":"0.0","abiModel":"register-machine"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ],
      "registers": [
        {"name":"rdi","class":"gpr","widthBytes":8,"hwEncoding":7},
        {"name":"rsp","class":"gpr","widthBytes":8,"hwEncoding":4}
      ],
      "callingConventions": [
        {
          "name":"none",
          "argGprs":["rdi"], "argFprs":[], "returnGprs":["rdi"],
          "returnFprs":[], "callerSaved":[], "calleeSaved":[],
          "stackAlignment":16, "stackPointer":"rsp"
        }
      ]
    })");
    EXPECT_FALSE(bad.has_value())
        << "a target declared a callingConventions[] row named 'none' — that "
           "spelling is reserved for a format declaring it has NO register-"
           "level C calling convention, and a row carrying it makes the "
           "declaration ambiguous";
}

// ── (G) THE COHERENCE PASS ON THE RESOLVED ROW (FF3 Coherence) ──────────────

TEST(FfiAbiResolve, ResolveAbiAcceptsShippedX86_64CcRegisters) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_TRUE(r.has_value())
        << abiResolveErrorKindName(r.error().kind) << ": " << r.error().detail;
    EXPECT_EQ(countCode(rep, DiagnosticCode::F_AbiCcRegistersInconsistent), 0u);
}

TEST(FfiAbiResolve, ResolveAbiAcceptsShippedArm64CcRegisters) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadShipped("elf64-aarch64-linux");
    ASSERT_TRUE(format.has_value());
    DiagnosticReporter rep;
    auto r = resolveAbi(**target, **format, rep);
    ASSERT_TRUE(r.has_value())
        << abiResolveErrorKindName(r.error().kind) << ": " << r.error().detail;
    EXPECT_EQ(countCode(rep, DiagnosticCode::F_AbiCcRegistersInconsistent), 0u);
}

TEST(FfiAbiResolve, SchemaLoaderRejectsPasteErrorRegistersInCc) {
    // Loader-side gate (the FIRST line of defense): a cc row carrying a
    // register name not in target.registers[] fails loadFromText. If this test
    // breaks, the loader-side check was removed; FF3's defense-in-depth still
    // catches.
    auto badTarget = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"x86_64","version":"0.0","abiModel":"register-machine"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ],
      "registers": [
        {"name":"rdi","class":"gpr","widthBytes":8,"hwEncoding":7},
        {"name":"rsp","class":"gpr","widthBytes":8,"hwEncoding":4}
      ],
      "callingConventions": [
        {
          "name":"sysv_amd64",
          "argGprs":["rdi","x0_paste_error_not_in_register_table"],
          "argFprs":[], "returnGprs":["rdi"], "returnFprs":[],
          "callerSaved":[], "calleeSaved":[],
          "stackAlignment":16, "stackPointer":"rsp"
        }
      ]
    })");
    EXPECT_FALSE(badTarget.has_value())
        << "TargetSchema loader must reject a cc carrying a register "
           "name absent from target.registers[].";
}

// ── (H) Diagnostic code name round-trips ────────────────────────────────────

TEST(FfiAbiResolve, DiagnosticCodeNameRoundTripFAbiUnknownTuple) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_AbiUnknownTuple),
              "F_AbiUnknownTuple");
}
TEST(FfiAbiResolve, DiagnosticCodeNameRoundTripFAbiNoMatchingCcInTarget) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_AbiNoMatchingCcInTarget),
              "F_AbiNoMatchingCcInTarget");
}
TEST(FfiAbiResolve, DiagnosticCodeNameRoundTripFAbiFormatAbiModelMismatch) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_AbiFormatAbiModelMismatch),
              "F_AbiFormatAbiModelMismatch");
}
TEST(FfiAbiResolve, DiagnosticCodeNameRoundTripFAbiCcRegistersInconsistent) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_AbiCcRegistersInconsistent),
              "F_AbiCcRegistersInconsistent");
}

TEST(FfiAbiResolve, AbiResolveErrorKindNameRoundTrip) {
    EXPECT_EQ(abiResolveErrorKindName(AbiResolveErrorKind::UnknownTuple),
              "UnknownTuple");
    EXPECT_EQ(abiResolveErrorKindName(AbiResolveErrorKind::NoMatchingCcInTarget),
              "NoMatchingCcInTarget");
    EXPECT_EQ(abiResolveErrorKindName(AbiResolveErrorKind::FormatAbiModelMismatch),
              "FormatAbiModelMismatch");
    EXPECT_EQ(abiResolveErrorKindName(AbiResolveErrorKind::CcRegistersInconsistent),
              "CcRegistersInconsistent");
}
