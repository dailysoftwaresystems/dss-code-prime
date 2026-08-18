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
#include "link/format/macho_codesign.hpp"  // adHocCodeSignatureSize
#include "link/format/pe.hpp"
#include "link/object_format_schema.hpp"
#include "lir/lir.hpp"
#include "macho_test_support.hpp"
#include "format_reject_support.hpp"   // countAtPath / rejectSummary

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace dss;
using dss::link_format::test::countAtPath;
using dss::link_format::test::errorCount;
using dss::link_format::test::rejectSummary;

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

// A module with ONE extern call — the shape that routes `macho::encode`
// to the DYNAMIC exec arm (`encodeExecDynamic`), the only arm that
// builds __LINKEDIT / __DATA_CONST,__got and therefore the only one
// that can carry an LC_CODE_SIGNATURE. Hoisted out of
// `DynamicPathEmitsLcCodeSignatureWithZeroReservation`, which built it
// inline, so the codesign pins that must differ ONLY in their schema
// share one module verbatim: a control that varies with the test is no
// control. `E8 rel32` call + `ret`, the call site relocated against the
// extern (RelocationKind 1 = the x86_64 `rel32` row every fixture here
// declares).
[[nodiscard]] AssembledModule makeDynamicModuleWithOneExtern() {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back(
        Relocation{1, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    return mod;
}

// ★ THE TWO DOORS every walker call in this file goes through — every
// format it exercises is EXEC-flavored, so there is no raw-`encode`
// exception here.
//
// They share the `Untrampolined` word with the same-purpose helper in the
// ELF / PE / Mach-O writer test files — one concept, one vocabulary — but
// keep the walker in the name because BOTH of them live in THIS
// translation unit with IDENTICAL signatures: a bare `encodeUntrampolined`
// declared twice is a redefinition, not an overload.
//
// D-LK10-ENTRY entry gate (`resolveEntryFnIdx`,
// src/link/format/exec_reloc_apply.hpp): a format declaring
// `processExit` has CONTRACTED that its image entry is the
// DSS-synthesized `_start` trampoline, and only `linker::link` injects
// one (entry_trampoline.cpp stamps `imageEntryOverride = 0`). These are
// placeholder-RESERVATION pins: they drive the walker directly, on
// purpose, and never want a trampoline. Stamping the override says that
// deliberately — semantically a no-op, since index 0 is what the
// pre-gate default produced implicitly. One door per walker so a new
// call site cannot quietly skip the contract.
[[nodiscard]] std::vector<std::uint8_t>
encodeMachoUntrampolined(AssembledModule           mod,  // by value: stamped copy
                         TargetSchema const&       target,
                         ObjectFormatSchema const& fmt,
                         DiagnosticReporter&       reporter) {
    mod.imageEntryOverride = std::size_t{0};
    return macho::encode(mod, target, fmt, reporter);
}

[[nodiscard]] std::vector<std::uint8_t>
encodePeUntrampolined(AssembledModule           mod,  // by value: stamped copy
                      TargetSchema const&       target,
                      ObjectFormatSchema const& fmt,
                      DiagnosticReporter&       reporter) {
    mod.imageEntryOverride = std::size_t{0};
    return pe::encode(mod, target, fmt, reporter);
}

} // namespace

// ── Mach-O: schema validate-reject for invalid reservation sizes ─────

TEST(MachOCodeSignPlaceholder, NonMultipleOfEightRejectedAtLoad) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-bad-cs","kind":"macho"},
      "$entryClusterComment": "D-LK10-ENTRY 2.13: validate() REJECTS an exec-flavored format (here MH_EXECUTE) that declares no processExit, so without this pair the fixture would be rejected for a reason unrelated to the defect it pins. Values verbatim from the shipped macho64-x86_64-darwin-exec.format.json, matching this fixture cputype 16777223 (x86_64); importLibraryPath is already in image.loadDylibs below. Inert here -- a load test builds no trampoline.",
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
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
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    // Pin THE diagnostic, not merely "something was wrong". `validate()`
    // ACCUMULATES (no early return), so a bare has_value() check stays
    // green the moment a NEW rule starts rejecting this fixture -- which
    // is exactly how D-LK10-ENTRY 2.13 emptied this family. The
    // `/processExit` zero-count is the anti-subsumption half.
    EXPECT_EQ(countAtPath(r, "/image/codeSignatureSize"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(MachOCodeSignPlaceholder, ZeroAcceptedAsDisabled) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-no-cs","kind":"macho"},
      "$entryClusterComment": "D-LK10-ENTRY: validate() now REJECTS an exec-flavored format (here MH_EXECUTE) that declares no processExit, so a synthetic macho-exec schema must carry the pair or it never LOADS. Values copied verbatim from the shipped macho64-x86_64-darwin-exec.format.json; importLibraryPath is already in image.loadDylibs below. Inert for these pins (the walker is driven directly -- no trampoline is built -- and the MH_EXECUTE walker arm reads no processExit field).",
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
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
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-obj-with-cs","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "object", "flags": 0 },
      "image": { "codeSignatureSize": 4096 },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
    // MH_OBJECT rejects the WHOLE 'image' block (not a per-field path) --
    // codeSignatureSize is one of nine fields the same `anySet` guard
    // covers (object_format_schema.cpp), so the diagnostic is stamped at
    // '/image' itself.
    EXPECT_EQ(countAtPath(r, "/image"), 1u) << rejectSummary(r);
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
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
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-cs-static","kind":"macho"},
      "$entryClusterComment": "D-LK10-ENTRY: validate() now REJECTS an exec-flavored format (here MH_EXECUTE) that declares no processExit, so a synthetic macho-exec schema must carry the pair or it never LOADS. Values copied verbatim from the shipped macho64-x86_64-darwin-exec.format.json; importLibraryPath is already in image.loadDylibs below. Inert for these pins (the walker is driven directly -- no trampoline is built -- and the MH_EXECUTE walker arm reads no processExit field).",
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
        "codeSignatureSize": 4096
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeTrivialModule();  // no externImports
    DiagnosticReporter rep;
    auto bytes = encodeMachoUntrampolined(mod, **target, **fmt, rep);
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
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-cs+lazy-no-externs","kind":"macho"},
      "$entryClusterComment": "D-LK10-ENTRY: validate() now REJECTS an exec-flavored format (here MH_EXECUTE) that declares no processExit, so a synthetic macho-exec schema must carry the pair or it never LOADS. Values copied verbatim from the shipped macho64-x86_64-darwin-exec.format.json; importLibraryPath is already in image.loadDylibs below. Inert for these pins (the walker is driven directly -- no trampoline is built -- and the MH_EXECUTE walker arm reads no processExit field).",
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
    auto bytes = encodeMachoUntrampolined(mod, **target, **fmt, rep);
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
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-cs-dyn","kind":"macho"},
      "$entryClusterComment": "D-LK10-ENTRY: validate() now REJECTS an exec-flavored format (here MH_EXECUTE) that declares no processExit, so a synthetic macho-exec schema must carry the pair or it never LOADS. Values copied verbatim from the shipped macho64-x86_64-darwin-exec.format.json; importLibraryPath is already in image.loadDylibs below. Inert for these pins (the walker is driven directly -- no trampoline is built -- and the MH_EXECUTE walker arm reads no processExit field).",
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
    AssembledModule mod = makeDynamicModuleWithOneExtern();
    DiagnosticReporter rep;
    auto bytes = encodeMachoUntrampolined(mod, **target, **fmt, rep);
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

// ★ REWRITTEN 2026-08-05, and the change is a STRENGTHENING, not a
// relaxation — say exactly what moved and why.
//
// WHAT IT USED TO DO: load the shipped `macho64-x86_64-darwin-exec`,
// assert `codeSignatureSize == 0`, encode `makeTrivialModule()` and
// walk the load commands asserting LC_CODE_SIGNATURE never appears.
//
// WHY IT HAD TO CHANGE: that format now DECLARES an ad-hoc
// `image.codeSignature` block (it is the mechanism that makes a Darwin
// binary pass AMFI, and this format has to produce a RUNNABLE macOS
// CLI), so it is no longer a specimen of "nothing was requested" and
// cannot be the subject of a no-request pin.
//
// ★ AND IT WAS WEAKER THAN IT LOOKED, which is the more useful finding.
// `makeTrivialModule()` carries no externImports, so `macho::encode`
// dispatched it to the STATIC `encodeExec` arm — an arm that emits no
// __LINKEDIT and therefore CANNOT emit LC_CODE_SIGNATURE under ANY
// schema. The old assertion could not distinguish "the zero
// reservation suppressed the load command" (the contract it claimed to
// pin) from "this code path never emits one anyway", so it would have
// stayed green with the zero-size rule deleted outright.
//
// WHAT IT ASSERTS NOW: the SAME contract, on the DYNAMIC arm where
// LC_CODE_SIGNATURE is genuinely emittable (the sibling test right
// below proves the identical module + schema DOES get one once a
// reservation is declared — a matched control), against a synthetic
// schema declaring NEITHER `codeSignatureSize` NOR `codeSignature`.
// Delete the walker's `wantsCodeSignature` gate and this goes red.
TEST(MachOCodeSignPlaceholder, ZeroSizeOmitsLcCodeSignature) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    // `macho-no-cs-walk` — byte-for-byte the `macho-cs-dyn` fixture
    // below MINUS its `codeSignatureSize`, so the two are a matched
    // pair differing ONLY in the reservation request.
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-no-cs-walk","kind":"macho"},
      "$entryClusterComment": "D-LK10-ENTRY: validate() now REJECTS an exec-flavored format (here MH_EXECUTE) that declares no processExit, so a synthetic macho-exec schema must carry the pair or it never LOADS. Values copied verbatim from the shipped macho64-x86_64-darwin-exec.format.json; importLibraryPath is already in image.loadDylibs below. Inert for these pins (the walker is driven directly -- no trampoline is built -- and the MH_EXECUTE walker arm reads no processExit field).",
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
      "entryPoint": "",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"]
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
    // The fixture's PREMISE: neither reservation key is set. Both are
    // checked — `codeSignature` is the newer of the two and a fixture
    // that silently grew one would make the absence below meaningless.
    EXPECT_EQ((*fmt)->machoImage().codeSignatureSize, 0u);
    EXPECT_FALSE((*fmt)->machoImage().codeSignature.has_value());

    AssembledModule mod = makeDynamicModuleWithOneExtern();
    DiagnosticReporter rep;
    auto bytes = encodeMachoUntrampolined(mod, **target, **fmt, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    // Prove we are on the arm that CAN emit it: __LINKEDIT must exist.
    // Without this the walk below is vacuous again (the exact defect
    // this rewrite fixes).
    ASSERT_TRUE(dss::macho::test::findSection(
                    std::span<std::uint8_t const>{bytes},
                    "__DATA_CONST", "__got")
                    .has_value())
        << "fixture must reach the DYNAMIC exec arm (the only one that "
           "builds __LINKEDIT and can emit LC_CODE_SIGNATURE) — "
           "otherwise the absence asserted below proves nothing";
    std::uint32_t ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t cmd = readU32LE(bytes, off);
        std::uint32_t cmdsize = readU32LE(bytes, off + 4);
        EXPECT_NE(cmd, 0x1Du)
            << "LC_CODE_SIGNATURE must not be emitted when neither "
               "codeSignatureSize nor codeSignature is declared";
        off += cmdsize;
    }
}

// ── The shipped x86_64 Darwin exec now SIGNS — the positive half ─────
//
// The pin above proves the walker withholds a signature nobody asked
// for; this proves it DELIVERS one that was. Together they are the
// matched pair, and this one is the load-bearing direction for the
// macOS-x86_64 leg: macOS refuses to exec a Mach-O whose signature does
// not verify, so a format that declares `image.codeSignature` and gets
// a zero-filled placeholder (or nothing) would produce a binary that
// builds green and dies at `exec`. Asserting only "a load command is
// present" would not catch that — the blob CONTENT is checked here:
// CSMAGIC_EMBEDDED_SIGNATURE framing, the CS_ADHOC flag, and the
// schema-declared identifier, all read BIG-ENDIAN (the codesign
// gotcha — every other Mach-O struct is little-endian).
TEST(MachOCodeSignPlaceholder, ShippedX86DarwinExecEmitsAdHocSignature) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    auto const& im = (*fmt)->machoImage();
    // The schema half: an ad-hoc block, and NO hand-typed reservation
    // (the size is DERIVED — a hand-typed one that disagreed with the
    // built blob is the failure `adHocCodeSignatureSize` exists to
    // prevent).
    ASSERT_TRUE(im.codeSignature.has_value())
        << "macho64-x86_64-darwin-exec must declare an ad-hoc "
           "image.codeSignature — it is what makes the emitted binary "
           "pass AMFI on macOS; without it the x86_64 Darwin leg "
           "builds green and fails at exec";
    EXPECT_TRUE(im.codeSignature->kind == MachOCodeSignature::Kind::AdHoc);
    EXPECT_TRUE(im.codeSignature->hashAlgorithm
                == MachOCodeSignature::HashAlgo::Sha256);
    EXPECT_EQ(im.codeSignature->pageSize, 4096u);
    EXPECT_FALSE(im.codeSignature->identifier.empty());
    EXPECT_EQ(im.codeSignatureSize, 0u)
        << "the reservation size must stay DERIVED from the ad-hoc "
           "block; a hand-typed value here can disagree with the blob "
           "the builder produces";

    // The walker half.
    AssembledModule mod = makeDynamicModuleWithOneExtern();
    DiagnosticReporter rep;
    auto bytes = encodeMachoUntrampolined(mod, **target, **fmt, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    std::uint32_t const ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    std::uint32_t dataOff = 0, dataSize = 0;
    bool foundCS = false;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmd == 0x1Du) {
            dataOff  = readU32LE(bytes, off + 8);
            dataSize = readU32LE(bytes, off + 12);
            EXPECT_EQ(cmdsize, 16u);
            foundCS = true;
        }
        off += cmdsize;
    }
    ASSERT_TRUE(foundCS)
        << "the shipped format declares image.codeSignature but the "
           "walker emitted no LC_CODE_SIGNATURE — the signature would "
           "be silently dropped and the binary would not exec";
    // Size DERIVED from the block, to the byte.
    EXPECT_EQ(dataSize,
              dss::macho::detail::adHocCodeSignatureSize(
                  dataOff, im.codeSignature->pageSize,
                  im.codeSignature->identifier));
    ASSERT_LE(static_cast<std::size_t>(dataOff) + dataSize, bytes.size());
    // Blob CONTENT — big-endian. SuperBlob: magic / length / count.
    auto beU32 = [&bytes](std::size_t at) {
        return (static_cast<std::uint32_t>(bytes[at]) << 24)
             | (static_cast<std::uint32_t>(bytes[at + 1]) << 16)
             | (static_cast<std::uint32_t>(bytes[at + 2]) << 8)
             |  static_cast<std::uint32_t>(bytes[at + 3]);
    };
    EXPECT_EQ(beU32(dataOff), 0xFADE0CC0u)      // CSMAGIC_EMBEDDED_SIGNATURE
        << "reserved region is not a SuperBlob — it was left as the "
           "zero-fill placeholder instead of being signed";
    EXPECT_EQ(beU32(dataOff + 4), dataSize);    // SuperBlob.length
    EXPECT_EQ(beU32(dataOff + 8), 1u);          // count = 1 (CodeDirectory)
    std::size_t const cdOff = dataOff + beU32(dataOff + 16);
    // The SuperBlob index entry is PARSED, not trusted: a wrong blob makes
    // `cdOff` arbitrary, and every read below indexes `bytes` raw. The three
    // EXPECT_EQs above are non-fatal, so without a FATAL bound here a corrupt
    // signature would ABORT the runner instead of reporting a failed
    // expectation — the same shape the `linkName` sibling test carried.
    ASSERT_LE(cdOff + 36u, bytes.size())
        << "SuperBlob index points outside the emitted image — the blob is "
           "malformed, not merely wrong";
    EXPECT_EQ(beU32(cdOff), 0xFADE0C02u);       // CSMAGIC_CODEDIRECTORY
    EXPECT_EQ(beU32(cdOff + 12), 0x00000002u)   // flags = CS_ADHOC
        << "CodeDirectory must carry CS_ADHOC — the kernel trusts the "
           "embedded page hashes only for an ad-hoc signature";
    // codeLimit is where the signature starts: the whole file before it
    // is covered, so a byte appended after `dataOff` would be unsigned.
    EXPECT_EQ(beU32(cdOff + 32), dataOff);
    std::size_t const identOff = cdOff + beU32(cdOff + 20);
    // Same rule for the identifier offset, and here the read is an UNBOUNDED C
    // string, so an in-range start is not enough on its own — require a NUL
    // inside the signed region before constructing from the pointer.
    ASSERT_LT(identOff, bytes.size())
        << "CodeDirectory identOffset points outside the emitted image";
    ASSERT_NE(std::find(bytes.begin() + static_cast<std::ptrdiff_t>(identOff),
                        bytes.end(), std::uint8_t{0}),
              bytes.end())
        << "CodeDirectory identifier is not NUL-terminated inside the image";
    std::string ident(reinterpret_cast<char const*>(&bytes[identOff]));
    EXPECT_EQ(ident, im.codeSignature->identifier)
        << "the CodeDirectory identifier must be the schema's, not a "
           "walker-invented one";
}

// ── FLIP MARKER: the static exec arm DROPS an ad-hoc request ─────────
//
// D-LK-MACHO-ADHOC-SIGNATURE-DROPPED-ON-STATIC-ARM — the REGISTERED id, read
// back from .plans/_deferred-anchor-registry.md rather than restated from
// memory. ⚠ This comment used to CARRY the withdrawn "proposed anchor"
// spelling, and said the guard could not catch that because it scanned src/,
// examples/ and real-examples/, never tests/. Both halves were right, and it
// stayed right for twelve days. The guard now scans tests/ and
// integrated_tests/, which is how the dead spelling was finally found —
// D-GATE-ANCHOR-GUARD-DOES-NOT-SCAN-INTEGRATED-TESTS.
// ⛔ THE DEAD SPELLING IS NO LONGER WRITTEN HERE, and that is deliberate rather
// than tidying: a retired id resolves only because something still MENTIONS it,
// so narration that spells one is how it survives its own retirement. The guard
// now refuses any citation of a RETIRED-ID-marked row by name
// (D-GATE-ANCHOR-CITATION-RESOLVES-VIA-ITS-OWN-BUG-REPORT), which would red on
// the sentence this comment replaced. Look the old id up in the registry, where
// it has a row whose only job is to redirect here.
//
// MEASURED 2026-08-05. `macho::encode`'s dispatch gate routes a
// codesign request to the dynamic arm by testing
// `machoImage().codeSignatureSize != 0` ONLY
// (src/link/format/macho.cpp:751), and `encodeExec`'s defensive
// invariant belt re-tests the SAME field only (:1853). Neither looks at
// `image.codeSignature`. So a zero-extern module against a schema whose
// ONLY signature request is the ad-hoc block takes the static arm,
// which emits no __LINKEDIT and no LC_CODE_SIGNATURE — the request is
// dropped with NO diagnostic. That is a silent-miscompile shape: the
// build reports success and the binary dies at `exec` on any machine
// that enforces signing.
//
// It is UNREACHABLE through the shipped pipeline, which is why it has
// survived: every Darwin exec schema declares `processExit`, so
// `linker::link` injects an entry trampoline importing `_exit`, so a
// real build always has >= 1 extern and always takes the dynamic arm.
// Only a direct walker call (this test) reaches it.
//
// This test PINS the defect rather than blessing it. The fix is one
// condition — `|| im.codeSignature.has_value()` at both sites — and it
// lives in src/link/format/macho.cpp, out of this cycle's scope. WHEN
// THAT LANDS this test goes RED, and the correct response is to flip it
// to the shape its sibling `StaticPathRejectsNonZeroCodeSigSize`
// already has (empty bytes + a loud D-LK7-1 diagnostic), NOT to delete
// it.
TEST(MachOCodeSignPlaceholder, StaticPathDropsAdHocCodeSignatureRequest) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    ASSERT_TRUE((*fmt)->machoImage().codeSignature.has_value());
    ASSERT_EQ((*fmt)->machoImage().codeSignatureSize, 0u);

    AssembledModule mod = makeTrivialModule();   // no externImports
    DiagnosticReporter rep;
    auto bytes = encodeMachoUntrampolined(mod, **target, **fmt, rep);

    if (bytes.empty()) {
        // The FIXED world: the dispatch gate learned about
        // `codeSignature`. Assert it failed LOUD with the D-LK7-1
        // anchor its `codeSignatureSize` sibling uses, then update this
        // test to the sibling's shape and retire the marker.
        bool sawAnchor = false;
        for (auto const& d : rep.all()) {
            if (d.code == DiagnosticCode::K_FormatLacksImportSupport
             && d.actual.find("D-LK7-1") != std::string::npos) {
                sawAnchor = true;
            }
        }
        EXPECT_TRUE(sawAnchor)
            << "the static arm now refuses an ad-hoc codeSignature "
               "request — good — but it must cite D-LK7-1 like the "
               "codeSignatureSize path does";
        return;
    }
    // TODAY'S world: encoded green, signature silently absent.
    ASSERT_EQ(rep.errorCount(), 0u);
    std::uint32_t const ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    bool foundCS = false;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmd == 0x1Du) foundCS = true;
        off += cmdsize;
    }
    EXPECT_FALSE(foundCS)
        << "the static arm emitted LC_CODE_SIGNATURE — it builds no "
           "__LINKEDIT, so the blob would sit outside every segment and "
           "the kernel's cs_validate_range would reject the binary";
}

// ── PE: schema validate-reject for invalid reservation sizes ─────────

TEST(PeCertPlaceholder, NonMultipleOfEightRejectedAtLoad) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"pe-bad-cert","kind":"pe"},
      "$entryClusterComment": "D-LK10-ENTRY 2.13: validate() REJECTS an exec-flavored format that declares no processExit, so without this pair the fixture would be rejected for a reason unrelated to the defect it pins. Values verbatim from the shipped pe64-x86_64-windows-exec.format.json. Inert here -- a load test builds no trampoline.",
      "runtimeLibraries": [{"role":"cLibrary","image":"ucrtbase.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "ms_x64",
      "pe": { "machine": 34404, "characteristics": 34, "type": "exec" },
      "optionalHeader": {
        "magic": 523, "imageBase": 5368709120,
        "sectionAlignment": 4096, "fileAlignment": 512,
        "subsystem": 3, "dllCharacteristics": 0,
        "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096,
        "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096,
        "attributeCertReserveSize": 7
      },
      "$sectionRowComment": "Row shape copied from the sibling pe-with-cert fixture below, which LOADS CLEANLY -- so the two differ ONLY in attributeCertReserveSize (7 here vs 2048 there) and form a matched pair. It previously carried flags 1610612768 + addrAlign 16, both of which PE rejects outright (PE folds alignment and flags into Characteristics via the substrate 'type' field): two further reasons to reject that had nothing to do with the multiple-of-8 rule under test. MEASURED, not assumed -- they showed up in a red-on-disable run.",
      "sections":[
        {"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}
      ]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    // `pe.characteristics` = 34 above is the same isolation move as the
    // entry cluster: without it the missing IMAGE_FILE_EXECUTABLE_IMAGE
    // bit is a SECOND reason to reject, and this test would pass even
    // with the multiple-of-8 rule deleted.
    EXPECT_EQ(countAtPath(r, "/optionalHeader/attributeCertReserveSize"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
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
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"pe-obj-with-cert","kind":"pe"},
      "pe": { "machine": 34404, "type": "obj" },
      "optionalHeader": { "attributeCertReserveSize": 1024 },
      "$sectionRowComment": "addrAlign MUST be 0 for a PE row (PE folds alignment into Characteristics via the substrate 'type' field, object_format_schema.cpp) -- the sibling 'pe-with-cert' fixture below documents the same trap ('it previously carried ... addrAlign 16 ... a further reason to reject that had nothing to do with the rule under test'). MEASURED, not assumed.",
      "sections":[{"kind":"text","name":".text","type":0,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
    // PE .obj rejects the WHOLE 'optionalHeader' block (not a per-field
    // path) -- attributeCertReserveSize is one of eleven fields the same
    // `anySet` guard covers, so the diagnostic is stamped at
    // '/optionalHeader' itself, not '/optionalHeader/attributeCertReserveSize'.
    EXPECT_EQ(countAtPath(r, "/optionalHeader"), 1u) << rejectSummary(r);
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
}

// ── PE: walker emits security directory entry + zero reservation ─────

TEST(PeCertPlaceholder, WalkerEmitsSecurityDirAndZeroReservation) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"pe-with-cert","kind":"pe"},
      "$entryClusterComment": "D-LK10-ENTRY: validate() now REJECTS an exec-flavored format that declares no processExit, so a synthetic pe-exec schema must carry the pair or it never LOADS. Values copied verbatim from the shipped pe64-x86_64-windows-exec.format.json; inert for these pins (the walker is driven directly, so no trampoline is built).",
      "runtimeLibraries": [{"role":"cLibrary","image":"ucrtbase.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "ms_x64",
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
    auto bytes = encodePeUntrampolined(mod, **target, **fmt, rep);
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
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"pe-no-exec-bit","kind":"pe"},
      "$entryClusterComment": "D-LK10-ENTRY 2.13: validate() REJECTS an exec-flavored format that declares no processExit, so without this pair the fixture would be rejected for a reason unrelated to the defect it pins. Values verbatim from the shipped pe64-x86_64-windows-exec.format.json. Inert here -- a load test builds no trampoline.",
      "runtimeLibraries": [{"role":"cLibrary","image":"ucrtbase.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "ms_x64",
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
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    // `characteristics: 0` IS this fixture's defect, so -- unlike its
    // siblings above -- it is deliberately NOT repaired to 34; only the
    // entry cluster is added.
    EXPECT_EQ(countAtPath(r, "/pe/characteristics"), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
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
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"pe-imports-with-cert","kind":"pe"},
      "$entryClusterComment": "D-LK10-ENTRY: validate() now REJECTS an exec-flavored format that declares no processExit, so a synthetic pe-exec schema must carry the pair or it never LOADS. Values copied verbatim from the shipped pe64-x86_64-windows-exec.format.json; inert for these pins (the walker is driven directly, so no trampoline is built).",
      "runtimeLibraries": [{"role":"cLibrary","image":"ucrtbase.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "ms_x64",
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
    auto bytes = encodePeUntrampolined(mod, **target, **fmt, rep);
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
    auto bytes = encodePeUntrampolined(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    constexpr std::size_t kSecurityDirOff = 152 + 112 + 4 * 8;
    EXPECT_EQ(readU32LE(bytes, kSecurityDirOff), 0u);
    EXPECT_EQ(readU32LE(bytes, kSecurityDirOff + 4), 0u);
}
