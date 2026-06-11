// D-LK7-ADHOC-CODESIGN-MACHO (increment 2/2) — ad-hoc Mach-O code-
// signature FILL tests.
//
// Increment 1/2 reserved a zero-filled LC_CODE_SIGNATURE region (see
// tests/link/test_codesign_placeholder.cpp). This file pins the FILL:
// the walker, when the schema carries an ad-hoc `codeSignature` block,
// writes a real CS_SuperBlob + CS_CodeDirectory into the reservation,
// sized EXACTLY by `adHocCodeSignatureSize` (Condition 2), with one
// SHA-256 page hash per code page over the signed file bytes.
//
// Pins:
//   * SuperBlob/CodeDirectory headers decode as BIG-ENDIAN (every
//     field) — a BE→LE regression on any pinned field fails here, and
//     a zeroed page hash fails the byte-match (red-on-disable levers
//     EXIST without being demonstrated here).
//   * The embedded page hashes byte-match a fresh `dss::crypto::sha256`
//     over the emitted bytes[0, codeLimit).
//   * blob.size() == reservation == LC_CODE_SIGNATURE.datasize, and
//     the derived `adHocCodeSignatureSize` equals the built blob length
//     (the size derivation and the builder agree to the byte).
//   * Malformed `codeSignature` (bad kind / bad hashAlgorithm /
//     non-power-of-two pageSize / empty identifier) and a MH_OBJECT
//     carrying `codeSignature` each fail loud at schema load.

#include "core/crypto/sha256.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/macho.hpp"
#include "link/format/macho_codesign.hpp"
#include "link/object_format_schema.hpp"
#include "macho_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::uint32_t readU32LE(std::vector<std::uint8_t> const& b,
                                      std::size_t off) {
    return dss::macho::test::readU32LE(std::span<std::uint8_t const>{b}, off);
}

// Big-endian readers — the SuperBlob / CodeDirectory headers are BE,
// the lone exception to Mach-O's otherwise-LE record layout. The shared
// helpers (link_test_support.hpp) are LE-only by design, so the codesign
// tests carry their own BE readers (mirrors the builder's own BE/LE
// split). These ARE the red-on-disable detectors: read a field the
// walker wrote LE through readU32BE and the magic/length assert fails.
[[nodiscard]] std::uint32_t readU32BE(std::vector<std::uint8_t> const& b,
                                      std::size_t off) {
    return (static_cast<std::uint32_t>(b[off + 0]) << 24) |
           (static_cast<std::uint32_t>(b[off + 1]) << 16) |
           (static_cast<std::uint32_t>(b[off + 2]) <<  8) |
            static_cast<std::uint32_t>(b[off + 3]);
}
[[nodiscard]] std::uint64_t readU64BE(std::vector<std::uint8_t> const& b,
                                      std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | static_cast<std::uint64_t>(b[off + i]);
    return v;
}

// Build an x86_64-darwin DYNAMIC-path Mach-O schema carrying an ad-hoc
// `codeSignature` block (otherwise identical to the increment-1 dynamic
// fixture). Mach-O codesign is format-specific (the walker owns it), so
// a Mach-O schema is the correct vehicle regardless of CPU; x86_64 here
// keeps the fixture host-buildable while the arm64 corpus carries the
// runnable end-to-end proof.
[[nodiscard]] std::string codeSignSchemaJson(char const* identifier,
                                             unsigned pageSize = 4096) {
    return std::string(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-cs-adhoc","kind":"macho"},
      "entryPoint": "",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "codeSignature": { "kind": "adhoc", "hashAlgorithm": "sha256", "pageSize": )")
        + std::to_string(pageSize) + R"(, "identifier": ")"
        + identifier + R"(" }
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
}

// A minimal extern-importing module (drives the dynamic path).
[[nodiscard]] AssembledModule makeDynamicModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel{1, SymbolId{99}, RelocationKind{1}, 0};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf", "/usr/lib/libSystem.B.dylib"});
    return mod;
}

// Locate the CodeDirectory in an emitted Mach-O (LC_CODE_SIGNATURE →
// SuperBlob → BlobIndex[0].offset → CD). Returns the CD file offset, or 0
// if no LC_CODE_SIGNATURE is present.
[[nodiscard]] std::size_t locateCodeDir(std::vector<std::uint8_t> const& bytes) {
    std::uint32_t const ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    std::uint32_t dataOff = 0;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmd == 0x1Du) dataOff = readU32LE(bytes, off + 8);
        if (cmdsize == 0u) return 0;
        off += cmdsize;
    }
    if (dataOff == 0u) return 0;
    return dataOff + readU32BE(bytes, dataOff + 16);  // + BlobIndex[0].offset
}

} // namespace

// ── The FILL: real CodeDirectory + SuperBlob, byte-pinned ────────────

TEST(MachOAdHocCodeSign, DynamicPathFillsCodeDirectorySuperBlob) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt =
        ObjectFormatSchema::loadFromText(codeSignSchemaJson("dss.codesign.test"));
    ASSERT_TRUE(fmt.has_value());
    ASSERT_TRUE((*fmt)->machoImage().codeSignature.has_value());

    AssembledModule mod = makeDynamicModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // Locate LC_CODE_SIGNATURE (0x1D) → dataoff / datasize.
    std::uint32_t ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    std::uint32_t dataOff = 0, dataSize = 0;
    bool foundCS = false;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t cmd = readU32LE(bytes, off);
        std::uint32_t cmdsize = readU32LE(bytes, off + 4);
        if (cmd == 0x1Du) {
            dataOff  = readU32LE(bytes, off + 8);
            dataSize = readU32LE(bytes, off + 12);
            foundCS = true;
        }
        ASSERT_NE(cmdsize, 0u);
        off += cmdsize;
    }
    ASSERT_TRUE(foundCS);
    ASSERT_LE(static_cast<std::size_t>(dataOff) + dataSize, bytes.size());

    std::size_t const sb = dataOff;  // SuperBlob start

    // ── CS_SuperBlob (BIG-ENDIAN) ──
    EXPECT_EQ(readU32BE(bytes, sb + 0), 0xFADE0CC0u);  // magic
    EXPECT_EQ(readU32BE(bytes, sb + 4), dataSize);     // length == datasize
    EXPECT_EQ(readU32BE(bytes, sb + 8), 1u);           // count
    // BlobIndex[0]: type = CSSLOT_CODEDIRECTORY (0), offset → CD.
    EXPECT_EQ(readU32BE(bytes, sb + 12), 0u);
    std::uint32_t const cdRel = readU32BE(bytes, sb + 16);
    EXPECT_EQ(cdRel, 20u);  // SuperBlob(12) + 1×BlobIndex(8)
    std::size_t const cd = sb + cdRel;

    // ── CS_CodeDirectory (BIG-ENDIAN) ──
    EXPECT_EQ(readU32BE(bytes, cd + 0),  0xFADE0C02u);  // magic
    EXPECT_EQ(readU32BE(bytes, cd + 8),  0x00020400u);  // version
    EXPECT_EQ(readU32BE(bytes, cd + 12), 0x00000002u);  // flags = CS_ADHOC
    std::uint32_t const hashOffset = readU32BE(bytes, cd + 16);
    std::uint32_t const identOffset = readU32BE(bytes, cd + 20);
    EXPECT_EQ(readU32BE(bytes, cd + 24), 0u);           // nSpecialSlots
    std::uint32_t const nCodeSlots = readU32BE(bytes, cd + 28);
    std::uint32_t const codeLimit = readU32BE(bytes, cd + 32);
    EXPECT_EQ(bytes[cd + 36], 32u);  // hashSize  = SHA-256 digest length
    EXPECT_EQ(bytes[cd + 37], 2u);   // hashType  = CS_HASHTYPE_SHA256
    EXPECT_EQ(bytes[cd + 38], 0u);   // platform
    EXPECT_EQ(bytes[cd + 39], 12u);  // pageSize  = log2(4096)
    // execSegBase = 0, execSegLimit = __TEXT filesize, flags = MAIN.
    EXPECT_EQ(readU64BE(bytes, cd + 64), 0u);            // execSegBase
    EXPECT_GT(readU64BE(bytes, cd + 72), 0u);            // execSegLimit
    EXPECT_EQ(readU64BE(bytes, cd + 80), 1u);            // CS_EXECSEG_MAIN_BINARY

    // codeLimit == the signature's file offset (== dataoff): everything
    // before the signature is what gets hashed.
    EXPECT_EQ(codeLimit, dataOff);
    // nCodeSlots == ceil(codeLimit / 4096).
    EXPECT_EQ(nCodeSlots, (codeLimit + 4095u) / 4096u);

    // identifier C-string at identOffset (offset is CD-relative).
    EXPECT_EQ(identOffset, 88u);  // the v0x20400 fixed-header length
    std::string const ident(reinterpret_cast<char const*>(&bytes[cd + identOffset]));
    EXPECT_EQ(ident, "dss.codesign.test");

    // ── Page hashes: recompute via sha256 over bytes[0, codeLimit) and
    //    byte-match each embedded 32-byte slot. ──
    ASSERT_LE(cd + hashOffset + static_cast<std::size_t>(nCodeSlots) * 32u,
              bytes.size());
    for (std::uint32_t s = 0; s < nCodeSlots; ++s) {
        std::uint32_t const start = s * 4096u;
        std::uint32_t const len   = std::min(4096u, codeLimit - start);
        std::array<std::uint8_t, 32> const expect =
            dss::crypto::sha256(std::span<std::uint8_t const>{
                bytes.data() + start, len});
        std::size_t const slot = cd + hashOffset + static_cast<std::size_t>(s) * 32u;
        for (int k = 0; k < 32; ++k) {
            ASSERT_EQ(bytes[slot + k], expect[k])
                << "page " << s << " hash byte " << k << " mismatch";
        }
    }

    // ── Condition 2: blob.size() == reservation == datasize. ──
    std::uint32_t const derived = dss::macho::detail::adHocCodeSignatureSize(
        codeLimit, 4096u, "dss.codesign.test");
    EXPECT_EQ(dataSize, derived);
    // The blob occupies exactly [dataOff, dataOff + dataSize) at the
    // file tail (no slack after it).
    EXPECT_EQ(static_cast<std::size_t>(dataOff) + dataSize, bytes.size());
}

// ── Builder-level Condition 2: size fn == built blob length ──────────

TEST(MachOAdHocCodeSign, SizeFunctionMatchesBuiltBlobLength) {
    // Host-independent: assemble over synthetic bytes and confirm the
    // derived size and the actual blob agree across page-boundary cases
    // (exact multiple, +1 byte → extra slot, sub-page).
    auto check = [](std::uint32_t codeLimit, std::uint32_t pageSize,
                    std::string_view id) {
        std::vector<std::uint8_t> buf(codeLimit, std::uint8_t{0xAB});
        auto const blob = dss::macho::detail::buildAdHocCodeSignature(
            std::span<std::uint8_t const>{buf}, codeLimit, pageSize, id,
            /*execSegLimit=*/0x4000);
        std::uint32_t const sz =
            dss::macho::detail::adHocCodeSignatureSize(codeLimit, pageSize, id);
        EXPECT_EQ(blob.size(), sz)
            << "codeLimit=" << codeLimit << " pageSize=" << pageSize;
        // nCodeSlots derivation is observable in the size: the blob must
        // be at least one slot.
        std::uint32_t const nSlots = (codeLimit + pageSize - 1u) / pageSize;
        EXPECT_GE(nSlots, 1u);
    };
    check(4096u, 4096u, "id.exact");        // one full page
    check(4097u, 4096u, "id.plusone");      // spills into a 2nd slot
    check(100u,  4096u, "id.subpage");      // single short page
    check(8192u, 4096u, "id.twopages");     // two full pages
    check(12345u, 4096u, "longer.identifier.string");
}

// ── Non-default pageSize flows loader → builder → bytes ──────────────

TEST(MachOAdHocCodeSign, NonDefaultPageSizeFlowsThroughWire) {
    // Drive a NON-default pageSize (16384 ≠ the 4096 struct default)
    // through the full JSON loader → builder → emitted bytes: the
    // CodeDirectory pageSize byte must become log2(16384) = 14. This
    // proves the JSON `pageSize` is a LIVE wire (a dead field, or a
    // builder ignoring it, would leave the default 12) — not a pass that
    // only works because the value equals the default.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(
        codeSignSchemaJson("dss.cs.pgsz", 16384u));
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeDynamicModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    std::size_t const cd = locateCodeDir(bytes);
    ASSERT_NE(cd, 0u);
    EXPECT_EQ(bytes[cd + 39], 14u);  // log2(16384) — the non-default flowed through
}

// ── Malformed codeSignature blocks fail loud at schema load ──────────

namespace {
[[nodiscard]] std::string codeSignSchemaRaw(char const* csBody) {
    return std::string(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-cs-bad","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "codeSignature": )") + csBody + R"(
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ]
    })";
}

// Count diagnostics whose JSON-pointer path contains `needle`.
[[nodiscard]] std::size_t
countAtPath(LoadResult<std::shared_ptr<ObjectFormatSchema>> const& r,
            char const* needle) {
    if (r.has_value()) return 0;
    std::size_t n = 0;
    for (auto const& d : r.error()) {
        if (d.path.find(needle) != std::string::npos) ++n;
    }
    return n;
}
} // namespace

TEST(MachOAdHocCodeSign, BadKindFailsLoud) {
    auto r = ObjectFormatSchema::loadFromText(codeSignSchemaRaw(
        R"({ "kind": "fullchain", "hashAlgorithm": "sha256", "identifier": "x" })"));
    ASSERT_FALSE(r.has_value());
    EXPECT_GE(countAtPath(r, "/image/codeSignature/kind"), 1u);
}

TEST(MachOAdHocCodeSign, BadHashAlgorithmFailsLoud) {
    auto r = ObjectFormatSchema::loadFromText(codeSignSchemaRaw(
        R"({ "kind": "adhoc", "hashAlgorithm": "sha1", "identifier": "x" })"));
    ASSERT_FALSE(r.has_value());
    EXPECT_GE(countAtPath(r, "/image/codeSignature/hashAlgorithm"), 1u);
}

TEST(MachOAdHocCodeSign, NonPowerOfTwoPageSizeFailsLoud) {
    auto r = ObjectFormatSchema::loadFromText(codeSignSchemaRaw(
        R"({ "kind": "adhoc", "hashAlgorithm": "sha256", "pageSize": 3000, "identifier": "x" })"));
    ASSERT_FALSE(r.has_value());
    EXPECT_GE(countAtPath(r, "/image/codeSignature/pageSize"), 1u);
}

TEST(MachOAdHocCodeSign, PageSizeBelowFloorFailsLoud) {
    // 1024 is a power of two but below the [4096, 65536] code-signing
    // floor — rejected (a sub-page pageSize would overflow the slot-table
    // size math on a multi-GiB binary; the floor closes it at load).
    auto r = ObjectFormatSchema::loadFromText(codeSignSchemaRaw(
        R"({ "kind": "adhoc", "hashAlgorithm": "sha256", "pageSize": 1024, "identifier": "x" })"));
    ASSERT_FALSE(r.has_value());
    EXPECT_GE(countAtPath(r, "/image/codeSignature/pageSize"), 1u);
}

TEST(MachOAdHocCodeSign, PageSizeAboveCeilingFailsLoud) {
    // 131072 is a power of two but above the [4096, 65536] ceiling
    // (log2 = 17 would not be a conventional code-signing page size).
    auto r = ObjectFormatSchema::loadFromText(codeSignSchemaRaw(
        R"({ "kind": "adhoc", "hashAlgorithm": "sha256", "pageSize": 131072, "identifier": "x" })"));
    ASSERT_FALSE(r.has_value());
    EXPECT_GE(countAtPath(r, "/image/codeSignature/pageSize"), 1u);
}

TEST(MachOAdHocCodeSign, EmptyIdentifierFailsLoud) {
    auto r = ObjectFormatSchema::loadFromText(codeSignSchemaRaw(
        R"({ "kind": "adhoc", "hashAlgorithm": "sha256", "identifier": "" })"));
    ASSERT_FALSE(r.has_value());
    EXPECT_GE(countAtPath(r, "/image/codeSignature/identifier"), 1u);
}

TEST(MachOAdHocCodeSign, MissingIdentifierFailsLoud) {
    auto r = ObjectFormatSchema::loadFromText(codeSignSchemaRaw(
        R"({ "kind": "adhoc", "hashAlgorithm": "sha256" })"));
    ASSERT_FALSE(r.has_value());
    EXPECT_GE(countAtPath(r, "/image/codeSignature/identifier"), 1u);
}

TEST(MachOAdHocCodeSign, ObjectFiletypeWithCodeSignatureFailsLoud) {
    // The image block (including codeSignature) is meaningful only on a
    // MH_EXECUTE image; a MH_OBJECT carrying it is a config error.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-obj-with-cs","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "object", "flags": 0 },
      "image": { "codeSignature": { "kind": "adhoc", "hashAlgorithm": "sha256", "identifier": "x" } },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
    EXPECT_GE(countAtPath(r, "/image"), 1u);
}
