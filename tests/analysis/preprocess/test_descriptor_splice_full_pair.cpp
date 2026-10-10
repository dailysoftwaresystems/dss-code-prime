// The descriptor splice selects a preprocessor-visible constant on the FULL pair
// (P69, D-FFI-FCNTL-AARCH64-OPEN-FLAGS-TAKE-X86-64-VALUES).
//
// The splice used to read the `constants` surface with the object format alone, and the
// loader refused every other axis on a visible constant so that the two could not
// disagree. That made a per-ARCH value inexpressible: Linux aarch64's <fcntl.h>
// overrides O_DIRECTORY and O_NOFOLLOW, so fcntl.json keyed them by format and aarch64
// took x86_64's bits — a DSS O_DIRECTORY there is the kernel's O_DIRECT. The splice now
// passes the pair's target, so `#if` sees the arm the semantic tier injects. These pins
// drive the real `preprocess()` entry with each pair's facts, over a scratch descriptor
// and over the real <fcntl.h>.

#include "analysis/preprocess/preprocessor.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/preprocess_config.hpp"
#include "core/types/source_buffer.hpp"

#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema = [] {
        auto loaded = GrammarSchema::loadShipped("c");
        return loaded.has_value() ? *loaded : std::shared_ptr<GrammarSchema const>{};
    }();
    return schema;
}

// `#include <header>` then the branch `#if` takes on the constant, preprocessed for the
// pair (`target`, `format`); an empty target is "no pair named".
struct SpliceRun {
    std::string synth;   // the synth buffer (every spliced `#define`)
    bool        errors = false;
};
[[nodiscard]] SpliceRun spliceFor(std::vector<std::filesystem::path> const& systemDirs,
                                  std::string_view source, std::string_view target,
                                  std::optional<ObjectFormatKind> format) {
    auto buf = SourceBuffer::fromString(std::string{source}, "main.c");
    PredefinedTypeFacts facts;
    facts.targetName = std::string{target};
    PreprocessResult const r =
        preprocess(buf, cLanguage(), {}, kDefaultHeaderNameMatching,
                   DiagnosticBudget::libraryDefault(), systemDirs, format, {}, {}, {},
                   std::nullopt, nullptr, target.empty() ? nullptr : &facts);
    SpliceRun out;
    out.synth  = r.synthBuffer ? std::string{r.synthBuffer->text()} : std::string{};
    out.errors = r.diagnostics->hasErrors();
    return out;
}

}  // namespace

TEST(DescriptorSpliceFullPair, AnArchKeyedConstantReachesIfOnItsOwnTarget) {
    ASSERT_NE(cLanguage(), nullptr);
    ScratchDir dir{Location::Temp, "splice-full-pair"};
    std::filesystem::create_directories(dir.path() / "sys");
    std::ofstream(dir.path() / "sys" / "archk.json", std::ios::binary) << R"({
  "header": "archk.h",
  "constants": [
    { "name": "ARCH_K", "variants": [
      { "when": { "format": "elf", "arch": "x86_64" }, "value": 1, "type": "i32" },
      { "when": { "format": "elf", "arch": "arm64" },  "value": 2, "type": "i32" }
    ] }
  ]
})";
    std::vector<std::filesystem::path> const sys{dir.path() / "sys"};
    std::string_view const source =
        "#include <archk.h>\n"
        "#if ARCH_K == 2\nint on_arm64;\n#elif ARCH_K == 1\nint on_x86_64;\n#else\nint on_neither;\n#endif\n";

    SpliceRun const arm = spliceFor(sys, source, "arm64", ObjectFormatKind::Elf);
    EXPECT_FALSE(arm.errors);
    EXPECT_NE(arm.synth.find("#define ARCH_K 2\n"), std::string::npos) << arm.synth;
    EXPECT_EQ(arm.synth.find("#define ARCH_K 1\n"), std::string::npos) << arm.synth;

    SpliceRun const x86 = spliceFor(sys, source, "x86_64", ObjectFormatKind::Elf);
    EXPECT_FALSE(x86.errors);
    EXPECT_NE(x86.synth.find("#define ARCH_K 1\n"), std::string::npos) << x86.synth;
    EXPECT_EQ(x86.synth.find("#define ARCH_K 2\n"), std::string::npos) << x86.synth;

    SpliceRun const none = spliceFor(sys, source, "", ObjectFormatKind::Elf);
    EXPECT_EQ(none.synth.find("#define ARCH_K"), std::string::npos)
        << "with no target an arch-keyed arm matches nothing: " << none.synth;
}

// The REAL <fcntl.h>: each Linux ISA's own open-flag values reach `#if`. ✔MEASURED glibc
// 2.39 / gcc 13.3.0 on each ISA (runs 20260930-175639-f43a1fd8 and 20260930-175657-9088f3f4).
TEST(DescriptorSpliceFullPair, FcntlOpenFlagsReachIfWithEachIsasValue) {
    ASSERT_NE(cLanguage(), nullptr);
    auto const root = dss::test::findConfigRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::configRootDiagnostic();
    std::vector<std::filesystem::path> const sys{*root / "shippedLibs"};
    struct Want {
        std::string_view target;
        std::string_view directory;
        std::string_view nofollow;
        std::string_view tmpfile;
    };
    std::array<Want, 2> const wants{{
        {"x86_64", "#define O_DIRECTORY 65536\n", "#define O_NOFOLLOW 131072\n",
         "#define O_TMPFILE 4259840\n"},
        {"arm64", "#define O_DIRECTORY 16384\n", "#define O_NOFOLLOW 32768\n",
         "#define O_TMPFILE 4210688\n"},
    }};
    for (Want const& w : wants) {
        SCOPED_TRACE(std::string{w.target});
        SpliceRun const r = spliceFor(sys, "#include <fcntl.h>\n", w.target, ObjectFormatKind::Elf);
        EXPECT_FALSE(r.errors);
        EXPECT_NE(r.synth.find(w.directory), std::string::npos) << r.synth;
        EXPECT_NE(r.synth.find(w.nofollow), std::string::npos) << r.synth;
        EXPECT_NE(r.synth.find(w.tmpfile), std::string::npos) << r.synth;
    }
}
