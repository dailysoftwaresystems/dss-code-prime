// D-FFI-IOCTL-SIZE-FIELD-OVERFLOW-SILENT — the per-format SIZE CEILING inside
// the `<sys/ioctl.h>` request-encoding macros, pinned as BEHAVIOUR.
//
// ★★ THE DEFECT WAS A SILENT WRONG VALUE AT A SYSCALL BOUNDARY, AND IT
// OVERFLOWED DIFFERENTLY ON EACH FORMAT.
//   macho — the SDK's `_IOC` MASKS the length (`sizeof(t) & IOCPARM_MASK`,
//     0x1fff), so a type LARGER than 8191 bytes was truncated to its low 13
//     bits: an 8192-byte `_IOR` encoded size ZERO. Well formed, wrong, and
//     diagnostic-free.
//   elf — musl's `_IOC` does NOT mask (`sizeof(t) << 16`), so at
//     `sizeof(t) >= 0x4000` the shifted length lit bit 30 and collided with the
//     2-bit DIRECTION field: a 0x4000-byte `_IOR` came out as 0x80000000 |
//     0x40000000, which IS `_IOWR` — the kernel would copy the argument the
//     WRONG WAY.
// Neither userspace header guards this (MEASURED: zero `_IOC_TYPECHECK` hits
// across the emsdk musl sysroot; Darwin sys/ioccom.h has no compile-time check
// at all — the Linux guard is `#ifdef __KERNEL__`-only). DSS is deliberately
// ABOVE its source headers here: faithfully reproducing a header governs what a
// name MEANS, not a licence to reproduce a silent wrong number.
//
// ★★★ WHY THIS FILE EXISTS ALONGSIDE `examples/c/shipped_ioctl_iowr_{elf,macho}/`,
// WHICH LOOK LIKE THEY COVER THE SAME GROUND. They do not, for two reasons.
//   (1) An example folds its contract into an EXIT CODE and this descriptor
//       ships only on elf and macho, so both example directories declare
//       `runOn: ["linux"]` / `["darwin"]` — on a Windows host neither RUNS.
//       The setjmp sibling measured that exact hole: deleting an elf variant
//       left an example GREEN on Windows. A ceiling is a COMPILE-time fact and
//       DSS cross-compiles, so it is observable on every host, which is what
//       this file does.
//   (2) The whole point of the row is a program that must NOT COMPILE. The
//       examples corpus has no compile-failure convention — every entry there
//       asserts a successful build and an exit code — so the RED half of this
//       contract has nowhere else to live.
//
// ★★ THE PIN THAT MAKES THE CEILING PROVABLY PER-FORMAT, not merely present.
// `SplitType` is 8192 bytes: OVER Darwin's IOCPARM_MASK (8191) and UNDER
// Linux's _IOC_SIZEMASK (16383). It must be REFUSED on macho and ACCEPTED on
// elf, with an exact encoded value. No single global ceiling — and no engine
// branch — can produce both answers; only the two `when:{format}` arms can,
// which is the agnosticism claim in `sys/ioctl.json`'s `$comment` stated as a
// test rather than as prose.
//
// ★ EVERY ACCEPTED VALUE IS ASSERTED, NOT JUST THE ACCEPTANCE. A ceiling that
// shifted the encoding of a type UNDER it would be a regression dressed as a
// fix, so each ACCEPT probe carries `_Static_assert`s for the exact request
// numbers at the boundary. The guard is `0u * sizeof(...)`, so its contribution
// is zero BY CONSTRUCTION rather than by an assumption about a struct's size —
// these asserts are what would catch it if that ever stopped being true.
//
// ★ THE INSTRUMENT CONTROLS. Without them no verdict here is readable: a probe
// that stopped reaching the compiler reports rc!=0 exactly like a fired
// ceiling, and one whose `_Static_assert`s stopped being evaluated reports rc=0
// exactly like a satisfied pin. `ControlMustPass` is the same program with a
// 32-byte type (sqlite's own `struct ByteRangeLockPB2` size) and must build;
// `ControlMustFail` asserts a deliberately wrong number and must fail AT
// S_StaticAssertFailed, proving the value pins are live.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// One shipped format of each KIND that `sys/ioctl.json` declares, plus a second
// architecture on each so the ceiling is pinned as a FORMAT property and not an
// x86_64 or an arm64 one — `when` is FORMAT-only by construction, so the two
// arches of a format MUST agree.
//
// `ceiling` is the largest length that still encodes, taken from the target's
// OWN reference header: Darwin `IOCPARM_MASK` 0x1fff (sys/ioccom.h) and Linux
// `_IOC_SIZEMASK` 0x3fff, i.e. the kernel's `sizeof(t) < (1 << _IOC_SIZEBITS)`.
struct Leg {
    std::string_view spec;
    unsigned         ceiling;        // largest ACCEPTED sizeof, in bytes
    std::string_view atCeilingSource; // a full TU pinning the boundary values
    std::string_view reference;
};

// ── macho ────────────────────────────────────────────────────────────────────
// 8191 = 0x1fff. _IOR('q',9)  = 0x40000000 | 0x1fff0000 | 0x7100 | 0x09
//                _IOW('q',7)  = 0x80000000 | 0x1fff0000 | 0x7100 | 0x07
//                _IOWR('q',5) = 0xc0000000 | 0x1fff0000 | 0x7100 | 0x05
constexpr std::string_view kMachoAtCeiling =
    "#include <sys/ioctl.h>\n"
    "struct DssAtCeiling { char b[0x1fff]; };\n"
    "_Static_assert(sizeof(struct DssAtCeiling) == 0x1fff, \"8191 bytes\");\n"
    "_Static_assert(_IOR('q', 9, struct DssAtCeiling)  == 0x5fff7109u, \"macho _IOR at the ceiling\");\n"
    "_Static_assert(_IOW('q', 7, struct DssAtCeiling)  == 0x9fff7107u, \"macho _IOW at the ceiling\");\n"
    "_Static_assert(_IOWR('q', 5, struct DssAtCeiling) == 0xdfff7105u, \"macho _IOWR at the ceiling\");\n"
    "int main(void) { return 0; }\n";

// ── elf ──────────────────────────────────────────────────────────────────────
// 16383 = 0x3fff. _IOR(0xf5,12)  = 0x80000000 | 0x3fff0000 | 0xf500 | 0x0c
//                 _IOW(0xf5,13)  = 0x40000000 | 0x3fff0000 | 0xf500 | 0x0d
//                 _IOWR(0xf5,14) = 0xc0000000 | 0x3fff0000 | 0xf500 | 0x0e
// 0xf5 is F2FS_IOCTL_MAGIC — the magic sqlite itself writes on this arm.
constexpr std::string_view kElfAtCeiling =
    "#include <sys/ioctl.h>\n"
    "struct DssAtCeiling { char b[0x3fff]; };\n"
    "_Static_assert(sizeof(struct DssAtCeiling) == 0x3fff, \"16383 bytes\");\n"
    "_Static_assert(_IOR(0xf5, 12, struct DssAtCeiling)  == 0xbffff50cul, \"elf _IOR at the ceiling\");\n"
    "_Static_assert(_IOW(0xf5, 13, struct DssAtCeiling)  == 0x7ffff50dul, \"elf _IOW at the ceiling\");\n"
    "_Static_assert(_IOWR(0xf5, 14, struct DssAtCeiling) == 0xfffff50eul, \"elf _IOWR at the ceiling\");\n"
    "int main(void) { return 0; }\n";

constexpr std::array<Leg, 4> kLegs{{
    {"arm64:macho64-arm64-darwin-exec", 0x1fffu, kMachoAtCeiling,
     "Darwin sys/ioccom.h IOCPARM_MASK 0x1fff — over it the SDK macro truncates to 13 bits"},
    {"x86_64:macho64-x86_64-darwin-exec", 0x1fffu, kMachoAtCeiling,
     "the same Darwin header — the ceiling is a FORMAT property, not an arch one"},
    {"x86_64:elf64-x86_64-linux-exec", 0x3fffu, kElfAtCeiling,
     "Linux asm-generic _IOC_SIZEMASK 0x3fff — over it the size overflows into the DIRECTION field"},
    {"arm64:elf64-aarch64-linux-exec", 0x3fffu, kElfAtCeiling,
     "the same asm-generic layout — both DSS elf arches are asm-generic"},
}};

// A type ONE BYTE past the leg's ceiling, used through every sized macro so no
// single arm can be the only one guarded. Nothing is asserted about the value:
// this program must not reach a value at all.
[[nodiscard]] std::string overCeilingSource(unsigned ceiling) {
    return "#include <sys/ioctl.h>\n"
           "struct DssOverCeiling { char b[" + std::to_string(ceiling + 1u) + "]; };\n"
           "unsigned long dss_r(void)  { return _IOR('q', 9, struct DssOverCeiling); }\n"
           "unsigned long dss_w(void)  { return _IOW('q', 7, struct DssOverCeiling); }\n"
           "unsigned long dss_rw(void) { return _IOWR('q', 5, struct DssOverCeiling); }\n"
           "int main(void) { return (int)(dss_r() + dss_w() + dss_rw()); }\n";
}

// A type at EXACTLY the ceiling + 1 is refused; the ceiling itself is accepted.
// The two together are what make this a ceiling rather than a vague size limit.

// 8192 bytes: over macho's ceiling, under elf's. The elf arm also pins the
// value, so "accepted" here means "accepted AND still encodes correctly".
constexpr std::string_view kSplit8192Elf =
    "#include <sys/ioctl.h>\n"
    "struct DssSplit { char b[0x2000]; };\n"
    "_Static_assert(sizeof(struct DssSplit) == 0x2000, \"8192 bytes\");\n"
    "_Static_assert(_IOR(0xf5, 12, struct DssSplit) == 0xa000f50cul, \"elf accepts 8192\");\n"
    "int main(void) { return 0; }\n";

constexpr std::string_view kSplit8192Macho =
    "#include <sys/ioctl.h>\n"
    "struct DssSplit { char b[0x2000]; };\n"
    "unsigned long dss_r(void) { return _IOR('q', 9, struct DssSplit); }\n"
    "int main(void) { return (int)dss_r(); }\n";

// ── the two instrument controls ──────────────────────────────────────────────
// sqlite's own `struct ByteRangeLockPB2` is 32 bytes; the encodings below are
// the ones `examples/c/shipped_ioctl_iowr_{elf,macho}/` already pin against the
// real headers, so a green here also says the ceiling moved nothing.
constexpr std::string_view kControlMustPassMacho =
    "#include <sys/ioctl.h>\n"
    "struct DssSmall { char b[32]; };\n"
    "_Static_assert(_IOR('q', 9, struct DssSmall) == 0x40207109u, \"32-byte macho _IOR is unmoved\");\n"
    "int main(void) { return 0; }\n";

constexpr std::string_view kControlMustPassElf =
    "#include <sys/ioctl.h>\n"
    "struct DssSmall { char b[32]; };\n"
    "_Static_assert(_IOR(0xf5, 12, struct DssSmall) == 0x8020f50cul, \"32-byte elf _IOR is unmoved\");\n"
    "int main(void) { return 0; }\n";

// Deliberately wrong by ONE, so a green verdict cannot come from asserts that
// stopped being evaluated.
constexpr std::string_view kControlMustFailMacho =
    "#include <sys/ioctl.h>\n"
    "struct DssSmall { char b[32]; };\n"
    "_Static_assert(_IOR('q', 9, struct DssSmall) == 0x4020710au, \"control: must fire\");\n"
    "int main(void) { return 0; }\n";

constexpr std::string_view kControlMustFailElf =
    "#include <sys/ioctl.h>\n"
    "struct DssSmall { char b[32]; };\n"
    "_Static_assert(_IOR(0xf5, 12, struct DssSmall) == 0x8020f50dul, \"control: must fire\");\n"
    "int main(void) { return 0; }\n";

[[nodiscard]] bool isMacho(std::string_view spec) {
    return spec.find("macho") != std::string_view::npos;
}

[[nodiscard]] int compileFor(ScratchDir& scratch, std::string_view source,
                             std::string const& spec, DiagnosticReporter& rep) {
    fs::path const src = scratch.path() / "ioctl_size_ceiling_probe.c";
    {
        std::ofstream out{src, std::ios::binary};
        if (!out.good()) return -1;
        out << source;
    }
    scratch.useAsCwd();
    Program prog;
    return prog.compileFiles({src.generic_string()}, "c", {spec}, rep);
}

[[nodiscard]] bool sawCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all())
        if (d.code == code) return true;
    return false;
}

} // namespace

// The ACCEPT half: a type at exactly the ceiling still compiles AND still
// encodes the number its own reference header computes.
TEST(ShippedIoctlSizeCeiling, ATypeAtTheCeilingCompilesAndKeepsItsEncoding) {
    for (Leg const& leg : kLegs) {
        ScratchDir         scratch{Location::InsideRepo, "ioctl-ceiling"};
        DiagnosticReporter rep{DiagnosticReporter::Config{}};
        int const rc = compileFor(scratch, leg.atCeilingSource,
                                  std::string{leg.spec}, rep);
        EXPECT_EQ(rc, 0)
            << "target " << leg.spec << ": a " << leg.ceiling
            << "-byte type is the LARGEST this format can encode (" << leg.reference
            << ") and it no longer compiles. Either the ceiling is off by one — "
               "it must REFUSE ceiling+1, not the ceiling — or the guard in "
               "src/dss-config/shippedLibs/sys/ioctl.json stopped contributing "
               "zero and shifted the encoded value.";
    }
}

// The REFUSE half — the whole reason the row exists. One byte past the ceiling
// must not produce a binary, and must say so as an array-length constraint
// violation rather than dying somewhere unrelated.
TEST(ShippedIoctlSizeCeiling, ATypeOneBytePastTheCeilingIsRefusedAtCompileTime) {
    for (Leg const& leg : kLegs) {
        ScratchDir         scratch{Location::InsideRepo, "ioctl-ceiling"};
        DiagnosticReporter rep{DiagnosticReporter::Config{}};
        std::string const  src = overCeilingSource(leg.ceiling);
        int const rc = compileFor(scratch, src, std::string{leg.spec}, rep);
        EXPECT_NE(rc, 0)
            << "target " << leg.spec << ": a " << (leg.ceiling + 1u)
            << "-byte type COMPILED. That is the silent wrong value this row "
               "exists to delete — " << leg.reference
            << ". The per-format ceiling in the `when:{format}` arm of "
               "src/dss-config/shippedLibs/sys/ioctl.json is missing or has "
               "been widened.";
        EXPECT_TRUE(sawCode(rep, DiagnosticCode::S_ArrayLengthOutOfRange))
            << "target " << leg.spec
            << ": the over-ceiling program failed, but NOT at the ceiling's own "
               "array-length constraint — so this red says nothing about the "
               "size field. Check what actually broke before trusting it.";
    }
}

// The ceiling is PER FORMAT, and this is the only assertion that can show it:
// one type, two answers, decided entirely by the `when:{format}` arm.
TEST(ShippedIoctlSizeCeiling, EightKiBSplitsTheTwoFormats) {
    for (Leg const& leg : kLegs) {
        ScratchDir         scratch{Location::InsideRepo, "ioctl-ceiling"};
        DiagnosticReporter rep{DiagnosticReporter::Config{}};
        bool const macho = isMacho(leg.spec);
        int const  rc    = compileFor(scratch,
                                      macho ? kSplit8192Macho : kSplit8192Elf,
                                      std::string{leg.spec}, rep);
        if (macho) {
            EXPECT_NE(rc, 0)
                << "target " << leg.spec
                << ": 8192 bytes is one past Darwin's IOCPARM_MASK and must be "
                   "refused here. Accepting it means the macho arm picked up "
                   "elf's 16383 ceiling — the copy-paste between the two arms "
                   "this pin exists to catch.";
            EXPECT_TRUE(sawCode(rep, DiagnosticCode::S_ArrayLengthOutOfRange))
                << "target " << leg.spec
                << ": refused, but not by the ceiling.";
        } else {
            EXPECT_EQ(rc, 0)
                << "target " << leg.spec
                << ": 8192 bytes is well UNDER Linux's _IOC_SIZEMASK (16383) "
                   "and must encode as 0xa000f50c. Refusing it means the elf "
                   "arm picked up macho's 8191 ceiling, which would break every "
                   "legitimate large-argument ioctl on Linux.";
        }
    }
}

// Neither verdict above is readable without these.
TEST(ShippedIoctlSizeCeiling, TheProbeInstrumentIsLiveOnEveryLeg) {
    for (Leg const& leg : kLegs) {
        bool const macho = isMacho(leg.spec);
        {
            ScratchDir         scratch{Location::InsideRepo, "ioctl-ceiling"};
            DiagnosticReporter rep{DiagnosticReporter::Config{}};
            int const rc = compileFor(
                scratch, macho ? kControlMustPassMacho : kControlMustPassElf,
                std::string{leg.spec}, rep);
            EXPECT_EQ(rc, 0)
                << "target " << leg.spec
                << ": a 32-byte type — sqlite's own `struct ByteRangeLockPB2` "
                   "size, far under either ceiling — does not compile, so every "
                   "RED in this file is unreadable: the header is failing for a "
                   "reason that has nothing to do with the size field.";
        }
        {
            ScratchDir         scratch{Location::InsideRepo, "ioctl-ceiling"};
            DiagnosticReporter rep{DiagnosticReporter::Config{}};
            int const rc = compileFor(
                scratch, macho ? kControlMustFailMacho : kControlMustFailElf,
                std::string{leg.spec}, rep);
            EXPECT_NE(rc, 0)
                << "target " << leg.spec
                << ": a deliberately WRONG encoded value passed, so the "
                   "`_Static_assert`s in this file are not being evaluated and "
                   "every GREEN above is vacuous.";
            EXPECT_TRUE(sawCode(rep, DiagnosticCode::S_StaticAssertFailed))
                << "target " << leg.spec
                << ": the control failed for some reason OTHER than its static "
                   "assertion, so it is not testing what it claims to.";
        }
    }
}
