// ELF ET_EXEC writer tests — plan 14 LK1 cycle 2.
//
// Pins golden byte-level invariants of the emitted ELF64 executable
// (ET_EXEC):
//   * e_type = ET_EXEC (2).
//   * e_entry = secText.virtualAddress + entry-function offset
//     (cycle 2: first function at offset 0 in .text).
//   * e_phoff = sizeof(Ehdr) = 64; e_phentsize = 56; e_phnum = 1.
//   * PT_LOAD program header: p_type=1, p_flags=5 (R|X), p_align=0x1000,
//     p_vaddr/p_paddr = secText.virtualAddress, p_filesz/p_memsz =
//     .text length.
//   * section header `.text`: sh_addr = virtualAddress (vs 0 in ET_REL).
//   * Section ordering preserved (IDX_TEXT=1) so the symtab's
//     STT_SECTION entry's st_shndx=1 still resolves.
//   * Cycle-2 fail-loud guard: modules with non-empty relocations[]
//     emit `K_RelocationKindMismatch` and return empty bytes.
//   * Cycle-2 fail-loud guard: zero-function module emits
//     `K_SymbolUndefined`.

#include "asm/asm.hpp"
#include "ffi/binary_reader.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf.hpp"
#include "link/format/exec_data_section.hpp"   // TLS C1: addTlsSymbolOffsets pin
#include "format_reject_support.hpp"   // countAtPath / countWithMessage / rejectSummary
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace dss;
using dss::link_format::test::countAtPath;
using dss::link_format::test::errorCount;
using dss::link_format::test::countWithMessage;
using dss::link_format::test::rejectSummary;

namespace {

[[nodiscard]] std::uint16_t readU16LE(std::span<std::uint8_t const> b,
                                       std::size_t off) {
    return static_cast<std::uint16_t>(b[off])
         | (static_cast<std::uint16_t>(b[off + 1]) << 8);
}
[[nodiscard]] std::uint32_t readU32LE(std::span<std::uint8_t const> b,
                                       std::size_t off) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(b[off + i]) << (i * 8);
    return v;
}
[[nodiscard]] std::uint64_t readU64LE(std::span<std::uint8_t const> b,
                                       std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(b[off + i]) << (i * 8);
    return v;
}

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadShipped() {
    Loaded out;
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(x86_64) failed";
        for (auto const& d : t.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(elf64-x86_64-linux-exec) failed";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

[[nodiscard]] AssembledModule makeTrivialModule(std::vector<std::uint8_t> bytes,
                                                  std::uint32_t symId) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{symId};
    fn.bytes = std::move(bytes);
    mod.functions.push_back(std::move(fn));
    return mod;
}

// ★ THE ONE SEAM every byte-level writer test in this file drives the
// ELF walker through — D-LK10-ENTRY §2.13, the `resolveEntryFnIdx`
// entry gate (`link/format/exec_reloc_apply.hpp`).
//
// A format that declares `processExit` has CONTRACTED that its image
// entry is the DSS-synthesized `_start` trampoline. `linker::link`
// sets `imageEntryOverride` on EVERY injection (entry_trampoline.cpp:
// 732 — the file's ONLY assignment to that field), so a module
// reaching a walker WITHOUT that override was never
// trampolined — which the gate now rejects loud rather than silently
// making functions[0] the process entry (the measured defect: an
// LC_MAIN pointing at a `sub rsp,0x10` prologue, rc=0, zero diags).
//
// These tests deliberately bypass `linker::link` to pin the writer's
// bytes, so they must SAY they want an untrampolined image. Index 0 is
// exactly what they were getting implicitly before the gate existed —
// this states the existing intent, it does not change any emitted byte
// (e_entry stays secText.virtualAddress + funcTextStart[0]).
//
// Funnelled HERE, not into the module builders, for two reasons: the
// override describes how the writer is being DRIVEN (not a property of
// the module — `LinkerEndToEnd.ElfExecDispatchProducesValidExecutable`
// hands `makeTrivialModule`'s output to `linker::link` and must still
// exercise real injection, which the linker SKIPS when an override is
// already present), and a new writer test then inherits the contract
// by construction instead of re-deriving it.
//
// NOT used by the four tests that exercise entry RESOLUTION itself
// (EntryPoint{HonoredOnDynamicPath,ResolvesSecondFunctionByName} +
// UnknownEntryPoint{FailsLoud,OnDynamicPathFailsLoud}): the override
// branch returns BEFORE the schema's `entryPoint` is consulted, so
// setting it there would erase the very behavior under test. Those
// four call `elf::encode` directly, on purpose.
[[nodiscard]] std::vector<std::uint8_t> encodeUntrampolined(
        AssembledModule&          mod,
        TargetSchema const&       target,
        ObjectFormatSchema const& format,
        DiagnosticReporter&       reporter) {
    mod.imageEntryOverride = 0u;
    return elf::encode(mod, target, format, reporter);
}

// c145: locate a section header index by its `.shstrtab` name (−1 if absent).
[[nodiscard]] std::string readCStr(std::vector<std::uint8_t> const& b,
                                   std::uint64_t off) {
    std::string s;
    for (std::uint64_t p = off; p < b.size() && b[p] != 0; ++p)
        s.push_back(static_cast<char>(b[p]));
    return s;
}
[[nodiscard]] int findSectionByName(std::vector<std::uint8_t> const& b,
                                    std::string const& name) {
    std::uint64_t const shoff    = readU64LE(b, 40);
    std::uint16_t const shnum    = readU16LE(b, 60);
    std::uint16_t const shstrndx = readU16LE(b, 62);
    std::uint64_t const shstrOff = readU64LE(b, shoff + shstrndx * 64 + 24);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint32_t const nameOff = readU32LE(b, shoff + i * 64 + 0);
        if (readCStr(b, shstrOff + nameOff) == name) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

// ── Shipped JSON loads with new fields ──────────────────────────

TEST(ElfExecFormatJson, ShippedFileLoadsCleanlyWithExecFields) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.format);
    EXPECT_EQ(loaded.format->kind(), ObjectFormatKind::Elf);
    EXPECT_EQ(loaded.format->name(), "elf64-x86_64-linux-exec");
    EXPECT_EQ(loaded.format->elf().objectType, ElfObjectType::Exec);
    // Empty entryPoint until D-LK1-1 closes (real symbol-name
    // thread); walker defaults to module.functions[0].
    EXPECT_EQ(loaded.format->entryPoint(), "");
    auto const* secText = loaded.format->sectionByKind(SectionKind::Text);
    ASSERT_NE(secText, nullptr);
    EXPECT_EQ(secText->virtualAddress, 0x401000u);
    // LK6 cycle 2b.1 substrate: PT_INTERP path declared on the
    // shipped exec schema. Walker emission (D-LK6-4) consumes
    // this when emitting the .interp section + PT_INTERP program
    // header.
    EXPECT_EQ(loaded.format->elf().interpreter,
              "/lib64/ld-linux-x86-64.so.2");
}

// ── Negative-load pins: the entry cluster + the SPECIFIC diagnostic ──
//
// Every ET_EXEC fixture below is EXEC-FLAVORED, so D-LK10-ENTRY §2.13
// (`isExecFlavor && !processExit` → reject) would reject it for a reason
// that has NOTHING to do with the defect under test. Each therefore
// declares the entry cluster — `processExit` + its paired
// `entryCallingConvention`, copied VERBATIM from the shipped
// `src/dss-config/object-formats/elf64-x86_64-linux-exec.format.json` —
// so the ONLY remaining reason to reject is the fixture's own defect.
// The cluster is inert here: nothing in a load test builds a trampoline.
//
// And each asserts THAT SPECIFIC diagnostic rather than a bare
// `ASSERT_FALSE(r.has_value())`. `validate()` ACCUMULATES (there is no
// early return before its `return problems;`), so a bare has_value check
// stays green when a NEW load rule starts rejecting the fixture — which
// is exactly how §2.13 silently emptied this whole family. The
// `countAtPath(r, "/processExit") == 0` line is the anti-subsumption
// half: it goes red the moment the entry-cluster reason comes back.
TEST(ElfExecFormatJson, InterpreterTypeCheckRejectsNonString) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bad-interp","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096, "interpreter": 42 },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/elf/interpreter"), 1u) << rejectSummary(r);
    EXPECT_EQ(countWithMessage(r, "'interpreter' must be a string"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(ElfExecFormatJson, EmptyInterpreterStringRejectedAtLoad) {
    // 5-agent CRITICAL convergence (code-reviewer + silent-failure +
    // comment-analyzer + type-design + architect): a literal
    // `"interpreter": ""` in JSON is a config error (Linux kernel
    // rejects zero-length PT_INTERP). Absent field is fine (default
    // empty); explicit empty is not.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"empty-interp","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096, "interpreter": "" },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/elf/interpreter"), 1u) << rejectSummary(r);
    // The message discriminates EMPTY from WRONG-TYPE — both rules emit
    // at `/elf/interpreter`, so the path alone cannot tell them apart.
    EXPECT_EQ(countWithMessage(r, "'interpreter' must not be empty"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(ElfRelFormatJson, InterpreterOnRelFormatRejectedAtLoad) {
    // type-design Concern #2 + test-analyzer Gap 2 (2-agent): ET_REL
    // must not carry an interpreter path (no PT_INTERP exists on
    // relocatable .o files). Symmetric with the existing
    // ET_REL/virtualAddress!=0 reject.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"rel-with-interp","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"rel", "interpreter": "/lib64/ld-linux-x86-64.so.2" }
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: class/data/machine are valid and no
    // sections are declared, so the ET_REL interpreter is the only
    // diagnostic.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/elf/interpreter"), 1u) << rejectSummary(r);
}

TEST(ElfRelFormatJson, BindNowFalseOnRelFormatRejectedAtLoad) {
    // Symmetric reject: ET_REL must not set bindNow=false. The
    // eager-vs-lazy binding choice is an exec-image concept;
    // .o files don't bind at all (the linker resolves at exec
    // build time). A typo on a .o would otherwise be silently
    // accepted. (Type-design HIGH fold, LK6 cycle 2c post-fold
    // review.)
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"rel-with-bindnow-false","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"rel", "bindNow": false }
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: class/data/machine are valid and no
    // sections are declared, so ET_REL's rejected bindNow=false is the
    // only diagnostic.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/elf/bindNow"), 1u) << rejectSummary(r);
}

TEST(ElfExecWriter, ExternImportsWithEmptyInterpreterCitesSubstrateGap) {
    // Cross-format symmetry with PE's fail-loud, but extended:
    // when `elf.interpreter` is empty AND externImports is
    // non-empty, the diagnostic surfaces the empty PT_INTERP
    // path (not just the missing walker emission).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    // D-LK10-ENTRY 2.13: an exec-flavored format MUST declare
    // `processExit` + its paired `entryCallingConvention`, or
    // loadFromText()'s validate() rejects this fixture AT LOAD.
    // (`elf.interpreter` stays absent — that is what's under test;
    // validate() ties the interpreter to the cluster on ET_DYN only.)
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"exec-no-interp","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096 },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol = SymbolId{99};
    imp.mangledName = "printf";
    imp.libraryPath = "libc.so.6";
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawSubstrateMention = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport
         && d.actual.find("interpreter") != std::string::npos) {
            sawSubstrateMention = true;
        }
    }
    EXPECT_TRUE(sawSubstrateMention);
}

namespace {
// Build a minimal AssembledModule + ExternImport pair for the
// non-x86_64 machine-guard tests. The walker should reject before
// reading any of these fields beyond externImports.size() > 0.
AssembledModule makeModuleWithOneExtern() {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = "printf";
    imp.libraryPath = "libc.so.6";
    mod.externImports.push_back(std::move(imp));
    return mod;
}
} // namespace

// D-LK6-8 CLOSED 2026-06-01: ARM64 dynamic walker landed.
// ExternImports on the shipped `elf64-aarch64-linux-exec.format.json`
// + the shipped `arm64.target.json` now PRODUCE bytes (no longer
// fail loud). The function body is ARM64-shape (BL + RET = 8 bytes;
// BL at offset 0 is the call26 reloc patch site, naturally 4-byte
// aligned per ARM64 instruction format).
TEST(ElfExecWriter, ExternImportsOnArm64MachineSucceedsAfterDLK68Close) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(fmt.has_value()) << [&]{
        std::string s; for (auto const& d : fmt.error()) s += d.message + "\n"; return s;
    }();

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    // BL #0 (0x94000000) + RET (0xD65F03C0). 8 bytes. BL at offset 0
    // is the call26 patch site.
    fn.bytes = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};
    Relocation rel;
    rel.offset = 0;            // 4-byte aligned (BL is at start)
    rel.target = SymbolId{99}; // extern
    rel.kind   = RelocationKind{1};  // arm64.target.json: call26
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = "printf";
    imp.libraryPath = "libc.so.6";
    mod.externImports.push_back(std::move(imp));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, **fmt, rep);
    std::string diags;
    for (auto const& d : rep.all()) diags += d.actual + "\n";
    EXPECT_EQ(rep.errorCount(), 0u) << diags;
    EXPECT_FALSE(bytes.empty()) << diags;
}

// D-LK6-8 byte-pin: ARM64 16-byte PLT stub layout.
// First stub at pltVa, GOT slot at gotVa. The 4 instructions are:
//   [0..3]   ADRP x16, page-of(slotVa)     high byte 3 in {0x90..0x9F}
//   [4..7]   LDR  x17, [x16, lo12(slotVa)] high byte 7 == 0xF9
//   [8..11]  BR   x17                       fixed 0xD61F0220
//   [12..15] NOP                            fixed 0xD503201F
// Test scans for the unique fixed-byte BR+NOP marker pair, then
// validates the ADRP/LDR opcode marker bytes at offsets -8/-4 from
// the BR. post-fold #1 — was previously docstring-claimed but not
// actually verified (pr-test-analyzer caught the gap).
TEST(ElfExecWriter, Arm64PltStubLayoutPinsAdrpLdrBrNop) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(fmt.has_value());

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};
    Relocation rel;
    rel.offset = 0; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol = SymbolId{99};
    imp.mangledName = "printf";
    imp.libraryPath = "libc.so.6";
    mod.externImports.push_back(std::move(imp));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // The PLT layout offset isn't trivially derivable from outside the
    // walker, so scan for the BR x17 (D6 1F 02 20 in BE; in LE that's
    // 0x20 0x02 0x1F 0xD6) — unique enough as a marker. Confirm the
    // NOP (1F 20 03 D5 LE) immediately follows.
    std::size_t brIdx = bytes.size();
    for (std::size_t i = 0; i + 8 <= bytes.size(); ++i) {
        if (bytes[i+0] == 0x20 && bytes[i+1] == 0x02
         && bytes[i+2] == 0x1F && bytes[i+3] == 0xD6
         && bytes[i+4] == 0x1F && bytes[i+5] == 0x20
         && bytes[i+6] == 0x03 && bytes[i+7] == 0xD5) {
            brIdx = i;
            break;
        }
    }
    ASSERT_LT(brIdx, bytes.size())
        << "ARM64 PLT stub must contain BR x17 (0xD61F0220) followed "
           "by NOP (0xD503201F) — the fixed back half of every stub.";
    // ADRP opcode pin: instruction at brIdx-8, byte 3 = 0x90 ORed
    // with the immhi top bits. The opcode marker (bits 28+31:25 of
    // the ADRP encoding) lands at byte 3 with mask 0x9F; the page-
    // pair immhi bits at byte 3 mask 0x60 (bits 22:21 of the inst).
    // For the test layout, immhi top bits are zero (small page-pair
    // values), so byte 3 should be exactly 0x90.
    ASSERT_GE(brIdx, 8u);
    // ADRP opcode pin: byte 3 high 5 bits = 0x90 mask 0x9F (the
    // immlo bits at [30:29] vary with page-pair value; mask them
    // out for the opcode marker). Also pin Rd=x16 in the inst's
    // low 5 bits (byte 0 low 5 bits = 16 = 0b10000 = 0x10).
    EXPECT_EQ(bytes[brIdx - 8 + 3] & 0x9Fu, 0x90u)
        << "ADRP opcode marker missing at PLT stub byte 3";
    EXPECT_EQ(bytes[brIdx - 8 + 0] & 0x1Fu, 0x10u)
        << "ADRP Rd must be x16 (IP0) per AArch64 PCS PLT contract";
    // LDR opcode pin: instruction at brIdx-4, byte 3 = 0xF9 fixed
    // for LDR (immediate, unsigned offset, 64-bit, size=11 + opc=01).
    EXPECT_EQ(bytes[brIdx - 4 + 3], 0xF9u)
        << "LDR opcode marker missing at PLT stub byte 7";
    // LDR Rt + low Rn bits: low byte holds Rt (bits[4:0]) +
    // Rn low 3 bits (bits[7:5]). For Rn=16, Rt=17 → byte 0 =
    // (16 << 5 | 17) & 0xFF = 0x211 & 0xFF = 0x11.
    EXPECT_EQ(bytes[brIdx - 4 + 0], 0x11u)
        << "LDR Rn/Rt regression — must be Rn=x16, Rt=x17 per "
           "PCS PLT contract";
    // LDR Rn high 2 bits live in byte 1 bits[1:0] — for Rn=16
    // (high bits = 0b10), byte 1 low 2 bits = 0b10 = 2.
    EXPECT_EQ(bytes[brIdx - 4 + 1] & 0x03u, 0x02u)
        << "LDR Rn upper bits regression";
}

// D-LK6-8 defense-in-depth: out-of-range ADRP page-pair must be
// rejected loudly. Constructed by pinning the GOT slot far enough
// from the PLT stub to overflow the signed-21-bit page-pair budget.
// (Direct testing of the helper would require exposing it; instead
// we drive the full walker with a small text + many externs to push
// the GOT slot past the ±4 GiB threshold. Out of test scope; the
// happy-path indirectly exercises the range-check path's
// instructions.) Anchored as D-LK6-8.range-test for when a synthetic
// VA-shaping fixture lands.

// D-LK6-8: a corrupted-config slotVa with low12 not multiple of 8
// would trigger the LDR misalignment reject. The current `.got`
// layout is page-aligned + 8-byte slots, so this can't be reached
// from valid AssembledModule inputs — anchored as a defense-in-depth
// test for if synthetic-Lir fixtures introduce GOT mislayouts.

// Universality pin (D-LK6-8 post-close): the guard is now
// `machine != x86_64 && machine != ARM64`. RISC-V / MIPS / PPC64
// still fail loud. A future regression dropping ARM64 from the
// supported set would also fail the ARM64 happy-path test.
// EM_RISCV = 243 per RISC-V ELF psABI.
TEST(ElfExecWriter, ExternImportsOnRiscVMachineFailsLoudCitingFutureWork) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    // D-LK10-ENTRY 2.13: exec-flavored ⇒ `processExit` + its paired
    // `entryCallingConvention` are REQUIRED at load. This fixture is
    // RISC-V only in `elf.machine` (243, to reach the PLT-emitter
    // guard) — the TARGET it is encoded against is the shipped x86_64
    // one, and no RISC-V target ships, so the cc names that target's
    // convention rather than inventing an unresolvable `lp64d`. The
    // machine guard rejects before any trampoline work reads it.
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"riscv-exec","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": {
        "class":"elf64", "data":"lsb", "machine": 243, "type":"exec",
        "pageAlign": 4096, "interpreter": "/lib/ld-linux-riscv64-lp64d.so.1", "bindNow": true
      },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4194304}]
    })");
    ASSERT_TRUE(fmt.has_value()) << [&]{
        std::string s; for (auto const& d : fmt.error()) s += d.message + "\n"; return s;
    }();
    auto mod = makeModuleWithOneExtern();

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawFutureWorkMention = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport
         && d.actual.find("future work") != std::string::npos) {
            sawFutureWorkMention = true;
        }
    }
    EXPECT_TRUE(sawFutureWorkMention);
}

TEST(ElfExecWriter, ExternImportsEmitFivePhdrsAndDynamicSegment) {
    // Dynamic image: PT_PHDR + PT_INTERP + PT_LOAD #1 + PT_LOAD #2
    // + PT_DYNAMIC = 5 program headers. PT_LOAD #2 is R+W (covers
    // .got + .dynamic).
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "printf", "libc.so.6"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // e_phnum @ +56 = 5
    EXPECT_EQ(readU16LE(bytes, 56), 5u);
    // PHT at e_phoff (read from +32, u64).
    std::uint64_t const phoff = readU64LE(bytes, 32);
    EXPECT_EQ(phoff, 64u);
    // First phdr = PT_PHDR (type 6).
    EXPECT_EQ(readU32LE(bytes, phoff + 0), 6u);
    // Second = PT_INTERP (type 3).
    EXPECT_EQ(readU32LE(bytes, phoff + 56), 3u);
    // Third = PT_LOAD #1 (type 1, flags R+X = 5).
    EXPECT_EQ(readU32LE(bytes, phoff + 56*2 + 0), 1u);
    EXPECT_EQ(readU32LE(bytes, phoff + 56*2 + 4), 5u);
    // Fourth = PT_LOAD #2 (type 1, flags R+W = 6).
    EXPECT_EQ(readU32LE(bytes, phoff + 56*3 + 0), 1u);
    EXPECT_EQ(readU32LE(bytes, phoff + 56*3 + 4), 6u);
    // Fifth = PT_DYNAMIC (type 2).
    EXPECT_EQ(readU32LE(bytes, phoff + 56*4 + 0), 2u);
}

TEST(ElfExecWriter, ExternImportsEmitPltStubAndRel32PatchToPlt) {
    // The walker emits a 6-byte PLT stub `FF 25 disp32` per extern
    // (jmp [rip+disp32] to the GOT slot). REL32 calls in .text are
    // patched to the PLT stub's VA so direct `call rel32` reaches
    // the PLT, which then jumps through GOT to the resolved fn.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    // call rel32 ; ret
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "printf", "libc.so.6"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // .text starts at file offset = pageAlign (0x1000).
    constexpr std::size_t textFileOff = 0x1000;
    // call opcode survives.
    EXPECT_EQ(bytes[textFileOff + 0], 0xE8u);
    // .plt starts at file offset = roundUp(textOff + textSize, 16).
    // For this test, textSize=6 → pltOff = 0x1010.
    std::size_t const pltFileOff = (textFileOff + 6 + 15) & ~15ull;
    // PLT stub byte 0+1 = FF 25 (jmp [rip+disp32])
    EXPECT_EQ(bytes[pltFileOff + 0], 0xFFu);
    EXPECT_EQ(bytes[pltFileOff + 1], 0x25u);
    // REL32 patch value = pltVa - (textVa + 1) - 4
    //   textVa = secText.virtualAddress = 0x401000
    //   pltVa  = baseImageVa + pltFileOff
    //          = (0x401000 - 0x1000) + 0x1010
    //          = 0x401010
    // Expected disp = 0x401010 - 0x401001 - 4 = 0xB
    std::uint32_t const expectedDisp =
        static_cast<std::uint32_t>(0x401010ull - 0x401001ull - 4ull);
    EXPECT_EQ(readU32LE(bytes, textFileOff + 1), expectedDisp);
}

// ── Test-analyzer post-audit folds ─────────────────────────────

TEST(ElfExecWriter, DynamicSectionEmitsExpectedDtEntriesInOrder) {
    // test-analyzer Gap #1 (criticality 10): walk Elf64_Dyn entries
    // in `.dynamic`. Verify tags appear in declared order, terminator
    // is DT_NULL, DT_FLAGS_1=DF_1_NOW present (eager binding).
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "printf", "libc.so.6"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Locate PT_DYNAMIC (phdr #5, 0-indexed = 4) → p_offset @ +8 (u64).
    std::uint64_t const phoff = readU64LE(bytes, 32);
    std::uint64_t const dynamicOff = readU64LE(bytes, phoff + 56*4 + 8);
    // Walk dyn entries until DT_NULL. Each entry is 16B: tag(u64) + val(u64).
    std::vector<std::uint64_t> tags;
    bool sawDfNow = false;
    bool sawSymentValid = false;
    bool sawNeededLibc = false;
    for (std::size_t off = dynamicOff; off + 16 <= bytes.size(); off += 16) {
        std::uint64_t const tag = readU64LE(bytes, off);
        std::uint64_t const val = readU64LE(bytes, off + 8);
        tags.push_back(tag);
        if (tag == 11u /*DT_SYMENT*/ && val == 24u) sawSymentValid = true;
        if (tag == 0x6ffffffbu /*DT_FLAGS_1*/ && val == 1u) sawDfNow = true;
        if (tag == 0u) break;
    }
    EXPECT_TRUE(sawSymentValid);
    EXPECT_TRUE(sawDfNow);
    // ★ FATAL before `.back()` / `[0]`. The walk above is bounded by
    // `off + 16 <= bytes.size()`, so a regression that drops PT_DYNAMIC (or
    // puts a bogus p_offset in it) makes `dynamicOff` land past the end, the
    // loop body never runs, and `tags` is EMPTY — `tags.back()` and `tags[0]`
    // are then UB on an empty vector, not a failing assertion. ✔MEASURED
    // 2026-08-05 (TF-C120): the same unguarded shape in
    // `tests/link/test_pe_writer.cpp` segfaulted that binary after 83 of 97
    // tests, so 14 tests never ran and nothing reported them missing (one of
    // them, `LinkerExternResolution.OkFalseWhenWalkerFailsLoud`, was genuinely
    // broken and had been hidden by the crash). A test may fail; it must never
    // delete the tests scheduled behind it.
    ASSERT_FALSE(tags.empty())
        << "the .dynamic walk produced NO entries — PT_DYNAMIC's p_offset "
           "(read from phdr #5) does not point into the image";
    EXPECT_EQ(tags.back(), 0u);  // DT_NULL terminator
    EXPECT_EQ(tags[0], 1u);      // first entry is DT_NEEDED
    // The DT_NEEDED val should index into .dynstr; locate .dynstr's
    // first byte after the leading NUL → "printf" or "libc.so.6".
    // Use the file-side substring presence already pinned by another
    // test as the simpler invariant.
    (void)sawNeededLibc;
}

TEST(ElfExecWriter, RelaDynEntriesPackedAsGlobDat) {
    // test-analyzer Gap #2 (criticality 10): pin .rela.dyn r_info
    // bytes. R_X86_64_GLOB_DAT=6 (NOT 7=JUMP_SLOT). r_info packed
    // as (symIdx << 32) | type. r_addend = 0.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "printf", "libc.so.6"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Walk .dynamic to find DT_RELA + DT_RELASZ for the rela.dyn VA.
    std::uint64_t const phoff = readU64LE(bytes, 32);
    std::uint64_t const dynamicOff = readU64LE(bytes, phoff + 56*4 + 8);
    std::uint64_t relaVa = 0, relaSz = 0;
    for (std::size_t off = dynamicOff; off + 16 <= bytes.size(); off += 16) {
        std::uint64_t const tag = readU64LE(bytes, off);
        std::uint64_t const val = readU64LE(bytes, off + 8);
        if (tag == 7u) relaVa = val;
        if (tag == 8u) relaSz = val;
        if (tag == 0u) break;
    }
    ASSERT_NE(relaVa, 0u);
    EXPECT_EQ(relaSz, 24u);  // 1 extern × 24 bytes per Elf64_Rela
    // Locate .rela.dyn file offset via PT_LOAD #1 (baseImageVa=0x400000).
    std::size_t const relaFileOff = relaVa - 0x400000ull;
    // r_info = (dynsymIdx << 32) | R_X86_64_GLOB_DAT (=6). dynsymIdx=1.
    std::uint64_t const rInfo = readU64LE(bytes, relaFileOff + 8);
    EXPECT_EQ(rInfo & 0xFFFFFFFFull, 6u);          // R_X86_64_GLOB_DAT
    EXPECT_EQ(rInfo >> 32, 1u);                     // dynsymIdx == 1
    // r_addend == 0 (eager binding doesn't use addend on GLOB_DAT)
    EXPECT_EQ(readU64LE(bytes, relaFileOff + 16), 0u);
}

TEST(ElfExecWriter, PtLoad2PageAlignCongruence) {
    // test-analyzer Gap #5 (criticality 9): PT_LOAD #2 must satisfy
    // p_vaddr % p_align == p_offset % p_align (kernel ENOEXEC trap
    // — known regression class from LK1 cycle 2 CRITICAL-1).
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "printf", "libc.so.6"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    std::uint64_t const phoff = readU64LE(bytes, 32);
    // PT_LOAD #2 = phdr #4 (0-indexed = 3); fields p_offset @ +8,
    // p_vaddr @ +16, p_align @ +48 (relative to phdr start).
    std::size_t const phdr2 = phoff + 56*3;
    std::uint64_t const offset = readU64LE(bytes, phdr2 + 8);
    std::uint64_t const vaddr  = readU64LE(bytes, phdr2 + 16);
    std::uint64_t const align  = readU64LE(bytes, phdr2 + 48);
    ASSERT_EQ(align, 0x1000u);
    EXPECT_EQ(vaddr % align, offset % align);
}

TEST(ElfExecWriter, MultipleExternsInOneLibraryCollapseToOneDtNeeded) {
    // test-analyzer Gap #4 (criticality 9): N=1 today collapses
    // loops; verify with 2 externs sharing ONE library that DT_NEEDED
    // appears exactly once and the .hash chain handles N>1. (Renamed
    // in c87 — the old name claimed two libraries; the true
    // two-library pin is TwoLibrariesEmitTwoDtNeededInLexicographicOrder.)
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8,0,0,0,0, 0xE8,0,0,0,0, 0xC3};
    Relocation r1; r1.offset = 1; r1.target = SymbolId{10};
    r1.kind = RelocationKind{1};
    Relocation r2; r2.offset = 6; r2.target = SymbolId{11};
    r2.kind = RelocationKind{1};
    fn.relocations.push_back(r1);
    fn.relocations.push_back(r2);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{10}, "printf", "libc.so.6"});
    mod.externImports.push_back(
        ExternImport{SymbolId{11}, "exit",   "libc.so.6"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Both externs share libc.so.6 → exactly 1 DT_NEEDED.
    std::uint64_t const phoff = readU64LE(bytes, 32);
    std::uint64_t const dynamicOff = readU64LE(bytes, phoff + 56*4 + 8);
    int dtNeeded = 0;
    for (std::size_t off = dynamicOff; off + 16 <= bytes.size(); off += 16) {
        std::uint64_t const tag = readU64LE(bytes, off);
        if (tag == 1u) ++dtNeeded;
        if (tag == 0u) break;
    }
    EXPECT_EQ(dtNeeded, 1);
    // .rela.dyn size = 2 externs × 24 = 48 bytes.
    std::uint64_t relaSz = 0;
    for (std::size_t off = dynamicOff; off + 16 <= bytes.size(); off += 16) {
        std::uint64_t const tag = readU64LE(bytes, off);
        std::uint64_t const val = readU64LE(bytes, off + 8);
        if (tag == 8u) relaSz = val;
        if (tag == 0u) break;
    }
    EXPECT_EQ(relaSz, 48u);
}

TEST(ElfExecWriter, TwoLibrariesEmitTwoDtNeededInLexicographicOrder) {
    // c87 (D-FFI-MATH-LIBM-DT-NEEDED): externs owned by TWO distinct
    // libraries (libm.so.6 `sqrt` + libc.so.6 `printf` — the math.json
    // shape after c87) emit exactly one DT_NEEDED per DISTINCT library,
    // in LEXICOGRAPHIC order of the library NAME — regardless of extern
    // declaration order. The externs here are deliberately declared
    // libm-FIRST so a first-appearance emission (the pre-c87 shape)
    // would flunk the order assertion: the pinned rule is sorted names
    // (libc.so.6 < libm.so.6), never declaration order (which shifts
    // with CU/merge order) and never a hardcoded library name.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8,0,0,0,0, 0xE8,0,0,0,0, 0xC3};
    Relocation r1; r1.offset = 1; r1.target = SymbolId{10};
    r1.kind = RelocationKind{1};
    Relocation r2; r2.offset = 6; r2.target = SymbolId{11};
    r2.kind = RelocationKind{1};
    fn.relocations.push_back(r1);
    fn.relocations.push_back(r2);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{10}, "sqrt",   "libm.so.6"});
    mod.externImports.push_back(
        ExternImport{SymbolId{11}, "printf", "libc.so.6"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Walk .dynamic: collect every DT_NEEDED val + DT_STRTAB va.
    std::uint64_t const phoff = readU64LE(bytes, 32);
    std::uint64_t const dynamicOff = readU64LE(bytes, phoff + 56*4 + 8);
    std::vector<std::uint64_t> neededOffs;
    std::uint64_t strtabVa = 0;
    for (std::size_t off = dynamicOff; off + 16 <= bytes.size(); off += 16) {
        std::uint64_t const tag = readU64LE(bytes, off);
        std::uint64_t const val = readU64LE(bytes, off + 8);
        if (tag == 1u) neededOffs.push_back(val);   // DT_NEEDED
        if (tag == 5u) strtabVa = val;              // DT_STRTAB
        if (tag == 0u) break;
    }
    ASSERT_EQ(neededOffs.size(), 2u);
    ASSERT_NE(strtabVa, 0u);
    // .dynstr lives in PT_LOAD #1 (baseImageVa = 0x400000, file offset
    // == va - base — the same mapping RelaDynEntriesPackedAsGlobDat uses).
    std::size_t const strtabFileOff = strtabVa - 0x400000ull;
    auto nameAt = [&](std::uint64_t strOff) {
        std::string s;
        for (std::size_t p = strtabFileOff + strOff;
             p < bytes.size() && bytes[p] != 0; ++p) {
            s.push_back(static_cast<char>(bytes[p]));
        }
        return s;
    };
    EXPECT_EQ(nameAt(neededOffs[0]), "libc.so.6");
    EXPECT_EQ(nameAt(neededOffs[1]), "libm.so.6");
    // Both dynsym import rows still emit (one GLOB_DAT each).
    std::uint64_t relaSz = 0;
    for (std::size_t off = dynamicOff; off + 16 <= bytes.size(); off += 16) {
        std::uint64_t const tag = readU64LE(bytes, off);
        std::uint64_t const val = readU64LE(bytes, off + 8);
        if (tag == 8u) relaSz = val;
        if (tag == 0u) break;
    }
    EXPECT_EQ(relaSz, 48u);
}

// ── c156: symbol versioning (D-LK-ELF-SYMBOL-VERSIONING) ────────────
// An import declaring a REQUIRED version binds that glibc version via a
// `.gnu.version_r` requirement instead of misbinding an unversioned
// reference to a library's OLDEST compat instance (realpath@GLIBC_2.2.5).

namespace {
// INDEPENDENT mirror of the writer's SysV/ELF version hash (gABI Fig.
// 5-13) — so the pin tests the hash ALGORITHM, not the writer's own
// constant. The same classic elf_hash `.hash` uses, applied to a version
// string. elf_hash("GLIBC_2.3") == 0x0d696913.
[[nodiscard]] std::uint32_t testVerHash(std::string_view s) {
    std::uint32_t h = 0;
    for (char const cc : s) {
        h = (h << 4) + static_cast<std::uint8_t>(cc);
        std::uint32_t const g = h & 0xf0000000u;
        if (g != 0) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}
// Elf64_Shdr field readers (64-byte section headers; SHT @ e_shoff/+40).
[[nodiscard]] std::uint32_t shType(std::vector<std::uint8_t> const& b, int i) {
    return readU32LE(b, readU64LE(b, 40) + std::uint64_t(i) * 64 + 4);
}
[[nodiscard]] std::uint64_t shOffset(std::vector<std::uint8_t> const& b, int i) {
    return readU64LE(b, readU64LE(b, 40) + std::uint64_t(i) * 64 + 24);
}
[[nodiscard]] std::uint64_t shSize(std::vector<std::uint8_t> const& b, int i) {
    return readU64LE(b, readU64LE(b, 40) + std::uint64_t(i) * 64 + 32);
}
[[nodiscard]] std::uint32_t shLink(std::vector<std::uint8_t> const& b, int i) {
    return readU32LE(b, readU64LE(b, 40) + std::uint64_t(i) * 64 + 40);
}
[[nodiscard]] std::uint32_t shInfo(std::vector<std::uint8_t> const& b, int i) {
    return readU32LE(b, readU64LE(b, 40) + std::uint64_t(i) * 64 + 44);
}
[[nodiscard]] std::uint64_t shEntsize(std::vector<std::uint8_t> const& b, int i) {
    return readU64LE(b, readU64LE(b, 40) + std::uint64_t(i) * 64 + 56);
}
// The DT_* tag→val map from `.dynamic` (PT_DYNAMIC = phdr #5, index 4).
[[nodiscard]] std::unordered_map<std::uint64_t, std::uint64_t>
dynEntries(std::vector<std::uint8_t> const& b) {
    std::unordered_map<std::uint64_t, std::uint64_t> m;
    std::uint64_t const phoff  = readU64LE(b, 32);
    std::uint64_t const dynOff = readU64LE(b, phoff + 56 * 4 + 8);
    for (std::size_t off = dynOff; off + 16 <= b.size(); off += 16) {
        std::uint64_t const tag = readU64LE(b, off);
        m[tag] = readU64LE(b, off + 8);
        if (tag == 0u) break;
    }
    return m;
}
} // namespace

TEST(ElfExecWriter, VersionedImportEmitsVerneedVersymAndDynamicEntries) {
    // The full trio for a single versioned import realpath@GLIBC_2.3:
    // DT_VERSYM/VERNEED/VERNEEDNUM + `.gnu.version` (versym) indexing the
    // `.gnu.version_r` (Verneed libc.so.6 / Vernaux GLIBC_2.3). RED-ON-
    // DISABLE: drop the emission (emitVersioning) → these sections vanish
    // and every ASSERT_GE(..., 0) below fails.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel; rel.offset = 1; rel.target = SymbolId{99}; rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    ExternImport imp{SymbolId{99}, "realpath", "libc.so.6"};
    imp.version = "GLIBC_2.3";
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // (a) .dynamic carries DT_VERSYM / DT_VERNEED / DT_VERNEEDNUM.
    auto const dyn = dynEntries(bytes);
    ASSERT_TRUE(dyn.count(0x6ffffff0u));   // DT_VERSYM
    ASSERT_TRUE(dyn.count(0x6ffffffeu));   // DT_VERNEED
    ASSERT_TRUE(dyn.count(0x6fffffffu));   // DT_VERNEEDNUM
    EXPECT_EQ(dyn.at(0x6fffffffu), 1u);    // exactly one Verneed (libc.so.6)

    // (b) `.gnu.version` (Versym): one u16 per dynsym entry; STN_UNDEF (#0)
    //     → 0, realpath (#1) → version index 2. link = .dynsym, entsize 2.
    int const vsymIdx = findSectionByName(bytes, ".gnu.version");
    ASSERT_GE(vsymIdx, 0);
    EXPECT_EQ(shType(bytes, vsymIdx), 0x6fffffffu);   // SHT_GNU_versym
    EXPECT_EQ(shEntsize(bytes, vsymIdx), 2u);
    int const dynsymIdx = findSectionByName(bytes, ".dynsym");
    ASSERT_GE(dynsymIdx, 0);
    EXPECT_EQ(shLink(bytes, vsymIdx), static_cast<std::uint32_t>(dynsymIdx));
    ASSERT_EQ(shSize(bytes, vsymIdx), 4u);            // 2 dynsym entries × 2 B
    std::uint64_t const vsymOff = shOffset(bytes, vsymIdx);
    EXPECT_EQ(readU16LE(bytes, vsymOff + 0), 0u);     // STN_UNDEF → LOCAL (0)
    EXPECT_EQ(readU16LE(bytes, vsymOff + 2), 2u);     // realpath → version 2

    // (c) `.gnu.version_r`: 1 Verneed (libc.so.6) + 1 Vernaux (GLIBC_2.3).
    //     link = .dynstr, info = Verneed count (1).
    int const vrIdx = findSectionByName(bytes, ".gnu.version_r");
    ASSERT_GE(vrIdx, 0);
    EXPECT_EQ(shType(bytes, vrIdx), 0x6ffffffeu);     // SHT_GNU_verneed
    EXPECT_EQ(shInfo(bytes, vrIdx), 1u);
    int const dynstrIdx = findSectionByName(bytes, ".dynstr");
    ASSERT_GE(dynstrIdx, 0);
    EXPECT_EQ(shLink(bytes, vrIdx), static_cast<std::uint32_t>(dynstrIdx));
    std::uint64_t const vrOff     = shOffset(bytes, vrIdx);
    std::uint64_t const dynstrOff = shOffset(bytes, dynstrIdx);
    // Elf64_Verneed (16 B): vn_version=1, vn_cnt=1, vn_file→"libc.so.6",
    // vn_aux=16, vn_next=0.
    EXPECT_EQ(readU16LE(bytes, vrOff + 0), 1u);       // vn_version
    EXPECT_EQ(readU16LE(bytes, vrOff + 2), 1u);       // vn_cnt
    EXPECT_EQ(readCStr(bytes, dynstrOff + readU32LE(bytes, vrOff + 4)), "libc.so.6");
    EXPECT_EQ(readU32LE(bytes, vrOff + 8), 16u);      // vn_aux
    EXPECT_EQ(readU32LE(bytes, vrOff + 12), 0u);      // vn_next (last)
    // Elf64_Vernaux (16 B at vrOff+16): vna_hash, flags=0, other=2,
    // vna_name→"GLIBC_2.3", vna_next=0.
    std::uint64_t const aux = vrOff + 16;
    EXPECT_EQ(readU32LE(bytes, aux + 0), testVerHash("GLIBC_2.3"));  // vna_hash (algo)
    EXPECT_EQ(readU32LE(bytes, aux + 0), 0x0d696913u);              // vna_hash (literal)
    EXPECT_EQ(readU16LE(bytes, aux + 4), 0u);         // vna_flags
    EXPECT_EQ(readU16LE(bytes, aux + 6), 2u);         // vna_other (version index)
    EXPECT_EQ(readCStr(bytes, dynstrOff + readU32LE(bytes, aux + 8)), "GLIBC_2.3");
    EXPECT_EQ(readU32LE(bytes, aux + 12), 0u);        // vna_next (last)
}

TEST(ElfExecWriter, UnversionedImportEmitsNoVersionMachinery) {
    // Control (opt-in regression): an import with NO declared version emits
    // no `.gnu.version` / `.gnu.version_r` / DT_VER* — byte-identical to the
    // pre-c156 image. This is the inverse of the versioned pin above.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel; rel.offset = 1; rel.target = SymbolId{99}; rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(ExternImport{SymbolId{99}, "printf", "libc.so.6"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const dyn = dynEntries(bytes);
    EXPECT_FALSE(dyn.count(0x6ffffff0u));   // no DT_VERSYM
    EXPECT_FALSE(dyn.count(0x6ffffffeu));   // no DT_VERNEED
    EXPECT_FALSE(dyn.count(0x6fffffffu));   // no DT_VERNEEDNUM
    EXPECT_EQ(findSectionByName(bytes, ".gnu.version"), -1);
    EXPECT_EQ(findSectionByName(bytes, ".gnu.version_r"), -1);
}

TEST(ElfExecWriter, MixedVersionedAndUnversionedImportsVersymDiscriminates) {
    // Opt-in per symbol: printf stays *global* (versym 1) while realpath binds
    // GLIBC_2.3 (versym 2) in ONE binary + ONE verneed — unversioned imports
    // are untouched by an image that also carries a versioned one.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0xC3};  // 2 call rel32; ret
    Relocation r1; r1.offset = 1; r1.target = SymbolId{98}; r1.kind = RelocationKind{1};
    Relocation r2; r2.offset = 6; r2.target = SymbolId{99}; r2.kind = RelocationKind{1};
    fn.relocations.push_back(r1);
    fn.relocations.push_back(r2);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(ExternImport{SymbolId{98}, "printf", "libc.so.6"});
    ExternImport imp{SymbolId{99}, "realpath", "libc.so.6"};
    imp.version = "GLIBC_2.3";
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    int const vsymIdx = findSectionByName(bytes, ".gnu.version");
    ASSERT_GE(vsymIdx, 0);
    std::uint64_t const vsymOff = shOffset(bytes, vsymIdx);
    ASSERT_EQ(shSize(bytes, vsymIdx), 6u);            // STN_UNDEF + printf + realpath
    EXPECT_EQ(readU16LE(bytes, vsymOff + 0), 0u);     // STN_UNDEF
    EXPECT_EQ(readU16LE(bytes, vsymOff + 2), 1u);     // printf → GLOBAL (unversioned)
    EXPECT_EQ(readU16LE(bytes, vsymOff + 4), 2u);     // realpath → GLIBC_2.3
    EXPECT_EQ(dynEntries(bytes).at(0x6fffffffu), 1u); // still one Verneed
}

TEST(ElfExecWriter, EntryPointHonoredOnDynamicPath) {
    // code-reviewer #1: dynamic path now honors fmt.entryPoint().
    // Schema overrides entry to function #2 → e_entry must reflect.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    // D-LK10-ENTRY 2.13: exec-flavored ⇒ `processExit` + its paired
    // `entryCallingConvention` are REQUIRED at load. NO
    // `imageEntryOverride` here (this test calls `elf::encode`
    // directly, not `encodeUntrampolined`): the override branch
    // returns before `entryPoint` is consulted, and resolving
    // "sym_42" IS the behavior under test.
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"exec-entry-named","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096, "interpreter": "/lib64/ld-linux-x86-64.so.2" },
      "entryPoint": "sym_42",
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    // fn[0] = 3 bytes; fn[1] (symbol 42) = 1 byte starting at offset 3.
    AssembledFunction a;
    a.symbol = SymbolId{1};
    a.bytes  = {0x90, 0x90, 0xC3};
    mod.functions.push_back(std::move(a));
    AssembledFunction b;
    b.symbol = SymbolId{42};
    b.bytes  = {0xC3};
    mod.functions.push_back(std::move(b));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "printf", "libc.so.6"});
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // e_entry @ +24 = 0x401000 + funcTextStart[1] = 0x401003.
    EXPECT_EQ(readU64LE(bytes, 24), 0x401003ull);
}

TEST(ElfExecWriter, UnknownEntryPointOnDynamicPathFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    // D-LK10-ENTRY 2.13: exec-flavored ⇒ `processExit` + its paired
    // `entryCallingConvention` are REQUIRED at load. NO
    // `imageEntryOverride` (direct `elf::encode`, not
    // `encodeUntrampolined`) — an override would satisfy the entry
    // resolver and erase the K_SymbolUndefined this test pins.
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"exec-bad-entry","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096, "interpreter": "/lib64/ld-linux-x86-64.so.2" },
      "entryPoint": "sym_99",
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction a;
    a.symbol = SymbolId{1};   // entry references sym_99 — undefined
    a.bytes  = {0xC3};
    mod.functions.push_back(std::move(a));
    mod.externImports.push_back(
        ExternImport{SymbolId{77}, "printf", "libc.so.6"});
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(ElfExecWriter, ExternImportsProduceDynamicImage) {
    // Cycle 2b.2 walker has landed: a populated `elf.interpreter`
    // now produces a real ELF dynamic image (not a fail-loud).
    // Pins ET_EXEC + non-empty bytes + presence of the
    // dynamic-linker path string in the binary.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol = SymbolId{99};
    imp.mangledName = "printf";
    imp.libraryPath = "libc.so.6";
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    // e_type = ET_EXEC (2)
    EXPECT_EQ(readU16LE(bytes, 16), 2u);
    // The interpreter path string appears verbatim in the file (.interp).
    std::string_view fileView{
        reinterpret_cast<char const*>(bytes.data()), bytes.size()};
    EXPECT_NE(fileView.find("/lib64/ld-linux-x86-64.so.2"),
              std::string_view::npos);
    // Library name appears in .dynstr.
    EXPECT_NE(fileView.find("libc.so.6"), std::string_view::npos);
    // Symbol name appears in .dynstr.
    EXPECT_NE(fileView.find("printf"), std::string_view::npos);
}

// ── D-LK6-11 substrate (post-audit fold: bindNow JSON pin) ─────

TEST(ElfExecFormatJson, BindNowTypeCheckRejectsNonBoolean) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bindnow-wrong-type","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096, "interpreter": "/lib64/ld-linux-x86-64.so.2", "bindNow": "true" },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/elf/bindNow"), 1u) << rejectSummary(r);
    EXPECT_EQ(countWithMessage(r, "'bindNow' must be a boolean"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(ElfExecFormatJson, BindNowDefaultsToTrue) {
    // D-LK10-ENTRY 2.13: exec-flavored ⇒ `processExit` + its paired
    // `entryCallingConvention` are REQUIRED, or this fixture never
    // reaches the bindNow default it is here to pin.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bindnow-default","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096, "interpreter": "/lib64/ld-linux-x86-64.so.2" },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}],
      "relocations":[{"name":"R_X86_64_PC32","kind":1,"nativeId":2}]
    })");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE((*r)->elf().bindNow);
}

TEST(ElfExecWriter, BindNowFalseFailsLoudCitingDLK611) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    // D-LK10-ENTRY 2.13: exec-flavored ⇒ `processExit` + its paired
    // `entryCallingConvention` are REQUIRED at load. No
    // `imageEntryOverride` needed: the bindNow reject fires in
    // encodeElfExecDynamic's pre-conditions, well before the entry
    // resolver runs.
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"elf-lazy-pending","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "entryPoint": "",
      "elf": {
        "class":"elf64","data":"lsb","machine":62,"type":"exec",
        "pageAlign":4096,
        "interpreter":"/lib64/ld-linux-x86-64.so.2",
        "bindNow": false
      },
      "sections":[
        {"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400},
        {"kind":"symtab","name":".symtab","type":2,"flags":0,"addrAlign":8,"entrySize":24,"virtualAddress":0},
        {"kind":"strtab","name":".strtab","type":3,"flags":0,"addrAlign":1,"entrySize":0,"virtualAddress":0},
        {"kind":"shstrtab","name":".shstrtab","type":3,"flags":0,"addrAlign":1,"entrySize":0,"virtualAddress":0}
      ],
      "relocations":[
        {"name":"R_X86_64_PC32","kind":1,"nativeId":2},
        {"name":"R_X86_64_64","kind":2,"nativeId":1},
        {"name":"R_X86_64_32","kind":3,"nativeId":10}
      ]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol = SymbolId{99};
    imp.mangledName = "printf";
    imp.libraryPath = "libc.so.6";
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    ASSERT_GE(rep.errorCount(), 1u);
    bool sawAnchor = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport
         && d.actual.find("D-LK6-11") != std::string::npos) {
            sawAnchor = true;
        }
    }
    EXPECT_TRUE(sawAnchor);
}

// ── ET_EXEC golden header bytes ────────────────────────────────

TEST(ElfExecWriter, Elf64EhdrTypeIsETEXEC) {
    auto loaded = loadShipped();
    // Single function with no calls — a `ret` (0xC3).
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 64u);
    // e_type @ +16 = ET_EXEC (2)
    EXPECT_EQ(readU16LE(bytes, 16), 2u);
}

TEST(ElfExecWriter, EntryAddressDerivesFromVirtualAddress) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // e_entry @ +24 = secText.virtualAddress (0x401000) + 0 (first
    // function lives at .text offset 0 in cycle 2).
    EXPECT_EQ(readU64LE(bytes, 24), 0x401000u);
}

TEST(ElfExecWriter, ProgramHeaderTableImmediatelyFollowsEhdr) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // e_phoff @ +32 = 64 (right after Ehdr)
    EXPECT_EQ(readU64LE(bytes, 32), 64u);
    // e_phentsize @ +54 = 56 (sizeof Elf64_Phdr)
    EXPECT_EQ(readU16LE(bytes, 54), 56u);
    // e_phnum @ +56 = 1 (single PT_LOAD)
    EXPECT_EQ(readU16LE(bytes, 56), 1u);
}

TEST(ElfExecWriter, PtLoadHeaderHasReadExecutePermsAndPageAlign) {
    auto loaded = loadShipped();
    // 3-byte function: nop nop ret
    AssembledModule mod = makeTrivialModule({0x90, 0x90, 0xC3}, 7);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Program header table at byte 64
    std::uint32_t const pType = readU32LE(bytes, 64);
    EXPECT_EQ(pType, 1u);  // PT_LOAD
    std::uint32_t const pFlags = readU32LE(bytes, 64 + 4);
    EXPECT_EQ(pFlags, 5u);  // PF_X | PF_R
    // p_offset @ +8 = file offset of .text (depends on layout, > 64+56)
    std::uint64_t const pOffset = readU64LE(bytes, 64 + 8);
    EXPECT_GE(pOffset, 64u + 56u);
    // p_vaddr / p_paddr @ +16, +24 = 0x401000
    EXPECT_EQ(readU64LE(bytes, 64 + 16), 0x401000u);
    EXPECT_EQ(readU64LE(bytes, 64 + 24), 0x401000u);
    // p_filesz / p_memsz @ +32, +40 = 3 (text length)
    EXPECT_EQ(readU64LE(bytes, 64 + 32), 3u);
    EXPECT_EQ(readU64LE(bytes, 64 + 40), 3u);
    // p_align @ +48 = 0x1000 (4KB page, kernel-required)
    EXPECT_EQ(readU64LE(bytes, 64 + 48), 0x1000u);
}

TEST(ElfExecWriter, TextSectionHeaderShAddrEqualsVirtualAddress) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // e_shoff @ +40
    std::uint64_t const shoff = readU64LE(bytes, 40);
    // Section header 1 is .text (header 0 is SHT_NULL). Each header
    // is 64 bytes. sh_addr @ +16 within the header.
    std::uint64_t const textShAddr = readU64LE(bytes, shoff + 64 + 16);
    EXPECT_EQ(textShAddr, 0x401000u);
}

// ── Section ordering preserved (.rela.text slot is SHT_NULL) ───

TEST(ElfExecWriter, RelaTextSlotDroppedForExec) {
    // ET_REL: 6 sections (NULL, .text, .rela.text, .symtab, .strtab,
    //                     .shstrtab).
    // ET_EXEC: 5 sections (NULL, .text, .symtab, .strtab, .shstrtab)
    //                     — no .rela.text slot at all (architect
    //                     convergence — pre-fix had a phantom
    //                     SHT_NULL placeholder).
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // e_shnum @ +60 = 5 in ET_EXEC mode.
    EXPECT_EQ(readU16LE(bytes, 60), 5u);
    // e_shstrndx @ +62 = 4 in ET_EXEC mode.
    EXPECT_EQ(readU16LE(bytes, 62), 4u);
    // Section index 2 in ET_EXEC is .symtab (SHT_SYMTAB = 2), not
    // SHT_NULL — verify by reading sh_type @ +4 within the header.
    std::uint64_t const shoff = readU64LE(bytes, 40);
    EXPECT_EQ(readU32LE(bytes, shoff + 2 * 64 + 4), 2u)
        << "ET_EXEC section index 2 must be .symtab (SHT_SYMTAB=2), "
           "not a phantom SHT_NULL placeholder";
}

// ── Reloc application (LK6 cycle 1) ────────────────────────────

TEST(ElfExecWriter, ExternRelocationFailsLoudAsUndefined) {
    // ET_EXEC's reloc applier resolves targets against the
    // module's own AssembledFunctions. A reloc whose target is
    // NOT defined locally is an extern — that's FFI / dynamic
    // linking (LK6 cycle 2), so the cycle-1 walker fails loud
    // with K_SymbolUndefined rather than silently zero-patching
    // or fabricating a value.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes = {0xE8, 0x00, 0x00, 0x00, 0x00};  // call rel32
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{99};       // extern — not in module
    rel.kind   = RelocationKind{1};  // rel32
    // `addend` left at default 0 — the structured `addendBias` on
    // the rel32 schema row carries the implicit −4 (instruction-end
    // offset). The assembler never stamps a non-zero `addend` for
    // x86_64 rel32; mirror that here.
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawUndefined = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined) sawUndefined = true;
    }
    EXPECT_TRUE(sawUndefined);
}

TEST(ElfExecWriter, IntraModuleRel32CallAppliedByteForByte) {
    // Multi-function ET_EXEC module: fn[0] is a `call rel32` to
    // fn[1] (a `ret`). The cycle-1 applier resolves the rel32
    // patch in-place to S + A − P − 4 (x86_64 rel32 formula:
    // pcRelative=true, addendBias=−4, widthBytes=4).
    //
    // Layout:
    //   .text @ vaddr = secText->virtualAddress (= 0x401000 in
    //   shipped exec schema).
    //   fn[0] @ +0:   E8 ?? ?? ?? ?? C3           (call rel32 ; ret)
    //   fn[1] @ +6:   C3                          (ret)
    //
    // Patch site:
    //   P = vaddr + 1, S = vaddr + 6, A = 0.
    //   value = S − P − 4 = (vaddr+6) − (vaddr+1) − 4 = 1
    //   → bytes at .text[1..5] = 01 00 00 00.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};        // → fn[1]
    rel.kind   = RelocationKind{1};  // rel32
    rel.addend = 0;
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));

    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3};
    mod.functions.push_back(std::move(f1));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // Locate `.text` body in the file image — derive from the
    // section header table rather than hard-coding (ET_EXEC pads
    // .text to a page boundary so execve() can mmap it).
    std::uint64_t const shoff       = readU64LE(bytes, 40);
    std::uint64_t const textFileOff = readU64LE(bytes, shoff + 1 * 64 + 24);
    // Patched displacement must equal 1 (LE 4 bytes).
    EXPECT_EQ(bytes[textFileOff + 0], 0xE8u);
    EXPECT_EQ(readU32LE(bytes, textFileOff + 1), 1u)
        << "x86_64 rel32 must apply to S - P - 4 = 1";
    EXPECT_EQ(bytes[textFileOff + 5], 0xC3u);  // ret at end of fn[0]
    EXPECT_EQ(bytes[textFileOff + 6], 0xC3u);  // ret at fn[1]
}

TEST(ElfExecWriter, IntraModuleAbs64AppliedByteForByte) {
    // Cycle-1 abs64 reloc: pcRelative=false, addendBias=0,
    // widthBytes=8 → value = S + A. Pin against an 8-byte slot.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    // fn[0]: 8 zero bytes (the abs64 slot) + ret.
    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0,0,0,0,0,0,0,0, 0xC3};
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{2};        // → fn[1]
    rel.kind   = RelocationKind{2};  // abs64
    rel.addend = 0;
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));

    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3};
    mod.functions.push_back(std::move(f1));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    std::uint64_t const shoff       = readU64LE(bytes, 40);
    std::uint64_t const textVa      = readU64LE(bytes, shoff + 1 * 64 + 16);
    std::uint64_t const textFileOff = readU64LE(bytes, shoff + 1 * 64 + 24);
    // Expected: virtualAddress(.text) + offset of fn[1] inside .text.
    // fn[0] is 9 bytes; fn[1] is at offset 9.
    std::uint64_t const expected = textVa + 9;
    EXPECT_EQ(readU64LE(bytes, textFileOff + 0), expected);
}

TEST(ElfExecWriter, NonZeroAddendIsRespectedSeparatelyFromAddendBias) {
    // Pins the `A` term of `value = S + A + (pcRelative ? -P : 0) +
    // addendBias`. A regression that drops `+ A` or that conflates
    // `A` with the schema's `addendBias` would still pass the
    // zero-addend tests above. Use rel32 with rel.addend = 7 →
    //   value = (vaddr+6) + 7 - (vaddr+1) - 4 = 8.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};  // rel32
    rel.addend = 7;
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));

    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3};
    mod.functions.push_back(std::move(f1));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    std::uint64_t const shoff       = readU64LE(bytes, 40);
    std::uint64_t const textFileOff = readU64LE(bytes, shoff + 1 * 64 + 24);
    EXPECT_EQ(readU32LE(bytes, textFileOff + 1), 8u)
        << "rel32 value must include + Relocation::addend (A=7) "
           "alongside the schema's addendBias (-4)";
}

TEST(ElfExecWriter, Abs32WriteRespectsWidthBytesAndPreservesAdjacentBytes) {
    // Pins (a) abs32 (widthBytes=4, pcRelative=false) writes exactly
    // 4 bytes, not 8, and (b) bytes immediately AFTER the patch
    // survive intact. A regression that hardcodes an 8-byte write
    // would clobber the post-patch sentinel.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    // [4-byte abs32 slot] [sentinel 0xDE 0xAD 0xBE 0xEF] [ret]
    f0.bytes  = {0,0,0,0,  0xDE,0xAD,0xBE,0xEF,  0xC3};
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{3};  // abs32
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));

    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3};
    mod.functions.push_back(std::move(f1));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    std::uint64_t const shoff       = readU64LE(bytes, 40);
    std::uint64_t const textVa      = readU64LE(bytes, shoff + 1 * 64 + 16);
    std::uint64_t const textFileOff = readU64LE(bytes, shoff + 1 * 64 + 24);
    // fn[1] starts at offset 9 inside .text; abs32 of fn[1] is the
    // low 4 bytes of (textVa + 9).
    std::uint32_t const expected = static_cast<std::uint32_t>(textVa + 9);
    EXPECT_EQ(readU32LE(bytes, textFileOff + 0), expected);
    // Sentinel must be untouched — abs32 wrote exactly 4 bytes.
    EXPECT_EQ(bytes[textFileOff + 4], 0xDEu);
    EXPECT_EQ(bytes[textFileOff + 5], 0xADu);
    EXPECT_EQ(bytes[textFileOff + 6], 0xBEu);
    EXPECT_EQ(bytes[textFileOff + 7], 0xEFu);
}

TEST(ElfExecWriter, RelocOffsetPastFunctionBytesFailsLoud) {
    // The bounds check guards `rel.offset + widthBytes` against the
    // ORIGINATING function's `fn.bytes.size()`, not just total
    // `.text`. A stale offset that "happens to fit" inside .text
    // would otherwise silently scribble into the next function.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xC3};   // 1 byte — far too small for rel32
    Relocation rel;
    rel.offset = 4;       // past end of f0.bytes (size=1)
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};  // rel32, widthBytes=4
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));

    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3, 0xC3, 0xC3, 0xC3};   // enough that .text would fit
    mod.functions.push_back(std::move(f1));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(ElfExecWriter, DisplacementOverflowFailsLoud) {
    // value = S + A - P - 4. For rel32 (widthBytes=4) the result
    // must fit in i32. Forge a `Relocation::addend` large enough to
    // push the displacement past INT32_MAX so the value range-check
    // fires — a regression that drops the check would silently
    // truncate to the low 32 bits.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;

    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{1};        // self — passes the extern guard
    rel.kind   = RelocationKind{1};
    rel.addend = std::numeric_limits<std::int64_t>::max() / 2;
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(ElfExecFormatJsonValidate, AbsoluteWithAddendBiasRejected) {
    // Coherence rule (c): `addendBias != 0` ⇒ `pcRelative`. An
    // absolute reloc with a non-zero bias has no real consumer and
    // is almost certainly a typo — load must fail at validate.
    auto r = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"bad-bias"},
      "relocations":[
        { "name": "weird", "kind": 1, "pcRelative": false, "addendBias": -4, "widthBytes": 4 }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfExecFormatJsonValidate, AddendBiasOverflowWidthRejected) {
    // Coherence rule (d): `|addendBias|` must fit signed in
    // `widthBytes`. A bias of 0x10000 in a widthBytes=2 slot would
    // overflow on every patch.
    auto r = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"bias-overflows-width"},
      "relocations":[
        { "name": "weird", "kind": 1, "pcRelative": true, "addendBias": 70000, "widthBytes": 4 }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })");
    // 70000 fits in i32 (range OK), in i16 (no) — widthBytes=4 (i32
    // range) accepts it. Use a 5 GiB bias with widthBytes=4 instead:
    if (r.has_value()) {
        auto r2 = TargetSchema::loadFromText(R"({
          "dssTargetVersion": 1,
          "target": {"name":"bias-overflows-width-2"},
          "relocations":[
            { "name": "weird", "kind": 1, "pcRelative": true, "addendBias": 2147483647, "widthBytes": 4 }
          ],
          "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
        })");
        // INT32_MAX is exactly the signed-4-byte cap → still accepted.
        // The coherence rule rejects only OVERFLOW; this is the
        // boundary that confirms inclusive bounds.
        ASSERT_TRUE(r2.has_value());
    }
}

TEST(ElfExecFormatJsonValidate, PcRelativeWithoutWidthBytesRejected) {
    // Coherence rule (b): `pcRelative` with `widthBytes == 0` is a
    // declared-but-inapplicable shape — load must fail.
    auto r = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"pcrel-no-width"},
      "relocations":[
        { "name": "weird", "kind": 1, "pcRelative": true }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfExecWriter, NoStructuredFormulaWidthFailsLoud) {
    // A relocation kind whose schema row uses formula='linear' but
    // OMITS widthBytes (the LK6 cycle-1 placeholder shape) fails loud
    // at apply time. D-LK6-1 closure: invalid formula strings are
    // rejected at JSON-load (closed enum); widthBytes=0 with valid
    // formula='linear' still reaches the walker and gets rejected
    // there. Synthesize via the JSON loader to keep the test target-
    // blind (no dependence on a particular shipped schema).
    auto fmtR = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(fmtR.has_value());

    auto tgtR = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"noapply"},
      "relocations":[
        { "name": "weird", "kind": 1, "formula": "linear" }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_TRUE(tgtR.has_value()) << [&]{
        std::string s; for (auto const& d : tgtR.error()) s += d.message + "\n"; return s;
    }();

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{1};        // self — passes extern check
    rel.kind   = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **tgtR, **fmtR, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(ElfExecWriter, ZeroFunctionModuleFailLoud) {
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 0;
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    // The LK6 cycle-1 empty-.text guard fires before the entry-point
    // lookup would reach K_SymbolUndefined — a zero-function module
    // produces zero `.text` bytes, and an executable with no entry
    // instructions would SIGSEGV at exec time. EITHER diagnostic
    // code counts as fail-loud; pin the contract loosely so future
    // re-orderings of the two guards don't churn this test.
    EXPECT_GT(rep.errorCount(), 0u);
}

// ── Schema validate rules pinned ───────────────────────────────

TEST(ElfExecFormatJson, ExecWithZeroVirtualAddressRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bad-exec","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096 },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 2 reasons, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    // TWO by design: the ELF-block ET_EXEC rule and the terminal
    // cross-format exec-flavor rule that restates the same contract
    // for PE / Mach-O. Both are about THIS value.
    EXPECT_EQ(errorCount(r), 2u) << rejectSummary(r);
    // Both land at this one pointer, so `GE` here -- retiring the
    // redundant restatement must not become a false red -- and the
    // message pin below names the ELF-block rule this test exists for.
    EXPECT_GE(countAtPath(r, "/sections/<text>/virtualAddress"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countWithMessage(r, "ELF ET_EXEC format requires"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(ElfExecFormatJson, ExecWithoutPageAlignRejected) {
    // D-LK6-3: ET_EXEC schemas MUST declare `elf.pageAlign`. The
    // Linux kernel rejects ELF executables whose PT_LOAD p_align
    // is smaller than the runtime page size — ARM64 / Apple
    // Silicon configurations declare 16384 or 65536 instead of
    // 4096, so the value cannot be hardcoded in the walker.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"no-page-align","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec" },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/elf/pageAlign"), 1u) << rejectSummary(r);
    EXPECT_EQ(countWithMessage(r, "must declare 'elf.pageAlign'"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(ElfExecFormatJson, PageAlignMustBePowerOfTwo) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"odd-page-align","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 3000 },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 2 reasons, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    // TWO, and the second is a KNOCK-ON of the first: the loader
    // rejects 3000 and therefore never assigns `elf.pageAlign`, so
    // validate() then sees 0 and adds its 'must declare' arm.
    EXPECT_EQ(errorCount(r), 2u) << rejectSummary(r);
    // BOTH land at `/elf/pageAlign`, so `GE` there; the message pin
    // below is what distinguishes THIS rule from the missing-key one
    // pinned by ExecWithoutPageAlignRejected.
    EXPECT_GE(countAtPath(r, "/elf/pageAlign"), 1u) << rejectSummary(r);
    EXPECT_EQ(countWithMessage(r, "must be a positive power of two"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(ElfRelFormatJson, RelWithNonZeroVirtualAddressRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bad-rel","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"rel" },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: class/data/machine are valid and
    // 'segment' is empty (an ELF row), so the non-zero virtualAddress
    // on this ET_REL row is the only diagnostic.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/sections/0/virtualAddress"), 1u)
        << rejectSummary(r);
}

TEST(ElfFormatJson, UnknownTypeStringRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bad-type","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"executable" }
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: the loader emits this diagnostic and
    // leaves `elf.type` at its Rel default (no return); class/data/
    // machine are valid and no sections are declared, so validate()
    // adds nothing further.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/elf/type"), 1u) << rejectSummary(r);
}

// PE NonZeroVirtualAddressRejected test relocated to
// tests/link/test_pe_writer.cpp (convention: per-format tests
// live with their format).

// ── entryPoint resolution (when set) ──────────────────────────

TEST(ElfExecWriter, EntryPointResolvesSecondFunctionByName) {
    // When `entryPoint` is non-empty, the walker looks up the named
    // function (matched against the synthesized `sym_<id>` form
    // today; real names land with D-LK1-1). Verify that a 2-function
    // module with `entryPoint = "sym_42"` (second function's id)
    // computes e_entry from the SECOND function's offset in .text,
    // not the first.
    auto execShared = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(execShared.has_value());
    // Forge a schema with entryPoint = "sym_42" by re-loading from text
    // (the shipped file has entryPoint=""; we override).
    // D-LK10-ENTRY 2.13: exec-flavored ⇒ `processExit` + its paired
    // `entryCallingConvention` are REQUIRED at load. NO
    // `imageEntryOverride` (direct `elf::encode`, not
    // `encodeUntrampolined`): resolving "sym_42" to the SECOND
    // function is the behavior under test, and the override branch
    // returns before `entryPoint` is read.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"forge-exec","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096 },
      "entryPoint": "sym_42",
      "sections":[
        {"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400},
        {"kind":"symtab","name":".symtab","type":2,"flags":0,"addrAlign":8,"entrySize":24,"virtualAddress":0},
        {"kind":"strtab","name":".strtab","type":3,"flags":0,"addrAlign":1,"entrySize":0,"virtualAddress":0},
        {"kind":"shstrtab","name":".shstrtab","type":3,"flags":0,"addrAlign":1,"entrySize":0,"virtualAddress":0}
      ]
    })");
    ASSERT_TRUE(r.has_value());
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());

    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction a;
    a.symbol = SymbolId{1};
    a.bytes = {0x90, 0x90, 0xC3};  // 3 bytes
    mod.functions.push_back(std::move(a));
    AssembledFunction b;
    b.symbol = SymbolId{42};       // matches entryPoint = "sym_42"
    b.bytes = {0xC3};
    mod.functions.push_back(std::move(b));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **r, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // e_entry = virtualAddress + funcTextStart[1] = 0x401000 + 3.
    EXPECT_EQ(readU64LE(bytes, 24), 0x401000u + 3u);
}

TEST(ElfExecWriter, UnknownEntryPointFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    // D-LK10-ENTRY 2.13: exec-flavored ⇒ `processExit` + its paired
    // `entryCallingConvention` are REQUIRED at load. NO
    // `imageEntryOverride` (direct `elf::encode`, not
    // `encodeUntrampolined`) — an override would resolve the entry
    // and erase the K_SymbolUndefined this test pins.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bad-entry","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096 },
      "entryPoint": "sym_99",
      "sections":[
        {"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400},
        {"kind":"symtab","name":".symtab","type":2,"flags":0,"addrAlign":8,"entrySize":24,"virtualAddress":0},
        {"kind":"strtab","name":".strtab","type":3,"flags":0,"addrAlign":1,"entrySize":0,"virtualAddress":0},
        {"kind":"shstrtab","name":".shstrtab","type":3,"flags":0,"addrAlign":1,"entrySize":0,"virtualAddress":0}
      ]
    })");
    ASSERT_TRUE(r.has_value());

    AssembledModule mod = makeTrivialModule({0xC3}, 1);  // sym 1, not 99
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **r, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawK = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined) sawK = true;
    }
    EXPECT_TRUE(sawK);
}

// ── PT_LOAD page alignment (kernel ENOEXEC guard) ──────────────

TEST(ElfExecWriter, PtLoadFileOffsetIsPageAligned) {
    // Silent-failure-hunter CRITICAL-1: p_offset must satisfy
    //   p_offset % p_align == p_vaddr % p_align
    // The kernel rejects mismatched ET_EXEC with ENOEXEC. p_vaddr
    // is 0x401000 (page-aligned); p_offset must therefore be a
    // multiple of 0x1000.
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 42);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    std::uint64_t const pOffset = readU64LE(bytes, 64 + 8);
    std::uint64_t const pVaddr = readU64LE(bytes, 64 + 16);
    std::uint64_t const pAlign = readU64LE(bytes, 64 + 48);
    EXPECT_EQ(pAlign, 0x1000u);
    EXPECT_EQ(pOffset % pAlign, pVaddr % pAlign)
        << "p_offset (" << pOffset << ") and p_vaddr (" << pVaddr
        << ") must be congruent modulo p_align (" << pAlign << ") "
           "or the Linux kernel will reject the executable with ENOEXEC";
}

// ── End-to-end via the format-blind linker::link() dispatch ────────────

TEST(LinkerEndToEnd, ElfExecDispatchProducesValidExecutable) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(image.ok());
    EXPECT_EQ(image.format, ObjectFormatKind::Elf);
    EXPECT_FALSE(image.bytes.empty());
    EXPECT_EQ(rep.errorCount(), 0u);
    // ★ FATAL size guard BEFORE the first index — the `EXPECT_FALSE(empty())`
    // above is NON-fatal, so on a regression that returns no (or too few)
    // bytes the pins below would walk into `image.bytes[0..3]` and
    // `readU16LE(image.bytes, 16)`, and `readU16LE` indexes its
    // `std::span` unchecked (this file's anonymous namespace). Its two
    // siblings already do it this way — cited by TEST NAME, not line, since
    // the numbers drift and the names do not:
    //   `LinkerEndToEnd.PeDispatchProducesNonEmptyBytes` (test_pe_writer.cpp)
    //   `LinkerEndToEnd.MachODispatchProducesNonEmptyBytes`
    //       (test_macho_writer.cpp)
    // ✔MEASURED 2026-08-05 (TF-C120) — why this is a fatal guard and not a
    // tidy-up: the SAME shape elsewhere in `tests/link/test_pe_writer.cpp`
    // (non-fatal empty-check, then an unchecked `std::span::operator[]`)
    // SEGFAULTED that test binary after 83 of 97 tests, so 14 tests never
    // ran and nothing reported them missing — and one of the 14,
    // `LinkerExternResolution.OkFalseWhenWalkerFailsLoud`, turned out to be
    // genuinely broken, hidden the whole time behind the crash. A
    // non-fatal guard in front of an unchecked read does not just fail this
    // test; it can delete every test scheduled after it.
    // 18u = the highest byte any pin below touches (e_type is the u16 at
    // offset 16, so bytes 16 and 17 must exist).
    ASSERT_GE(image.bytes.size(), 18u)
        << "the format-blind dispatch returned too few bytes to hold an ELF "
           "header — asserting FATALLY so the pins below cannot read out of "
           "bounds and take the rest of this binary's tests down with them";
    // ELF magic
    EXPECT_EQ(image.bytes[0], 0x7Fu);
    EXPECT_EQ(image.bytes[1], 'E');
    EXPECT_EQ(image.bytes[2], 'L');
    EXPECT_EQ(image.bytes[3], 'F');
    // e_type = ET_EXEC
    EXPECT_EQ(readU16LE(image.bytes, 16), 2u);
}

// ── LK6 cycle 2b.2: ELF GOT/PLT walker — D-LK6-4 closed ────────

TEST(ElfExecWriter, ExternImportsWithEmptyInterpreterFailsLoud) {
    // Cycle 2b.1 substrate gate: ET_EXEC schemas with externs
    // require non-empty `elf.interpreter`. A schema with empty
    // interpreter (no PT_INTERP path) cannot produce a loadable
    // dynamic image.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    // D-LK10-ENTRY 2.13: exec-flavored ⇒ `processExit` + its paired
    // `entryCallingConvention` are REQUIRED, else the fixture is
    // rejected at load. `elf.interpreter` stays absent on purpose —
    // the empty-PT_INTERP reject is what this test pins.
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"exec-no-interp","kind":"elf"},
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096 },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{99};
    rel.kind   = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = "printf";
    imp.libraryPath = "libc.so.6";
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

// ★ RED-ON-DISABLE for the ELF walker's OWN data-import binding belt, and for
//   the ownership change that put it there.
//   D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET.
//
// The walker USED to DERIVE the binding model from the image flavour —
// `required = isDyn ? GotIndirect : CopyRelocation` — and then merely ERROR if
// the schema's `dataImportBinding` disagreed. That made the schema key a
// redundant restatement the engine validated instead of a decision it read, and
// it put the choice in an `isDyn` identity branch. The walker now READS the
// declared value and implements exactly `got-indirect`.
//
// So the belt this pins is: a schema that declares NO model reaching the walker
// must FAIL LOUD, never silently receive a GOT slot it did not declare. It is
// reachable only by driving the walker DIRECTLY — the linker's pre-walker gate
// refuses a nullopt binding first — which is exactly what this test does, and is
// why the belt is not dead code. The PE and Mach-O walkers carry the identical
// assertion (test_pe_writer.cpp's
// `DataExternUnderUndeclaredDataImportBindingFailsLoud`).
//
// ⓘ There is deliberately no "declares a FOREIGN model" arm any more: the second
// enum member was DELETED, so a foreign spelling is refused AT LOAD (pinned by
// test_elf_dyn_writer.cpp's
// `DeletedCopyRelocationBindingRefusedAtLoadOnEveryFlavour`) and can never
// reach a walker.
TEST(ElfExecWriter, DataExternUnderUndeclaredDataImportBindingFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    // A complete exec schema EXCEPT that `dataImportBinding` is absent.
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "format": {"name":"exec-no-data-binding","kind":"elf"},
      "externCallDispatch": "direct-plt",
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096, "interpreter": "/lib64/ld-linux-x86-64.so.2" },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}],
      "relocations":[{"name":"R_X86_64_PC32","kind":1,"nativeId":2}]
    })");
    ASSERT_TRUE(fmt.has_value())
        << "the fixture must LOAD — otherwise this measures the loader, not "
           "the walker's belt";
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3};   // mov rax,[rip+disp]
    Relocation rel;
    rel.offset = 3;
    rel.target = SymbolId{99};
    rel.kind   = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    ExternImport dataExt;
    dataExt.symbol         = SymbolId{99};
    dataExt.mangledName    = "__environ";
    dataExt.libraryPath    = "libc.so.6";
    dataExt.isData         = true;
    dataExt.dataSizeBytes  = 8;
    dataExt.dataAlignBytes = 8;
    mod.externImports.push_back(std::move(dataExt));
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty())
        << "no image may be produced under an undeclared binding model";
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport) sawCode = true;
    }
    EXPECT_TRUE(sawCode)
        << "the walker must name the missing 'dataImportBinding' declaration — "
           "silently binding the data extern is the miscompile class this belt "
           "exists for";

    // POSITIVE CONTROL, ONE VARIABLE: the SAME module under the SAME synthetic
    // schema with `dataImportBinding` ADDED, and nothing else changed. Without
    // it the reject above could be the module being malformed rather than the
    // schema being silent; with a *different* schema as the control it could be
    // any of a dozen other keys.
    auto declared = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "format": {"name":"exec-with-data-binding","kind":"elf"},
      "externCallDispatch": "direct-plt",
      "dataImportBinding": "got-indirect",
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism":"by-name-import", "role": "cLibrary","importMangledName":"exit" },
      "entryCallingConvention": "sysv_amd64",
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096, "interpreter": "/lib64/ld-linux-x86-64.so.2" },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}],
      "relocations":[{"name":"R_X86_64_PC32","kind":1,"nativeId":2}]
    })");
    ASSERT_TRUE(declared.has_value());
    // …and the SHIPPED formats really do declare it, so the control is the
    // shipped configuration and not a fixture-only shape.
    auto shipped = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(shipped.has_value());
    ASSERT_TRUE((*shipped)->dataImportBinding().has_value());
    EXPECT_TRUE(*(*shipped)->dataImportBinding()
                == DataImportBinding::GotIndirect)
        << "the shipped ELF exec format must declare got-indirect: a copy "
           "relocation claims ONE name of an alias set and splits the object";
    AssembledModule ok;
    ok.expectedFuncCount = 1;
    // D-LK10-ENTRY 2.13: an exec schema declaring `processExit` CONTRACTS that
    // the image entry is the injected `_start` trampoline. This test drives the
    // walker directly (no `linker::link`, hence no injection), so the override
    // must name the entry explicitly — the walker refuses to default it rather
    // than silently make functions[0] the process entry.
    ok.imageEntryOverride = 0u;
    AssembledFunction fn2;
    fn2.symbol = SymbolId{1};
    fn2.bytes  = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3};
    Relocation rel2;
    rel2.offset = 3;
    rel2.target = SymbolId{99};
    rel2.kind   = RelocationKind{1};
    fn2.relocations.push_back(rel2);
    ok.functions.push_back(std::move(fn2));
    ExternImport dataOk;
    dataOk.symbol         = SymbolId{99};
    dataOk.mangledName    = "__environ";
    dataOk.libraryPath    = "libc.so.6";
    dataOk.isData         = true;
    dataOk.dataSizeBytes  = 8;
    dataOk.dataAlignBytes = 8;
    ok.externImports.push_back(std::move(dataOk));
    DiagnosticReporter rep2;
    auto okBytes = elf::encode(ok, **target, **declared, rep2);
    std::ostringstream why;
    for (auto const& d : rep2.all()) why << " | " << d.actual;
    EXPECT_EQ(rep2.errorCount(), 0u)
        << "positive control: adding ONLY `dataImportBinding` must make the "
           "SAME module encode:" << why.str();
    EXPECT_FALSE(okBytes.empty());
}

TEST(ElfRelWriter, ExternDataImportNoLongerFailsLoudOnEtRel) {
    // D-LK-OBJECT-DATA-EXTERN-RELOCATABLE (c144): a relocatable object no
    // longer rejects an extern DATA import. It emits the referenced ones as
    // SHN_UNDEF NOTYPE symbols + PC32 relocs the FINAL linker resolves by
    // copy-relocation (see ElfWriter.ObjectDataExternRefEmitsUndefNameAnd
    // Pc32NotPlt32 for the byte-level pin), and silently DROPS the
    // UNREFERENCED ones (dead surface — no reloc names them, so the
    // reloc-driven symtab loop never emits them). This module's `stdout` is
    // unreferenced (the sole function has no relocation), so encode succeeds
    // and drops it — where it previously failed loud with
    // K_FormatLacksImportSupport. Red-on-disable: restore the writer's isData
    // reject and `errorCount` becomes non-zero.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = "stdout";
    imp.libraryPath = "libc.so.6";
    imp.isData      = true;          // extern DATA — now accepted, not rejected
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "an extern DATA import in a relocatable object must NOT fail loud";
    EXPECT_FALSE(bytes.empty())
        << "the object still encodes (the unreferenced data extern is dropped)";
    for (auto const& d : rep.all()) {
        EXPECT_NE(d.code, DiagnosticCode::K_FormatLacksImportSupport)
            << "the ET_REL data-import reject is lifted (c144)";
    }
}

// ── D-LK1-ELF-EXEC-DATA-SECTIONS: .rodata emitted from dataItems ──
//
// The ET_EXEC walker emits a loadable `.rodata` section (folded into
// the single R+X PT_LOAD) whenever `module.dataItems` carries any
// item with `section == DataSectionKind::Rodata`. These pins are
// host-independent (decided entirely from the emitted bytes — the
// .rodata section content/placement IS the deliverable, fully
// decidable from the wire image; not a runtime claim). Runtime
// behavior (`int g=42; return g;` → exit 42) is the corpus
// `examples/c-subset/global_int`'s ELF arms on the Linux + native-
// arm64 CI legs.
namespace {

// Each Elf64_Shdr is 64 bytes. Field offsets within a header:
//   sh_name(u32)@0, sh_type(u32)@4, sh_flags(u64)@8, sh_addr(u64)@16,
//   sh_offset(u64)@24, sh_size(u64)@32.
constexpr std::size_t kShdrSize = 64;

// Locate the `.rodata` section header index by its distinguishing
// fields: SHT_PROGBITS(1) + SHF_ALLOC(2) + the expected sh_addr.
// Returns the header's file offset, or 0 if not found (0 is the
// SHT_NULL header, never .rodata, so it doubles as "not found").
[[nodiscard]] std::size_t findRodataShdrOff(
        std::span<std::uint8_t const> bytes, std::uint64_t expectedAddr) {
    std::uint64_t const shoff  = readU64LE(bytes, 40);
    std::uint16_t const shnum  = readU16LE(bytes, 60);
    for (std::uint16_t i = 1; i < shnum; ++i) {
        std::size_t const off = static_cast<std::size_t>(shoff)
                              + static_cast<std::size_t>(i) * kShdrSize;
        if (readU32LE(bytes, off + 4) == 1u            // SHT_PROGBITS
         && readU64LE(bytes, off + 8) == 2u            // SHF_ALLOC
         && readU64LE(bytes, off + 16) == expectedAddr) {
            return off;
        }
    }
    return 0;
}

// Build a module with one trivial function + one rodata dataItem
// {42,0,0,0} (an `int g=42;`-shaped item), NO relocations.
[[nodiscard]] AssembledModule makeModuleWithRodata() {
    AssembledModule mod = makeTrivialModule({0xC3}, 1);  // ret
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::Rodata;
    d.bytes     = {42, 0, 0, 0};            // int 42, little-endian
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    return mod;
}

} // namespace

// ★ THE BELT, EXERCISED RATHER THAN ASSERTED — and it exists because the
// copy-relocation deletion took away a link-time constant.
// D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET.
//
// `FILE **pp = &stdout;` needs the ADDRESS of an imported object. Under copy
// relocation the exec owned a `.bss` copy that WAS the object, so the address
// was a link-time constant and the data-item apply could bake it in. Under
// got-indirect there is no link-time address (the object lives in libc; the
// exec holds a GOT slot ld.so fills), so the slot gets a SYMBOL-BASED absolute
// dynamic relocation and ld.so writes the real address at load. ✔MEASURED: the
// first cut of the deletion baked `symbolVa[extern]` — the GOT SLOT — and
// `examples/c-subset/extern_data_addr_static_init` went red at exit 1, a
// pointer one indirection off with no diagnostic.
//
// A load-time write cannot reach a READ-ONLY page. The normal C shapes never
// get there — a reloc-bearing `const` global is classified RELRO and folded
// into writable `.data` (measured: `FILE **const pp = &stdout;` lands in
// `.data` WA with the symbol-based row and runs) — so this is a BELT for a
// HAND-BUILT module, exactly like the alignment re-check in the same walker.
// A belt nobody ever fires asserts nothing, hence this test: it builds that
// module directly and requires the loud refusal, because the alternative the
// walker would otherwise take is baking the wrong pointer SILENTLY.
TEST(ElfExecWriter, RodataItemHoldingImportedObjectAddressFailsLoud) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::Rodata;   // READ-ONLY on purpose
    d.bytes     = {0, 0, 0, 0, 0, 0, 0, 0};  // an 8-byte pointer slot
    d.alignment = Alignment::of<8>();
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{99};               // the extern DATA import
    rel.kind   = RelocationKind{2};          // abs64 (R_X86_64_64)
    d.relocations.push_back(rel);
    mod.dataItems.push_back(std::move(d));
    ExternImport dataExt;
    dataExt.symbol         = SymbolId{99};
    dataExt.mangledName    = "stdout";
    dataExt.libraryPath    = "libc.so.6";
    dataExt.isData         = true;
    dataExt.dataSizeBytes  = 8;
    dataExt.dataAlignBytes = 8;
    mod.externImports.push_back(std::move(dataExt));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty())
        << "no image may ship with an unfixable read-only address slot";
    std::ostringstream why;
    bool sawCode = false;
    for (auto const& d2 : rep.all()) {
        why << " | " << d2.actual;
        if (d2.code == DiagnosticCode::K_RelocationKindMismatch) sawCode = true;
    }
    EXPECT_TRUE(sawCode)
        << "the walker must name the read-only slot and the import whose "
           "address it cannot fix up:" << why.str();

    // POSITIVE CONTROL, ONE VARIABLE: the SAME item in a WRITABLE section
    // encodes cleanly. Without it the refusal above could be the extern, the
    // reloc kind, or the fixture — rather than the section's writability.
    AssembledModule ok = makeTrivialModule({0xC3}, 1);
    AssembledData dw;
    dw.symbol    = SymbolId{42};
    dw.section   = DataSectionKind::Data;    // the ONLY difference
    dw.bytes     = {0, 0, 0, 0, 0, 0, 0, 0};
    dw.alignment = Alignment::of<8>();
    Relocation relw;
    relw.offset = 0;
    relw.target = SymbolId{99};
    relw.kind   = RelocationKind{2};
    dw.relocations.push_back(relw);
    ok.dataItems.push_back(std::move(dw));
    ExternImport dataOk;
    dataOk.symbol         = SymbolId{99};
    dataOk.mangledName    = "stdout";
    dataOk.libraryPath    = "libc.so.6";
    dataOk.isData         = true;
    dataOk.dataSizeBytes  = 8;
    dataOk.dataAlignBytes = 8;
    ok.externImports.push_back(std::move(dataOk));
    DiagnosticReporter rep2;
    auto okBytes = encodeUntrampolined(ok, *loaded.target, *loaded.format, rep2);
    std::ostringstream why2;
    for (auto const& d2 : rep2.all()) why2 << " | " << d2.actual;
    EXPECT_EQ(rep2.errorCount(), 0u)
        << "positive control: the same slot in `.data` must encode:"
        << why2.str();
    EXPECT_FALSE(okBytes.empty());
}

TEST(ElfExecWriter, RodataSectionHeaderPinnedFromDataItems) {
    auto loaded = loadShipped();
    AssembledModule mod = makeModuleWithRodata();
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    // .text sh_addr = secText.virtualAddress (0x401000); .rodata VA =
    // 0x401000 + alignUp(textSize=1, 8) = 0x401008.
    constexpr std::uint64_t kTextVa   = 0x401000ull;
    constexpr std::uint64_t kRodataVa = kTextVa + 8;  // alignUp(1,8)=8
    std::size_t const off = findRodataShdrOff(bytes, kRodataVa);
    ASSERT_NE(off, 0u)
        << "no .rodata section header with SHT_PROGBITS/SHF_ALLOC and "
           "sh_addr=0x401008 — D-LK1-ELF-EXEC-DATA-SECTIONS not emitted";
    EXPECT_EQ(readU32LE(bytes, off + 4), 1u)   << "sh_type SHT_PROGBITS";
    EXPECT_EQ(readU64LE(bytes, off + 8), 2u)   << "sh_flags SHF_ALLOC";
    EXPECT_EQ(readU64LE(bytes, off + 16), kRodataVa) << "sh_addr";
    EXPECT_EQ(readU64LE(bytes, off + 32), 4u)  << "sh_size == 4 bytes";
    // The .rodata bytes in the file equal {42,0,0,0}.
    std::uint64_t const rodataFileOff = readU64LE(bytes, off + 24);
    ASSERT_GE(bytes.size(), rodataFileOff + 4u);
    EXPECT_EQ(bytes[rodataFileOff + 0], 42u);
    EXPECT_EQ(bytes[rodataFileOff + 1], 0u);
    EXPECT_EQ(bytes[rodataFileOff + 2], 0u);
    EXPECT_EQ(bytes[rodataFileOff + 3], 0u);
}

TEST(ElfExecWriter, RodataExtendsSinglePtLoadAndStaysReadExecute) {
    // The single PT_LOAD's p_filesz must cover THROUGH the rodata
    // extent (p_filesz == rodataFileEnd - textFileOffset), and p_flags
    // stays R+X (5) — SHF_ALLOC-only rodata adds NO write permission.
    //
    // RED-on-disable: this is the load-bearing pin. If the PT_LOAD
    // extension in elf.cpp is reverted to `p_filesz = text.size()`,
    // this p_filesz assertion FAILS (text.size()=1 != the rodata-
    // covering extent), so the loader would never map the global's
    // page → `return g` reads an unmapped VA → SIGSEGV at runtime.
    auto loaded = loadShipped();
    AssembledModule mod = makeModuleWithRodata();
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // e_phnum @ +56 = 1 (still a SINGLE PT_LOAD — no 2nd segment).
    EXPECT_EQ(readU16LE(bytes, 56), 1u)
        << "rodata folds into the existing PT_LOAD — kPtLoadCount stays 1";
    // PT_LOAD program header at byte 64.
    EXPECT_EQ(readU32LE(bytes, 64 + 0), 1u);   // p_type PT_LOAD
    EXPECT_EQ(readU32LE(bytes, 64 + 4), 5u)    // p_flags PF_R|PF_X
        << "rodata is SHF_ALLOC-only — the segment stays R+X (W^X "
           "preserved); a write bit here would be a regression";
    std::uint64_t const pOffset = readU64LE(bytes, 64 + 8);
    std::uint64_t const pFilesz = readU64LE(bytes, 64 + 32);
    std::uint64_t const pMemsz  = readU64LE(bytes, 64 + 40);
    // Locate .rodata to derive its file end.
    constexpr std::uint64_t kRodataVa = 0x401000ull + 8;
    std::size_t const roOff = findRodataShdrOff(bytes, kRodataVa);
    ASSERT_NE(roOff, 0u);
    std::uint64_t const rodataFileOff = readU64LE(bytes, roOff + 24);
    std::uint64_t const rodataFileEnd = rodataFileOff + 4u;
    EXPECT_EQ(pFilesz, rodataFileEnd - pOffset)
        << "p_filesz must span .text through end of .rodata";
    EXPECT_EQ(pMemsz, pFilesz)
        << "p_memsz == p_filesz (no BSS this cycle)";
}

TEST(ElfExecWriter, RodataShiftsShnumAndShstrndxVsControl) {
    // A no-dataItems control and the rodata module differ by exactly
    // one section header, and e_shstrndx tracks the +1 index shift.
    auto loaded = loadShipped();

    // Control: identical function, NO dataItems.
    AssembledModule control = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter repC;
    auto bytesC = encodeUntrampolined(control, *loaded.target, *loaded.format, repC);
    ASSERT_EQ(repC.errorCount(), 0u);
    std::uint16_t const shnumC     = readU16LE(bytesC, 60);
    std::uint16_t const shstrndxC  = readU16LE(bytesC, 62);

    AssembledModule mod = makeModuleWithRodata();
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    std::uint16_t const shnum     = readU16LE(bytes, 60);
    std::uint16_t const shstrndx  = readU16LE(bytes, 62);

    EXPECT_EQ(shnum, shnumC + 1u)
        << "e_shnum increases by exactly 1 (the .rodata header)";
    EXPECT_EQ(shstrndx, shstrndxC + 1u)
        << "e_shstrndx shifts +1 (.rodata at index 2 pushes .shstrtab)";
    // e_shstrndx must point at the ACTUAL .shstrtab: verify the
    // section it indexes is SHT_STRTAB(3). (Defends against the IDX
    // shift desyncing the header table order from the e_shstrndx math.)
    std::uint64_t const shoff = readU64LE(bytes, 40);
    std::size_t const shstrtabHdr = static_cast<std::size_t>(shoff)
        + static_cast<std::size_t>(shstrndx) * kShdrSize;
    EXPECT_EQ(readU32LE(bytes, shstrtabHdr + 4), 3u)
        << "e_shstrndx must index a SHT_STRTAB section (the real "
           ".shstrtab), proving the +1 shift is coherent";
}

TEST(ElfExecWriter, NoDataItemsEmitsNoRodataByteIdenticalToBaseline) {
    // Control / agnosticism pin: a module with NO dataItems emits NO
    // .rodata section. This is the byte-identity guarantee — the
    // rodata path is fully gated on `!module.dataItems.empty()`, so
    // the no-rodata static-exec output is unchanged from before
    // D-LK1-ELF-EXEC-DATA-SECTIONS landed.
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0x90, 0x90, 0xC3}, 7);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // 5 sections (NULL, .text, .symtab, .strtab, .shstrtab) — NO
    // .rodata. Same count the RelaTextSlotDroppedForExec pin asserts.
    EXPECT_EQ(readU16LE(bytes, 60), 5u);
    // No SHT_PROGBITS+SHF_ALLOC header other than .text (which has
    // SHF_ALLOC|SHF_EXECINSTR = 6, not 2) — scan confirms zero
    // SHF_ALLOC-only PROGBITS sections.
    std::uint64_t const shoff = readU64LE(bytes, 40);
    std::uint16_t const shnum = readU16LE(bytes, 60);
    int rodataLike = 0;
    for (std::uint16_t i = 1; i < shnum; ++i) {
        std::size_t const off = static_cast<std::size_t>(shoff)
                              + static_cast<std::size_t>(i) * kShdrSize;
        if (readU32LE(bytes, off + 4) == 1u
         && readU64LE(bytes, off + 8) == 2u) {
            ++rodataLike;
        }
    }
    EXPECT_EQ(rodataLike, 0)
        << "no-dataItems module must not emit any SHF_ALLOC-only "
           "PROGBITS (.rodata) section";
}

TEST(ElfExecWriter, RodataAlsoEmittedOnArm64SharedCodePath) {
    // Agnosticism pin: the SAME elf.cpp code path emits .rodata on the
    // aarch64 exec format — arch differs only via config (machine /
    // pageAlign), zero `if(arch)` branches. arm64 .text base = 0x400000;
    // rodata VA = 0x400000 + alignUp(textSize=4, 8) = 0x400008 (the
    // arm64 function body is RET = 4 bytes).
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeTrivialModule(
        {0xC0, 0x03, 0x5F, 0xD6}, 1);  // RET (0xD65F03C0 LE)
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::Rodata;
    d.bytes     = {42, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    constexpr std::uint64_t kRodataVa = 0x400000ull + 8;
    std::size_t const off = findRodataShdrOff(bytes, kRodataVa);
    ASSERT_NE(off, 0u)
        << "arm64 ELF exec must emit .rodata via the same code path";
    EXPECT_EQ(readU64LE(bytes, off + 32), 4u);
    std::uint64_t const rodataFileOff = readU64LE(bytes, off + 24);
    EXPECT_EQ(bytes[rodataFileOff + 0], 42u);
    // p_flags still R+X (5) — same W^X invariant cross-arch.
    EXPECT_EQ(readU32LE(bytes, 64 + 4), 5u);
}

// D-LK4-DATA-PRODUCER writer pin (was NonRodataDataItemFailsLoud — flipped when
// the writable-data-sections cycle CLOSED the former fail-loud). A Data item now
// lands in a `.data` section whose sh_flags carry SHF_WRITE (3 = SHF_ALLOC |
// SHF_WRITE) — the bit that makes a runtime store legal — AND it sits in a
// SEPARATE R+W PT_LOAD from the R+X text (W^X). RED if the ELF writer reverts to
// rejecting Data items, drops SHF_WRITE, or folds .data into the R+X segment.
TEST(ElfExecWriter, DataSectionEmittedWritableInSeparateLoadSegment) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);  // ret
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::Data;
    d.bytes     = {7, 0, 0, 0};                 // int = 7
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "ELF writer must ACCEPT a Data item now that D-LK4-DATA-PRODUCER "
           "closed (it was fail-loud before)";
    ASSERT_FALSE(bytes.empty());
    // Scan the section-header table for a SHT_PROGBITS section whose sh_flags
    // carry SHF_WRITE (the .data section). sh_flags(u64)@8.
    constexpr std::uint64_t kShfWrite = 0x1ull;   // SHF_WRITE
    std::uint64_t const shoff = readU64LE(bytes, 40);
    std::uint16_t const shnum = readU16LE(bytes, 60);
    bool sawWritableData = false;
    std::uint64_t dataVa = 0;
    for (std::uint16_t i = 1; i < shnum; ++i) {
        std::size_t const off = static_cast<std::size_t>(shoff)
                              + static_cast<std::size_t>(i) * kShdrSize;
        std::uint32_t const shType  = readU32LE(bytes, off + 4);
        std::uint64_t const shFlags = readU64LE(bytes, off + 8);
        std::uint64_t const shSize  = readU64LE(bytes, off + 32);
        if (shType == 1u /*SHT_PROGBITS*/ && (shFlags & kShfWrite)
            && shSize == 4u) {
            sawWritableData = true;
            dataVa = readU64LE(bytes, off + 16);
            break;
        }
    }
    ASSERT_TRUE(sawWritableData)
        << ".data must be a SHT_PROGBITS section carrying SHF_WRITE (a mutable "
           "global's store must not fault) — D-LK4-DATA-PRODUCER";
    // W^X: the writable segment must be a SEPARATE PT_LOAD with PF_W set, NOT
    // the R+X text segment. Walk the program headers (e_phoff@32, e_phnum@56,
    // 56-byte entries: p_type@0, p_flags@4, p_vaddr@16, p_memsz@40) and find
    // the PT_LOAD whose VA range covers dataVa; assert PF_W (2) is set, PF_X (1)
    // is NOT.
    std::uint64_t const phoff = readU64LE(bytes, 32);
    std::uint16_t const phnum = readU16LE(bytes, 56);
    bool sawWritableSeg = false;
    for (std::uint16_t i = 0; i < phnum; ++i) {
        std::size_t const po = static_cast<std::size_t>(phoff) + i * 56u;
        if (readU32LE(bytes, po + 0) != 1u) continue;   // PT_LOAD
        std::uint32_t const pFlags = readU32LE(bytes, po + 4);
        std::uint64_t const pVaddr = readU64LE(bytes, po + 16);
        std::uint64_t const pMemsz = readU64LE(bytes, po + 40);
        if (dataVa >= pVaddr && dataVa < pVaddr + pMemsz) {
            sawWritableSeg = (pFlags & 0x2u) != 0u    // PF_W set
                          && (pFlags & 0x1u) == 0u;   // PF_X NOT set (W^X)
            break;
        }
    }
    EXPECT_TRUE(sawWritableSeg)
        << ".data must live in a PT_LOAD with PF_W and WITHOUT PF_X (W^X — a "
           "mutable global must never share a page with executable code)";
}

// D-LK4-DATA-PRODUCER writer pin: a Bss (zero-init) item lands in a SHT_NOBITS
// section that is writable (SHF_WRITE) and carries a non-zero sh_size (the
// zero-fill extent). The R+W PT_LOAD's p_memsz must exceed its p_filesz (bss
// adds memory but no file bytes). RED if the writer rejects Bss or emits it as
// SHT_PROGBITS / inflates p_filesz to cover it.
TEST(ElfExecWriter, BssSectionEmittedNobitsWritable) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    AssembledData d;
    d.symbol       = SymbolId{43};
    d.section      = DataSectionKind::Bss;
    d.reservedSize = 4;                          // int g; → 4 zero-fill bytes
    d.alignment    = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "ELF writer must ACCEPT a Bss item now that D-LK4-DATA-PRODUCER closed";
    ASSERT_FALSE(bytes.empty());
    // Find the SHT_NOBITS(8) section with SHF_WRITE + sh_size 4.
    std::uint64_t const shoff = readU64LE(bytes, 40);
    std::uint16_t const shnum = readU16LE(bytes, 60);
    bool sawNobits = false;
    std::uint64_t bssVa = 0;
    for (std::uint16_t i = 1; i < shnum; ++i) {
        std::size_t const off = static_cast<std::size_t>(shoff)
                              + static_cast<std::size_t>(i) * kShdrSize;
        if (readU32LE(bytes, off + 4) == 8u /*SHT_NOBITS*/
            && (readU64LE(bytes, off + 8) & 0x1ull) /*SHF_WRITE*/
            && readU64LE(bytes, off + 32) == 4u /*sh_size*/) {
            sawNobits = true;
            bssVa = readU64LE(bytes, off + 16);
            break;
        }
    }
    ASSERT_TRUE(sawNobits)
        << ".bss must be SHT_NOBITS + SHF_WRITE with sh_size == 4 "
           "(D-LK4-DATA-PRODUCER)";
    // The PT_LOAD covering bssVa must have p_memsz > p_filesz (the bss extent is
    // in memory but not in the file).
    std::uint64_t const phoff = readU64LE(bytes, 32);
    std::uint16_t const phnum = readU16LE(bytes, 56);
    bool sawMemGtFile = false;
    for (std::uint16_t i = 0; i < phnum; ++i) {
        std::size_t const po = static_cast<std::size_t>(phoff) + i * 56u;
        if (readU32LE(bytes, po + 0) != 1u) continue;   // PT_LOAD
        std::uint64_t const pVaddr  = readU64LE(bytes, po + 16);
        std::uint64_t const pFilesz = readU64LE(bytes, po + 32);
        std::uint64_t const pMemsz  = readU64LE(bytes, po + 40);
        if (bssVa >= pVaddr && bssVa < pVaddr + pMemsz) {
            sawMemGtFile = pMemsz > pFilesz;
            break;
        }
    }
    EXPECT_TRUE(sawMemGtFile)
        << "the .bss PT_LOAD must have p_memsz > p_filesz (zero-fill adds "
           "memory, not file bytes)";
}

TEST(ElfExecWriter, RodataDataItemWithRelocationFailsLoud) {
    // Fail-loud: a rodata item carrying its OWN relocations (data->data
    // reference) is deferred this cycle — the ELF writer patches
    // FUNCTION relocations only. Cited anchor: D-LK1-ELF-RODATA-
    // DATAITEM-RELOC.
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::Rodata;
    d.bytes     = {0, 0, 0, 0, 0, 0, 0, 0};    // 8-byte pointer slot
    d.alignment = Alignment::of<8>();
    Relocation r;
    r.offset = 0; r.target = SymbolId{1}; r.kind = RelocationKind{2};
    d.relocations.push_back(r);
    mod.dataItems.push_back(std::move(d));
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& diag : rep.all()) {
        if (diag.code == DiagnosticCode::K_NoMatchingObjectFormat
         && diag.actual.find("D-LK1-ELF-RODATA-DATAITEM-RELOC")
                != std::string::npos) {
            sawCode = true;
        }
    }
    EXPECT_TRUE(sawCode)
        << "rodata dataItem with relocations must fail loud citing "
           "D-LK1-ELF-RODATA-DATAITEM-RELOC";
}

TEST(ElfExecWriter, RelRoConstItemFoldsIntoDataAndPatchesInPlace) {
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): in the EXECUTABLE image a relro
    // item is FOLDED into `.data` ("treat relro like .data") and its abs64 slot is
    // patched IN PLACE with the target's absolute VA — NOT fail-loud, and no
    // `.rela`. RED-on-disable: without the merge+apply the writer either
    // fail-louds (the old D-LK1-ELF-RODATA-DATAITEM-RELOC path, now relro-routed)
    // or leaves the pointer slot zero.
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeTrivialModule({0xC3}, 1);   // `ret` at .text+0
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::RelRoConst;
    d.bytes     = {0, 0, 0, 0, 0, 0, 0, 0};   // 8-byte slot, patched below
    d.alignment = Alignment::of<8>();
    Relocation r;
    r.offset = 0;
    r.target = SymbolId{1};          // points at the intra-module function
    r.kind   = RelocationKind{2};    // abs64
    r.addend = 0;
    d.relocations.push_back(r);
    mod.dataItems.push_back(std::move(d));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "a relro item must NOT fail loud in an executable image";
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(readU16LE(bytes, 16), 2u) << "e_type = ET_EXEC";
    // The exec image folds relro into `.data` — NO separate `.data.rel.ro`.
    EXPECT_LT(findSectionByName(bytes, ".data.rel.ro"), 0)
        << "the exec image folds relro into .data (no separate .data.rel.ro)";
    int const dataIdx = findSectionByName(bytes, ".data");
    ASSERT_GE(dataIdx, 0) << "the relro item rides `.data` in the exec image";
    std::uint64_t const shoff   = readU64LE(bytes, 40);
    std::uint64_t const dataOff = readU64LE(bytes, shoff + dataIdx * 64 + 24);
    auto const* secText = loaded.format->sectionByKind(SectionKind::Text);
    ASSERT_NE(secText, nullptr);
    EXPECT_EQ(readU64LE(bytes, dataOff), secText->virtualAddress)
        << "the relro pointer slot is patched in place with the function VA";
}

// ── Code-review FOLD pins (H1 / M1 / L1) ──────────────────────────

TEST(ElfExecWriter, ElfRelAcceptsRodataDataItem) {
    // D-LK-OBJECT-DATA-SECTION-RELOCATABLE: a relocatable object now EMITS a
    // plain (no-reloc) rodata item — the former "(ET_REL) … D-LK1-ELF-EXEC-
    // DATA-SECTIONS exec-only" reject is LIFTED. The item lands in `.rodata`
    // (sh_addr=0) with a section-relative `.symtab` symbol the final linker
    // binds. (The section-relative data-symbol shape is pinned by
    // ElfWriter.ObjectEmitsDataSectionAndSectionRelativeDataSymbol; a data item
    // WITH its own relocation still fails loud — the D-LK1-ELF-RODATA-DATAITEM-
    // RELOC test above.) RED if the ELF writer reverts to rejecting ET_REL data.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::Rodata;
    d.bytes     = {42, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "ET_REL must ACCEPT a plain rodata item "
           "(D-LK-OBJECT-DATA-SECTION-RELOCATABLE)";
    EXPECT_FALSE(bytes.empty());
}

TEST(ElfExecWriter, ElfExecMultipleRodataItemsLayoutWithAlignmentPadding) {
    // H1 + offset placement: TWO rodata items with DIFFERENT alignments,
    // the STRICTER exceeding the schema floor (8) so H1's section-align
    // raise is load-bearing (NOT a no-op against the floor):
    //   item0 = {1,0,0,0}       align 4   → section offset 0  (size 4)
    //   item1 = {0x2a × 16}     align 16  → section offset 16 (item0's
    //                                       4-byte tail padded up to 16)
    // H1 raises sh_addralign to max(schema 8, 4, 16) = 16 (WITHOUT H1
    // it would stay 8). So .rodata VA = textVa + alignUp(textSize=1,16)
    // = 0x401010 (without H1: alignUp(1,8)=0x401008 — the VA itself
    // differs, the RED-on-disable lever). Each data symbol resolves to
    // sectionBaseVa + its offset; the single PT_LOAD's p_filesz spans
    // .text through end of .rodata.
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);   // 1-byte .text
    AssembledData d0;
    d0.symbol    = SymbolId{10};
    d0.section   = DataSectionKind::Rodata;
    d0.bytes     = {1, 0, 0, 0};
    d0.alignment = Alignment::of<4>();
    AssembledData d1;
    d1.symbol    = SymbolId{11};
    d1.section   = DataSectionKind::Rodata;
    d1.bytes     = std::vector<std::uint8_t>(16, 0x2a);   // i128-shaped
    d1.alignment = Alignment::of<16>();                    // > schema floor 8
    mod.dataItems.push_back(std::move(d0));
    mod.dataItems.push_back(std::move(d1));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    constexpr std::uint64_t kTextVa     = 0x401000ull;
    constexpr std::uint64_t kRodataVa   = kTextVa + 16;  // H1: alignUp(1,16)=16
    constexpr std::uint64_t kItem0Va    = kRodataVa + 0; // offset 0
    constexpr std::uint64_t kItem1Va    = kRodataVa + 16;// offset 16 (4→16 pad)

    std::size_t const off = findRodataShdrOff(bytes, kRodataVa);
    ASSERT_NE(off, 0u)
        << "two-item .rodata must sit at H1-raised sh_addr 0x401010";
    // H1 DIRECT proof: sh_addralign (Elf64_Shdr field @ +48: name@0,
    // type@4, flags@8, addr@16, offset@24, size@32, link@40, info@44,
    // addralign@48) carries the raised value (16), NOT the schema
    // floor (8).
    EXPECT_EQ(readU64LE(bytes, off + 48), 16u)
        << "sh_addralign must be H1-raised to 16 (the strictest item)";
    // sh_size spans both items + the inter-item pad: 16 (item1 start) +
    // 16 (item1 size) = 32 bytes.
    EXPECT_EQ(readU64LE(bytes, off + 32), 32u) << "sh_size == 32";
    EXPECT_EQ(readU64LE(bytes, off + 16), kRodataVa) << "sh_addr";
    // The 12-byte padding gap between item0 (4 bytes) and item1 (at
    // offset 16) is zero-filled.
    std::uint64_t const roFileOff = readU64LE(bytes, off + 24);
    ASSERT_GE(bytes.size(), roFileOff + 32u);
    EXPECT_EQ(bytes[roFileOff + 0], 1u);                 // item0 byte 0
    for (std::size_t i = 4; i < 16; ++i)
        EXPECT_EQ(bytes[roFileOff + i], 0u) << "pad byte " << i;
    EXPECT_EQ(bytes[roFileOff + 16], 0x2au);             // item1 byte 0
    EXPECT_EQ(bytes[roFileOff + 31], 0x2au);             // item1 byte 15

    // Each data symbol's VA == sectionBaseVa + its section offset. The
    // symbolVa map (rodataSectionVa + rodataItemOffsets[i]) that the
    // reloc kernel resolves against is INTERNAL — ELF data items are
    // NOT emitted as symtab entries this cycle (the symtab loop emits
    // only STT_FUNC symbols). So the per-item VAs are proven through
    // the two wire facts pinned above: the section base VA (sh_addr =
    // 0x401010) and each item's byte offset within .rodata (item0 @ +0,
    // item1 @ +16, the 4→16 alignment pad observed as the zero gap).
    //   item0 VA == 0x401010 + 0 == kItem0Va, item1 VA == +16 == kItem1Va.
    EXPECT_EQ(readU64LE(bytes, off + 16) + 0u,  kItem0Va) << "item0 VA";
    EXPECT_EQ(readU64LE(bytes, off + 16) + 16u, kItem1Va) << "item1 VA";

    // The single PT_LOAD's p_filesz covers .text through end of .rodata.
    EXPECT_EQ(readU16LE(bytes, 56), 1u) << "still ONE PT_LOAD";
    std::uint64_t const pOffset = readU64LE(bytes, 64 + 8);
    std::uint64_t const pFilesz = readU64LE(bytes, 64 + 32);
    std::uint64_t const rodataFileEnd = roFileOff + 32u;
    EXPECT_EQ(pFilesz, rodataFileEnd - pOffset)
        << "p_filesz spans .text through end of both rodata items";
}

TEST(ElfExecWriter, ElfExecAnonymousRodataItemsDoNotCollide) {
    // M1: TWO anonymous rodata items (the `SymbolId{}` sentinel —
    // read-only constants / padding, per asm.hpp "Multiple sentinel
    // items are legitimate"). They are referenced by section offset,
    // NOT by symbol, so they must NOT join symbolVa — encode SUCCEEDS
    // (no K_DuplicateDataSymbol false-fire on the 2nd SymbolId{}).
    // Distinct bytes so the two items are individually meaningful.
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    AssembledData a0;
    a0.symbol    = SymbolId{};                // anonymous sentinel
    a0.section   = DataSectionKind::Rodata;
    a0.bytes     = {0xAA, 0xBB, 0xCC, 0xDD};
    a0.alignment = Alignment::of<4>();
    AssembledData a1;
    a1.symbol    = SymbolId{};                // ALSO anonymous — legit
    a1.section   = DataSectionKind::Rodata;
    a1.bytes     = {0x11, 0x22, 0x33, 0x44};
    a1.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(a0));
    mod.dataItems.push_back(std::move(a1));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "two anonymous SymbolId{} rodata items must NOT collide";
    ASSERT_FALSE(bytes.empty());
    for (auto const& diag : rep.all()) {
        EXPECT_NE(diag.code, DiagnosticCode::K_DuplicateDataSymbol)
            << "anonymous items are offset-referenced, never reloc "
               "targets — must not join symbolVa";
    }
    // Both items still land in .rodata contiguously: 4 + 4 = 8 bytes.
    constexpr std::uint64_t kRodataVa = 0x401000ull + 8;
    std::size_t const off = findRodataShdrOff(bytes, kRodataVa);
    ASSERT_NE(off, 0u);
    EXPECT_EQ(readU64LE(bytes, off + 32), 8u) << "sh_size == 8 (4+4)";
    std::uint64_t const roFileOff = readU64LE(bytes, off + 24);
    ASSERT_GE(bytes.size(), roFileOff + 8u);
    EXPECT_EQ(bytes[roFileOff + 0], 0xAAu);
    EXPECT_EQ(bytes[roFileOff + 4], 0x11u);
}

// ═════════════════════════════════════════════════════════════════
// D-CSUBSET-THREAD-LOCAL (TLS C1): the ELF dynamic walker's PT_TLS /
// .tdata / .tbss / tpoff structural pins.
//
// ★ RED-ON-DISABLE POSTURE (the arc's test-methodology insight): a
// SINGLE-THREAD runtime witness cannot distinguish real TLS from a
// process-shared static alias — both pass "init visible + mutation
// sticks". THESE STRUCTURAL PINS ARE THE DISCRIMINATOR: rerouting
// isThreadLocal globals through Data/Bss (the alias miscompile) keeps
// the single-thread examples green while PT_TLS disappears, the
// SHF_TLS headers vanish, and the patched disp32s become VAs instead
// of the hand-derived tpoffs — turning every EXPECT below red. The
// runnable 2-thread discriminator is examples/c-subset/
// thread_local_pthread (each worker must observe the TEMPLATE value).
// ═════════════════════════════════════════════════════════════════

namespace {

// Dynamic-arm scaffolding: 1 function + 1 extern (the dynamic walker
// requires a non-empty externImports). The function body is a rel32
// call at offset 1 (-> the extern's PLT stub) followed by `nSlots`
// 5-byte "90 + 4 zero bytes" groups whose zero-byte windows host the
// tls-tpoff32 relocs the tests place, then C3 (ret).
[[nodiscard]] AssembledModule makeTlsDynModule(std::size_t nSlots) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0};
    for (std::size_t i = 0; i < nSlots; ++i) {
        fn.bytes.push_back(0x90);
        for (int b = 0; b < 4; ++b) fn.bytes.push_back(0);
    }
    fn.bytes.push_back(0xC3);
    Relocation call;
    call.offset = 1; call.target = SymbolId{99};
    call.kind = RelocationKind{1};                   // rel32
    fn.relocations.push_back(call);
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = "exit";
    imp.libraryPath = "libc.so.6";
    mod.externImports.push_back(std::move(imp));
    return mod;
}

// The text offset of tls slot `i`'s 4-byte disp window (the zero bytes
// after the i-th 0x90): 5 (call) + i*5 + 1.
[[nodiscard]] constexpr std::uint64_t tlsSlotOff(std::size_t i) {
    return 5u + static_cast<std::uint64_t>(i) * 5u + 1u;
}

[[nodiscard]] AssembledData makeTdataItem(std::uint32_t sym,
                                          std::vector<std::uint8_t> bytes,
                                          std::uint32_t align) {
    AssembledData d;
    d.symbol    = SymbolId{sym};
    d.section   = DataSectionKind::Tdata;
    d.bytes     = std::move(bytes);
    d.alignment = Alignment::ofRuntimePow2(align);
    return d;
}

[[nodiscard]] AssembledData makeTbssItem(std::uint32_t sym,
                                         std::uint64_t size,
                                         std::uint32_t align) {
    AssembledData d;
    d.symbol       = SymbolId{sym};
    d.section      = DataSectionKind::Tbss;
    d.reservedSize = size;
    d.alignment    = Alignment::ofRuntimePow2(align);
    return d;
}

struct PhdrView {
    std::uint32_t type = 0, flags = 0;
    std::uint64_t off = 0, va = 0, filesz = 0, memsz = 0, align = 0;
};

[[nodiscard]] std::vector<PhdrView> readPhdrs(
        std::span<std::uint8_t const> b) {
    std::vector<PhdrView> out;
    std::uint64_t const phoff = readU64LE(b, 32);
    std::uint16_t const phnum = readU16LE(b, 56);
    for (std::uint16_t i = 0; i < phnum; ++i) {
        std::size_t const o = static_cast<std::size_t>(phoff) + i * 56u;
        PhdrView p;
        p.type   = readU32LE(b, o + 0);
        p.flags  = readU32LE(b, o + 4);
        p.off    = readU64LE(b, o + 8);
        p.va     = readU64LE(b, o + 16);
        p.filesz = readU64LE(b, o + 32);
        p.memsz  = readU64LE(b, o + 40);
        p.align  = readU64LE(b, o + 48);
        out.push_back(p);
    }
    return out;
}

// Find the FIRST section header whose sh_type+sh_flags match. Returns
// the header's file offset, or 0 when absent.
[[nodiscard]] std::size_t findShdrByTypeFlags(
        std::span<std::uint8_t const> b,
        std::uint32_t type, std::uint64_t flags) {
    std::uint64_t const shoff = readU64LE(b, 40);
    std::uint16_t const shnum = readU16LE(b, 60);
    for (std::uint16_t i = 1; i < shnum; ++i) {
        std::size_t const off = static_cast<std::size_t>(shoff)
                              + static_cast<std::size_t>(i) * kShdrSize;
        if (readU32LE(b, off + 4) == type
            && readU64LE(b, off + 8) == flags) {
            return off;
        }
    }
    return 0;
}

constexpr std::uint64_t kShfTls   = 0x400;
constexpr std::uint64_t kTlsFlags = 0x403;   // WRITE|ALLOC|TLS

} // namespace

TEST(ElfExecTls, TwoVarLayoutEmitsPtTlsWithExactVariant2Tpoffs) {
    // The audit HIGH-1/HIGH-2 layout, hand-derived (shipped schema
    // floors: .tdata/.tbss addrAlign 1 — member-driven, gcc parity):
    //   tdata: g {07 00 00 00} align 4 -> offset 0, span 4, maxAlign 4
    //   tbss:  h 4 bytes align 4       -> offset 0, span 4, maxAlign 4
    //   tlsAlign = 4; tbssBlockBase = alignUp(4,4) = 4
    //   blockMemsz = 4+4 = 8; alignedBlockSize = alignUp(8,4) = 8
    //   Variant II tpoffs: g = 0-8 = -8 (F8 FF FF FF LE)
    //                      h = 4-8 = -4 (FC FF FF FF LE)
    //   PT_TLS: filesz=4 (template), memsz=8 (+tbss), align=4
    auto loaded = loadShipped();
    AssembledModule mod = makeTlsDynModule(2);
    Relocation r0; r0.offset = tlsSlotOff(0); r0.target = SymbolId{42};
    r0.kind = RelocationKind{4};                    // tls-tpoff32
    Relocation r1; r1.offset = tlsSlotOff(1); r1.target = SymbolId{43};
    r1.kind = RelocationKind{4};
    mod.functions[0].relocations.push_back(r0);
    mod.functions[0].relocations.push_back(r1);
    mod.dataItems.push_back(makeTdataItem(42, {7, 0, 0, 0}, 4));
    mod.dataItems.push_back(makeTbssItem(43, 4, 4));
    // Control non-TLS mutable global: proves .data still lays out
    // AFTER .tdata with its own correct VA.
    AssembledData ctl;
    ctl.symbol = SymbolId{44}; ctl.section = DataSectionKind::Data;
    ctl.bytes = {9, 0, 0, 0}; ctl.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(ctl));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // e_phnum == 6 (PHDR/INTERP/LOAD/LOAD/DYNAMIC + PT_TLS).
    EXPECT_EQ(readU16LE(bytes, 56), 6u);
    auto const phdrs = readPhdrs(bytes);
    ASSERT_EQ(phdrs.size(), 6u);
    // The header block shifted by exactly one phdr: PT_INTERP body now
    // starts at 64 + 6*56 = 400 (the no-TLS control pins 344).
    ASSERT_EQ(phdrs[1].type, 3u /*PT_INTERP*/);
    EXPECT_EQ(phdrs[1].off, 400u);

    PhdrView const* tls = nullptr;
    PhdrView const* load2 = nullptr;
    for (auto const& p : phdrs) {
        if (p.type == 7u) tls = &p;
        if (p.type == 1u && (p.flags & 0x2u)) load2 = &p;   // R+W LOAD
    }
    ASSERT_NE(tls, nullptr) << "PT_TLS must be present";
    ASSERT_NE(load2, nullptr);
    // .tdata opens PT_LOAD #2 (audit HIGH-2): PT_TLS points at the
    // segment head.
    EXPECT_EQ(tls->off, load2->off);
    EXPECT_EQ(tls->va, load2->va);
    EXPECT_EQ(tls->flags, 4u /*PF_R*/);
    EXPECT_EQ(tls->filesz, 4u);
    EXPECT_EQ(tls->memsz, 8u);
    EXPECT_EQ(tls->align, 4u);
    // ★ .tbss occupies NO PT_LOAD memory: with no .bss present the R+W
    // segment's p_memsz equals its p_filesz exactly (a Bss-style tail
    // for tbss would break this).
    EXPECT_EQ(load2->memsz, load2->filesz)
        << ".tbss must not extend PT_LOAD #2 p_memsz — per-thread "
           "copies are loader-allocated from PT_TLS, not mapped here";

    // .tdata section header: schema-driven SHT_PROGBITS + 0x403.
    std::size_t const tdOff = findShdrByTypeFlags(bytes, 1u, kTlsFlags);
    ASSERT_NE(tdOff, 0u) << ".tdata (PROGBITS, WRITE|ALLOC|TLS) missing";
    EXPECT_EQ(readU64LE(bytes, tdOff + 16), tls->va);       // sh_addr
    EXPECT_EQ(readU64LE(bytes, tdOff + 24), tls->off);      // sh_offset
    EXPECT_EQ(readU64LE(bytes, tdOff + 32), 4u);            // sh_size
    // template bytes == 07 00 00 00.
    std::size_t const tdFile =
        static_cast<std::size_t>(readU64LE(bytes, tdOff + 24));
    EXPECT_EQ(bytes[tdFile + 0], 7u);
    EXPECT_EQ(bytes[tdFile + 1], 0u);
    EXPECT_EQ(bytes[tdFile + 2], 0u);
    EXPECT_EQ(bytes[tdFile + 3], 0u);

    // .tbss: SHT_NOBITS + 0x403; nominal overlap convention
    // sh_addr = tdataVa + tdataSpan; sh_offset = tdata end; sh_size 4.
    std::size_t const tbOff = findShdrByTypeFlags(bytes, 8u, kTlsFlags);
    ASSERT_NE(tbOff, 0u) << ".tbss (NOBITS, WRITE|ALLOC|TLS) missing";
    EXPECT_EQ(readU64LE(bytes, tbOff + 16), tls->va + 4u);
    EXPECT_EQ(readU64LE(bytes, tbOff + 24), tls->off + 4u);
    EXPECT_EQ(readU64LE(bytes, tbOff + 32), 4u);

    // ★ The tpoff pins (HIGH-1 physics, Variant II): the patched
    // disp32s are the hand-derived NEGATIVE offsets, byte-exact.
    std::uint64_t const textFile = 0x1000;   // pageAlign
    std::size_t const g0 = static_cast<std::size_t>(textFile + tlsSlotOff(0));
    EXPECT_EQ(bytes[g0 + 0], 0xF8u);  // -8 LE
    EXPECT_EQ(bytes[g0 + 1], 0xFFu);
    EXPECT_EQ(bytes[g0 + 2], 0xFFu);
    EXPECT_EQ(bytes[g0 + 3], 0xFFu);
    std::size_t const h0 = static_cast<std::size_t>(textFile + tlsSlotOff(1));
    EXPECT_EQ(bytes[h0 + 0], 0xFCu);  // -4 LE
    EXPECT_EQ(bytes[h0 + 1], 0xFFu);
    EXPECT_EQ(bytes[h0 + 2], 0xFFu);
    EXPECT_EQ(bytes[h0 + 3], 0xFFu);

    // The non-TLS control global still lands in a writable .data AFTER
    // the template: dataVa = alignUp(tdataVa + 4, 8) = tdataVa + 8, and
    // its byte is the 9 we stored.
    std::size_t const daOff = findShdrByTypeFlags(bytes, 1u, 0x3u);
    ASSERT_NE(daOff, 0u) << ".data (PROGBITS, WRITE|ALLOC) missing";
    EXPECT_EQ(readU64LE(bytes, daOff + 16), tls->va + 8u);
    std::size_t const daFile =
        static_cast<std::size_t>(readU64LE(bytes, daOff + 24));
    EXPECT_EQ(bytes[daFile], 9u);
}

TEST(ElfExecTls, Alignas32MemberRecomputesTpoffsAndPAlign) {
    // ★ The audit HIGH-1 _Alignas(32) pin — the case that catches the
    // naive "tpoff = offset - memsz" formula (gcc-witnessed: the tp
    // sits at alignUp(memsz, p_align), NOT at raw memsz):
    //   tdata: g {07..} align 4  -> offset 0
    //          a32 {01..} align 32 -> offset alignUp(4,32)=32, span 36
    //          maxAlign 32
    //   tbss:  h 4 bytes align 4 -> tbssBlockBase = alignUp(36,4) = 36
    //   blockMemsz = 36+4 = 40; tlsAlign = 32
    //   alignedBlockSize = alignUp(40,32) = 64        <- THE crux
    //   tpoffs: g = 0-64 = -64 (C0 FF FF FF)
    //           a32 = 32-64 = -32 (E0 FF FF FF)
    //           h = 36-64 = -28 (E4 FF FF FF)
    //   PT_TLS: filesz=36, memsz=40, align=32
    auto loaded = loadShipped();
    AssembledModule mod = makeTlsDynModule(3);
    struct Slot { std::size_t i; std::uint32_t sym; };
    for (Slot s : {Slot{0, 42}, Slot{1, 45}, Slot{2, 43}}) {
        Relocation r;
        r.offset = static_cast<std::uint32_t>(tlsSlotOff(s.i));
        r.target = SymbolId{s.sym};
        r.kind   = RelocationKind{4};
        mod.functions[0].relocations.push_back(r);
    }
    mod.dataItems.push_back(makeTdataItem(42, {7, 0, 0, 0}, 4));
    mod.dataItems.push_back(makeTdataItem(45, {1, 0, 0, 0}, 32));
    mod.dataItems.push_back(makeTbssItem(43, 4, 4));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    auto const phdrs = readPhdrs(bytes);
    PhdrView const* tls = nullptr;
    for (auto const& p : phdrs) if (p.type == 7u) tls = &p;
    ASSERT_NE(tls, nullptr);
    EXPECT_EQ(tls->filesz, 36u);
    EXPECT_EQ(tls->memsz, 40u);
    EXPECT_EQ(tls->align, 32u);

    std::uint64_t const textFile = 0x1000;
    auto expect4 = [&](std::size_t slot, std::uint8_t b0, std::uint8_t b1,
                       std::uint8_t b2, std::uint8_t b3) {
        std::size_t const o =
            static_cast<std::size_t>(textFile + tlsSlotOff(slot));
        EXPECT_EQ(bytes[o + 0], b0) << "slot " << slot;
        EXPECT_EQ(bytes[o + 1], b1) << "slot " << slot;
        EXPECT_EQ(bytes[o + 2], b2) << "slot " << slot;
        EXPECT_EQ(bytes[o + 3], b3) << "slot " << slot;
    };
    expect4(0, 0xC0, 0xFF, 0xFF, 0xFF);   // g   = -64
    expect4(1, 0xE0, 0xFF, 0xFF, 0xFF);   // a32 = -32
    expect4(2, 0xE4, 0xFF, 0xFF, 0xFF);   // h   = -28
}

TEST(ElfExecTls, NoTlsModuleByteIdenticalToPreTlsShape) {
    // ★ The byte-identity control (HIGH-2 / sqlite-dormant guarantee):
    // the SAME module minus the TLS items must show ZERO trace of the
    // TLS machinery — 5 phdrs (PT_INTERP body back at 64+5*56=344), no
    // PT_TLS, no SHF_TLS section header. Every TLS emission is gated on
    // hasTls, so a no-TLS image is byte-identical to the pre-TLS
    // walker's output (sqlite's images must not move).
    auto loaded = loadShipped();
    AssembledModule mod = makeTlsDynModule(0);
    AssembledData ctl;
    ctl.symbol = SymbolId{44}; ctl.section = DataSectionKind::Data;
    ctl.bytes = {9, 0, 0, 0}; ctl.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(ctl));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    EXPECT_EQ(readU16LE(bytes, 56), 5u) << "no PT_TLS slot without TLS items";
    auto const phdrs = readPhdrs(bytes);
    // ★ FATAL size guard, mirroring the TLS sibling this control pairs with
    // (`ElfExecTls.TwoVarLayoutEmitsPtTlsWithExactVariant2Tpoffs`, which
    // asserts `phdrs.size() == 6u` before touching `phdrs[1]` — cited by test
    // NAME, not line, because the numbers drift and the name does not).
    // `readPhdrs` returns exactly e_phnum entries, and the ONLY check on
    // e_phnum here is the NON-FATAL EXPECT above — so a regression that emits
    // 0 or 1 phdrs would fall straight through it into `phdrs[1]`, an
    // out-of-range index on a std::vector.
    // ✔MEASURED 2026-08-05 (TF-C120): that class — a non-fatal
    // guard in front of an unchecked index — segfaulted
    // `tests/link/test_pe_writer.cpp` after 83 of 97 tests, hiding 14 tests
    // (one of them, `LinkerExternResolution.OkFalseWhenWalkerFailsLoud`,
    // genuinely broken) with no report that they had not run. 5u restates the
    // count already pinned on the line above; it adds no new expectation.
    ASSERT_EQ(phdrs.size(), 5u);
    ASSERT_EQ(phdrs[1].type, 3u);
    EXPECT_EQ(phdrs[1].off, 344u) << "layout must NOT shift without TLS";
    for (auto const& p : phdrs) EXPECT_NE(p.type, 7u);
    std::uint64_t const shoff = readU64LE(bytes, 40);
    std::uint16_t const shnum = readU16LE(bytes, 60);
    for (std::uint16_t i = 1; i < shnum; ++i) {
        std::size_t const off = static_cast<std::size_t>(shoff)
                              + static_cast<std::size_t>(i) * kShdrSize;
        EXPECT_EQ(readU64LE(bytes, off + 8) & kShfTls, 0u)
            << "no SHF_TLS section may exist in a no-TLS image";
    }
}

TEST(ElfExecTls, Crit1DataRelocTargetingTlsSymbolFailsLoud) {
    // ★ CRIT-1(a): `int *p = &tls_var;` at the walker tier — a data
    // slot cannot hold a thread-local "address" (C11 6.6p9; the
    // semantic tier's 0xE048 sibling). Without the backstop the abs64
    // patch would embed the bit-cast NEGATIVE tpoff as a pointer.
    auto loaded = loadShipped();
    AssembledModule mod = makeTlsDynModule(1);
    Relocation r0; r0.offset = static_cast<std::uint32_t>(tlsSlotOff(0));
    r0.target = SymbolId{42};
    r0.kind = RelocationKind{4};
    mod.functions[0].relocations.push_back(r0);
    mod.dataItems.push_back(makeTdataItem(42, {7, 0, 0, 0}, 4));
    AssembledData p;
    p.symbol = SymbolId{50}; p.section = DataSectionKind::Data;
    p.bytes.assign(8, 0); p.alignment = Alignment::of<8>();
    Relocation pr; pr.offset = 0; pr.target = SymbolId{42};
    pr.kind = RelocationKind{2};                     // abs64
    p.relocations.push_back(pr);
    mod.dataItems.push_back(std::move(p));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch
            && d.actual.find("CRIT-1") != std::string::npos
            && d.actual.find("THREAD-LOCAL") != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw) << "a data reloc against a TLS symbol must fail "
                        "loud, never patch a garbage pointer";
}

TEST(ElfExecTls, Crit1NonTlsFunctionRelocTargetingTlsSymbolFailsLoud) {
    // ★ CRIT-1(b) direction 1: a NON-tls-flagged function reloc (rel32)
    // against a TLS symbol would embed the bit-cast tpoff as an address.
    auto loaded = loadShipped();
    AssembledModule mod = makeTlsDynModule(1);
    Relocation r0; r0.offset = static_cast<std::uint32_t>(tlsSlotOff(0));
    r0.target = SymbolId{42};
    r0.kind = RelocationKind{1};                     // rel32 — WRONG class
    mod.functions[0].relocations.push_back(r0);
    mod.dataItems.push_back(makeTdataItem(42, {7, 0, 0, 0}, 4));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch
            && d.actual.find("CRIT-1") != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
}

TEST(ElfExecTls, Crit1TlsRelocTargetingNonTlsSymbolFailsLoud) {
    // ★ CRIT-1(b) direction 2: a tls-tpoff32 reloc against a NON-TLS
    // symbol would embed an absolute VA as a thread-pointer offset.
    auto loaded = loadShipped();
    AssembledModule mod = makeTlsDynModule(1);
    Relocation r0; r0.offset = static_cast<std::uint32_t>(tlsSlotOff(0));
    r0.target = SymbolId{44};
    r0.kind = RelocationKind{4};                     // tls kind, non-TLS target
    mod.functions[0].relocations.push_back(r0);
    mod.dataItems.push_back(makeTdataItem(42, {7, 0, 0, 0}, 4));
    AssembledData ctl;
    ctl.symbol = SymbolId{44}; ctl.section = DataSectionKind::Data;
    ctl.bytes = {9, 0, 0, 0}; ctl.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(ctl));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch
            && d.actual.find("CRIT-1") != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
}

TEST(ElfExecTls, TdataTemplateSlotPatchedWithRodataTargetVa) {
    // ★ CRIT-2 witness: `thread_local char *msg = "hi";` — the .tdata
    // TEMPLATE slot is patched at link time with the rodata target's
    // ABSOLUTE VA (sound for fixed-base ET_EXEC: every thread's copy
    // starts from the patched template). A demotion to .data (the
    // pre-fold behavior) or an unpatched slot flips this red.
    auto loaded = loadShipped();
    AssembledModule mod = makeTlsDynModule(0);
    AssembledData ro;
    ro.symbol = SymbolId{60}; ro.section = DataSectionKind::Rodata;
    ro.bytes = {0x68, 0x69, 0}; ro.alignment = Alignment::of<1>();   // "hi"
    mod.dataItems.push_back(std::move(ro));
    AssembledData slot;
    slot.symbol = SymbolId{61}; slot.section = DataSectionKind::Tdata;
    slot.bytes.assign(8, 0); slot.alignment = Alignment::of<8>();
    Relocation sr; sr.offset = 0; sr.target = SymbolId{60};
    sr.kind = RelocationKind{2};                     // abs64
    slot.relocations.push_back(sr);
    mod.dataItems.push_back(std::move(slot));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "a reloc-bearing .tdata template must ENCODE (CRIT-2), not "
           "reject or demote";
    ASSERT_FALSE(bytes.empty());

    // rodata VA from its header — PROGBITS + SHF_ALLOC + sh_size 3
    // (the "hi\0" bytes). The size discriminant matters: `.interp` is
    // ALSO PROGBITS + SHF_ALLOC, so a type+flags-only scan finds it
    // first (0x40xxxx — the initial red run of this pin caught that).
    std::uint64_t roVa = 0;
    {
        std::uint64_t const shoff = readU64LE(bytes, 40);
        std::uint16_t const shnum = readU16LE(bytes, 60);
        for (std::uint16_t i = 1; i < shnum; ++i) {
            std::size_t const off = static_cast<std::size_t>(shoff)
                                  + static_cast<std::size_t>(i) * kShdrSize;
            if (readU32LE(bytes, off + 4) == 1u
                && readU64LE(bytes, off + 8) == 0x2u
                && readU64LE(bytes, off + 32) == 3u) {
                roVa = readU64LE(bytes, off + 16);
                break;
            }
        }
    }
    ASSERT_NE(roVa, 0u) << ".rodata (PROGBITS, ALLOC, 3 bytes) missing";
    // .tdata header + its 8 template bytes == the rodata VA.
    std::size_t const tdOff = findShdrByTypeFlags(bytes, 1u, kTlsFlags);
    ASSERT_NE(tdOff, 0u) << "the slot must stay in .tdata (thread-local "
                            "identity preserved), never demoted to .data";
    std::size_t const tdFile =
        static_cast<std::size_t>(readU64LE(bytes, tdOff + 24));
    EXPECT_EQ(readU64LE(bytes, tdFile), roVa)
        << "template slot must hold the patched target VA";
}

TEST(ElfExecTls, AddTlsSymbolOffsetsVariant1FormulaPinnedNow) {
    // ★ The Variant-I (arm64) formula arm, config-keyed and pinned NOW
    // (TLS C2 only supplies the arm64 `tls` identity block — zero code):
    //   tpoff = alignUp(tcbHeaderBytes, tlsAlignment) + templateOffset
    // gcc-witnessed HIGH-1(c): alignUp(16, 32) = 32, NOT the naive 16.
    std::vector<AssembledData> items;
    items.push_back(makeTdataItem(42, {7, 0, 0, 0}, 4));
    DiagnosticReporter rep;
    auto layoutOpt = link::format::buildExecDataSection(
        items, DataSectionKind::Tdata, /*floor=*/1, "tls-test", rep);
    ASSERT_TRUE(layoutOpt.has_value());

    TlsIdentity v1{TlsVariant::Variant1, /*tcbHeaderBytes=*/16};
    {
        std::unordered_map<SymbolId, std::uint64_t> symbolVa;
        std::unordered_set<SymbolId> tlsSymbols;
        ASSERT_TRUE(link::format::addTlsSymbolOffsets(
            items, *layoutOpt, /*blockBaseOffset=*/0,
            /*alignedBlockSize=*/4, /*tlsAlignment=*/4, v1,
            symbolVa, tlsSymbols, "tls-test", rep));
        ASSERT_TRUE(symbolVa.contains(SymbolId{42}));
        EXPECT_EQ(symbolVa[SymbolId{42}], 16u)
            << "Variant1 @align4: alignUp(16,4)+0 == 16";
        EXPECT_TRUE(tlsSymbols.contains(SymbolId{42}));
    }
    {
        std::unordered_map<SymbolId, std::uint64_t> symbolVa;
        std::unordered_set<SymbolId> tlsSymbols;
        ASSERT_TRUE(link::format::addTlsSymbolOffsets(
            items, *layoutOpt, 0, /*alignedBlockSize=*/64,
            /*tlsAlignment=*/32, v1, symbolVa, tlsSymbols,
            "tls-test", rep));
        EXPECT_EQ(symbolVa[SymbolId{42}], 32u)
            << "Variant1 @align32: alignUp(16,32)+0 == 32 (HIGH-1(c) — "
               "the naive 16+offset is WRONG under a 32-aligned block)";
    }
    {
        // Variant II bit-cast exactness: u64(-64) round-trips.
        TlsIdentity v2{TlsVariant::Variant2, 0};
        std::unordered_map<SymbolId, std::uint64_t> symbolVa;
        std::unordered_set<SymbolId> tlsSymbols;
        ASSERT_TRUE(link::format::addTlsSymbolOffsets(
            items, *layoutOpt, 0, /*alignedBlockSize=*/64,
            /*tlsAlignment=*/32, v2, symbolVa, tlsSymbols,
            "tls-test", rep));
        EXPECT_EQ(symbolVa[SymbolId{42}], 0xFFFFFFFFFFFFFFC0ull)
            << "Variant2: int64(0-64) bit-cast to u64";
    }
}

TEST(ElfExecTls, StaticElfArmRejectsTlsItemsLoud) {
    // Audit LOW-b: the STATIC (externless) ELF arm has no PT_TLS
    // emission — a Tdata item reaching it must fail 0x8015, never lay
    // out as a process-shared alias. (Unreachable via the shipped
    // pipeline — every DSS ELF exe imports libc exit and routes to the
    // dynamic arm — this is the hand-built/premature-opt-in belt.)
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);   // NO externs
    mod.dataItems.push_back(makeTdataItem(42, {7, 0, 0, 0}, 4));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksThreadLocalSupport
            && d.actual.find("static ELF arm") != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw) << "static-arm TLS items must reject "
                        "K_FormatLacksThreadLocalSupport (0x8015)";
}

// ── TF-C124: a version READ from a real library round-trips to verneed ───
// D-FFI-BINARY-READER-SURFACES-NO-SYMBOL-VERSION.
//
// The three c156 pins above hand the writer a version string this test file
// typed. That proves the EMITTER, and it is exactly the shape that cannot
// notice the defect this cycle fixes: the string had to be typed, because
// until now nothing in the compiler could produce one from a library that has
// no shipped descriptor — and no third-party library the project links (Tcl,
// zlib) has one.
//
// So this arm types no version at all. It reads a REAL GNU ld 2.42 shared
// library (`tests/ffi/data/libdssver.so.1`) with the SHIPPED FF1 reader, takes
// whatever version that library records for its own export, and follows it
// through the writer into `.gnu.version_r`. Every version string asserted
// below is compared against the one the READER produced; the literals are a
// second, independent statement of the same fact, so a reader that silently
// returned "" could not make this test pass.
//
// ⚠ The read-back is a verneed decode here rather than another
// `readImportsFromBytes` call, and that asymmetry is real, not laziness: what
// a DSS image emits is a REQUIREMENT, carried on an SHN_UNDEF `.dynsym` row,
// and `readElf64` deliberately surfaces only DEFINITIONS (an export surface
// must not report a library's own imports —
// D-LK-ELF-EMITS-ONE-DT-NEEDED-WHEN-TWO-LIBRARIES-ARE-REFERENCED). The two
// halves speak different sections by design: `.gnu.version_d` in, verneed out.
TEST(ElfExecWriter, VersionReadFromRealLibraryRoundTripsIntoVerneed) {
    auto const libPath = dss::test::repoRoot()
                       / "tests" / "ffi" / "data" / "libdssver.so.1";
    std::ifstream in(libPath, std::ios::binary);
    ASSERT_TRUE(in.good()) << "missing real-linker fixture " << libPath.string();
    std::vector<std::uint8_t> const libBytes(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(libBytes.empty());

    // (1) READ — the shipped FF1 reader, on the real file.
    DiagnosticReporter readRep;
    auto rows = dss::ffi::readImportsFromBytes(libBytes, "libdssver.so.1", readRep);
    ASSERT_TRUE(rows.has_value());
    dss::ffi::ImportSurface const* row = nullptr;
    for (auto const& r : *rows) {
        if (r.mangledName == "dss_ver_default") { row = &r; break; }
    }
    ASSERT_NE(row, nullptr);
    ASSERT_TRUE(row->elfSymbolVersion.has_value())
        << "the reader recovered no version — nothing downstream can invent one";
    std::string const observedVersion = row->elfSymbolVersion->name;
    std::string const observedLibrary = row->soname;
    EXPECT_EQ(observedVersion, "DSSVER_2.0");      // independent cross-check
    EXPECT_EQ(observedLibrary, "libdssver.so.1");  // independent cross-check

    // (2) EMIT — the observed strings, never a typed one, drive the writer.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99}; rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    ExternImport imp{SymbolId{99}, row->mangledName, observedLibrary};
    imp.version = observedVersion;
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // (3) READ BACK — recover the version STRING out of the emitted image.
    auto const dyn = dynEntries(bytes);
    ASSERT_TRUE(dyn.count(0x6ffffff0u)) << "no DT_VERSYM";
    ASSERT_TRUE(dyn.count(0x6ffffffeu)) << "no DT_VERNEED";
    EXPECT_EQ(dyn.at(0x6fffffffu), 1u) << "exactly one Verneed (one library)";

    int const vrIdx = findSectionByName(bytes, ".gnu.version_r");
    ASSERT_GE(vrIdx, 0);
    int const dynstrIdx = findSectionByName(bytes, ".dynstr");
    ASSERT_GE(dynstrIdx, 0);
    std::uint64_t const vrOff     = shOffset(bytes, vrIdx);
    std::uint64_t const dynstrOff = shOffset(bytes, dynstrIdx);
    // Elf64_Verneed: vn_file names the library; Elf64_Vernaux at +vn_aux
    // names the required version.
    std::string const emittedLibrary =
        readCStr(bytes, dynstrOff + readU32LE(bytes, vrOff + 4));
    std::uint64_t const aux = vrOff + readU32LE(bytes, vrOff + 8);
    std::string const emittedVersion =
        readCStr(bytes, dynstrOff + readU32LE(bytes, aux + 8));

    // ★ The round-trip assertion: what came out is what the library said,
    // compared to the READER's own output rather than to a literal.
    EXPECT_EQ(emittedVersion, observedVersion);
    EXPECT_EQ(emittedLibrary, observedLibrary);
    EXPECT_EQ(readU32LE(bytes, aux + 0), testVerHash(observedVersion))
        << "vna_hash must be elf_hash of the version the library recorded";

    // And the import's own versym slot points AT that requirement (index >= 2),
    // which is what makes ld.so bind it rather than the library's default.
    int const vsymIdx = findSectionByName(bytes, ".gnu.version");
    ASSERT_GE(vsymIdx, 0);
    std::uint16_t const vnaOther = readU16LE(bytes, aux + 6);
    EXPECT_GE(vnaOther, 2u);
    EXPECT_EQ(readU16LE(bytes, shOffset(bytes, vsymIdx) + 2), vnaOther);
}

// ── D-LINK-ELF-EXEC-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS ──────────────────
//
// THE red-on-disable pin: a FINAL IMAGE's `.symtab` must carry each function's
// DECLARED name, and must keep `sym_<id>` for exactly the symbols that have no
// declared name.
//
// ✔MEASURED, the defect this closes: gdb over a DSS-built ELF exec printed
//   #0 sym_84 / #1 sym_89 / #2 sym_93
// where the source says `static_helper` / `global_helper` / `main`, on BOTH
// x86_64 and aarch64 — beside a CORRECT `.eh_frame`. The name column was the
// only wrong part of the image: every st_value/st_size/st_info byte, and every
// instruction, is unchanged by the fix (the same run after it reports the same
// three addresses, now named).
//
// ★ WHY THIS LOOPS OVER BOTH IMAGE ARMS AND NOT JUST BOTH PORTS. The ELF walker
// has TWO image symtab builders and BOTH hardcoded `sym_<id>`:
// `encodeElfExecDynamic` (any module carrying an extern — which, because every
// shipped exec/PIE format spells `processExit` as `by-name-import` for `exit`,
// is EVERY REAL EXECUTABLE) and the shared static-ET_EXEC arm's `emitFuncSym`
// (reachable with zero externs). Pinning one arm would have left the other free
// to rot, and the LIVE one is the dynamic arm — the one a "minimal ET_EXEC"
// test would never have reached.
//
// ★ EVERY CELL RUNS THROUGH A `void` CALLABLE so a failed ASSERT returns from
// the BODY instead of aborting the whole matrix. A sibling cycle measured
// exactly this vacuity: a `for (port : {x86, arm})` loop with a bare ASSERT
// aborted at the x86_64 half and NEVER RAN the aarch64 half, where the mutant
// failed silently — the safe arm masked the dangerous one.
namespace {

// Every `.symtab` name in an emitted image, in emission order. Returned as a
// LIST and asserted by membership so the pin survives a legitimate reordering of
// the symtab (which is not what it exists to police) while still failing on a
// changed SPELLING (which is).
[[nodiscard]] std::vector<std::string>
imageSymtabNames(std::vector<std::uint8_t> const& bytes) {
    std::vector<std::string> out;
    int const symtabIdx = findSectionByName(bytes, ".symtab");
    int const strtabIdx = findSectionByName(bytes, ".strtab");
    if (symtabIdx < 0 || strtabIdx < 0) return out;
    std::uint64_t const shoff  = readU64LE(bytes, 40);
    std::uint64_t const symOff = readU64LE(bytes, shoff + symtabIdx * 64 + 24);
    std::uint64_t const symSz  = readU64LE(bytes, shoff + symtabIdx * 64 + 32);
    std::uint64_t const strOff = readU64LE(bytes, shoff + strtabIdx * 64 + 24);
    for (std::uint64_t i = 0; i < symSz / 24; ++i) {
        out.push_back(
            readCStr(bytes, strOff + readU32LE(bytes, symOff + i * 24 + 0)));
    }
    return out;
}

struct ImagePortSpec {
    char const*               label;
    char const*               targetName;
    char const*               formatName;
    std::vector<std::uint8_t> retBytes;    // a lone return
    std::vector<std::uint8_t> callBytes;   // call-extern + return
    std::uint32_t             callOffset;  // the reloc patch site
};

} // namespace

TEST(ElfImageSymbolNames,
     ImageSymtabNamesEveryDeclaredFunctionOnBothPortsAndBothImageArms) {
    // x86_64: `call rel32` (operand at +1) + RET.
    // arm64:  `BL #0` (patch site at +0, 4-byte aligned) + RET.
    // These byte strings are the ONLY per-port data here; every assertion below
    // is identical for both, because the naming decision is made in
    // format-neutral substrate (`ObjectSymbolNames::imageName`) that no target
    // or format can reach into.
    std::vector<ImagePortSpec> const ports{
        {"x86_64", "x86_64", "elf64-x86_64-linux-exec",
         {0xC3},
         {0xE8, 0, 0, 0, 0, 0xC3},
         1},
        {"arm64", "arm64", "elf64-aarch64-linux-exec",
         {0xC0, 0x03, 0x5F, 0xD6},
         {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6},
         0},
    };

    // ONE cell = one (port, image arm) pair. `void` on purpose — see above.
    auto runCell = [](ImagePortSpec const& port, bool dynamicArm) -> void {
        std::string const label =
            std::string{port.label} + (dynamicArm ? " [dynamic image arm]"
                                                  : " [static ET_EXEC arm]");
        auto target = TargetSchema::loadShipped(port.targetName);
        ASSERT_TRUE(target.has_value()) << label;
        auto fmt = ObjectFormatSchema::loadShipped(port.formatName);
        ASSERT_TRUE(fmt.has_value()) << label;

        AssembledModule mod;
        mod.expectedFuncCount = 3;

        // fn #7 — a `static` (Local binding) function WITH a declared name. THE
        // discriminating case: an image wants this name (it is a backtrace
        // frame), while a relocatable `.o` must NOT expose it (a foreign linker
        // keys by name, and two TUs' `helper`s would collide — D-LK-INTERNAL-
        // LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION). Pre-fix: `sym_7`.
        AssembledFunction f7;
        f7.symbol = SymbolId{7};
        f7.bytes  = dynamicArm ? port.callBytes : port.retBytes;
        if (dynamicArm) {
            Relocation rel;
            rel.offset = port.callOffset;
            rel.target = SymbolId{99};
            rel.kind   = RelocationKind{1};   // call rel32 / call26
            f7.relocations.push_back(rel);
        }
        mod.functions.push_back(std::move(f7));

        // fn #8 — an externally-visible function with a declared name.
        AssembledFunction f8;
        f8.symbol = SymbolId{8};
        f8.bytes  = port.retBytes;
        mod.functions.push_back(std::move(f8));

        // fn #9 — NO `ModuleSymbol` row at all: the shape of the linker-injected
        // `_start` trampoline (minted SymbolId, no declared name). This one MUST
        // keep `sym_9`; the fallback is the deliberate answer for a symbol that
        // genuinely has no name, not an oversight.
        AssembledFunction f9;
        f9.symbol = SymbolId{9};
        f9.bytes  = port.retBytes;
        mod.functions.push_back(std::move(f9));

        mod.symbols.push_back(ModuleSymbol{SymbolId{7}, "img_static_fn",
                                           SymbolBinding::Local,
                                           SymbolVisibility::Default});
        mod.symbols.push_back(ModuleSymbol{SymbolId{8}, "img_global_fn",
                                           SymbolBinding::Global,
                                           SymbolVisibility::Default});

        if (dynamicArm) {
            mod.externImports.push_back(
                ExternImport{SymbolId{99}, "printf", "libc.so.6"});
        }

        DiagnosticReporter rep;
        auto bytes = encodeUntrampolined(mod, **target, **fmt, rep);
        std::string diags;
        for (auto const& d : rep.all()) diags += d.actual + "\n";
        ASSERT_EQ(rep.errorCount(), 0u) << label << "\n" << diags;
        ASSERT_FALSE(bytes.empty()) << label << "\n" << diags;

        // The DYNAMIC cell must really reach the dynamic builder and the STATIC
        // cell must really not — otherwise both cells could be silently
        // exercising ONE builder and half the matrix would assert nothing about
        // the arm it claims to cover.
        EXPECT_EQ(findSectionByName(bytes, ".dynsym") >= 0, dynamicArm)
            << label
            << ": this cell did not reach the image arm it names, so its result "
               "says nothing about that arm";

        auto const names = imageSymtabNames(bytes);
        ASSERT_FALSE(names.empty()) << label << ": no `.symtab` names read";
        auto has = [&](std::string_view want) {
            return std::find(names.begin(), names.end(), want) != names.end();
        };

        // THE FIX, both directions: the declared names are PRESENT...
        EXPECT_TRUE(has("img_static_fn"))
            << label
            << ": a `static` function must appear under its DECLARED name in a "
               "final image — this is the frame a debugger prints";
        EXPECT_TRUE(has("img_global_fn"))
            << label << ": an externally-visible function must keep its name";
        // ...and the pre-fix synthetic spellings are GONE. These two are the
        // red-on-disable discriminators: restore either hardcoded `"sym_" + id`
        // and they fail, alongside the EXPECT_TRUEs above.
        EXPECT_FALSE(has("sym_7"))
            << label
            << ": `sym_7` is the PRE-FIX spelling of `img_static_fn`; emitting "
               "it means the image threw the declared name away again";
        EXPECT_FALSE(has("sym_8"))
            << label << ": `sym_8` is the PRE-FIX spelling of `img_global_fn`";

        // The deliberate fallback survives, and is asserted rather than assumed:
        // if this ever fails, a nameless symbol has acquired a name from
        // somewhere it should not have.
        EXPECT_TRUE(has("sym_9"))
            << label
            << ": a symbol with NO declared name (the injected entry "
               "trampoline's shape) must keep the `sym_<id>` fallback";
    };

    for (auto const& port : ports) {
        runCell(port, /*dynamicArm=*/false);
        runCell(port, /*dynamicArm=*/true);
    }
}
