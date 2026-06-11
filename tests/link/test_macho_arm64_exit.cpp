// macOS-ARM64 runnable backend — host-independent byte-pins
// (D-LK10-ENTRY-MACHO-EXIT, v0.0.3).
//
// The `examples/c-subset/macho_arm64_exit` corpus is the END-TO-END
// runtime proof, but it only SPAWNS on the macos-latest (= Apple
// Silicon) CI leg. These byte-pins are the ALWAYS-ON guard: they run on
// EVERY leg (Windows / Linux included) and regression-catch the arm64
// `__stubs` encoder, the per-cputype stub-size threading, the
// direct-plt trampoline dispatch, the dispatch/symbolVa fail-loud, and
// the ad-hoc signature — so a miscompile is caught at `ctest` time even
// where the binary cannot run.
//
// Pins:
//   * emitArm64MachoStub bytes: ADRP x16, page → LDR x16,[x16,#lo12] →
//     BR x16, with the ADRP page-pair + LDR imm12 math INDEPENDENTLY
//     recomputed here from the emitted __stubs/__got section VAs (a
//     genuine cross-check, not an echo of the implementation).
//   * Stub stride = 12 (section_64 __stubs reserved2) and stubsVa is
//     4-byte aligned (arm64 instruction-fetch requirement — advisory
//     section align is 1, so this pins the natural alignment).
//   * The trampoline makes a DIRECT BL (0x94......) to the `_exit`
//     import under externCallDispatch=direct-plt — NOT a deref-the-stub
//     indirect call (which has no arm64 opcode anyway).
//   * A Mach-O format that declares indirect-slot fails LOUD at encode
//     (the dispatch ↔ symbolVa-target coherence guard).
//   * The ad-hoc LC_CODE_SIGNATURE SuperBlob/CodeDirectory is well-
//     formed (BE magic + 16384 pageSize + codeLimit == sig offset).
//   * The shipped macho64-arm64-darwin-exec.format.json loads with the
//     arm64 cputype + direct-plt dispatch + the `_exit` processExit.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/entry_trampoline.hpp"
#include "link/format/macho.hpp"
#include "link/object_format_schema.hpp"
#include "macho_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

using dss::macho::test::findSection;
using dss::macho::test::readU32LE;
using dss::macho::test::readU64LE;

// Ancestor-walk to the shipped arm64-darwin-exec format JSON (mirrors
// findShippedConfig so the test works whether ctest runs from build/ or
// the repo root). Returns the parsed schema or nullptr (with an
// ADD_FAILURE) on miss.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const>
loadDarwinExecByName(char const* fname) {
    fs::path here = fs::current_path();
    fs::path shipped;
    for (int i = 0; i < 8 && !here.empty(); ++i) {
        fs::path const candidate = here / "src" / "dss-config"
            / "object-formats" / fname;
        if (fs::exists(candidate)) { shipped = candidate; break; }
        here = here.parent_path();
    }
    if (shipped.empty()) {
        ADD_FAILURE() << fname << " not found in any ancestor "
                         "src/dss-config/object-formats";
        return nullptr;
    }
    auto f = ObjectFormatSchema::loadFromFile(shipped.string());
    if (!f.has_value()) {
        ADD_FAILURE() << "loadFromFile(" << fname << ") failed";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
        return nullptr;
    }
    return std::move(f).value();
}

[[nodiscard]] std::shared_ptr<ObjectFormatSchema const>
loadArm64DarwinExecFormat() {
    return loadDarwinExecByName("macho64-arm64-darwin-exec.format.json");
}

[[nodiscard]] std::shared_ptr<TargetSchema const> loadArm64Target() {
    auto t = TargetSchema::loadShipped("arm64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(arm64) failed";
        for (auto const& d : t.error()) ADD_FAILURE() << "  " << d.message;
        return nullptr;
    }
    return std::move(t).value();
}

// A minimal AArch64 extern-importing module that drives the dynamic
// `__stubs` path: one function whose first instruction is a `BL` to the
// extern (with a call26 relocation at offset 0), followed by `RET`.
//   94 00 00 00   BL   <extern>   (linker patches imm26 → __stubs entry)
//   C0 03 5F D6   RET
// kind 1 = call26 per arm64.target.json's relocation table (matches the
// macho64-arm64-darwin-exec format's ARM64_RELOC_BRANCH26 kind=1).
[[nodiscard]] AssembledModule makeArm64DynamicModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};
    Relocation rel{/*offset=*/0, SymbolId{99}, RelocationKind{1}, /*addend=*/0};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_abs", "/usr/lib/libSystem.B.dylib"});
    return mod;
}

// Decode the single __stubs entry (3 AArch64 words) from an emitted
// Mach-O. Returns the 3 little-endian instruction words and the stub's
// runtime VA + the __got slot VA, so the test can recompute the
// expected ADRP/LDR encodings independently.
struct DecodedStub {
    std::uint32_t adrp = 0;
    std::uint32_t ldr  = 0;
    std::uint32_t br   = 0;
    std::uint64_t stubVa    = 0;
    std::uint64_t gotSlotVa = 0;
    std::uint32_t reserved2 = 0;  // section_64 __stubs stub-size field
};

[[nodiscard]] std::optional<DecodedStub>
decodeSoleStub(std::vector<std::uint8_t> const& bytes) {
    std::span<std::uint8_t const> sp{bytes};
    auto const stubsSec = findSection(sp, "__TEXT", "__stubs");
    auto const gotSec   = findSection(sp, "__DATA_CONST", "__got");
    if (!stubsSec || !gotSec) return std::nullopt;
    DecodedStub d;
    // section_64 record layout (findSection returns the record start =
    // the `sectname` field): sectname[16] segname[16] addr@32(u64)
    // size@40(u64) offset@48(u32) align@52 reloff@56 nreloc@60 flags@64
    // reserved1@68 reserved2@72 reserved3@76. The __stubs stub-size is
    // reserved2 (@72); the __got slot VA is __got's addr (@32).
    std::uint64_t const stubsAddr = readU64LE(bytes, *stubsSec + 32);
    std::uint32_t const stubsOff  = readU32LE(bytes, *stubsSec + 48);
    d.reserved2                   = readU32LE(bytes, *stubsSec + 72);
    d.gotSlotVa                   = readU64LE(bytes, *gotSec + 32);
    d.stubVa                      = stubsAddr;
    d.adrp = readU32LE(bytes, stubsOff + 0);
    d.ldr  = readU32LE(bytes, stubsOff + 4);
    d.br   = readU32LE(bytes, stubsOff + 8);
    return d;
}

} // namespace

// ── The shipped arm64-darwin-exec format JSON loads cleanly ──────────

TEST(MachOArm64Exit, ShippedX86DarwinExecIsDirectPlt) {
    // MUST-FIX 1 regression pin: macho64-x86_64-darwin-exec was FLIPPED
    // indirect-slot→direct-plt this cycle. Mach-O sets symbolVa→__stubs
    // (code), so an indirect-slot call site (`FF 15` deref of the stub
    // bytes as a pointer) is a latent SIGSEGV. The shipped format MUST
    // declare direct-plt; pin it so a revert to indirect-slot is caught on
    // EVERY leg — the bug had no macOS runtime to expose it before.
    auto fmt = loadDarwinExecByName("macho64-x86_64-darwin-exec.format.json");
    ASSERT_TRUE(fmt);
    ASSERT_TRUE(fmt->externCallDispatch().has_value());
    EXPECT_TRUE(*fmt->externCallDispatch() == ExternCallDispatch::DirectPlt);
}

TEST(MachOArm64Exit, ShippedFormatLoadsWithArm64Identity) {
    auto fmt = loadArm64DarwinExecFormat();
    ASSERT_TRUE(fmt);
    EXPECT_EQ(fmt->kind(), ObjectFormatKind::MachO);
    EXPECT_EQ(fmt->name(), "macho64-arm64-darwin-exec");
    // CPU_TYPE_ARM64 = 0x0100000C; CPU_SUBTYPE_ARM64_ALL = 0.
    EXPECT_EQ(fmt->macho().cputype, 0x0100000Cu);
    EXPECT_EQ(fmt->macho().cpusubtype, 0u);
    EXPECT_TRUE(fmt->macho().filetype == MachOObjectType::Execute);
    // direct-plt (NOT indirect-slot) — Mach-O symbolVa→stub.
    ASSERT_TRUE(fmt->externCallDispatch().has_value());
    EXPECT_TRUE(*fmt->externCallDispatch() == ExternCallDispatch::DirectPlt);
    // processExit = ByNameImport _exit via libSystem.
    ASSERT_TRUE(fmt->processExit().has_value());
    EXPECT_TRUE(fmt->processExit()->mechanism == ExitMechanism::ByNameImport);
    EXPECT_EQ(fmt->processExit()->importMangledName, "_exit");
    EXPECT_EQ(fmt->processExit()->importLibraryPath,
              "/usr/lib/libSystem.B.dylib");
    EXPECT_EQ(fmt->entryCallingConvention(), "apple_arm64");
    // Ad-hoc code signature reserved (Apple Silicon requires signing).
    // The code-hash page size is the conventional 4096 — a SEPARATE knob
    // from the 16 KiB VM segment page (segmentPageSize) below.
    ASSERT_TRUE(fmt->machoImage().codeSignature.has_value());
    EXPECT_EQ(fmt->machoImage().codeSignature->pageSize, 4096u);
    // 16 KiB VM segment page — the Apple-Silicon EBADMACHO fix.
    EXPECT_EQ(fmt->machoImage().segmentPageSize, 16384u);
    // LC_BUILD_VERSION platform = macos, minOs = 11.0.0 (0x000B0000).
    ASSERT_TRUE(fmt->machoImage().buildVersion.has_value());
    EXPECT_TRUE(fmt->machoImage().buildVersion->platform
                == MachOBuildVersion::Platform::MacOs);
    EXPECT_EQ(fmt->machoImage().buildVersion->minOs, 0x000B0000u);
}

// ── emitArm64MachoStub: exact ADRP+LDR+BR bytes (independent recompute)

TEST(MachOArm64Exit, StubEmitsAdrpLdrBrThroughGot) {
    auto target = loadArm64Target();
    auto fmt    = loadArm64DarwinExecFormat();
    ASSERT_TRUE(target);
    ASSERT_TRUE(fmt);

    AssembledModule mod = makeArm64DynamicModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *target, *fmt, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    auto decoded = decodeSoleStub(bytes);
    ASSERT_TRUE(decoded.has_value());
    auto const& d = *decoded;

    // ── Recompute the expected encodings INDEPENDENTLY from the emitted
    //    section VAs (ARM ARM C6.2.10 ADRP / C6.2.131 LDR imm / C6.2.37
    //    BR). If the encoder diverged, these fail. ──
    auto const pageOf = [](std::uint64_t v) { return v & ~std::uint64_t{0xFFF}; };
    std::int64_t const pageDiff =
        static_cast<std::int64_t>(pageOf(d.gotSlotVa)) -
        static_cast<std::int64_t>(pageOf(d.stubVa));
    std::int64_t const adrpValue = pageDiff >> 12;
    std::uint32_t const adrpV = static_cast<std::uint32_t>(adrpValue) & 0x1FFFFFu;
    std::uint32_t const expectAdrp =
        0x90000010u | ((adrpV & 0x3u) << 29) | (((adrpV >> 2) & 0x7FFFFu) << 5);

    std::uint64_t const lo12 = d.gotSlotVa & 0xFFFull;
    ASSERT_EQ(lo12 & 0x7u, 0u) << "__got slot must be 8-byte aligned";
    std::uint32_t const imm12 = static_cast<std::uint32_t>(lo12 >> 3);
    std::uint32_t const expectLdr = 0xF9400210u | (imm12 << 10);

    constexpr std::uint32_t kExpectBrX16 = 0xD61F0200u;

    EXPECT_EQ(d.adrp, expectAdrp)
        << "ADRP x16 word mismatch (stubVa=0x" << std::hex << d.stubVa
        << " gotSlotVa=0x" << d.gotSlotVa << ")";
    EXPECT_EQ(d.ldr, expectLdr) << "LDR x16,[x16,#lo12] word mismatch";
    EXPECT_EQ(d.br, kExpectBrX16) << "BR x16 word mismatch";

    // Field-level cross-checks so a wrong-register regression is loud:
    //   ADRP Rd = bits[4:0] must be 16 (x16).
    EXPECT_EQ(d.adrp & 0x1Fu, 16u) << "ADRP destination must be x16";
    EXPECT_EQ((d.adrp >> 24) & 0x9Fu, 0x90u) << "op must be ADRP (immlo|1|10000)";
    //   LDR Rn (bits[9:5]) = 16, Rt (bits[4:0]) = 16, size/op fixed.
    EXPECT_EQ((d.ldr >> 5) & 0x1Fu, 16u) << "LDR base must be x16";
    EXPECT_EQ(d.ldr & 0x1Fu, 16u) << "LDR dest must be x16";
    EXPECT_EQ(d.ldr & 0xFFC003FFu, 0xF9400210u) << "LDR opcode/reg frame";
    //   BR Rn (bits[9:5]) = 16.
    EXPECT_EQ((d.br >> 5) & 0x1Fu, 16u) << "BR register must be x16";
}

// ── Stub stride = 12 + stubsVa 4-aligned ─────────────────────────────

TEST(MachOArm64Exit, StubSizeIsTwelveAndStubsVaIsInstructionAligned) {
    auto target = loadArm64Target();
    auto fmt    = loadArm64DarwinExecFormat();
    ASSERT_TRUE(target);
    ASSERT_TRUE(fmt);

    AssembledModule mod = makeArm64DynamicModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    auto decoded = decodeSoleStub(bytes);
    ASSERT_TRUE(decoded.has_value());
    // section_64.reserved2 carries the per-stub size (12 for arm64).
    EXPECT_EQ(decoded->reserved2, 12u);
    // AArch64 instructions must be fetched from 4-byte-aligned VAs; the
    // section's advisory align is 1, so the natural alignment of stubsVa
    // (page-aligned sectionVa + 4-byte-multiple __text body) is what
    // keeps the stub fetchable. Pin it so a future codegen change that
    // makes __text a non-multiple-of-4 byte count fails here.
    EXPECT_EQ(decoded->stubVa % 4u, 0u) << "arm64 __stubs VA must be 4-aligned";
}

// ── The trampoline makes a DIRECT BL to _exit under direct-plt ───────

TEST(MachOArm64Exit, TrampolineEmitsDirectBlForExitImport) {
    auto target = loadArm64Target();
    auto fmt    = loadArm64DarwinExecFormat();
    ASSERT_TRUE(target);
    ASSERT_TRUE(fmt);

    // A user fn that returns 42: MOVZ X0,#42 ; RET (LE byte stream).
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction user;
    user.symbol = SymbolId{1};
    user.bytes  = {0x40, 0x05, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6};
    mod.functions.push_back(std::move(user));

    DiagnosticReporter rep;
    bool const ok = linker::injectEntryTrampoline(mod, *target, *fmt, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_TRUE(ok);
    ASSERT_EQ(rep.errorCount(), 0u);

    // The trampoline is now functions[0]; the synthetic `_exit` import
    // was appended to externImports.
    ASSERT_GE(mod.functions.size(), 2u);
    bool sawExit = false;
    for (auto const& e : mod.externImports) {
        if (e.mangledName == "_exit") sawExit = true;
    }
    EXPECT_TRUE(sawExit) << "trampoline must append the _exit import";

    // Scan the trampoline's assembled words for a BL (0x94000000 family
    // — top 6 bits 0b100101). direct-plt → the exit call is a plain BL,
    // NOT an indirect deref-the-stub (which arm64 has no opcode for).
    auto const& tbytes = mod.functions[0].bytes;
    ASSERT_EQ(tbytes.size() % 4u, 0u);
    bool sawBl = false;
    int blCount = 0;
    for (std::size_t i = 0; i + 4 <= tbytes.size(); i += 4) {
        std::uint32_t const w =
            static_cast<std::uint32_t>(tbytes[i]) |
            (static_cast<std::uint32_t>(tbytes[i + 1]) << 8) |
            (static_cast<std::uint32_t>(tbytes[i + 2]) << 16) |
            (static_cast<std::uint32_t>(tbytes[i + 3]) << 24);
        if ((w & 0xFC000000u) == 0x94000000u) { sawBl = true; ++blCount; }
    }
    EXPECT_TRUE(sawBl) << "trampoline must contain a BL instruction";
    // Two BLs: call user_entry + call _exit (both direct under
    // direct-plt). If the exit call had lowered to an indirect shape
    // the second BL would be absent.
    EXPECT_EQ(blCount, 2) << "expected BL user_entry + BL _exit";

    // The whole module must then encode to a Mach-O without error — the
    // trampoline's BL→_exit-stub is a coherent direct-plt call site.
    auto bytes = macho::encode(mod, *target, *fmt, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(bytes.empty());
}

// ── A Mach-O format declaring indirect-slot fails LOUD ───────────────
//
// The walker points symbolVa at the __stubs STUB (direct-plt); an
// indirect-slot call site would deref the stub's code → SIGSEGV. The
// dispatch ↔ symbolVa-target coherence guard must catch this at link.

TEST(MachOArm64Exit, IndirectSlotDispatchOnMachOFailsLoud) {
    auto target = loadArm64Target();
    ASSERT_TRUE(target);

    // Clone the x86_64-darwin codesign-free dynamic schema shape but set
    // externCallDispatch=indirect-slot (the mislabel MUST-FIX 1 forbids).
    // x86_64 cputype keeps the fixture host-encodable; the guard is
    // CPU-agnostic (it checks dispatch vs the symbolVa target, which is
    // the stub on every cputype).
    std::string const json = R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-indirect-bad","kind":"macho"},
      "entryPoint": "",
      "externCallDispatch": "indirect-slot",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "bindNow": true
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ],
      "relocations":[
        {"name":"X86_64_RELOC_BRANCH","kind":1,"nativeId":369098752},
        {"name":"X86_64_RELOC_UNSIGNED_8","kind":2,"nativeId":100663296},
        {"name":"X86_64_RELOC_UNSIGNED_4","kind":3,"nativeId":33554432}
      ]
    })";
    auto fmtRes = ObjectFormatSchema::loadFromText(json);
    ASSERT_TRUE(fmtRes.has_value());
    auto fmt = std::move(fmtRes).value();  // shared_ptr<ObjectFormatSchema>

    // A minimal x86_64 extern-importing module (E8 rel32 call + ret).
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back(
        Relocation{1, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_abs", "/usr/lib/libSystem.B.dylib"});

    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *target, *fmt, rep);
    EXPECT_GT(rep.errorCount(), 0u)
        << "indirect-slot on a symbolVa→stub Mach-O must fail loud";
    EXPECT_TRUE(bytes.empty());
}

// ── The ad-hoc LC_CODE_SIGNATURE is well-formed for arm64 ────────────

TEST(MachOArm64Exit, AdHocCodeSignatureSuperBlobWellFormed) {
    auto target = loadArm64Target();
    auto fmt    = loadArm64DarwinExecFormat();
    ASSERT_TRUE(target);
    ASSERT_TRUE(fmt);

    AssembledModule mod = makeArm64DynamicModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // Locate LC_CODE_SIGNATURE (0x1D) → dataoff / datasize.
    std::uint32_t const ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    std::uint32_t dataOff = 0, dataSize = 0;
    bool foundCS = false;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmd == 0x1Du) {
            dataOff  = readU32LE(bytes, off + 8);
            dataSize = readU32LE(bytes, off + 12);
            foundCS  = true;
        }
        ASSERT_NE(cmdsize, 0u);
        off += cmdsize;
    }
    ASSERT_TRUE(foundCS) << "arm64 exec must carry LC_CODE_SIGNATURE";
    ASSERT_LE(static_cast<std::size_t>(dataOff) + dataSize, bytes.size());

    // Big-endian SuperBlob + CodeDirectory header (the one BE region).
    auto readU32BE = [&](std::size_t o) {
        return (static_cast<std::uint32_t>(bytes[o + 0]) << 24) |
               (static_cast<std::uint32_t>(bytes[o + 1]) << 16) |
               (static_cast<std::uint32_t>(bytes[o + 2]) <<  8) |
                static_cast<std::uint32_t>(bytes[o + 3]);
    };
    std::size_t const sb = dataOff;
    EXPECT_EQ(readU32BE(sb + 0), 0xFADE0CC0u);   // CSMAGIC_EMBEDDED_SIGNATURE
    EXPECT_EQ(readU32BE(sb + 4), dataSize);      // SuperBlob length
    EXPECT_EQ(readU32BE(sb + 8), 1u);            // 1 sub-blob
    std::size_t const cd = sb + readU32BE(sb + 16);
    EXPECT_EQ(readU32BE(cd + 0), 0xFADE0C02u);   // CSMAGIC_CODEDIRECTORY
    EXPECT_EQ(readU32BE(cd + 12), 0x00000002u);  // CS_ADHOC
    std::uint32_t const codeLimit = readU32BE(cd + 32);
    EXPECT_EQ(codeLimit, dataOff);               // sig offset == codeLimit
    EXPECT_EQ(bytes[cd + 37], 2u);               // hashType = SHA-256
    EXPECT_EQ(bytes[cd + 39], 12u);              // pageSize = log2(4096)
}

// ── 16 KiB segment alignment — the EBADMACHO fix, red-on-disable ──────
//
// Apple Silicon rejects an executable whose LC_SEGMENT_64 vmaddr / vmsize
// / fileoff are not 16 KiB-aligned (errno EBADMACHO). EVERY segment must
// be 16 KiB-aligned in all three fields. This is the host-independent
// structural guard: it runs on every CI leg and goes RED if
// `segmentPageSize` ever reverts to 4096 (the segments downstream of
// __TEXT — __DATA_CONST.vmaddr/fileoff and __LINKEDIT — fall on a 4 KiB
// boundary that is NOT a 16 KiB boundary).
TEST(MachOArm64Exit, Arm64SegmentsAre16KAligned) {
    auto target = loadArm64Target();
    auto fmt    = loadArm64DarwinExecFormat();
    ASSERT_TRUE(target);
    ASSERT_TRUE(fmt);

    AssembledModule mod = makeArm64DynamicModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *target, *fmt, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    constexpr std::uint64_t k16K = 16384u;
    // Walk every load command; for each LC_SEGMENT_64 (cmd 0x19) assert
    // vmaddr@24 / vmsize@32 / fileoff@40 are all 16 KiB-aligned.
    std::uint32_t const ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    int segments = 0;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        ASSERT_NE(cmdsize, 0u);
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            ++segments;
            std::string seg(reinterpret_cast<char const*>(&bytes[off + 8]),
                            ::strnlen(reinterpret_cast<char const*>(
                                          &bytes[off + 8]), 16));
            std::uint64_t const vmaddr   = readU64LE(bytes, off + 24);
            std::uint64_t const vmsize   = readU64LE(bytes, off + 32);
            std::uint64_t const fileoff  = readU64LE(bytes, off + 40);
            EXPECT_EQ(vmaddr  % k16K, 0u) << seg << ".vmaddr 0x"
                << std::hex << vmaddr << " not 16 KiB-aligned";
            EXPECT_EQ(vmsize  % k16K, 0u) << seg << ".vmsize 0x"
                << std::hex << vmsize << " not 16 KiB-aligned";
            EXPECT_EQ(fileoff % k16K, 0u) << seg << ".fileoff 0x"
                << std::hex << fileoff << " not 16 KiB-aligned";
        }
        off += cmdsize;
    }
    // __PAGEZERO + __TEXT + __DATA_CONST + __LINKEDIT.
    EXPECT_EQ(segments, 4) << "expected 4 LC_SEGMENT_64 commands";
}

// ── LC_BUILD_VERSION byte-pin (the platform LC dyld needs) ────────────
TEST(MachOArm64Exit, Arm64ExecEmitsBuildVersionMacOs) {
    auto target = loadArm64Target();
    auto fmt    = loadArm64DarwinExecFormat();
    ASSERT_TRUE(target);
    ASSERT_TRUE(fmt);

    AssembledModule mod = makeArm64DynamicModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *target, *fmt, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // LC_BUILD_VERSION = 0x32. build_version_command (all u32 LE):
    //   cmd@0 cmdsize@4 platform@8 minos@12 sdk@16 ntools@20.
    std::span<std::uint8_t const> sp{bytes};
    auto bvOff = dss::macho::test::findLoadCommand(sp, 0x32u);
    ASSERT_TRUE(bvOff.has_value())
        << "arm64 exec must carry LC_BUILD_VERSION (dyld platform LC)";
    EXPECT_EQ(readU32LE(bytes, *bvOff + 4), 24u);   // cmdsize (ntools=0)
    EXPECT_EQ(readU32LE(bytes, *bvOff + 8), 1u);    // PLATFORM_MACOS
    EXPECT_EQ(readU32LE(bytes, *bvOff + 12), 0x000B0000u);  // minos 11.0.0
    EXPECT_EQ(readU32LE(bytes, *bvOff + 16), 0x000B0000u);  // sdk 11.0.0
    EXPECT_EQ(readU32LE(bytes, *bvOff + 20), 0u);   // ntools = 0
}

// ── Backward-compat: x86_64-darwin defaults to a 4 KiB segment page ───
//
// macho64-x86_64-darwin-exec declares NO segmentPageSize → it defaults to
// 4096, so the x86 layout is byte-identical to before this cycle. Pin the
// default explicitly (a knob-that-lies guard: prove the absence path
// yields 4096, not 0 or 16384).
TEST(MachOArm64Exit, X86DarwinExecDefaultsTo4KSegmentPageSize) {
    auto fmt = loadDarwinExecByName("macho64-x86_64-darwin-exec.format.json");
    ASSERT_TRUE(fmt);
    EXPECT_EQ(fmt->machoImage().segmentPageSize, 4096u);
    // x86 declares no buildVersion (Intel/Rosetta is lenient).
    EXPECT_FALSE(fmt->machoImage().buildVersion.has_value());
}

// ── validate() fail-loud: a non-power-of-two segmentPageSize ──────────
TEST(MachOArm64Exit, SegmentPageSizeNonPowerOfTwoFailsLoud) {
    // segmentPageSize = 12288 (0x3000) — a multiple of 4096 but NOT a
    // power of two; alignUp() would corrupt the layout. Must reject.
    std::string const json = R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-badpage","kind":"macho"},
      "entryPoint": "",
      "macho": { "cputype": 16777228, "cpusubtype": 0, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "segmentPageSize": 12288,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "bindNow": true
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294983680}
      ],
      "relocations":[{"name":"ARM64_RELOC_BRANCH26","kind":1,"nativeId":620756992}]
    })";
    auto res = ObjectFormatSchema::loadFromText(json);
    ASSERT_FALSE(res.has_value())
        << "non-power-of-two segmentPageSize must fail validate";
    bool sawMsg = false;
    for (auto const& d : res.error())
        if (d.message.find("segmentPageSize") != std::string::npos
         && d.message.find("power of two") != std::string::npos)
            sawMsg = true;
    EXPECT_TRUE(sawMsg) << "expected a segmentPageSize power-of-two diag";
}

// ── validate() fail-loud: the EXACT 16 KiB-vs-4 KiB-VA bug ────────────
//
// segmentPageSize = 16384 but __text.virtualAddress = 0x100001000 (only
// 4 KiB above pageZeroSize) — the canonical arm64 EBADMACHO. validate()
// must reject it at schema-load (host-independent, fast), not leave it to
// the encode-time walker. This pins the congruence guard red-on-disable.
TEST(MachOArm64Exit, TextVaNotCongruentTo16KPageFailsLoud) {
    std::string const json = R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-badva","kind":"macho"},
      "entryPoint": "",
      "macho": { "cputype": 16777228, "cpusubtype": 0, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "segmentPageSize": 16384,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "bindNow": true
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ],
      "relocations":[{"name":"ARM64_RELOC_BRANCH26","kind":1,"nativeId":620756992}]
    })";
    auto res = ObjectFormatSchema::loadFromText(json);
    ASSERT_FALSE(res.has_value())
        << "4 KiB-congruent text VA under a 16 KiB page must fail validate";
    bool sawMsg = false;
    for (auto const& d : res.error())
        if (d.message.find("EBADMACHO") != std::string::npos
         || d.message.find("segmentPageSize") != std::string::npos)
            sawMsg = true;
    EXPECT_TRUE(sawMsg) << "expected an mmap-congruence diag";
}

// ── __TEXT,__const read-only data section (D-LK1-MACHO-EXEC-DATA-SECTIONS)
//
// The parallel of the ELF `.rodata` cycle: a c-subset `int answer=42;`
// global reaches a loadable __const folded into the R+X __TEXT segment.
// These host-independent pins run on EVERY CI leg (the runtime "exit 42"
// proof only spawns on macos-latest).

namespace {

using dss::macho::test::findSegment;

// Read the __TEXT nsects field (LC_SEGMENT_64 __TEXT, nsects@64) and the
// __const section_64 record (if present) from an emitted Mach-O.
struct ConstSectionView {
    std::uint32_t textNsects = 0;
    bool          hasConst   = false;
    std::uint64_t addr       = 0;   // section_64.addr  @32
    std::uint64_t size       = 0;   // section_64.size  @40
    std::uint32_t offset     = 0;   // section_64.offset@48
    std::uint32_t align      = 0;   // section_64.align @52 (log2)
    std::uint32_t flags      = 0;   // section_64.flags @64
};

[[nodiscard]] ConstSectionView readConstView(
        std::vector<std::uint8_t> const& bytes) {
    ConstSectionView v;
    std::span<std::uint8_t const> sp{bytes};
    auto const segOff = findSegment(sp, "__TEXT");
    if (segOff) v.textNsects = readU32LE(bytes, *segOff + 64);
    auto const secOff = findSection(sp, "__TEXT", "__const");
    if (secOff) {
        v.hasConst = true;
        v.addr   = readU64LE(bytes, *secOff + 32);
        v.size   = readU64LE(bytes, *secOff + 40);
        v.offset = readU32LE(bytes, *secOff + 48);
        v.align  = readU32LE(bytes, *secOff + 52);
        v.flags  = readU32LE(bytes, *secOff + 64);
    }
    return v;
}

// `CodeReloc` selects which code→data relocation shape the fixture's
// function carries into the __const data symbol (SymbolId{50}).
enum class CodeReloc {
    None,      // BL extern + RET; no reloc into the data symbol
    AdrpPage,  // ADRP x0, page(answer) — the PC-relative page-pair half
               //   (Aarch64AdrPrelPgHi21). PC-relative, NO 32-bit guard,
               //   so it resolves cleanly even at Mach-O's >4 GiB VAs.
    AdrpAdd,   // ADRP + ADD lo12(answer) — the FULL GlobalAddr lea. The
               //   ADD half is Aarch64AddAbsLo12, a magnitude-INDEPENDENT
               //   low-12 extraction; the page magnitude is range-checked
               //   by the paired ADRP. Resolves cleanly even at Mach-O's
               //   >4 GiB VAs (D-LK6-AARCH64-ADDABSLO12-HIGH-VA relaxed the
               //   prior ELF-centric UINT32_MAX upper bound).
};

// Build an extern-importing arm64 module (routes the dynamic path) that
// ALSO carries a rodata data item. The function optionally references the
// data symbol so the reloc kernel patches the data VA into the code — the
// host-independent "wire" proof.
[[nodiscard]] AssembledModule makeArm64ModuleWithConst(CodeReloc shape) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    if (shape == CodeReloc::AdrpAdd) {
        // ADRP x0, page(answer)        (reloc kind 2 @ off 0)
        // ADD  x0, x0, lo12(answer)    (reloc kind 3 @ off 4)
        // BL   <extern>                (reloc kind 1 @ off 8)
        // RET
        fn.bytes = {0x00, 0x00, 0x00, 0x90,   // ADRP x0 (imm cleared)
                    0x00, 0x00, 0x00, 0x91,   // ADD  x0,x0 (imm12 cleared)
                    0x00, 0x00, 0x00, 0x94,   // BL (imm26 cleared)
                    0xC0, 0x03, 0x5F, 0xD6};  // RET
        fn.relocations.push_back(
            Relocation{/*offset=*/0, SymbolId{50}, RelocationKind{2}, 0});
        fn.relocations.push_back(
            Relocation{/*offset=*/4, SymbolId{50}, RelocationKind{3}, 0});
        fn.relocations.push_back(
            Relocation{/*offset=*/8, SymbolId{99}, RelocationKind{1}, 0});
    } else if (shape == CodeReloc::AdrpPage) {
        // ADRP x0, page(answer)        (reloc kind 2 @ off 0)
        // <NOP padding to push __const onto a DIFFERENT 4 KiB page than
        //  this ADRP patch site, so the resolved page-pair is NON-ZERO —
        //  a genuine page-of(constVa) encoding, not a degenerate 0 that a
        //  same-page layout would produce>
        // BL   <extern>
        // RET
        fn.bytes = {0x00, 0x00, 0x00, 0x90};  // ADRP x0 (imm cleared)
        fn.relocations.push_back(
            Relocation{/*offset=*/0, SymbolId{50}, RelocationKind{2}, 0});
        // Pad with NOPs (0xD503201F LE) so the function spans > 4 KiB; the
        // __stubs + __const that follow then land on a higher page.
        constexpr std::uint32_t kNop = 0xD503201Fu;
        while (fn.bytes.size() < 0x1010u) {
            fn.bytes.push_back(static_cast<std::uint8_t>(kNop & 0xFFu));
            fn.bytes.push_back(static_cast<std::uint8_t>((kNop >> 8) & 0xFFu));
            fn.bytes.push_back(static_cast<std::uint8_t>((kNop >> 16) & 0xFFu));
            fn.bytes.push_back(static_cast<std::uint8_t>((kNop >> 24) & 0xFFu));
        }
        std::uint32_t const blOff = static_cast<std::uint32_t>(fn.bytes.size());
        fn.bytes.insert(fn.bytes.end(),
                        {0x00, 0x00, 0x00, 0x94,   // BL (imm26 cleared)
                         0xC0, 0x03, 0x5F, 0xD6}); // RET
        fn.relocations.push_back(
            Relocation{/*offset=*/blOff, SymbolId{99}, RelocationKind{1}, 0});
    } else {
        fn.bytes = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6}; // BL;RET
        fn.relocations.push_back(
            Relocation{/*offset=*/0, SymbolId{99}, RelocationKind{1}, 0});
    }
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_abs", "/usr/lib/libSystem.B.dylib"});
    AssembledData d;
    d.symbol    = SymbolId{50};
    d.section   = DataSectionKind::Rodata;
    d.bytes     = {0x2a, 0x00, 0x00, 0x00};   // int answer = 42;
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    return mod;
}

} // namespace

TEST(MachOArm64Exit, MachoArm64ConstSectionFollowsStubsWithComputedVa) {
    auto target = loadArm64Target();
    auto fmt    = loadArm64DarwinExecFormat();
    ASSERT_TRUE(target);
    ASSERT_TRUE(fmt);

    AssembledModule mod = makeArm64ModuleWithConst(CodeReloc::None);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *target, *fmt, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    auto const cv = readConstView(bytes);
    EXPECT_EQ(cv.textNsects, 3u) << "__TEXT must carry __text+__stubs+__const";
    ASSERT_TRUE(cv.hasConst) << "a rodata dataItem must emit __TEXT,__const";

    // __const sits H1-aligned right after __stubs. The single extern's
    // stub stride is 12, so stubsFileSize = 12. The schema floor is 8 and
    // the item alignment is 4, so H1 maxAlign = max(8,4) = 8 → align field
    // log2(8) = 3 (NOT 2: the floor dominates the 4-byte item — identical
    // to the ELF writer, whose .rodata floor is also 8). stubsVa is the
    // page-aligned sectionVa + the 8-byte __text body.
    auto const stubsSec = findSection(std::span<std::uint8_t const>{bytes},
                                      "__TEXT", "__stubs");
    ASSERT_TRUE(stubsSec.has_value());
    std::uint64_t const stubsVa   = readU64LE(bytes, *stubsSec + 32);
    std::uint64_t const stubsSize = readU64LE(bytes, *stubsSec + 40);
    auto const alignUp8 = [](std::uint64_t n) { return (n + 7u) & ~std::uint64_t{7}; };
    EXPECT_EQ(cv.addr, alignUp8(stubsVa + stubsSize))
        << "__const VA = alignUp(stubsVa+stubsSize, 8)";
    EXPECT_EQ(cv.size, 4u) << "the single int global is 4 bytes";
    EXPECT_EQ(cv.align, 3u) << "align (log2) = log2(max(floor 8, item 4)) = 3";
    EXPECT_EQ(cv.flags, 0u) << "__const is S_REGULAR (no attribute bits)";

    // The 4 const bytes `2a 00 00 00` appear at the section's file offset.
    ASSERT_GE(bytes.size(), static_cast<std::size_t>(cv.offset) + 4u);
    EXPECT_EQ(bytes[cv.offset + 0], 0x2au);
    EXPECT_EQ(bytes[cv.offset + 1], 0x00u);
    EXPECT_EQ(bytes[cv.offset + 2], 0x00u);
    EXPECT_EQ(bytes[cv.offset + 3], 0x00u);
}

TEST(MachOArm64Exit, MachoArm64ConstDataSymbolEntersSymbolVaAndPatchesCode) {
    auto target = loadArm64Target();
    auto fmt    = loadArm64DarwinExecFormat();
    ASSERT_TRUE(target);
    ASSERT_TRUE(fmt);

    // The host-independent end-to-end "wire" proof short of execution: a
    // code reloc INTO the __const data symbol resolves through the SAME
    // shared applyExecRelocations kernel. We use the ADRP page-pair half
    // (Aarch64AdrPrelPgHi21) — it is PC-relative, so it resolves cleanly
    // at Mach-O's >4 GiB VAs (the ADD lo12 companion hits a pre-existing
    // kernel guard at those VAs; see the deferral pin below). The ADRP
    // word ALONE proves the data symbol entered symbolVa: its encoded
    // page-pair value is computed from constVa, so a wrong/absent
    // symbolVa entry would either fail loud or encode a different page.
    AssembledModule mod = makeArm64ModuleWithConst(CodeReloc::AdrpPage);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *target, *fmt, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u)
        << "an ADRP code reloc into the __const data symbol must resolve "
           "through the shared applyExecRelocations kernel";
    ASSERT_FALSE(bytes.empty());

    auto const cv = readConstView(bytes);
    ASSERT_TRUE(cv.hasConst);
    std::uint64_t const constVa = cv.addr;

    // The function's __text VA = secText VA (it is functions[0] at offset
    // 0). Read the patched ADRP (word 0) from __text and recompute the
    // expected encoding INDEPENDENTLY from constVa + the patch-site VA
    // (ARM ARM C6.2.10 ADRP). If the data symbol had NOT entered symbolVa,
    // applyExecRelocations would have failed loud (K_SymbolUndefined) and
    // rep.errorCount() != 0 above.
    auto const textSec = findSection(std::span<std::uint8_t const>{bytes},
                                     "__TEXT", "__text");
    ASSERT_TRUE(textSec.has_value());
    std::uint64_t const textVa  = readU64LE(bytes, *textSec + 32);
    std::uint32_t const textOff = readU32LE(bytes, *textSec + 48);
    std::uint32_t const adrp = readU32LE(bytes, textOff + 0);

    auto const pageOf = [](std::uint64_t v) { return v & ~std::uint64_t{0xFFF}; };
    std::uint64_t const adrpPatchVa = textVa + 0;     // ADRP at off 0
    std::int64_t const pageDiff =
        static_cast<std::int64_t>(pageOf(constVa)) -
        static_cast<std::int64_t>(pageOf(adrpPatchVa));
    std::int64_t const adrpValue = pageDiff >> 12;
    std::uint32_t const adrpV = static_cast<std::uint32_t>(adrpValue) & 0x1FFFFFu;
    std::uint32_t const expectAdrp =
        0x90000000u | ((adrpV & 0x3u) << 29) | (((adrpV >> 2) & 0x7FFFFu) << 5);
    EXPECT_EQ(adrp, expectAdrp)
        << "ADRP x0 must encode page(constVa) — data symbol VA resolved "
           "through symbolVa (constVa=0x" << std::hex << constVa << ")";
    // The fixture pads __text past 4 KiB so __const lands on a HIGHER page
    // than the ADRP patch site: the page-pair is NON-ZERO, so the ADRP
    // immediate field genuinely carries page-of(constVa) (not a degenerate
    // same-page 0). A missing symbolVa entry would have fail-louded
    // (K_SymbolUndefined) above; this byte-equality additionally cross-
    // checks symbolVa-target ↔ section_64.addr coherence.
    EXPECT_NE(adrp, 0x90000000u)
        << "ADRP page-pair must be non-zero (constVa is a page above textVa)";
}

// ── POSITIVE pin: the FULL GlobalAddr lea (ADRP + ADD lo12) into __const
//    RESOLVES at Mach-O's >4 GiB VAs — the proof of the high-VA relaxation
//    (D-LK6-AARCH64-ADDABSLO12-HIGH-VA).
//
// `Aarch64AddAbsLo12` (the ADD companion of the ADRP page-pair) computes
// `(S+A) & 0xFFF` — a magnitude-INDEPENDENT low-12 extraction. The shared
// `applyExecRelocations` kernel previously carried an ELF-centric
// `S+A ≤ UINT32_MAX` upper bound that WRONGLY rejected Mach-O PIE VAs (all
// above the 4 GiB pageZeroSize), blocking a real `int answer; return
// answer;` lea (which the producer lowers to ADRP+ADD). The relaxation
// dropped that bound (keeping `S+A ≥ 0`); the ADRP carries the high bits
// PC-relative + range-checked, the ADD needs only the low-12. This pin is
// RED-ON-DISABLE: restoring the UINT32_MAX bound makes encode fail loud
// (errorCount > 0), flipping the errorCount assertion below.
TEST(MachOArm64Exit, MachoArm64ConstAdrpAddLeaHighVaResolvesLowTwelve) {
    auto target = loadArm64Target();
    auto fmt    = loadArm64DarwinExecFormat();
    ASSERT_TRUE(target);
    ASSERT_TRUE(fmt);

    AssembledModule mod = makeArm64ModuleWithConst(CodeReloc::AdrpAdd);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *target, *fmt, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u)
        << "ADRP+ADD lea into a >4 GiB __const VA must RESOLVE: the ADD "
           "lo12 is magnitude-independent (D-LK6-AARCH64-ADDABSLO12-HIGH-VA)";
    ASSERT_FALSE(bytes.empty());

    auto const cv = readConstView(bytes);
    ASSERT_TRUE(cv.hasConst);
    std::uint64_t const constVa = cv.addr;

    // The ADD is word 1 (offset 4) of __text. Recompute the expected
    // encoding INDEPENDENTLY: ADD (imm) base 0x91000000 (x0,x0) OR the
    // low-12 of constVa shifted into imm12 [21:10]. constVa is > 4 GiB
    // (0x1_0000_40xx), so this would have fail-louded under the old bound.
    auto const textSec = findSection(std::span<std::uint8_t const>{bytes},
                                     "__TEXT", "__text");
    ASSERT_TRUE(textSec.has_value());
    std::uint32_t const textOff = readU32LE(bytes, *textSec + 48);
    std::uint32_t const add = readU32LE(bytes, textOff + 4);
    std::uint32_t const expectAdd =
        0x91000000u | (static_cast<std::uint32_t>(constVa & 0xFFFu) << 10);
    EXPECT_EQ(add, expectAdd)
        << "ADD imm12 must encode lo12(constVa)=0x" << std::hex
        << (constVa & 0xFFFu) << " (constVa=0x" << constVa
        << ") — the high-VA low-12 extraction resolved through the kernel";
    // Non-vacuity: constVa genuinely exceeds UINT32_MAX (the old bound), so
    // this test actually exercises the high-VA path the relaxation enables.
    EXPECT_GT(constVa, std::uint64_t{0xFFFFFFFFu})
        << "the proof is vacuous unless constVa is actually > 4 GiB";
}

TEST(MachOArm64Exit, MachoArm64NoDataItemsByteIdenticalToBaseline) {
    auto target = loadArm64Target();
    auto fmt    = loadArm64DarwinExecFormat();
    ASSERT_TRUE(target);
    ASSERT_TRUE(fmt);

    // The mandatory control: the EXISTING macho_arm64_exit-shaped module
    // (extern + function, NO dataItems) → the hasConst=false path. __TEXT
    // must carry exactly 2 sections and NO __const, and the __DATA_CONST
    // segment + __got VAs are exactly where they are WITHOUT a __const
    // section — proving an empty dataItems leaves the output unchanged.
    AssembledModule mod = makeArm64DynamicModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *target, *fmt, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    auto const cv = readConstView(bytes);
    EXPECT_EQ(cv.textNsects, 2u) << "no dataItems → __TEXT keeps 2 sections";
    EXPECT_FALSE(cv.hasConst) << "no dataItems → NO __const section emitted";

    // The __got slot VA + __DATA_CONST segment VA are byte-pinned by the
    // existing decodeSoleStub-based pins (StubEmitsAdrpLdrBrThroughGot et
    // al.) against this SAME no-dataItems fixture. Re-assert the structural
    // VA here: __DATA_CONST starts on the page above __stubs end, with NO
    // __const in between (the hasConst=false control). stubsVa + stubsSize
    // → gotVa = alignUp(that_fileoff, 16384)-relative VA == __got addr.
    std::span<std::uint8_t const> sp{bytes};
    auto const gotSec = findSection(sp, "__DATA_CONST", "__got");
    ASSERT_TRUE(gotSec.has_value())
        << "__got must still exist on the no-dataItems path";
    auto const stubsSec = findSection(sp, "__TEXT", "__stubs");
    ASSERT_TRUE(stubsSec.has_value());
    std::uint64_t const stubsVa   = readU64LE(bytes, *stubsSec + 32);
    std::uint64_t const stubsSize = readU64LE(bytes, *stubsSec + 40);
    std::uint64_t const stubsOff  = readU32LE(bytes, *stubsSec + 48);
    std::uint64_t const gotOff    = readU32LE(bytes, *gotSec + 48);
    // __got file offset = alignUp(stubsEnd_fileoff, 16384) — directly above
    // __stubs (NO __const padding), unchanged from before this cycle.
    std::uint64_t const stubsEndOff = stubsOff + stubsSize;
    auto const alignUp16k = [](std::uint64_t n) {
        return (n + 16383u) & ~std::uint64_t{16383};
    };
    EXPECT_EQ(gotOff, alignUp16k(stubsEndOff))
        << "__got file offset sits directly above __stubs (no __const gap)";
    (void)stubsVa;
}
