// P69 fold 3f (ledger 563 (A)): THE PROGRAM INTERPRETER AS A RUNTIME ROLE.
//
// aarch64 glibc's stack-protector canary is exported by the program interpreter, ld-linux-aarch64.so.1, not by
// libc.so.6 (MEASURED, P69 fold 3f: `readelf -V` of gcc's aarch64 PIE files `__stack_chk_guard@GLIBC_2.17` under
// the interpreter), so a program IMPORTS from the interpreter and a shipped row must be able to name it: the
// `programInterpreter` role. These cells pin (1) every ELF flavour answering the role with ITS arch's interpreter --
// the exec and pie documents from their own row, the relocatable, dyn and staticlib ones from their (kind, arch)
// family; and (2) the one-fact rule: a document that declares the row declares it beside its `elf.interpreter`
// path, the row's image that path's last component.

#include "core/types/object_format_kind.hpp"
#include "link/object_format_schema.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::string shippedText(std::string_view name) {
    auto const root = dss::test::findConfigRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    std::ifstream in(*root / "object-formats" / (std::string{name} + ".format.json"), std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

template <class Loaded>
[[nodiscard]] std::string messagesOf(Loaded const& loaded) {
    std::string s;
    if (loaded.has_value()) return "\n  <loaded>";
    for (auto const& d : loaded.error()) s += "\n  " + d.message;
    return s;
}

[[nodiscard]] std::string replacedOnce(std::string text, std::string_view old, std::string_view now) {
    auto const at = text.find(old);
    EXPECT_NE(at, std::string::npos) << "the shipped document no longer holds: " << old;
    EXPECT_EQ(text.find(old, at == std::string::npos ? 0 : at + 1), std::string::npos) << "twice: " << old;
    if (at != std::string::npos) text.replace(at, old.size(), now);
    return text;
}

}  // namespace

TEST(ProgramInterpreterRole, EveryElfFlavourAnswersTheRoleWithItsArchsInterpreter) {
    struct Case {
        char const* flavour;
        char const* image;
    };
    std::vector<Case> const cases{
        {"elf64-aarch64-linux-exec", "ld-linux-aarch64.so.1"}, {"elf64-aarch64-linux-pie", "ld-linux-aarch64.so.1"},
        {"elf64-aarch64-linux", "ld-linux-aarch64.so.1"},      {"elf64-aarch64-linux-dyn", "ld-linux-aarch64.so.1"},
        {"elf64-aarch64-linux-staticlib", "ld-linux-aarch64.so.1"},
        {"elf64-x86_64-linux-exec", "ld-linux-x86-64.so.2"},   {"elf64-x86_64-linux-pie", "ld-linux-x86-64.so.2"},
        {"elf64-x86_64-linux", "ld-linux-x86-64.so.2"},        {"elf64-x86_64-linux-dyn", "ld-linux-x86-64.so.2"},
        {"elf64-x86_64-linux-staticlib", "ld-linux-x86-64.so.2"},
    };
    for (Case const& c : cases) {
        auto const loaded = ObjectFormatSchema::loadShipped(c.flavour);
        ASSERT_TRUE(loaded.has_value()) << c.flavour << messagesOf(loaded);
        FormatRuntimeLibraryRoleResolver const resolver{**loaded};
        std::string refusal;
        auto const* const row = resolver.rowForRole(RuntimeLibraryRole::ProgramInterpreter, refusal);
        EXPECT_TRUE(refusal.empty())
            << c.flavour << ": the family must answer the role with its own arch's interpreter alone -- a family "
               "keyed by the format kind alone meets two interpreters: " << refusal;
        ASSERT_NE(row, nullptr) << c.flavour << " answers no programInterpreter row";
        EXPECT_EQ(row->image, c.image) << c.flavour;
    }
}

TEST(ProgramInterpreterRole, ARoleImageThatIsNotTheInterpretersLastComponentIsRefused) {
    std::string const text = replacedOnce(shippedText("elf64-aarch64-linux-exec"),
                                          R"("role": "programInterpreter", "image": "ld-linux-aarch64.so.1")",
                                          R"("role": "programInterpreter", "image": "ld-linux-x86-64.so.2")");
    auto const loaded = ObjectFormatSchema::loadFromText(text, "mismatched-interpreter");
    ASSERT_FALSE(loaded.has_value()) << "a role image that is not the interpreter path's last component must be "
                                        "refused";
    EXPECT_NE(messagesOf(loaded).find("is not the last component of 'elf.interpreter'"), std::string::npos)
        << "the refusal must name the two images" << messagesOf(loaded);
}

TEST(ProgramInterpreterRole, ARoleRowWithoutAnInterpreterPathIsRefused) {
    std::string const text =
        replacedOnce(shippedText("elf64-aarch64-linux-dyn"), R"("runtimeLibraries": [)",
                     R"("runtimeLibraries": [ { "role": "programInterpreter", "image": "ld-linux-aarch64.so.1" },)");
    auto const loaded = ObjectFormatSchema::loadFromText(text, "interpreter-row-without-a-path");
    ASSERT_FALSE(loaded.has_value()) << "a programInterpreter row on a document with no interpreter path must be "
                                        "refused";
    EXPECT_NE(messagesOf(loaded).find("is declared beside the path it names"), std::string::npos)
        << messagesOf(loaded);
}
