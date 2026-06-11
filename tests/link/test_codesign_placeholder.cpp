// LK7 (plan 14) — codesign-placeholder reservation tests.
//
// Pins (across Mach-O + PE):
//   * Schema `codeSignatureSize` / `attributeCertReserveSize` accepted
//     when multiple-of-8; rejected otherwise (PE COFF §5.9.1 / Apple
//     `cs_blobs.h` SuperBlob alignment).
//   * MH_OBJECT / PE Obj reject any non-zero reservation (config-error
//     trap — the placeholder lives only on executable images).
//   * When the reservation is non-zero:
//       - Mach-O emits LC_CODE_SIGNATURE (cmd=0x1D, cmdsize=16)
//         pointing dataoff at the reserved file offset and datasize
//         at the schema-declared byte count.
//       - PE sets `IMAGE_DIRECTORY_ENTRY_SECURITY[4]` to the file
//         offset + size (NOT an RVA; the cert table sits outside
//         the loaded image).
//       - Reserved bytes appear at the file tail (8-byte-aligned),
//         all zero, awaiting plan 16's post-link fill.
//   * Existing schemas with the field absent / zero are unchanged
//     (no LC_CODE_SIGNATURE, no IMAGE_DIRECTORY_ENTRY_SECURITY
//     directory entry).

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/macho.hpp"
#include "link/format/pe.hpp"
#include "link/object_format_schema.hpp"
#include "lir/lir.hpp"
#include "macho_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

// Wrapper around the shared helper so the vector → span conversion
// is implicit at the call sites below (code-simplifier REQUIRED
// fold, LK7 review — reuses tests/link/macho_test_support.hpp).
[[nodiscard]] std::uint32_t readU32LE(std::vector<std::uint8_t> const& bytes,
                                       std::size_t off) {
    return dss::macho::test::readU32LE(std::span<std::uint8_t const>{bytes},
                                       off);
}

[[nodiscard]] AssembledModule
makeTrivialModule(std::uint32_t funcSymV = 1) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{funcSymV};
    fn.bytes  = {0xC3};  // ret
    mod.functions.push_back(std::move(fn));
    return mod;
}

} // namespace

// ── Mach-O: schema validate-reject for invalid reservation sizes ─────

TEST(MachOCodeSignPlaceholder, NonMultipleOfEightRejectedAtLoad) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-bad-cs","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "codeSignatureSize": 17
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(MachOCodeSignPlaceholder, ZeroAcceptedAsDisabled) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-no-cs","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"]
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ]
    })");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->machoImage().codeSignatureSize, 0u);
}

TEST(MachOCodeSignPlaceholder, ObjectFiletypeRejectsCodeSigField) {
    // MH_OBJECT must not declare codeSignatureSize — the
    // placeholder is meaningful only on executable images.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-obj-with-cs","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "object", "flags": 0 },
      "image": { "codeSignatureSize": 4096 },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
}

// ── Mach-O: walker emits LC_CODE_SIGNATURE + zero reservation ────────

TEST(MachOCodeSignPlaceholder, StaticPathRejectsNonZeroCodeSigSize) {
    // silent-failure HIGH fold (LK7 review): the static encodeExec
    // path emits no __LINKEDIT segment, so an LC_CODE_SIGNATURE
    // would point outside any segment and the kernel's
    // cs_validate_range would reject the binary. Walker must fail
    // loud at dispatch, citing the D-LK7-1 anchor.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-cs-static","kind":"macho"},
      "entryPoint": "",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "codeSignatureSize": 4096
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeTrivialModule();  // no externImports
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawAnchor = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport
         && d.actual.find("D-LK7-1") != std::string::npos) {
            sawAnchor = true;
        }
    }
    EXPECT_TRUE(sawAnchor);
}

TEST(MachOCodeSignPlaceholder, StaticPathRejectsTakesPrecedenceOverBindNow) {
    // Dispatch-ordering pin (pr-test-analyzer FOLD-NOW Gap 3,
    // LK7 post-fold review): when codeSignatureSize > 0 AND
    // externImports is empty AND bindNow = false, the codesign-
    // on-static-path gate must fire BEFORE the bindNow gate. A
    // future reorder of the gates would silently switch the
    // diagnostic anchor — users would "fix" bindNow and be
    // confused why codesign still fails.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-cs+lazy-no-externs","kind":"macho"},
      "entryPoint": "",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "bindNow": false,
        "codeSignatureSize": 4096
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeTrivialModule();  // no externImports
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawD_LK7_1 = false;
    bool sawD_LK6_13 = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport) {
            if (d.actual.find("D-LK7-1") != std::string::npos)
                sawD_LK7_1 = true;
            if (d.actual.find("D-LK6-13") != std::string::npos)
                sawD_LK6_13 = true;
        }
    }
    EXPECT_TRUE(sawD_LK7_1)
        << "Codesign-on-static-path gate must fire first; reorder "
           "would silently mask this anchor.";
    EXPECT_FALSE(sawD_LK6_13)
        << "bindNow gate must NOT fire when externImports is empty.";
}

TEST(MachOCodeSignPlaceholder, DynamicPathEmitsLcCodeSignatureWithZeroReservation) {
    // 4096-byte SuperBlob reservation (typical Apple CodeDirectory
    // size) on the dynamic path. The walker must emit
    // LC_CODE_SIGNATURE pointing at a file offset inside __LINKEDIT
    // whose region holds 4096 zero bytes that plan 16 patches
    // post-link. (pr-test-analyzer Gap 1 fold.)
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-cs-dyn","kind":"macho"},
      "entryPoint": "",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "codeSignatureSize": 4096
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ],
      "relocations":[
        {"name":"X86_64_RELOC_BRANCH","kind":1,"nativeId":369098752},
        {"name":"X86_64_RELOC_UNSIGNED_8","kind":2,"nativeId":100663296},
        {"name":"X86_64_RELOC_UNSIGNED_4","kind":3,"nativeId":33554432}
      ]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel{1, SymbolId{99}, RelocationKind{1}, 0};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // Scan load commands for LC_CODE_SIGNATURE (0x1D).
    std::uint32_t ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    std::uint32_t dataOff = 0, dataSize = 0;
    std::uint64_t linkeditFileOff = 0, linkeditFileSize = 0;
    bool foundCS = false;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t cmd = readU32LE(bytes, off);
        std::uint32_t cmdsize = readU32LE(bytes, off + 4);
        if (cmd == 0x1Du) {
            dataOff  = readU32LE(bytes, off + 8);
            dataSize = readU32LE(bytes, off + 12);
            EXPECT_EQ(cmdsize, 16u);
            foundCS = true;
        } else if (cmd == 0x19u) {  // LC_SEGMENT_64
            std::string segName(
                reinterpret_cast<char const*>(&bytes[off + 8]),
                strnlen(reinterpret_cast<char const*>(
                            &bytes[off + 8]), 16));
            if (segName == "__LINKEDIT") {
                for (int b = 0; b < 8; ++b) {
                    linkeditFileOff |= static_cast<std::uint64_t>(
                                           bytes[off + 40 + b]) << (b*8);
                    linkeditFileSize |= static_cast<std::uint64_t>(
                                            bytes[off + 48 + b]) << (b*8);
                }
            }
        }
        off += cmdsize;
    }
    ASSERT_TRUE(foundCS);
    EXPECT_EQ(dataSize, 4096u);
    EXPECT_EQ(dataOff % 8u, 0u);
    ASSERT_LE(static_cast<std::size_t>(dataOff) + dataSize, bytes.size());
    // __LINKEDIT.filesize must cover the reservation so dyld maps
    // the SuperBlob in alongside the other linkedit payloads
    // (silent-failure HIGH fold).
    ASSERT_NE(linkeditFileOff, 0u);
    EXPECT_GE(linkeditFileOff + linkeditFileSize,
              static_cast<std::uint64_t>(dataOff) + dataSize);
    // Reserved bytes are zero.
    for (std::uint32_t i = 0; i < dataSize; ++i) {
        ASSERT_EQ(bytes[dataOff + i], 0u);
    }
}

TEST(MachOCodeSignPlaceholder, ZeroSizeOmitsLcCodeSignature) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    EXPECT_EQ((*fmt)->machoImage().codeSignatureSize, 0u);
    AssembledModule mod = makeTrivialModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    std::uint32_t ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t cmd = readU32LE(bytes, off);
        std::uint32_t cmdsize = readU32LE(bytes, off + 4);
        EXPECT_NE(cmd, 0x1Du)
            << "LC_CODE_SIGNATURE must not be emitted when "
               "codeSignatureSize == 0";
        off += cmdsize;
    }
}

// ── PE: schema validate-reject for invalid reservation sizes ─────────

TEST(PeCertPlaceholder, NonMultipleOfEightRejectedAtLoad) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"pe-bad-cert","kind":"pe"},
      "pe": { "machine": 34404, "type": "exec" },
      "optionalHeader": {
        "magic": 523, "imageBase": 5368709120,
        "sectionAlignment": 4096, "fileAlignment": 512,
        "subsystem": 3, "dllCharacteristics": 0,
        "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096,
        "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096,
        "attributeCertReserveSize": 7
      },
      "sections":[
        {"kind":"text","name":".text","type":0,"flags":1610612768,"addrAlign":16,"entrySize":0,"virtualAddress":4096}
      ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(PeCertPlaceholder, ZeroAcceptedAsDisabled) {
    auto r = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->peOptionalHeader().attributeCertReserveSize, 0u);
}

TEST(PeCertPlaceholder, ObjFormatRejectsCertField) {
    // PE .obj must not declare attributeCertReserveSize.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"pe-obj-with-cert","kind":"pe"},
      "pe": { "machine": 34404, "type": "obj" },
      "optionalHeader": { "attributeCertReserveSize": 1024 },
      "sections":[{"kind":"text","name":".text","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
}

// ── PE: walker emits security directory entry + zero reservation ─────

TEST(PeCertPlaceholder, WalkerEmitsSecurityDirAndZeroReservation) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"pe-with-cert","kind":"pe"},
      "entryPoint": "",
      "pe": { "machine": 34404, "characteristics": 34, "type": "exec" },
      "optionalHeader": {
        "magic": 523, "imageBase": 5368709120,
        "sectionAlignment": 4096, "fileAlignment": 512,
        "majorOperatingSystemVersion": 6, "minorOperatingSystemVersion": 0,
        "majorSubsystemVersion": 6, "minorSubsystemVersion": 0,
        "subsystem": 3, "dllCharacteristics": 33120,
        "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096,
        "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096,
        "attributeCertReserveSize": 2048
      },
      "sections":[
        {"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}
      ],
      "relocations":[
        {"name":"IMAGE_REL_AMD64_REL32","kind":1,"nativeId":4},
        {"name":"IMAGE_REL_AMD64_ADDR64","kind":2,"nativeId":1},
        {"name":"IMAGE_REL_AMD64_ADDR32","kind":3,"nativeId":2}
      ]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeTrivialModule();
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // Walk PE header: DOS(64) + DOS stub(64) + "PE\0\0"(4) + FileHdr(20)
    //                = 152 → optional header starts here.
    // Optional header fixed = 112 bytes → data directory starts at
    // 152 + 112 = 264. Directory[4] (security) is at offset
    // 264 + 4 * 8 = 296 (RVA u32 + Size u32 = 8 bytes per entry).
    constexpr std::size_t kSecurityDirOff = 152 + 112 + 4 * 8;
    std::uint32_t const certFileOff = readU32LE(bytes, kSecurityDirOff);
    std::uint32_t const certSize    = readU32LE(bytes, kSecurityDirOff + 4);
    EXPECT_EQ(certSize, 2048u);
    EXPECT_NE(certFileOff, 0u);
    EXPECT_EQ(certFileOff % 8u, 0u)
        << "WIN_CERTIFICATE table must be 8-byte aligned per "
           "PE COFF §5.9.1";
    ASSERT_LE(static_cast<std::size_t>(certFileOff) + certSize,
              bytes.size());
    for (std::uint32_t i = 0; i < certSize; ++i) {
        ASSERT_EQ(bytes[certFileOff + i], 0u)
            << "cert table byte " << i << " must be zero";
    }
}

TEST(PeExecFormatJsonValidate,
     ExecMissingExecutableImageCharacteristicRejected) {
    // architect LK7-readiness gap: PE32+ Exec without
    // IMAGE_FILE_EXECUTABLE_IMAGE (0x0002) in characteristics is
    // unrunnable on Windows (loader returns ERROR_BAD_EXE_FORMAT
    // with no user-visible diagnostic). Validate must reject at
    // schema load so a hand-rolled JSON missing the bit can't
    // silently produce a non-executable binary.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"pe-no-exec-bit","kind":"pe"},
      "pe": { "machine": 34404, "characteristics": 0, "type": "exec" },
      "optionalHeader": {
        "magic": 523, "imageBase": 5368709120,
        "sectionAlignment": 4096, "fileAlignment": 512,
        "subsystem": 3, "dllCharacteristics": 0,
        "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096,
        "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096
      },
      "sections":[
        {"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}
      ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(PeCertPlaceholder, CertTableLandsAfterIdataWhenImportsPresent) {
    // pr-test-analyzer Gap 2 fold: when both .idata AND cert
    // reservation are emitted, the cert table must sit AFTER
    // idataRawPointer + idataRawSize. A future refactor that
    // accidentally reordered raw-data offsets would overlap the
    // cert table with the IAT — the Windows loader would still
    // load the image, but the imports would silently corrupt at
    // first call (or Authenticode signing would corrupt the IAT).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"pe-imports-with-cert","kind":"pe"},
      "entryPoint": "",
      "pe": { "machine": 34404, "characteristics": 34, "type": "exec" },
      "optionalHeader": {
        "magic": 523, "imageBase": 5368709120,
        "sectionAlignment": 4096, "fileAlignment": 512,
        "majorOperatingSystemVersion": 6, "minorOperatingSystemVersion": 0,
        "majorSubsystemVersion": 6, "minorSubsystemVersion": 0,
        "subsystem": 3, "dllCharacteristics": 33120,
        "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096,
        "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096,
        "attributeCertReserveSize": 2048
      },
      "sections":[
        {"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}
      ],
      "relocations":[
        {"name":"IMAGE_REL_AMD64_REL32","kind":1,"nativeId":4},
        {"name":"IMAGE_REL_AMD64_ADDR64","kind":2,"nativeId":1},
        {"name":"IMAGE_REL_AMD64_ADDR32","kind":3,"nativeId":2}
      ]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xFF, 0x15, 0, 0, 0, 0, 0xC3};
    Relocation rel{2, SymbolId{99}, RelocationKind{1}, 0};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "GetStdHandle", "kernel32.dll"});
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    constexpr std::size_t kSecurityDirOff = 152 + 112 + 4 * 8;
    std::uint32_t const certFileOff = readU32LE(bytes, kSecurityDirOff);
    std::uint32_t const certSize    = readU32LE(bytes, kSecurityDirOff + 4);
    EXPECT_EQ(certSize, 2048u);
    EXPECT_NE(certFileOff, 0u);
    EXPECT_EQ(certFileOff % 8u, 0u);
    // The cert table must sit at the file tail (after both .text
    // AND .idata). Verify it doesn't overlap any section by
    // checking that certFileOff + certSize matches bytes.size().
    EXPECT_EQ(static_cast<std::size_t>(certFileOff) + certSize,
              bytes.size());
    for (std::uint32_t i = 0; i < certSize; ++i) {
        ASSERT_EQ(bytes[certFileOff + i], 0u);
    }
}

TEST(PeCertPlaceholder, ZeroSizeOmitsSecurityDir) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeTrivialModule();
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    constexpr std::size_t kSecurityDirOff = 152 + 112 + 4 * 8;
    EXPECT_EQ(readU32LE(bytes, kSecurityDirOff), 0u);
    EXPECT_EQ(readU32LE(bytes, kSecurityDirOff + 4), 0u);
}
