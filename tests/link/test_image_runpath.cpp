// D-LK-IMAGE-CANNOT-DECLARE-A-RUNPATH — the runpath vocabulary, its format
// declarations, the loader's rules over them, and the request gate.
//
// What each group pins, and the red-on-disable lever it answers to:
//   * RunpathRules — the three rules `link/runpath.hpp` owns, once each: the
//     GATE rule (only an empty entry or one holding a NUL byte is refused), the
//     PORTABLE rule (the project manifest's) and the TRANSLATION (a LEADING
//     `${ORIGIN}` takes the format's spelling, every other entry is verbatim,
//     an exact duplicate is dropped).
//   * RunpathDeclarations — the one rule set over a declaration's VALUES, and
//     the shipped documents: exactly the ten ELF/Mach-O image documents
//     declare a carrier, the two PE image documents declare the reason, and
//     nothing else declares either.
//   * RunpathLoader — every load-time refusal, each asserted at its own JSON
//     pointer with its own message, on the SHIPPED document with one splice.
//   * RunpathGate — `enforceImageRequest` (refuse) vs `reportUnrecordedRunpaths`
//     (warn), and the writers' belt `runpathsToRecord`.
// The bytes each writer emits are pinned through the real driver in
// tests/program/test_image_runpath_build.cpp.

#include "asm/asm.hpp"                        // AssembledModule — the walkers' direct input
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/unsuppressable_codes.hpp"
#include "diagnostic_count.hpp"
#include "format_reject_support.hpp"
#include "link/format/elf.hpp"
#include "link/format/macho.hpp"
#include "link/format/pe.hpp"
#include "link/image_request.hpp"
#include "link/object_format_backend.hpp"
#include "link/object_format_schema.hpp"
#include "link/runpath.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using dss::link_format::test::countAtPath;
using dss::link_format::test::countWithMessage;
using dss::link_format::test::rejectSummary;
using dss::test_support::countCode;

namespace {

[[nodiscard]] std::shared_ptr<ObjectFormatSchema>
shipped(std::string_view name) {
    auto r = ObjectFormatSchema::loadShipped(name);
    if (!r.has_value()) {
        ADD_FAILURE() << "shipped format '" << name
                      << "' did not load: " << rejectSummary(r);
        return nullptr;
    }
    return *r;
}

[[nodiscard]] std::string shippedText(std::string_view name) {
    auto const p = dss::test::configRoot() / "object-formats"
                 / (std::string{name} + ".format.json");
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

// The SHIPPED document with one edit applied to its parsed JSON, re-loaded —
// the fixture is the real file, so it cannot drift from what ships.
template <typename Edit>
[[nodiscard]] dss::link_format::test::FormatLoadResult
loadEdited(std::string_view name, Edit&& edit) {
    auto doc = nlohmann::json::parse(shippedText(name));
    edit(doc);
    return ObjectFormatSchema::loadFromText(doc.dump(), std::string{name});
}

[[nodiscard]] RunpathDeclaration elfDecl() {
    auto s = shipped("elf64-x86_64-linux-exec");
    return s && s->runpath() ? *s->runpath() : RunpathDeclaration{};
}
[[nodiscard]] RunpathDeclaration machoDecl() {
    auto s = shipped("macho64-arm64-darwin-exec");
    return s && s->runpath() ? *s->runpath() : RunpathDeclaration{};
}

} // namespace

// ── THE GATE RULE ───────────────────────────────────────────────────────────

TEST(RunpathRules, GateRefusesOnlyTheEmptyEntryAndANulByte) {
    auto const empty = runpathEntryRefusal("");
    ASSERT_TRUE(empty.has_value()) << "an empty entry names no directory";
    // The refusal carries the MEASURED ground, not a bare verdict.
    EXPECT_NE(empty->find("127"), std::string::npos) << *empty;
    EXPECT_NE(empty->find("ld.lld"), std::string::npos) << *empty;

    std::string const withNul{"/opt/a\0b", 8};
    auto const nul = runpathEntryRefusal(withNul);
    ASSERT_TRUE(nul.has_value());
    EXPECT_NE(nul->find("NUL"), std::string::npos) << *nul;

    // gcc's LITERAL semantics: every other string is the user's to write,
    // loader-specific tokens and `:` lists included.
    for (std::string_view ok : {"$ORIGIN", "${ORIGIN}", "$LIB/x", "${PLATFORM}",
                                "@loader_path/../lib", "relative/dir",
                                "/opt/a:/opt/b", "${ORIGIN}x", "/x/${ORIGIN}"}) {
        EXPECT_FALSE(runpathEntryRefusal(ok).has_value()) << ok;
    }
}

// ── THE PORTABLE RULE ───────────────────────────────────────────────────────

TEST(RunpathRules, PortableRuleAcceptsAbsoluteAndOriginRootedEntries) {
    for (std::string_view ok : {"/opt/app/lib", "/", "${ORIGIN}", "${ORIGIN}/",
                                "${ORIGIN}/../lib", "${ORIGIN}/deps/x y"}) {
        auto const why = portableRunpathEntryRefusal(ok);
        EXPECT_FALSE(why.has_value()) << ok << ": " << why.value_or("");
    }
}

TEST(RunpathRules, PortableRuleRefusesEveryNonPortableShapeAndIsNeverADeadEnd) {
    struct Case { std::string_view entry; std::string_view says; };
    Case const cases[] = {
        {"lib",                 "RELATIVE"},
        {"C:\\opt\\lib",        "RELATIVE"},
        {"$ORIGIN",             "'$'"},
        {"$ORIGIN/../lib",      "'$'"},
        {"/opt/$LIB",           "'$'"},
        {"${ORIGIN}/${LIB}",    "'$'"},
        {"@loader_path",        "begins with '@'"},
        {"@executable_path/..", "begins with '@'"},
        {"${ORIGIN}x",          "continues '${ORIGIN}'"},
        {"/opt/a:/opt/b",       "':'"},
        {"",                    "EMPTY"},
    };
    for (auto const& c : cases) {
        auto const why = portableRunpathEntryRefusal(c.entry);
        ASSERT_TRUE(why.has_value()) << "must refuse '" << c.entry << "'";
        EXPECT_NE(why->find(c.says), std::string::npos)
            << "'" << c.entry << "' refused for the wrong reason: " << *why;
        if (!c.entry.empty()) {
            // Not a dead end: the refusal names what it refused AND where a
            // loader-specific spelling is still accepted.
            EXPECT_NE(why->find(std::string{"'"} + std::string{c.entry} + "'"),
                      std::string::npos) << *why;
            EXPECT_NE(why->find("--rpath"), std::string::npos) << *why;
            EXPECT_NE(why->find("$LIB"), std::string::npos) << *why;
        }
    }
}

// ── THE TRANSLATION ─────────────────────────────────────────────────────────

TEST(RunpathRules, LeadingOriginTakesEachFormatsSpellingAndDuplicatesDrop) {
    auto const elf = elfDecl();
    auto const macho = machoDecl();
    ASSERT_EQ(elf.origin, "$ORIGIN");
    ASSERT_EQ(macho.origin, "@loader_path");

    std::vector<std::string> const req{"${ORIGIN}", "/opt/a", "${ORIGIN}/../lib",
                                       "/opt/a", "${ORIGIN}"};
    EXPECT_EQ(recordedRunpaths(req, elf),
              (std::vector<std::string>{"$ORIGIN", "/opt/a", "$ORIGIN/../lib"}));
    EXPECT_EQ(recordedRunpaths(req, macho),
              (std::vector<std::string>{"@loader_path", "/opt/a",
                                        "@loader_path/../lib"}));
    // A duplicate is judged AFTER translation: the literal `$ORIGIN` a gcc user
    // types and the portable token are the same directory on ELF.
    EXPECT_EQ(recordedRunpaths(std::vector<std::string>{"$ORIGIN", "${ORIGIN}"},
                               elf),
              (std::vector<std::string>{"$ORIGIN"}));
}

TEST(RunpathRules, TheTokenIsTranslatedOnlyAsTheLeadingComponent) {
    auto const elf = elfDecl();
    auto const macho = machoDecl();
    // Measured: glibc leaves `$ORIGINx` unexpanded (exit 127) while
    // `${ORIGIN}x` finds `binx` (42) — a rewrite there would change meaning,
    // and dyld expands `@loader_path` only before '/' or the end. So every
    // other position passes through exactly as gcc and ld64 would record it.
    for (std::string_view verbatim : {"${ORIGIN}x", "/x/${ORIGIN}",
                                      "${ORIGIN}${ORIGIN}", "$ORIGIN"}) {
        std::vector<std::string> const one{std::string{verbatim}};
        EXPECT_EQ(recordedRunpaths(one, elf), one) << verbatim;
        EXPECT_EQ(recordedRunpaths(one, macho), one) << verbatim;
    }
}

// ── THE DECLARATIONS ────────────────────────────────────────────────────────

TEST(RunpathDeclarations, TheOneRuleSetFlagsEachBadValueAtItsOwnKey) {
    ASSERT_TRUE(runpathDeclarationProblems(elfDecl()).empty());
    ASSERT_TRUE(runpathDeclarationProblems(machoDecl()).empty());

    auto const keysOf = [](RunpathDeclaration const& d) {
        std::vector<std::string> keys;
        for (auto const& p : runpathDeclarationProblems(d))
            keys.emplace_back(p.key);
        return keys;
    };
    auto d = elfDecl();
    d.dynamicTag = 1;   // DT_NEEDED — a string tag, but not a search path
    EXPECT_EQ(keysOf(d), std::vector<std::string>{"dynamicTag"});
    d = elfDecl();
    d.dynamicTag = kElfDtRpath;   // the old-dtags spelling is legitimate
    EXPECT_TRUE(keysOf(d).empty());
    d = elfDecl();
    d.separator = "";
    EXPECT_EQ(keysOf(d), std::vector<std::string>{"separator"});
    d = elfDecl();
    d.origin = "$OR:IGIN";   // would be split by the loader
    EXPECT_EQ(keysOf(d), std::vector<std::string>{"origin"});
    d = elfDecl();
    d.origin = "";
    EXPECT_EQ(keysOf(d), std::vector<std::string>{"origin"});

    auto m = machoDecl();
    m.loadCommand = 0x0Cu;   // LC_LOAD_DYLIB — a path command, not a search path
    EXPECT_EQ(keysOf(m), std::vector<std::string>{"loadCommand"});
    m = machoDecl();
    m.separator = ":";       // dyld never joins, so a separator is inert config
    EXPECT_EQ(keysOf(m), std::vector<std::string>{"separator"});

    RunpathDeclaration unset;
    unset.origin = "$ORIGIN";
    EXPECT_EQ(keysOf(unset), std::vector<std::string>{"carrier"})
        << "a default-constructed declaration must not read as an ELF one";
}

TEST(RunpathDeclarations, ExactlyTheImageDocumentsWhoseWalkerRecordsOneDeclareIt) {
    auto const dir = dss::test::configRoot() / "object-formats";
    std::size_t carriers = 0, reasons = 0, seen = 0;
    for (auto const& e : fs::directory_iterator{dir}) {
        auto const file = e.path().filename().string();
        if (!file.ends_with(".format.json")) continue;
        ++seen;
        auto const name = file.substr(0, file.size() - std::string{".format.json"}.size());
        auto const s = shipped(name);
        ASSERT_NE(s, nullptr) << name;
        bool const walkerRecords =
            s->backend() != nullptr && !s->backend()->runpathCarriers().empty();
        bool const expectCarrier = walkerRecords && s->isImageFlavor();
        EXPECT_EQ(s->runpath().has_value(), expectCarrier) << name;
        if (s->runpath().has_value()) {
            ++carriers;
            EXPECT_TRUE(runpathDeclarationProblems(*s->runpath()).empty()) << name;
        }
        // The reason is declared by the image documents that record NONE.
        bool const expectReason = !walkerRecords && s->isImageFlavor();
        EXPECT_EQ(s->runpathUnsupportedReason().has_value(), expectReason) << name;
        if (s->runpathUnsupportedReason().has_value()) {
            ++reasons;
            EXPECT_EQ(*s->runpathUnsupportedReason(),
                      RunpathUnsupportedReason::ApplicationDirectorySearch)
                << name;
        }
    }
    EXPECT_EQ(seen, 24u) << "the sweep must see every shipped document";
    EXPECT_EQ(carriers, 10u) << "ELF exec/pie/dyn x2 + Mach-O exec/dylib x2";
    EXPECT_EQ(reasons, 2u) << "the pe64 exec and dll documents";
}

TEST(RunpathDeclarations, ShippedValuesAreTheMeasuredOnes) {
    for (std::string_view n : {"elf64-x86_64-linux-exec", "elf64-x86_64-linux-pie",
                               "elf64-x86_64-linux-dyn", "elf64-aarch64-linux-exec",
                               "elf64-aarch64-linux-pie", "elf64-aarch64-linux-dyn"}) {
        auto const s = shipped(n);
        ASSERT_TRUE(s && s->runpath().has_value()) << n;
        EXPECT_EQ(s->runpath()->carrier, RunpathCarrier::ElfDynamicEntry) << n;
        EXPECT_EQ(s->runpath()->dynamicTag, kElfDtRunpath)
            << n << ": GNU ld and ld.lld both default to DT_RUNPATH";
        EXPECT_EQ(s->runpath()->separator, ":") << n;
        EXPECT_EQ(s->runpath()->origin, "$ORIGIN") << n;
    }
    for (std::string_view n : {"macho64-arm64-darwin-exec", "macho64-arm64-darwin-dylib",
                               "macho64-x86_64-darwin-exec", "macho64-x86_64-darwin-dylib"}) {
        auto const s = shipped(n);
        ASSERT_TRUE(s && s->runpath().has_value()) << n;
        EXPECT_EQ(s->runpath()->carrier, RunpathCarrier::MachoLoadCommand) << n;
        EXPECT_EQ(s->runpath()->loadCommand, kMachoLcRpath) << n;
        EXPECT_TRUE(s->runpath()->separator.empty()) << n;
        EXPECT_EQ(s->runpath()->origin, "@loader_path") << n;
    }
}

TEST(RunpathDeclarations, EachBackendAnswersForItsOwnWalker) {
    std::size_t answered = 0;
    for (auto const* b : link::objectFormatBackendTable()) {
        auto const carriers = b->runpathCarriers();
        std::vector<RunpathCarrier> got(carriers.begin(), carriers.end());
        if (b->configName() == "elf") {
            EXPECT_EQ(got, std::vector<RunpathCarrier>{RunpathCarrier::ElfDynamicEntry});
        } else if (b->configName() == "macho") {
            EXPECT_EQ(got, std::vector<RunpathCarrier>{RunpathCarrier::MachoLoadCommand});
        } else {
            EXPECT_TRUE(got.empty()) << b->configName()
                << ": records no runpath (PE's loader searches the application "
                   "directory; wasm and spirv are not loaded by an OS loader)";
        }
        ++answered;
    }
    EXPECT_EQ(answered, 5u);
}

// ── THE LOADER'S REFUSALS, ONE SPLICE EACH ON THE SHIPPED DOCUMENT ─────────

TEST(RunpathLoader, ADeclarationOnANonImageFormatIsRefused) {
    auto const r = loadEdited("elf64-x86_64-linux", [](nlohmann::json& d) {
        d["runpath"] = {{"carrier", "elf-dynamic-entry"}, {"dynamicTag", 29},
                        {"separator", ":"}, {"origin", "$ORIGIN"}};
    });
    ASSERT_FALSE(r.has_value()) << "a relocatable object is never loaded";
    EXPECT_EQ(countWithMessage(r, "not an IMAGE flavor"), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/runpath/"), 0u) << rejectSummary(r);
}

TEST(RunpathLoader, ACarrierAnotherBackendWritesIsRefused) {
    auto const r = loadEdited("elf64-x86_64-linux-exec", [](nlohmann::json& d) {
        d["runpath"] = {{"carrier", "macho-load-command"},
                        {"loadCommand", 2147483676u}, {"origin", "@loader_path"}};
    });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(countAtPath(r, "/runpath/carrier"), 1u) << rejectSummary(r);
    EXPECT_EQ(countWithMessage(r, "recorded by the 'macho' walker"), 1u)
        << rejectSummary(r);
}

TEST(RunpathLoader, AKeyOfTheOtherCarrierIsNamedAsSuch) {
    auto const r = loadEdited("elf64-x86_64-linux-exec", [](nlohmann::json& d) {
        d["runpath"]["loadCommand"] = 2147483676u;
    });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(countAtPath(r, "/runpath/loadCommand"), 1u) << rejectSummary(r);
    EXPECT_EQ(countWithMessage(r, "macho-load-command"), 1u) << rejectSummary(r);
}

TEST(RunpathLoader, ATypoedKeyAndAnUnknownCarrierAreRefused) {
    auto const typo = loadEdited("elf64-x86_64-linux-exec", [](nlohmann::json& d) {
        d["runpath"]["seperator"] = ":";
    });
    EXPECT_EQ(countAtPath(typo, "/runpath/seperator"), 1u) << rejectSummary(typo);

    auto const unknown = loadEdited("elf64-x86_64-linux-exec", [](nlohmann::json& d) {
        d["runpath"]["carrier"] = "elf-dynamic";
    });
    EXPECT_EQ(countAtPath(unknown, "/runpath/carrier"), 1u) << rejectSummary(unknown);
    EXPECT_EQ(countWithMessage(unknown, "'elf-dynamic-entry', 'macho-load-command'"), 1u)
        << "the accepted set is PROJECTED from the table: " << rejectSummary(unknown);
}

TEST(RunpathLoader, ABadValueIsRefusedThroughTheOneRuleSet) {
    auto const r = loadEdited("elf64-aarch64-linux-dyn", [](nlohmann::json& d) {
        d["runpath"]["dynamicTag"] = 1;
    });
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(countAtPath(r, "/runpath/dynamicTag"), 1u) << rejectSummary(r);
    auto const m = loadEdited("macho64-x86_64-darwin-dylib", [](nlohmann::json& d) {
        d["runpath"]["loadCommand"] = 12;
    });
    EXPECT_EQ(countAtPath(m, "/runpath/loadCommand"), 1u) << rejectSummary(m);
    auto const missing = loadEdited("macho64-arm64-darwin-exec", [](nlohmann::json& d) {
        d["runpath"].erase("origin");
    });
    EXPECT_EQ(countAtPath(missing, "/runpath/origin"), 1u) << rejectSummary(missing);
}

TEST(RunpathLoader, TheReasonIsExclusiveWithACarrierAndImageOnly) {
    auto const both = loadEdited("elf64-x86_64-linux-exec", [](nlohmann::json& d) {
        d["runpathUnsupportedReason"] = "application-directory-search";
    });
    ASSERT_FALSE(both.has_value());
    EXPECT_EQ(countAtPath(both, "/runpathUnsupportedReason"), 1u)
        << rejectSummary(both);
    EXPECT_EQ(countWithMessage(both, "BOTH 'runpath'"), 1u) << rejectSummary(both);

    auto const onObject = loadEdited("pe64-x86_64-windows", [](nlohmann::json& d) {
        d["runpathUnsupportedReason"] = "application-directory-search";
    });
    EXPECT_EQ(countWithMessage(onObject, "not an IMAGE flavor"), 1u)
        << rejectSummary(onObject);

    auto const unknownVerb = loadEdited("pe64-x86_64-windows-dll", [](nlohmann::json& d) {
        d["runpathUnsupportedReason"] = "app-dir";
    });
    EXPECT_EQ(countAtPath(unknownVerb, "/runpathUnsupportedReason"), 1u)
        << rejectSummary(unknownVerb);
}

// ── THE GATE, THE WARNING, AND THE WRITERS' BELT ───────────────────────────

TEST(RunpathGate, EnforceRefusesAnUnrecordableEntryOnEveryFormat) {
    for (std::string_view n : {"elf64-x86_64-linux-exec", "pe64-x86_64-windows-exec",
                               "macho64-arm64-darwin-dylib", "elf64-x86_64-linux"}) {
        auto const s = shipped(n);
        ASSERT_NE(s, nullptr);
        ImageRequest req;
        req.runpaths = {"/opt/a", ""};
        DiagnosticReporter rep;
        EXPECT_FALSE(enforceImageRequest(req, *s, "test", rep)) << n;
        EXPECT_EQ(countCode(rep, DiagnosticCode::K_InvalidRunpathRequest), 1u) << n;
        EXPECT_EQ(rep.errorCount(), 1u) << n;
        EXPECT_EQ(countCode(rep, DiagnosticCode::K_FormatLacksRunpath), 0u)
            << n << ": the gate's refusal is not the format's warning";
    }
}

TEST(RunpathGate, EnforceIsAPurePredicateOnSuccess) {
    // gcc's literal strings pass, and nothing is reported — so the linker gate
    // and a public walker may both run it on one path without echoing.
    auto const pe = shipped("pe64-x86_64-windows-exec");
    ASSERT_NE(pe, nullptr);
    ImageRequest req;
    req.runpaths = {"$LIB", "${PLATFORM}/x", "rel", "/a:/b", "${ORIGIN}"};
    ASSERT_FALSE(req.empty()) << "runpaths alone make a request non-empty";
    DiagnosticReporter rep;
    EXPECT_TRUE(enforceImageRequest(req, *pe, "test", rep));
    EXPECT_TRUE(enforceImageRequest(req, *pe, "test", rep));
    EXPECT_TRUE(rep.all().empty());
}

TEST(RunpathGate, AFormatThatRecordsNoneWarnsOnceAndSaysWhereItsLoaderLooks) {
    ImageRequest req;
    req.runpaths = {"${ORIGIN}", "/opt/a"};

    for (std::string_view n : {"pe64-x86_64-windows-exec", "pe64-x86_64-windows-dll"}) {
        auto const s = shipped(n);
        ASSERT_NE(s, nullptr);
        DiagnosticReporter rep;
        EXPECT_TRUE(reportUnrecordedRunpaths(req, *s, "test", rep)) << n;
        ASSERT_EQ(rep.all().size(), 1u) << n;
        auto const& d = rep.all().front();
        EXPECT_EQ(d.code, DiagnosticCode::K_FormatLacksRunpath);
        EXPECT_EQ(d.severity, DiagnosticSeverity::Warning);
        EXPECT_NE(d.actual.find("APPLICATION was loaded from"), std::string::npos)
            << d.actual;
        EXPECT_NE(d.actual.find("declared reason: 'application-directory-search'"),
                  std::string::npos) << d.actual;
        EXPECT_NE(d.actual.find("2 runpath entries were requested"), std::string::npos)
            << d.actual;
    }
    for (std::string_view n : {"elf64-x86_64-linux", "macho64-arm64-darwin-staticlib"}) {
        auto const s = shipped(n);
        ASSERT_NE(s, nullptr);
        DiagnosticReporter rep;
        EXPECT_TRUE(reportUnrecordedRunpaths(req, *s, "test", rep)) << n;
        ASSERT_EQ(rep.all().size(), 1u) << n;
        EXPECT_NE(rep.all().front().actual.find("object file or an archive"),
                  std::string::npos) << rep.all().front().actual;
    }
    for (std::string_view n : {"elf64-x86_64-linux-exec", "macho64-x86_64-darwin-dylib"}) {
        auto const s = shipped(n);
        ASSERT_NE(s, nullptr);
        DiagnosticReporter rep;
        EXPECT_FALSE(reportUnrecordedRunpaths(req, *s, "test", rep))
            << n << " records runpaths — nothing to report";
        EXPECT_TRUE(rep.all().empty()) << n;
    }
    auto const pe = shipped("pe64-x86_64-windows-exec");
    ASSERT_NE(pe, nullptr);
    DiagnosticReporter quiet;
    EXPECT_FALSE(reportUnrecordedRunpaths(ImageRequest{}, *pe, "test", quiet));
    EXPECT_TRUE(quiet.all().empty()) << "no request, nothing to say";
}

TEST(RunpathGate, TheWritersBeltRefusesADeclarationItsWalkerCannotRecord) {
    auto const elf = shipped("elf64-x86_64-linux-exec");
    ASSERT_NE(elf, nullptr);
    ImageRequest req;
    req.runpaths = {"${ORIGIN}", "${ORIGIN}"};

    DiagnosticReporter ok;
    auto const recorded = runpathsToRecord(req, *elf, RunpathCarrier::ElfDynamicEntry,
                                           "test", ok);
    ASSERT_TRUE(recorded.has_value());
    EXPECT_EQ(*recorded, std::vector<std::string>{"$ORIGIN"});
    EXPECT_TRUE(ok.all().empty());

    // The ELF declaration handed to a walker that records LC_RPATH.
    DiagnosticReporter rep;
    EXPECT_FALSE(runpathsToRecord(req, *elf, RunpathCarrier::MachoLoadCommand,
                                  "test", rep).has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_NoMatchingObjectFormat), 1u);

    // A format with no declaration records nothing, without a word (the gate
    // has already warned).
    auto const pe = shipped("pe64-x86_64-windows-exec");
    ASSERT_NE(pe, nullptr);
    DiagnosticReporter silent;
    auto const none = runpathsToRecord(req, *pe, RunpathCarrier::ElfDynamicEntry,
                                       "test", silent);
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none->empty());
    EXPECT_TRUE(silent.all().empty());
}

TEST(RunpathGate, TheTwoCodesAndTheirSuppressibility) {
    EXPECT_EQ(static_cast<unsigned>(DiagnosticCode::K_FormatLacksRunpath), 0x8027u);
    EXPECT_EQ(static_cast<unsigned>(DiagnosticCode::K_InvalidRunpathRequest), 0x8028u);
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::K_FormatLacksRunpath),
              "K_FormatLacksRunpath");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::K_InvalidRunpathRequest),
              "K_InvalidRunpathRequest");
    // The warning hides a request that had no effect and ships the reference's
    // own bytes; the refusal is the only line explaining a failed link.
    EXPECT_FALSE(isUnsuppressable(DiagnosticCode::K_FormatLacksRunpath));
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::K_InvalidRunpathRequest));
}

// ── THE WALKERS REACHED DIRECTLY — the doors that bypass the linker gate ────
//
// `elf::encode`, `macho::encode` and `pe::encode` are public entry points with
// direct callers in `tests/`, so each runs `enforceImageRequest` itself; and
// the two carrier-declaring writers each own an arm that writes an image with
// NO dynamic information, where a declared runpath has nothing to carry it.

namespace {

[[nodiscard]] std::shared_ptr<TargetSchema> target(std::string_view name) {
    auto r = TargetSchema::loadShipped(name);
    if (!r.has_value()) {
        ADD_FAILURE() << "shipped target '" << name << "' did not load";
        return nullptr;
    }
    return *r;
}

// One function, no import: the shape every static arm is reached with. The
// entry override says the module was not trampolined on purpose — these calls
// bypass `linker::link`, which is what would otherwise inject the entry.
[[nodiscard]] AssembledModule oneFunction(std::vector<std::uint8_t> body) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = std::move(body);
    mod.functions.push_back(std::move(fn));
    mod.imageEntryOverride = 0u;
    return mod;
}

} // namespace

TEST(RunpathWalkers, EachWalkerRefusesAnUnrecordableEntryWhenReachedDirectly) {
    ImageRequest bad;
    bad.runpaths = {""};
    auto const x64 = target("x86_64");
    auto const a64 = target("arm64");
    ASSERT_TRUE(x64 && a64);
    auto const mod = oneFunction({0xC3});
    struct Door {
        std::string_view format;
        std::shared_ptr<TargetSchema> tgt;
        std::vector<std::uint8_t> (*encode)(AssembledModule const&, TargetSchema const&,
                                             ObjectFormatSchema const&, DiagnosticReporter&,
                                             ImageRequest const&);
    };
    Door const doors[] = {
        {"elf64-x86_64-linux-exec",   x64, &elf::encode},
        {"macho64-arm64-darwin-exec", a64, &macho::encode},
        {"pe64-x86_64-windows-exec",  x64, &pe::encode},
    };
    for (auto const& d : doors) {
        auto const f = shipped(d.format);
        ASSERT_NE(f, nullptr);
        DiagnosticReporter rep;
        auto const bytes = d.encode(mod, *d.tgt, *f, rep, bad);
        EXPECT_TRUE(bytes.empty()) << d.format << ": a refused request writes nothing";
        EXPECT_EQ(countCode(rep, DiagnosticCode::K_InvalidRunpathRequest), 1u)
            << d.format << ": the walker must refuse it itself — nothing else "
                           "stands between a direct caller and the bytes";
    }
}

TEST(RunpathWalkers, AnElfImageWithNoDynamicSectionSaysSo) {
    auto const x64 = target("x86_64");
    auto const fmt = shipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(x64 && fmt);
    ImageRequest req;
    req.runpaths = {"${ORIGIN}"};
    DiagnosticReporter rep;
    auto const bytes = elf::encode(oneFunction({0xC3}), *x64, *fmt, rep, req);
    ASSERT_FALSE(bytes.empty()) << "the image is still written";
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_FormatLacksRunpath), 1u)
        << "a module with no import is written WITHOUT a dynamic section, where "
           "nothing can carry the runpath — said, never dropped";
    ASSERT_FALSE(rep.all().empty());
    EXPECT_NE(rep.all().front().actual.find("WITHOUT a dynamic section"),
              std::string::npos) << rep.all().front().actual;
}

TEST(RunpathWalkers, AMachOExecutableWithNoDyldInformationSaysSo) {
    // Every shipped Mach-O exec document asks for a code signature and an
    // LC_BUILD_VERSION, both of which the static arm refuses outright (they
    // live in the dynamic writer's __LINKEDIT / load commands), so the arm is
    // reached through the SHIPPED document minus those two requests — a real
    // document, not a copy.
    auto const a64 = target("arm64");
    ASSERT_NE(a64, nullptr);
    auto const loaded = loadEdited("macho64-arm64-darwin-exec", [](nlohmann::json& d) {
        d["image"].erase("codeSignature");
        d["image"].erase("buildVersion");
    });
    ASSERT_TRUE(loaded.has_value()) << rejectSummary(loaded);
    ImageRequest req;
    req.runpaths = {"${ORIGIN}", "/opt/a"};
    DiagnosticReporter rep;
    auto const bytes = macho::encode(oneFunction({0xC0, 0x03, 0x5F, 0xD6}),  // ret
                                     *a64, **loaded, rep, req);
    std::string diags;
    for (auto const& d : rep.all()) {
        diags += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    }
    ASSERT_FALSE(bytes.empty()) << "the image is still written" << diags;
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_FormatLacksRunpath), 1u)
        << "an executable that imports nothing carries no dyld information, so "
           "an LC_RPATH would be read by nothing — said, never dropped";
}
