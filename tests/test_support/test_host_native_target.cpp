// Self-test for the host-native-target chokepoint.
//
// D-TEST-HOST-SPAWNS-FOREIGN-BINARY. `hostNativeTarget()` selects its arm with
// the preprocessor, so ONLY the arm matching the machine running this test is
// ever compiled. A typo or a stale spelling in any other arm is invisible here
// and shows up as a CI red on that host — which is exactly how the arm64 arm
// came to be missing in the first place (TF-C69 fixed the macOS ladder and left
// its Linux duplicate alone; the native ubuntu-24.04-arm leg then built an
// x86_64 ELF, spawned it, and got `posix_spawn ... rc=8` = ENOEXEC).
//
// So this pins the arm THIS host selects: both spellings must name a SHIPPED
// target and a SHIPPED object format that actually load. Every CI leg runs it,
// so between them all four arms are covered — the x86_64-Linux, arm64-Linux,
// Windows and macOS legs each validate their own. RED-ON-DISABLE: misspell any
// component of an arm (e.g. `aarch64:` for the CPU, which is `arm64`, or
// `elf64-arm64-linux-dyn` for the format, which is `elf64-aarch64-linux-dyn`)
// and the corresponding leg fails here with the exact bad spelling printed,
// instead of failing later and far away inside a spawn.

#include "core/types/target_schema.hpp"
#include "host_native_target.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace dss::test_support;

namespace {

// "arm64:elf64-aarch64-linux-dyn" -> ("arm64", "elf64-aarch64-linux-dyn").
// Fails the test rather than returning a half-parsed pair: a spec with no ':'
// is itself the defect this file exists to catch.
void assertSpecLoads(std::string_view spec, char const* what) {
    auto const colon = spec.find(':');
    ASSERT_NE(colon, std::string_view::npos)
        << what << " spec '" << spec << "' must be '<cpu>:<object-format>'";

    std::string const cpu{spec.substr(0, colon)};
    std::string const fmt{spec.substr(colon + 1)};

    auto target = dss::TargetSchema::loadShipped(cpu);
    ASSERT_TRUE(target.has_value())
        << what << ": CPU '" << cpu << "' is not a shipped .target.json "
        << "(from spec '" << spec << "')";

    auto format = dss::ObjectFormatSchema::loadShipped(fmt);
    ASSERT_TRUE(format.has_value())
        << what << ": object format '" << fmt << "' is not a shipped "
        << ".format.json (from spec '" << spec << "')";
}

}  // namespace

TEST(HostNativeTarget, ThisHostsArmNamesShippedTargetAndFormat) {
    auto const host = hostNativeTarget();
    ASSERT_FALSE(host.libTarget.empty()) << "no arm matched this host";
    ASSERT_FALSE(host.execTarget.empty()) << "no arm matched this host";

    assertSpecLoads(host.libTarget, "libTarget");
    assertSpecLoads(host.execTarget, "execTarget");
}

// The suffixes must agree with the format family the arm selected, or a test
// will build `dsslib.so` and then look for `dsslib.dylib`.
TEST(HostNativeTarget, ArtifactSuffixesMatchTheSelectedFormatFamily) {
    auto const host = hostNativeTarget();

    if (host.execTarget.find("pe64") != std::string_view::npos) {
        EXPECT_EQ(host.libSuffix, ".dll");
        EXPECT_EQ(host.exeSuffix, ".exe");
    } else if (host.execTarget.find("macho") != std::string_view::npos) {
        EXPECT_EQ(host.libSuffix, ".dylib");
        EXPECT_EQ(host.exeSuffix, "");
    } else {
        EXPECT_NE(host.execTarget.find("elf64"), std::string_view::npos)
            << "unrecognised format family in '" << host.execTarget << "'";
        EXPECT_EQ(host.libSuffix, ".so");
        EXPECT_EQ(host.exeSuffix, "");
    }

    EXPECT_EQ(hostLibArtifact("dsslib"),
              "dsslib" + std::string{host.libSuffix});
    EXPECT_EQ(hostExeArtifact("decl"), "decl" + std::string{host.exeSuffix});
}

// The lib arm must be a SHARED-library flavor and the exec arm an EXECUTABLE
// one. Swapping them compiles and links, then fails at spawn time.
TEST(HostNativeTarget, LibArmIsSharedAndExecArmIsExecutable) {
    auto const host = hostNativeTarget();
    EXPECT_NE(host.libTarget, host.execTarget);

    bool const libIsShared =
        host.libTarget.find("-dyn") != std::string_view::npos ||
        host.libTarget.find("dylib") != std::string_view::npos ||
        host.libTarget.find("-dll") != std::string_view::npos;
    EXPECT_TRUE(libIsShared)
        << "libTarget '" << host.libTarget << "' is not a shared-library flavor";

    EXPECT_NE(host.execTarget.find("-exec"), std::string_view::npos)
        << "execTarget '" << host.execTarget << "' is not an executable flavor";
}
