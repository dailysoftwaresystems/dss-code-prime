// D-CONFIG-FORMAT-IDENTITY-BLOCKS-ACCEPTED-ANY-KEY (P69, found by lane `xa` while adding `pe.linkerDirectives`) —
// every IDENTITY block a format backend reads (`pe`, `optionalHeader`, `elf`, `macho`, `image` and the objects
// nested in it) has a CLOSED key set.
//
// WHAT WAS WRONG: the loader refused an unknown ROOT key and an unknown key in each closed root-level block, but the
// blocks a BACKEND reads (`readIdentity`) looked up only the keys they knew and ignored the rest, so a misspelled key
// — `"machnie"`, `"sizeOfStackReserv"`, `"interpeter"`, `"installNmae"` — loaded clean and left the field it names
// at its default. ✔MEASURED 2026-10-01 at this tree before the fix, by the cases below: every one loaded.
//
// Each case mutates a SHIPPED document by exactly one misspelled key and asserts the refusal at the key's own JSON
// pointer; the control is that every shipped document still loads (the loader's own census tests).

#include "core/types/parse_diagnostic.hpp"
#include "link/object_format_schema.hpp"
#include "format_reject_support.hpp"   // countAtPath / countWithMessage / rejectSummary
#include "repo_root.hpp"               // findConfigRoot

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace dss;
using dss::link_format::test::countAtPath;
using dss::link_format::test::countWithMessage;
using dss::link_format::test::rejectSummary;

namespace {

[[nodiscard]] std::string shippedText(char const* stem) {
    auto const root = dss::test::findConfigRoot();
    if (!root.has_value()) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    std::ifstream in{*root / "object-formats" / (std::string{stem} + ".format.json"), std::ios::binary};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct TypoCase {
    char const* label;
    char const* stem;
    void (*mutate)(nlohmann::json&);
    char const* path;
};

}  // namespace

TEST(FormatIdentityBlockKeys, AMisspelledKeyInEveryBackendBlockIsRefusedAtItsOwnPointer) {
    TypoCase const kCases[] = {
        {"pe", "pe64-x86_64-windows", [](nlohmann::json& d) { d["pe"]["machnie"] = 34404; }, "/pe/machnie"},
        {"optionalHeader", "pe64-x86_64-windows-exec",
         [](nlohmann::json& d) { d["optionalHeader"]["sizeOfStackReserv"] = 8388608; },
         "/optionalHeader/sizeOfStackReserv"},
        {"elf", "elf64-x86_64-linux-exec", [](nlohmann::json& d) { d["elf"]["interpeter"] = "/lib/ld.so"; },
         "/elf/interpeter"},
        {"macho", "macho64-arm64-darwin-exec", [](nlohmann::json& d) { d["macho"]["cpuTyp"] = 1; },
         "/macho/cpuTyp"},
        {"image", "macho64-arm64-darwin-dylib", [](nlohmann::json& d) { d["image"]["installNmae"] = "@rpath/x"; },
         "/image/installNmae"},
        {"image.buildVersion", "macho64-arm64-darwin-exec",
         [](nlohmann::json& d) { d["image"]["buildVersion"]["minOS"] = "11.0"; }, "/image/buildVersion/minOS"},
        {"image.codeSignature", "macho64-arm64-darwin-exec",
         [](nlohmann::json& d) { d["image"]["codeSignature"]["hashAlgorithim"] = "sha256"; },
         "/image/codeSignature/hashAlgorithim"},
        {"image.uuid", "macho64-arm64-darwin-exec",
         [](nlohmann::json& d) { d["image"]["uuid"]["derivaton"] = "content"; }, "/image/uuid/derivaton"},
        {"image.loadDylibs row", "macho64-arm64-darwin-exec",
         [](nlohmann::json& d) {
             d["image"]["loadDylibs"] = nlohmann::json::array({nlohmann::json{{"path", "/usr/lib/libSystem.B.dylib"},
                                                                              {"pth", "x"}}});
         },
         "/image/loadDylibs/0/pth"},
    };
    for (auto const& c : kCases) {
        SCOPED_TRACE(c.label);
        auto const text = shippedText(c.stem);
        ASSERT_FALSE(text.empty());
        {
            auto const control = ObjectFormatSchema::loadFromText(text, c.stem);
            ASSERT_TRUE(control.has_value()) << "the unmodified document must load: " << rejectSummary(control);
        }
        nlohmann::json doc = nlohmann::json::parse(text);
        c.mutate(doc);
        auto const r = ObjectFormatSchema::loadFromText(doc.dump(), c.stem);
        EXPECT_FALSE(r.has_value()) << "a misspelled key must be refused, not left at its default";
        EXPECT_GE(countAtPath(r, c.path), 1u) << rejectSummary(r);
        EXPECT_GE(countWithMessage(r, "unknown key"), 1u) << rejectSummary(r);
    }
}

TEST(FormatIdentityBlockKeys, AProseKeyInABackendBlockIsNotAKey) {
    // `$`-prefixed keys are documentation everywhere in the config (`isDocumentationKey`).
    auto const text = shippedText("elf64-x86_64-linux-exec");
    ASSERT_FALSE(text.empty());
    nlohmann::json doc = nlohmann::json::parse(text);
    doc["elf"]["$whyThisInterpreter"] = "prose";
    auto const r = ObjectFormatSchema::loadFromText(doc.dump(), "elf64-x86_64-linux-exec");
    EXPECT_TRUE(r.has_value()) << rejectSummary(r);
}
