#pragma once

// Shared helpers for Mach-O writer tests. Promoted from per-test-
// file duplicates after LK6 cycle 2c's 3 byte-pin tests tripped the
// 3rd-consumer threshold for the LC-scan walk (code-simplifier
// REQUIRED fold, post-fold review). Mirrors tests/asm/asm_test_
// support.hpp precedent.

#include "link_test_support.hpp"

#include "link/format/macho.hpp"          // requestsCodeSignature
#include "link/object_format_schema.hpp"  // loadUnsignedExec

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dss::macho::test {

// Walk the Mach-O load-command sequence and return the byte offset
// of the first load command whose `cmd` field matches `cmd`. The
// returned offset points at the START of the load command (i.e. its
// `cmd` u32), so callers can read sub-fields by adding their
// documented offsets from `<mach-o/loader.h>`. Returns nullopt if
// the command is not present.
[[nodiscard]] inline std::optional<std::size_t>
findLoadCommand(std::span<std::uint8_t const> bytes, std::uint32_t cmd) {
    if (bytes.size() < 32) return std::nullopt;
    std::uint32_t const ncmds =
        static_cast<std::uint32_t>(bytes[16]) |
        (static_cast<std::uint32_t>(bytes[17]) << 8) |
        (static_cast<std::uint32_t>(bytes[18]) << 16) |
        (static_cast<std::uint32_t>(bytes[19]) << 24);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (off + 8 > bytes.size()) return std::nullopt;
        std::uint32_t const thisCmd =
            static_cast<std::uint32_t>(bytes[off]) |
            (static_cast<std::uint32_t>(bytes[off+1]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+2]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+3]) << 24);
        std::uint32_t const cmdsize =
            static_cast<std::uint32_t>(bytes[off+4]) |
            (static_cast<std::uint32_t>(bytes[off+5]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+6]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+7]) << 24);
        if (thisCmd == cmd) return off;
        if (cmdsize == 0) return std::nullopt;  // malformed
        off += cmdsize;
    }
    return std::nullopt;
}

// Walk the Mach-O LC_SEGMENT_64 sequence to locate the segment with
// the given name. Returns the byte offset of the LC_SEGMENT_64
// header (the cmd field), or nullopt if not found.
[[nodiscard]] inline std::optional<std::size_t>
findSegment(std::span<std::uint8_t const> bytes, std::string_view name) {
    if (bytes.size() < 32) return std::nullopt;
    std::uint32_t const ncmds =
        static_cast<std::uint32_t>(bytes[16]) |
        (static_cast<std::uint32_t>(bytes[17]) << 8) |
        (static_cast<std::uint32_t>(bytes[18]) << 16) |
        (static_cast<std::uint32_t>(bytes[19]) << 24);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (off + 24 > bytes.size()) return std::nullopt;
        std::uint32_t const thisCmd =
            static_cast<std::uint32_t>(bytes[off]) |
            (static_cast<std::uint32_t>(bytes[off+1]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+2]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+3]) << 24);
        std::uint32_t const cmdsize =
            static_cast<std::uint32_t>(bytes[off+4]) |
            (static_cast<std::uint32_t>(bytes[off+5]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+6]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+7]) << 24);
        if (thisCmd == 0x19u) {  // LC_SEGMENT_64
            std::string segName(
                reinterpret_cast<char const*>(&bytes[off + 8]),
                strnlen(reinterpret_cast<char const*>(
                            &bytes[off + 8]), 16));
            if (segName == name) return off;
        }
        if (cmdsize == 0) return std::nullopt;
        off += cmdsize;
    }
    return std::nullopt;
}

// Locate a section_64 record by (segment name, section name).
// Returns the byte offset of the section_64 record (NOT the
// containing LC_SEGMENT_64), or nullopt.
//
// ⚠ IMAGE-SHAPED ONLY — it resolves the SEGMENT through `findSegment`, i.e. by
// the name on the LC_SEGMENT_64 COMMAND. That is right for an MH_EXECUTE /
// MH_DYLIB (`__TEXT`, `__DATA_CONST`, …) and CANNOT work for an MH_OBJECT: a
// relocatable object carries ONE anonymous catch-all segment whose segname is
// the empty string (`macho.cpp`: `appendName16(bytes, "")  // segname empty for
// MH_OBJECT`), and the two-level naming lives in each section_64's OWN
// `segname` field instead. So `findSection(obj, "__TEXT", "__text")` returns
// nullopt for an object that plainly has that section — a silent
// not-found, not an error. MEASURED 2026-08-13 (it cost a debug cycle in
// tests/link/test_macho_ld64_local_collision.cpp, which now carries the
// object-shaped `findObjectSection` that matches on the section record's own
// pair). Left UNCHANGED here on purpose: teaching this one to fall back to the
// section's segname would make an exec test that names the WRONG segment pass
// anyway, and that is the assertion those callers are paying for.
[[nodiscard]] inline std::optional<std::size_t>
findSection(std::span<std::uint8_t const> bytes,
            std::string_view segment, std::string_view section) {
    auto segOff = findSegment(bytes, segment);
    if (!segOff) return std::nullopt;
    if (*segOff + 68 > bytes.size()) return std::nullopt;
    std::uint32_t const nsects =
        static_cast<std::uint32_t>(bytes[*segOff + 64]) |
        (static_cast<std::uint32_t>(bytes[*segOff + 65]) << 8) |
        (static_cast<std::uint32_t>(bytes[*segOff + 66]) << 16) |
        (static_cast<std::uint32_t>(bytes[*segOff + 67]) << 24);
    std::size_t secOff = *segOff + 72;
    for (std::uint32_t s = 0; s < nsects; ++s) {
        if (secOff + 16 > bytes.size()) return std::nullopt;
        std::string secName(
            reinterpret_cast<char const*>(&bytes[secOff]),
            strnlen(reinterpret_cast<char const*>(
                        &bytes[secOff]), 16));
        if (secName == section) return secOff;
        secOff += 80;
    }
    return std::nullopt;
}

// readU32LE / readU64LE re-exported from the shared substrate
// (`link_test_support.hpp`). Keeping the `dss::macho::test::*`
// symbol names alive via `using` declarations avoids touching the
// existing Mach-O test call sites — one-line aliases rather than
// 4-line delegating wrappers (simplifier post-fold review: the
// wrappers were over-engineered DRY violations).
using ::dss::link_format::test::readU32LE;
using ::dss::link_format::test::readU64LE;

// ── AN EXEC SCHEMA THE STATIC WALKER MAY LEGALLY ENCODE ──────────────
//
// D-LK-MACHO-ADHOC-SIGNATURE-DROPPED-ON-STATIC-ARM.
//
// `macho::encodeExec` — the arm `macho::encode` takes when `externImports`
// is empty — builds no `__LINKEDIT`, so it can host no LC_CODE_SIGNATURE
// under ANY schema. `macho::encode` therefore REFUSES a zero-extern module
// whenever the format requests a signature by either key, instead of
// encoding it and dropping the request in silence. EVERY shipped Darwin exec
// document requests one (it is what makes the image pass AMFI), so the static
// arm is unreachable from a shipped exec schema by construction.
//
// That leaves the static walker live code with no schema to drive it, which
// is what this returns: **the shipped exec document MINUS its signature
// request, and nothing else changed.** Keeping every other field is not
// tidiness — `macho64-arm64-darwin-exec` also declares `image.buildVersion`,
// which `encodeExec` refuses on its own, and dropping that too would silently
// delete the only pin that boundary has that runs.
// D-LK10-ENTRY-MACHO-STATIC-BUILD-VERSION
//
// ★ THE CROSS-CHECK IS THE POINT, NOT THE COPY. A synthetic document that
// merely resembles the shipped one drifts out from under its pins the first
// time the shipped one moves, so every field a static-arm test reads back out
// of an emitted image is asserted here to still equal the shipped document's,
// and the ONE sanctioned divergence is asserted in BOTH directions.
//
// Returns nullptr after ADD_FAILURE on any trouble; callers assert on it.
[[nodiscard]] inline std::shared_ptr<ObjectFormatSchema>
loadUnsignedExec(std::string_view shippedExecName) {
    // Values copied from the shipped documents. Divergences from a sibling
    // are REAL and per-port: arm64 uses a 16 KiB VM segment page (Apple
    // Silicon rejects 4 KiB-aligned segments at exec) with __text at
    // pageZero+0x4000, and declares `image.buildVersion`; x86_64 uses the
    // 4 KiB default with __text at pageZero+0x1000 and deliberately omits
    // `buildVersion`. Neither carries `codeSignature` — that omission is
    // this fixture's entire reason to exist.
    constexpr std::string_view kArm64Json = R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
      "cCallingConvention": { "convention": "apple_arm64" },
      "outputExtension": "",
      "dataModel": "LP64",
      "headerNameMatching": "case-insensitive",
      "format": {"name":"macho64-arm64-darwin-exec-unsigned","kind":"macho"},
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "_exit" },
      "entryCallingConvention": "apple_arm64",
      "entryPoint": "",
      "macho": { "cputype": 16777228, "cpusubtype": 0, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "segmentPageSize": 16384,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "bindNow": true,
        "buildVersion": {"platform":"macos","minOs":"11.0","sdk":"11.0"}
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294983680}
      ],
      "relocations":[
        {"name":"ARM64_RELOC_BRANCH26","kind":1,"nativeId":620756992,"isCall":true},
        {"name":"ARM64_RELOC_PAGE21","kind":2,"nativeId":889192448},
        {"name":"ARM64_RELOC_PAGEOFF12","kind":3,"nativeId":1140850688},
        {"name":"ARM64_RELOC_UNSIGNED","kind":4,"nativeId":100663296}
      ]
    })";
    constexpr std::string_view kX86Json = R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
      "cCallingConvention": { "convention": "sysv_amd64" },
      "outputExtension": "",
      "dataModel": "LP64",
      "headerNameMatching": "case-insensitive",
      "format": {"name":"macho64-x86_64-darwin-exec-unsigned","kind":"macho"},
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
      "entryPoint": "",
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

    std::string_view json;
    if (shippedExecName == "macho64-arm64-darwin-exec")       json = kArm64Json;
    else if (shippedExecName == "macho64-x86_64-darwin-exec")  json = kX86Json;
    else {
        ADD_FAILURE() << "loadUnsignedExec: no unsigned counterpart for '"
                      << shippedExecName << "' — add one beside the two above "
                         "rather than hand-copying a schema into a test";
        return {};
    }

    auto fixture = ObjectFormatSchema::loadFromText(json);
    if (!fixture.has_value()) {
        ADD_FAILURE() << "loadUnsignedExec: the unsigned counterpart of '"
                      << shippedExecName << "' failed to load";
        for (auto const& d : fixture.error()) ADD_FAILURE() << "  " << d.message;
        return {};
    }
    auto shipped = ObjectFormatSchema::loadShipped(shippedExecName);
    if (!shipped.has_value()) {
        ADD_FAILURE() << "loadUnsignedExec: loadShipped('" << shippedExecName
                      << "') failed";
        return {};
    }

    // ── The anti-drift cross-check ──────────────────────────────────
    // EXPECT throughout: this function RETURNS a value, so a fatal assertion
    // cannot be spelled in it at all; every comparison is bounds-guarded by
    // hand instead.
    auto const& fIm = (*fixture)->machoImage();
    auto const& sIm = (*shipped)->machoImage();
    char const* const drift = "unsigned exec fixture drifted from the shipped "
                              "document it is derived from";
    EXPECT_EQ(fIm.pageZeroSize,    sIm.pageZeroSize)    << drift;
    EXPECT_EQ(fIm.segmentPageSize, sIm.segmentPageSize) << drift;
    EXPECT_EQ(fIm.dylinkerPath,    sIm.dylinkerPath)    << drift;
    EXPECT_EQ(fIm.bindNow,         sIm.bindNow)         << drift;
    EXPECT_EQ(fIm.buildVersion.has_value(),
              sIm.buildVersion.has_value())
        << drift << " — and buildVersion decides whether the static arm "
                    "REFUSES or ENCODES, so this one is load-bearing";
    EXPECT_EQ(fIm.loadDylibs.size(), sIm.loadDylibs.size()) << drift;
    for (std::size_t i = 0;
         i < std::min(fIm.loadDylibs.size(), sIm.loadDylibs.size()); ++i) {
        EXPECT_EQ(fIm.loadDylibs[i].path, sIm.loadDylibs[i].path) << drift;
    }
    EXPECT_TRUE((*fixture)->macho().filetype == (*shipped)->macho().filetype)
        << drift;
    EXPECT_EQ((*fixture)->macho().cputype,    (*shipped)->macho().cputype)
        << drift;
    EXPECT_EQ((*fixture)->macho().cpusubtype, (*shipped)->macho().cpusubtype)
        << drift;
    EXPECT_EQ((*fixture)->macho().flags,      (*shipped)->macho().flags)
        << drift;
    auto const* fText = (*fixture)->sectionByKind(SectionKind::Text);
    auto const* sText = (*shipped)->sectionByKind(SectionKind::Text);
    EXPECT_NE(fText, nullptr);
    EXPECT_NE(sText, nullptr);
    if (fText != nullptr && sText != nullptr) {
        EXPECT_EQ(fText->virtualAddress, sText->virtualAddress) << drift;
        EXPECT_EQ(fText->addrAlign,      sText->addrAlign)      << drift;
    }
    // The ONE sanctioned divergence, both directions so neither half rots.
    EXPECT_TRUE(dss::macho::requestsCodeSignature(sIm))
        << shippedExecName
        << " no longer requests a code signature — it can take the static arm "
           "again and this fixture is unnecessary; delete it rather than leave "
           "a copy of a shipped document behind";
    EXPECT_FALSE(dss::macho::requestsCodeSignature(fIm))
        << "the unsigned fixture requests a signature — macho::encode will "
           "refuse it on the static arm and every pin driving it becomes a "
           "test of the refusal instead";
    return *fixture;
}

}  // namespace dss::macho::test
