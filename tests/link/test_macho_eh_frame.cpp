// D-UNWIND-NO-EH-FRAME-ANY-LANGUAGE-ON-ELF-OR-MACHO — the Mach-O half.
//
// `dwarf_cfi.hpp` has been a complete, tested CIE/FDE encoder wired into
// NO writer, so a DSS Mach-O image carried no unwind table at all and
// nothing — debugger, profiler, `throw` — could walk a DSS stack frame.
// This file pins the writer half: the Mach-O EXEC/dynamic arm emits
// `__TEXT,__eh_frame` when any function carries CFI, and emits NO section
// when none does.
//
// ── WHAT IS PINNED, AND WHY EACH ONE ──
//
//  * The section EXISTS with non-zero size, found by PARSING the emitted
//    load commands (never by eyeballing a hex dump).
//  * `__TEXT.nsects` equals the number of `section_64` records its own
//    `cmdsize` reserves. That is exactly the invariant a HAND-MAINTAINED
//    section count breaks: the count and the reserved size were two
//    independent literals, and the next section added would have had to
//    remember both.
//  * The section's file extent lies inside `__TEXT`'s `fileoff`/`filesize`
//    — a section mapped outside its own segment is bytes dyld never loads.
//  * ★ Every FDE's `initial_location` resolves to the VA of the function
//    it describes. This is the assertion that earns the file: a
//    mis-derived section VA does not fail to load and does not crash —
//    it produces a table whose every entry points a FIXED DISTANCE away
//    from the right function, which an unwinder follows without complaint
//    into a plausible-looking wrong backtrace.
//  * The CIE's `return_address_register` is the number the TARGET SCHEMA
//    declares (16 on x86_64 SysV — a synthetic column that is not a
//    register), and a register rule names the register's DWARF number,
//    not its hardware encoding. On x86_64 those are different
//    permutations, so this discriminates; on AArch64 they coincide and it
//    would not.
//  * A module whose functions carry NO `cfi` encodes successfully and
//    emits NO `__eh_frame` — an empty section would advertise an unwind
//    table describing nothing.
//  * `__DATA_CONST,__got`'s VA still satisfies the __TEXT-relative
//    congruence identity after a section was appended to __TEXT
//    — extern DATA imports bind their `symbolVa` to the EARLY chain, so
//    a section that slid __got without extending that chain would point
//    every data import a page away: a wrong-address read, not a link
//    error.
//  * A target that declares NO DWARF register numbering still encodes a
//    module that carries no CFI, and still REFUSES one that does. The
//    unwind table's config must not be a precondition for linking at all.
//
// The fixture is x86_64-darwin deliberately: the DWARF numbering is a
// genuine PERMUTATION of the hardware encoding there (rsp is DWARF 7 /
// hardware 4; the return address is column 16, which is no register at
// all), so a writer that reached for `hwEncoding` fails here. It keeps
// the test host-buildable while the arm64 corpus carries the runnable
// end-to-end proof.

#include "core/types/cfi.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/macho.hpp"
#include "link/object_format_schema.hpp"
#include "macho_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::macho::test::findSection;
using dss::macho::test::findSegment;

namespace {

[[nodiscard]] std::uint32_t rd32(std::vector<std::uint8_t> const& b,
                                 std::size_t off) {
    return dss::macho::test::readU32LE(std::span<std::uint8_t const>{b}, off);
}
[[nodiscard]] std::uint64_t rd64(std::vector<std::uint8_t> const& b,
                                 std::size_t off) {
    return dss::macho::test::readU64LE(std::span<std::uint8_t const>{b}, off);
}
[[nodiscard]] std::int32_t rdS32(std::vector<std::uint8_t> const& b,
                                 std::size_t off) {
    return static_cast<std::int32_t>(rd32(b, off));
}

// The physical register ordinal the target itself assigns to `name`.
// READ from the loaded schema rather than typed here, so the fixture
// cannot drift from the shipped register table.
[[nodiscard]] std::optional<std::uint16_t>
ordinalOf(TargetSchema const& t, std::string_view name) {
    auto const regs = t.registers();
    for (std::size_t i = 0; i < regs.size(); ++i) {
        if (regs[i].name == name) return static_cast<std::uint16_t>(i);
    }
    return std::nullopt;
}

// An x86_64-darwin MH_EXECUTE schema on the DYNAMIC path (an extern
// import routes the module through `encodeExecDynamic`, the arm that
// produces real executables). Entry cluster copied from the shipped
// macho64-x86_64-darwin-exec format — an exec-flavored schema is
// REJECTED at load without it.
[[nodiscard]] std::string execSchemaJson() {
    return R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
      "cCallingConvention": { "convention": "sysv_amd64" },
      "outputExtension": ".dylib",
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-ehframe-test","kind":"macho"},
      "entryPoint": "",
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute",
                 "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"]
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,
         "flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ],
      "relocations":[
        {"name":"X86_64_RELOC_BRANCH","kind":1,"nativeId":369098752},
        {"name":"X86_64_RELOC_UNSIGNED_8","kind":2,"nativeId":100663296},
        {"name":"X86_64_RELOC_UNSIGNED_4","kind":3,"nativeId":33554432}
      ]
    })";
}

// Two functions of known length, plus one extern (the dynamic arm's
// gate). `withCfi` decides whether function #1 carries call-frame
// information; function #0 NEVER does — it stands in for the
// linker-synthesized entry trampoline, the outermost frame that
// legitimately has no FDE (gcc/clang emit none for it either), and it
// makes the FDE↔function binding non-trivial: an off-by-one in the
// patch loop would describe function #0.
constexpr std::size_t kFn0Len = 0x10;
constexpr std::size_t kFn1Len = 0x24;

[[nodiscard]] AssembledModule makeModule(TargetSchema const& target,
                                         bool withCfi) {
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    AssembledFunction fn0;
    fn0.symbol = SymbolId{1};
    fn0.bytes.assign(kFn0Len, 0x90);
    mod.functions.push_back(std::move(fn0));

    AssembledFunction fn1;
    fn1.symbol = SymbolId{2};
    fn1.bytes.assign(kFn1Len, 0x90);
    if (withCfi) {
        CfiFunction cfi;
        cfi.codeLength = static_cast<std::uint32_t>(kFn1Len);
        // Entry state, as the calling convention states it: the CFA is
        // SP + 8 (the bytes the CALL pushed) and the return address sits
        // at CFA-8.
        cfi.initial.cfaRegister = *ordinalOf(target, "rsp");
        cfi.initial.cfaOffset   = 8;
        cfi.initial.returnAddressAtCfaOffset = -8;
        // `sub rsp,0x20` at +7, then `mov [rsp],rbp` at +11. ★ `rbp` is
        // chosen because its DWARF number (6) and hardware encoding (5)
        // DIFFER — the assertions below refuse to run on a register where
        // the two coincide (`rbx` is 3 and 3), which would make the pin
        // pass for a writer that emitted the wrong one.
        cfi.ops.push_back(CfiOp{7, CfiOpKind::DefCfaOffset, CfiRegRef{},
                                CfiRegRef{}, 40});
        cfi.ops.push_back(CfiOp{11, CfiOpKind::RegAtCfaOffset,
                                CfiRegRef::physical(*ordinalOf(target, "rbp")),
                                CfiRegRef{}, -40});
        fn1.cfi = std::move(cfi);
    }
    mod.functions.push_back(std::move(fn1));

    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf", "/usr/lib/libSystem.B.dylib"});
    // The schema declares `processExit`, which contracts that the image
    // entry is the `_start` trampoline only `linker::link` injects. These
    // tests call `macho::encode` DIRECTLY, so no trampoline exists — state
    // that the untrampolined entry is functions[0].
    mod.imageEntryOverride = 0u;
    return mod;
}

struct Encoded {
    std::vector<std::uint8_t> bytes;
    std::uint64_t textSectionVa = 0;  // __text's VA (schema constant)
};

[[nodiscard]] Encoded encodeWith(TargetSchema const& target,
                                 ObjectFormatSchema const& fmt, bool withCfi) {
    AssembledModule mod = makeModule(target, withCfi);
    DiagnosticReporter rep;
    Encoded out;
    out.bytes = macho::encode(mod, target, fmt, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    auto const* sec = fmt.sectionByKind(SectionKind::Text);
    if (sec != nullptr) out.textSectionVa = sec->virtualAddress;
    return out;
}

// ── ULEB/SLEB readers for the CIE header fields ──
[[nodiscard]] std::uint64_t readULeb(std::vector<std::uint8_t> const& b,
                                     std::size_t& p) {
    std::uint64_t v = 0;
    unsigned shift = 0;
    while (true) {
        std::uint8_t const byte = b[p++];
        v |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) break;
        shift += 7;
    }
    return v;
}
[[nodiscard]] std::int64_t readSLeb(std::vector<std::uint8_t> const& b,
                                    std::size_t& p) {
    std::int64_t v = 0;
    unsigned shift = 0;
    std::uint8_t byte = 0;
    do {
        byte = b[p++];
        v |= static_cast<std::int64_t>(byte & 0x7Fu) << shift;
        shift += 7;
    } while ((byte & 0x80u) != 0);
    if (shift < 64 && (byte & 0x40u) != 0)
        v |= -(static_cast<std::int64_t>(1) << shift);
    return v;
}

// One decoded FDE: the VA it claims to describe and how far it reaches.
struct Fde {
    std::uint64_t initialLocationVa = 0;
    std::uint32_t addressRange      = 0;
    std::vector<std::uint8_t> ops;
};

struct EhChain {
    std::uint64_t             returnAddressColumn = 0;
    std::int64_t              dataAlignmentFactor = 0;
    std::uint8_t              fdePointerEncoding  = 0;
    std::vector<std::uint8_t> cieInitialOps;
    std::vector<Fde>          fdes;
    bool                      sawTerminator = false;
};

// Walk the emitted `.eh_frame` bytes as a real reader would: length-
// prefixed records, CIE first, each FDE's `cie_pointer` a DISTANCE BACK
// to it, `initial_location` a PC-relative sdata4 from its own field.
[[nodiscard]] EhChain decodeEhFrame(std::vector<std::uint8_t> const& image,
                                    std::size_t secFileOff,
                                    std::uint64_t secVa, std::size_t secSize) {
    EhChain out;
    std::size_t p = secFileOff;
    std::size_t const end = secFileOff + secSize;
    std::size_t cieStart = 0;
    while (p + 4 <= end) {
        std::uint32_t const len = rd32(image, p);
        if (len == 0) { out.sawTerminator = true; break; }
        std::size_t const body = p + 4;
        std::uint32_t const idOrPtr = rd32(image, body);
        if (idOrPtr == 0) {
            cieStart = p;
            std::size_t q = body + 4;
            EXPECT_EQ(image[q], 1u) << "CIE version";
            ++q;
            std::string aug;
            while (image[q] != 0) aug.push_back(static_cast<char>(image[q++]));
            ++q;
            EXPECT_EQ(aug, "zR");
            (void)readULeb(image, q);                      // code alignment
            out.dataAlignmentFactor = readSLeb(image, q);
            out.returnAddressColumn = readULeb(image, q);
            std::uint64_t const augLen = readULeb(image, q);
            EXPECT_EQ(augLen, 1u);
            out.fdePointerEncoding = image[q++];
            out.cieInitialOps.assign(image.begin() + static_cast<long>(q),
                                     image.begin() + static_cast<long>(body + len));
        } else {
            // `cie_pointer` is the distance BACK from its own field.
            EXPECT_EQ(body - idOrPtr, cieStart)
                << "FDE cie_pointer must reach the CIE (this is `.eh_frame`, "
                   "where the field is a back-distance, not a section offset)";
            std::size_t const ilocField = body + 4;
            std::int64_t const rel = rdS32(image, ilocField);
            Fde f;
            f.initialLocationVa = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(secVa + (ilocField - secFileOff))
                + rel);
            f.addressRange = rd32(image, ilocField + 4);
            std::size_t q = ilocField + 8;
            std::uint64_t const augLen = readULeb(image, q);
            EXPECT_EQ(augLen, 0u);
            f.ops.assign(image.begin() + static_cast<long>(q),
                         image.begin() + static_cast<long>(body + len));
            out.fdes.push_back(std::move(f));
        }
        p = body + len;
    }
    return out;
}

// The __TEXT LC_SEGMENT_64's declared nsects and the count its own
// cmdsize reserves — the two numbers a hand-maintained literal desyncs.
struct SegShape {
    std::uint32_t declaredNsects = 0;
    std::uint32_t reservedNsects = 0;
    bool          cmdsizeIsWhole = false;
    std::uint64_t fileOff = 0;
    std::uint64_t fileSize = 0;
    std::uint64_t vmaddr = 0;
};

[[nodiscard]] SegShape segShape(std::vector<std::uint8_t> const& b,
                                std::string_view name) {
    SegShape s;
    auto const segOff = findSegment(std::span<std::uint8_t const>{b}, name);
    EXPECT_TRUE(segOff.has_value()) << "segment " << name << " absent";
    if (!segOff) return s;
    std::uint32_t const cmdsize = rd32(b, *segOff + 4);
    s.vmaddr         = rd64(b, *segOff + 24);
    s.fileOff        = rd64(b, *segOff + 40);
    s.fileSize       = rd64(b, *segOff + 48);
    s.declaredNsects = rd32(b, *segOff + 64);
    constexpr std::uint32_t kSegmentCommand64Size = 72;
    constexpr std::uint32_t kSection64Size        = 80;
    s.cmdsizeIsWhole =
        cmdsize >= kSegmentCommand64Size
        && (cmdsize - kSegmentCommand64Size) % kSection64Size == 0;
    s.reservedNsects =
        s.cmdsizeIsWhole ? (cmdsize - kSegmentCommand64Size) / kSection64Size : 0;
    return s;
}

} // namespace

// ── The section exists, is well-formed, and lives inside __TEXT ──────

TEST(MachOEhFrame, ExecDynamicEmitsEhFrameSectionInsideText) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(execSchemaJson());
    ASSERT_TRUE(fmt.has_value());

    Encoded const enc = encodeWith(**target, **fmt, /*withCfi=*/true);
    ASSERT_FALSE(enc.bytes.empty());

    auto const secOff = findSection(std::span<std::uint8_t const>{enc.bytes},
                                    "__TEXT", "__eh_frame");
    ASSERT_TRUE(secOff.has_value())
        << "no __TEXT,__eh_frame section_64 record — nothing can walk a DSS "
           "stack frame (D-UNWIND-NO-EH-FRAME-ANY-LANGUAGE-ON-ELF-OR-MACHO)";

    // section_64 field offsets (<mach-o/loader.h>): sectname 0, segname 16,
    // addr 32, size 40, offset 48, align 52, reloff 56, nreloc 60, flags 64.
    std::uint64_t const secVa   = rd64(enc.bytes, *secOff + 32);
    std::uint64_t const secSize = rd64(enc.bytes, *secOff + 40);
    std::uint32_t const secFileOff = rd32(enc.bytes, *secOff + 48);
    std::uint32_t const align   = rd32(enc.bytes, *secOff + 52);
    std::uint32_t const flags   = rd32(enc.bytes, *secOff + 64);

    EXPECT_GT(secSize, 0u) << "a zero-size __eh_frame advertises an unwind "
                              "table that describes nothing";
    EXPECT_EQ(align, 3u) << "section_64.align is a LOG2 exponent; the DWARF "
                            "records are 8-byte aligned";
    EXPECT_EQ(flags, 0u) << "S_REGULAR, no attributes — these are data bytes, "
                            "and S_COALESCED addresses a static-link step that "
                            "a fully linked image has already passed";

    // The section's file extent lies inside __TEXT's.
    SegShape const text = segShape(enc.bytes, "__TEXT");
    EXPECT_GE(static_cast<std::uint64_t>(secFileOff), text.fileOff);
    EXPECT_LE(static_cast<std::uint64_t>(secFileOff) + secSize,
              text.fileOff + text.fileSize)
        << "__eh_frame runs past __TEXT.filesize — dyld never maps the tail";
    EXPECT_LE(static_cast<std::size_t>(secFileOff) + secSize, enc.bytes.size());

    // __TEXT.fileoff == 0 by Apple convention, so VA − fileoff is constant
    // across every section in the segment. The FDE pcrel arithmetic is
    // computed from this VA; a desync is invisible to a loader.
    EXPECT_EQ(secVa - secFileOff, text.vmaddr)
        << "__eh_frame's VA/file congruence inside __TEXT is broken";
}

// ── nsects vs the records actually emitted ───────────────────────────

TEST(MachOEhFrame, TextSegmentNsectsMatchesRecordsEmitted) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(execSchemaJson());
    ASSERT_TRUE(fmt.has_value());

    for (bool withCfi : {true, false}) {
        Encoded const enc = encodeWith(**target, **fmt, withCfi);
        ASSERT_FALSE(enc.bytes.empty()) << "withCfi=" << withCfi;
        SegShape const text = segShape(enc.bytes, "__TEXT");
        EXPECT_TRUE(text.cmdsizeIsWhole)
            << "__TEXT cmdsize is not segment_command_64 + N×section_64";
        EXPECT_EQ(text.declaredNsects, text.reservedNsects)
            << "__TEXT.nsects (" << text.declaredNsects << ") disagrees with "
               "the section_64 records its own cmdsize reserves ("
            << text.reservedNsects << ") — withCfi=" << withCfi
            << ". This is the invariant a hand-maintained section count "
               "breaks.";
        // __text + __stubs, plus __eh_frame exactly when CFI is present.
        EXPECT_EQ(text.declaredNsects, withCfi ? 3u : 2u);
        // And every declared record is reachable + distinctly named.
        EXPECT_TRUE(findSection(std::span<std::uint8_t const>{enc.bytes},
                                "__TEXT", "__text").has_value());
        EXPECT_TRUE(findSection(std::span<std::uint8_t const>{enc.bytes},
                                "__TEXT", "__stubs").has_value());
    }
}

// ── ★ Every FDE points at the function it describes ──────────────────

TEST(MachOEhFrame, FdeInitialLocationResolvesToItsFunctionVa) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(execSchemaJson());
    ASSERT_TRUE(fmt.has_value());

    Encoded const enc = encodeWith(**target, **fmt, /*withCfi=*/true);
    ASSERT_FALSE(enc.bytes.empty());
    auto const secOff = findSection(std::span<std::uint8_t const>{enc.bytes},
                                    "__TEXT", "__eh_frame");
    ASSERT_TRUE(secOff.has_value());
    std::uint64_t const secVa   = rd64(enc.bytes, *secOff + 32);
    std::uint64_t const secSize = rd64(enc.bytes, *secOff + 40);
    std::uint32_t const secFileOff = rd32(enc.bytes, *secOff + 48);

    EhChain const chain = decodeEhFrame(enc.bytes, secFileOff, secVa,
                                        static_cast<std::size_t>(secSize));
    EXPECT_TRUE(chain.sawTerminator)
        << "the `.eh_frame` chain must end in a zero-length record";
    ASSERT_EQ(chain.fdes.size(), 1u)
        << "exactly one function carries CFI in this fixture; a second FDE "
           "would mean a function without call-frame information got one";

    // Function #1 begins kFn0Len bytes into __text. That address is also
    // what the nlist_64 record for its symbol carries — one statement of
    // where the function lives, read here independently.
    std::uint64_t const expectedVa = enc.textSectionVa + kFn0Len;
    EXPECT_EQ(chain.fdes[0].initialLocationVa, expectedVa)
        << "the FDE's pcrel initial_location does not resolve to the "
           "function it describes — an unwinder follows this without "
           "complaint into the wrong frame";
    EXPECT_EQ(chain.fdes[0].addressRange, kFn1Len);
}

// ── The DWARF numbering is the psABI permutation, not the hardware one ──

TEST(MachOEhFrame, CieAndFdeCarryDwarfNumbersNotHardwareEncodings) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(execSchemaJson());
    ASSERT_TRUE(fmt.has_value());

    Encoded const enc = encodeWith(**target, **fmt, /*withCfi=*/true);
    ASSERT_FALSE(enc.bytes.empty());
    auto const secOff = findSection(std::span<std::uint8_t const>{enc.bytes},
                                    "__TEXT", "__eh_frame");
    ASSERT_TRUE(secOff.has_value());
    EhChain const chain =
        decodeEhFrame(enc.bytes, rd32(enc.bytes, *secOff + 48),
                      rd64(enc.bytes, *secOff + 32),
                      static_cast<std::size_t>(rd64(enc.bytes, *secOff + 40)));

    // The CIE's return-address column is the target's DECLARED value, read
    // from the schema rather than typed here.
    ASSERT_TRUE((*target)->dwarfReturnAddressColumn().has_value());
    EXPECT_EQ(chain.returnAddressColumn,
              *(*target)->dwarfReturnAddressColumn());
    EXPECT_EQ(chain.dataAlignmentFactor, -1);
    EXPECT_EQ(chain.fdePointerEncoding, 0x1Bu)  // DW_EH_PE_pcrel | sdata4
        << "the FDE pointer encoding must match how initial_location was "
           "written";

    // The CIE's initial instructions open with `DW_CFA_def_cfa <SP>, 8`.
    // ★ The register operand must be the SP's DWARF number, which on
    //   x86_64 is a DIFFERENT number from its hardware encoding — the
    //   whole reason the numbering is a declared table.
    auto const spOrd = ordinalOf(**target, "rsp");
    ASSERT_TRUE(spOrd.has_value());
    auto const* spInfo = (*target)->registerInfo(*spOrd);
    ASSERT_NE(spInfo, nullptr);
    ASSERT_TRUE(spInfo->dwarfNumber.has_value());
    ASSERT_NE(*spInfo->dwarfNumber, spInfo->hwEncoding)
        << "fixture precondition: rsp's DWARF number must differ from its "
           "hardware encoding, or this test cannot discriminate";
    ASSERT_GE(chain.cieInitialOps.size(), 3u);
    EXPECT_EQ(chain.cieInitialOps[0], 0x0Cu);  // DW_CFA_def_cfa
    EXPECT_EQ(chain.cieInitialOps[1], *spInfo->dwarfNumber);
    EXPECT_EQ(chain.cieInitialOps[2], 8u);     // CFA = SP + 8 at entry

    // The FDE's register rule likewise names rbp by DWARF number.
    auto const bpOrd = ordinalOf(**target, "rbp");
    ASSERT_TRUE(bpOrd.has_value());
    auto const* bpInfo = (*target)->registerInfo(*bpOrd);
    ASSERT_NE(bpInfo, nullptr);
    ASSERT_TRUE(bpInfo->dwarfNumber.has_value());
    ASSERT_NE(*bpInfo->dwarfNumber, bpInfo->hwEncoding)
        << "fixture precondition: rbp's DWARF number must differ from its "
           "hardware encoding, or this assertion cannot discriminate";
    ASSERT_EQ(chain.fdes.size(), 1u);
    // advance_loc(7) | def_cfa_offset 40 | advance_loc(4) |
    // DW_CFA_offset(rbp) 40   (offset factored by the -1 data alignment)
    std::vector<std::uint8_t> const expected{
        0x40u | 7u, 0x0Eu, 40u,
        0x40u | 4u, static_cast<std::uint8_t>(0x80u | *bpInfo->dwarfNumber),
        40u};
    ASSERT_GE(chain.fdes[0].ops.size(), expected.size());
    EXPECT_EQ(std::vector<std::uint8_t>(chain.fdes[0].ops.begin(),
                                        chain.fdes[0].ops.begin()
                                            + static_cast<long>(expected.size())),
              expected);
}

// ── No CFI anywhere ⇒ no section, and the encode still succeeds ──────

TEST(MachOEhFrame, ModuleWithoutCfiEmitsNoEhFrameAndStillEncodes) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(execSchemaJson());
    ASSERT_TRUE(fmt.has_value());

    Encoded const enc = encodeWith(**target, **fmt, /*withCfi=*/false);
    ASSERT_FALSE(enc.bytes.empty())
        << "a module without call-frame information must still encode";
    EXPECT_FALSE(findSection(std::span<std::uint8_t const>{enc.bytes},
                             "__TEXT", "__eh_frame").has_value())
        << "an empty __eh_frame advertises an unwind table describing nothing";

    // And the image is otherwise intact: the load-command walk lands
    // exactly on sizeofcmds.
    std::uint32_t const ncmds      = rd32(enc.bytes, 16);
    std::uint32_t const sizeofcmds = rd32(enc.bytes, 20);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmdsize = rd32(enc.bytes, off + 4);
        ASSERT_NE(cmdsize, 0u);
        off += cmdsize;
    }
    EXPECT_EQ(off, 32u + sizeofcmds);
}

// ── The __got VA survives a section being appended to __TEXT ─────────
//
// Extern DATA imports bind their
// `symbolVa` to the EARLY __TEXT VA chain, long before the LATE file
// layout runs. Nothing compared the two, so a section appended to __TEXT
// without extending the EARLY chain would slide __got out from under
// every already-resolved data-import relocation. The writer now fails
// loud on the divergence; this pins the observable identity in both
// __TEXT shapes.

TEST(MachOEhFrame, GotVaStaysCongruentWithTextInBothShapes) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(execSchemaJson());
    ASSERT_TRUE(fmt.has_value());

    for (bool withCfi : {true, false}) {
        Encoded const enc = encodeWith(**target, **fmt, withCfi);
        ASSERT_FALSE(enc.bytes.empty()) << "withCfi=" << withCfi;
        auto const gotOff = findSection(std::span<std::uint8_t const>{enc.bytes},
                                        "__DATA_CONST", "__got");
        ASSERT_TRUE(gotOff.has_value()) << "withCfi=" << withCfi;
        std::uint64_t const gotVa      = rd64(enc.bytes, *gotOff + 32);
        std::uint32_t const gotFileOff = rd32(enc.bytes, *gotOff + 48);
        SegShape const text = segShape(enc.bytes, "__TEXT");
        // __TEXT.fileoff == 0, so the file→VA identity is segment-wide.
        EXPECT_EQ(gotVa - gotFileOff, text.vmaddr)
            << "__got's VA no longer matches its file offset relative to "
               "__TEXT.vmaddr — withCfi=" << withCfi;
        SegShape const dataConst = segShape(enc.bytes, "__DATA_CONST");
        EXPECT_EQ(gotVa, dataConst.vmaddr);
        EXPECT_EQ(static_cast<std::uint64_t>(gotFileOff), dataConst.fileOff);
        // __got must start at or after the end of __TEXT's file extent.
        EXPECT_GE(static_cast<std::uint64_t>(gotFileOff),
                  text.fileOff + text.fileSize);
    }
}

// ── A target with no DWARF numbering is not thereby unlinkable ───────
//
// `buildEhFrame` refuses up front for a target that declares no DWARF
// register numbering — correctly, since a rule it cannot name produces a
// table that walks the wrong frame. But that refusal is decided BEFORE it
// discovers whether any rule exists, so a writer that called it
// unconditionally would make a missing psABI table fail EVERY image on
// that target, including one with no call-frame information at all. The
// writer therefore asks only when a function actually carries CFI.
//
// Both directions are pinned: the no-CFI module must LINK, and the
// with-CFI module on the same target must REFUSE. Asserting only the
// first would pass for a writer that dropped the table silently.

namespace {

// A Mach-O target carrying registers but NO `dwarfNumber` on any of them
// and no `dwarfReturnAddressColumn` — the shape the loader accepts when a
// target has not declared its psABI unwind table.
[[nodiscard]] std::string noDwarfNumberingTargetJson() {
    return R"({
      "dssTargetVersion": 1,
      "target": {"name":"x86_64-no-dwarf-numbering","version":"0.0"},
      "opcodes": [ {"mnemonic":"invalid","result":"none"} ],
      "relocations": [
        { "name":"rel32", "kind":1, "pcRelative":true, "addendBias":-4,
          "widthBytes":4 },
        { "name":"abs64", "kind":2, "pcRelative":false, "addendBias":0,
          "widthBytes":8 },
        { "name":"abs32", "kind":3, "pcRelative":false, "addendBias":0,
          "widthBytes":4 }
      ]
    })";
}

} // namespace

TEST(MachOEhFrame, TargetWithoutDwarfNumberingStillLinksAModuleWithoutCfi) {
    auto plain = TargetSchema::loadFromText(noDwarfNumberingTargetJson());
    ASSERT_TRUE(plain.has_value());
    // Fixture precondition: this target really declares no numbering, or
    // the test proves nothing.
    ASSERT_FALSE((*plain)->dwarfReturnAddressColumn().has_value());
    auto fmt = ObjectFormatSchema::loadFromText(execSchemaJson());
    ASSERT_TRUE(fmt.has_value());

    // The CFI-carrying fixture needs named registers this bare target has
    // none of, so build the module against the SHIPPED table and encode it
    // against the numbering-less one — which is exactly the real hazard:
    // a producer that emitted CFI for a target that cannot describe it.
    auto shipped = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(shipped.has_value());

    {   // No CFI anywhere ⇒ must link clean and emit no section.
        AssembledModule mod = makeModule(**shipped, /*withCfi=*/false);
        DiagnosticReporter rep;
        auto const bytes = macho::encode(mod, **plain, **fmt, rep);
        EXPECT_EQ(rep.errorCount(), 0u)
            << "a target's missing DWARF numbering must not block linking a "
               "module that has no call-frame information to describe"
            << (rep.all().empty() ? "" : ": " + rep.all().front().actual);
        ASSERT_FALSE(bytes.empty());
        EXPECT_FALSE(findSection(std::span<std::uint8_t const>{bytes},
                                 "__TEXT", "__eh_frame").has_value());
    }
    {   // CFI present ⇒ must REFUSE, loudly, rather than drop the table.
        AssembledModule mod = makeModule(**shipped, /*withCfi=*/true);
        DiagnosticReporter rep;
        auto const bytes = macho::encode(mod, **plain, **fmt, rep);
        EXPECT_GT(rep.errorCount(), 0u)
            << "a module WITH call-frame information on a target that "
               "declares no DWARF numbering must fail loud, never silently "
               "ship without the table it was asked to write";
        EXPECT_TRUE(bytes.empty());
    }
}

// ═══════════════════════════════════════════════════════════════════════
// `section_64.align` IS A LOG2 EXPONENT —
// D-MACHO-TEXT-SECTION-ALIGN-RAW-BYTES-INTO-LOG2-FIELD
//
// ✔MEASURED 2026-08-13, the defect this pin closes: `__text`'s
// `section_64.align` was written from the schema row VERBATIM at all three
// writer arms, while `__const` / `__data` / `__got` / `__thread_*` / `__bss`
// all convert with `std::countr_zero`. The schemas had then been edited to
// match each arm locally, so ONE KEY MEANT TWO THINGS:
//   * the MH_OBJECT rows carried `addrAlign: 4` ("log2 form") — verbatim gave
//     align 4 = 16 bytes, which happened to be right;
//   * the exec/dylib rows carried `addrAlign: 16` ("log2 not used here") —
//     verbatim gave align 16 = **2^16 = 64 KiB**, a claim no DSS image can
//     honour and no other section makes.
//
// ★★ THE TRAP THIS PIN EXISTS TO CATCH IS THE *FIX*, NOT THE BUG.
//    Adding `countr_zero` at the three `__text` sites WITHOUT also retiring
//    the object schemas' log2 convention turns the object path's correct 4
//    into 2 — i.e. silently drops `__text` from 16-byte to 4-byte alignment
//    in exactly the `.o` ld64 honours. So this asserts the LOG2 RELATIONSHIP
//    against the schema's own byte count, on every arm, rather than a bare
//    constant that a half-applied fix would still satisfy.
// ═══════════════════════════════════════════════════════════════════════

namespace {

// The one fact under test, read straight out of the emitted image.
[[nodiscard]] std::optional<std::uint32_t>
textSectionAlignField(std::vector<std::uint8_t> const& bytes,
                      std::string_view segName) {
    auto const off = findSection(std::span<std::uint8_t const>{bytes},
                                 segName, "__text");
    if (!off.has_value()) return std::nullopt;
    return rd32(bytes, *off + 52);   // section_64.align
}

} // namespace

TEST(MachOTextSectionAlign, EveryArmWritesLog2OfTheSchemasByteCount) {
    // All three writer arms, both machines, SHIPPED schemas — the exec and
    // dylib arms are where the 2^16 shipped, and the object arm is the one a
    // careless fix regresses. A single test covering one arm would have gone
    // green for either mistake.
    struct Case { std::string_view target, format, segment; };
    std::vector<Case> const cases{
        {"arm64",  "macho64-arm64-darwin",             ""},
        {"x86_64", "macho64-x86_64-darwin",            ""},
        {"arm64",  "macho64-arm64-darwin-staticlib",   ""},
        {"x86_64", "macho64-x86_64-darwin-staticlib",  ""},
        {"arm64",  "macho64-arm64-darwin-exec",   "__TEXT"},
        {"x86_64", "macho64-x86_64-darwin-exec",  "__TEXT"},
    };
    for (auto const& c : cases) {
        SCOPED_TRACE(std::string{c.format});
        auto target = TargetSchema::loadShipped(c.target);
        ASSERT_TRUE(target.has_value());
        auto fmt = ObjectFormatSchema::loadShipped(c.format);
        ASSERT_TRUE(fmt.has_value());
        auto const* row = (*fmt)->sectionByKind(SectionKind::Text);
        ASSERT_NE(row, nullptr);

        // The object arms take a plain module: `makeModule` is exec-shaped
        // (it carries an extern import + an entry override) and MH_OBJECT
        // neither needs nor accepts those.
        AssembledModule mod;
        if (c.segment.empty()) {
            mod.expectedFuncCount = 1;
            AssembledFunction fn;
            fn.symbol = SymbolId{1};
            fn.bytes.assign(16, 0x90);
            mod.functions.push_back(std::move(fn));
            mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_a",
                                               SymbolBinding::Global,
                                               SymbolVisibility::Default});
        } else {
            mod = makeModule(**target, /*withCfi=*/false);
        }
        DiagnosticReporter rep;
        auto const bytes = macho::encode(mod, **target, **fmt, rep);
        ASSERT_EQ(rep.errorCount(), 0u);
        ASSERT_FALSE(bytes.empty());

        auto const align = textSectionAlignField(bytes, c.segment);
        ASSERT_TRUE(align.has_value()) << "no __text section_64 record";

        // The relationship, not a constant: align == log2(schema byte count).
        ASSERT_TRUE(std::has_single_bit(row->addrAlign))
            << "a non-power-of-two addrAlign has no log2 at all";
        EXPECT_EQ(*align,
                  static_cast<std::uint32_t>(std::countr_zero(row->addrAlign)))
            << "section_64.align must be log2(addrAlign). Writing the schema "
               "row VERBATIM is how `addrAlign: 16` became a claimed 2^16 = "
               "64 KiB alignment on every image DSS shipped";
        // And the absolute answer, spelled out so a schema edit that changed
        // BOTH sides consistently but wrongly is still caught.
        EXPECT_EQ(*align, 4u) << "log2(16) = 4 — 16-byte code alignment";
        EXPECT_NE(*align, 16u)
            << "16 in this field means 2^16 = 65536-byte alignment, the exact "
               "value the exec and dylib arms used to ship";
    }
}

TEST(MachOTextSectionAlign, DylibSchemasDeclareTheSameRawByteCount) {
    // The dylib arm rides `encodeExecDynamic` — the SAME `textAlignLog2`, in
    // the same function the exec cases above drive — so what is left to pin is
    // its own schema row. It carried the "log2 not used here" 16 that the exec
    // rows did, and a partial schema migration would leave it behind.
    for (auto const& f : {std::string_view{"macho64-arm64-darwin-dylib"},
                          std::string_view{"macho64-x86_64-darwin-dylib"}}) {
        SCOPED_TRACE(std::string{f});
        auto fmt = ObjectFormatSchema::loadShipped(f);
        ASSERT_TRUE(fmt.has_value());
        auto const* row = (*fmt)->sectionByKind(SectionKind::Text);
        ASSERT_NE(row, nullptr);
        EXPECT_EQ(row->addrAlign, 16u)
            << "addrAlign is RAW BYTES on every Mach-O row since "
               "D-MACHO-TEXT-SECTION-ALIGN-RAW-BYTES-INTO-LOG2-FIELD; a 4 here "
               "is the retired log2 convention returning";
        EXPECT_EQ(std::countr_zero(row->addrAlign), 4);
    }
}

TEST(MachOTextSectionAlign, NonPowerOfTwoTextAlignFailsLoud) {
    // `countr_zero` on a non-power-of-two silently returns the exponent of the
    // LOW BIT — 24 would become 3 (8 bytes), a quieter and more damaging
    // answer than a refusal. The four data sections have carried this belt all
    // along; `__text` never did, which is why its row could drift unnoticed.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    std::string json{execSchemaJson()};
    // Rewrite the __text row's addrAlign to a non-power-of-two. The
    // replacement is asserted to have landed — a no-op edit would make this
    // test pass over an unmutated schema and assert nothing.
    static constexpr std::string_view kTextAlignKey = "\"addrAlign\":16";
    auto const pos = json.find(kTextAlignKey);
    ASSERT_NE(pos, std::string::npos)
        << "the synthetic exec schema no longer spells __text's addrAlign as "
           "16; this test would otherwise silently exercise nothing";
    json.replace(pos, kTextAlignKey.size(), "\"addrAlign\":24");
    ASSERT_NE(json.find("\"addrAlign\":24"), std::string::npos);

    auto fmt = ObjectFormatSchema::loadFromText(json);
    ASSERT_TRUE(fmt.has_value())
        << "24 is a legal addrAlign as far as the schema is concerned — the "
           "refusal must come from the writer, at the point the log2 is needed";

    AssembledModule mod = makeModule(**target, /*withCfi=*/false);
    DiagnosticReporter rep;
    auto const bytes = macho::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    ASSERT_GT(rep.errorCount(), 0u);
    bool named = false;
    for (auto const& d : rep.all()) {
        if (d.actual.find("is not a power of two") != std::string::npos
            && d.actual.find("LOG2") != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named)
        << "the refusal must name the log2 field, not merely fail";
}

// ── THE TWO ARMS THE ALIGN PIN ABOVE DOES NOT ACTUALLY READ ────────────────
//    D-FORMAT-MACHO-SECTION-ALIGN-EMITTED-RAW-NOT-LOG2 (the residual half)
//
// ✔MEASURED 2026-08-26, byte-level, on images emitted by the SHIPPED writer:
// `__text`'s `section_64.align` is 4 (= 2^4 = 16 bytes) on BOTH
// `macho64-arm64-darwin-exec` and `macho64-x86_64-darwin-exec`. The raw-16
// defect is GONE, closed by
//   D-MACHO-TEXT-SECTION-ALIGN-RAW-BYTES-INTO-LOG2-FIELD
// and `EveryArmWritesLog2OfTheSchemasByteCount` above pins it.
//
// ★ WHAT THAT SUITE STILL COULD NOT SEE, and why these two exist:
//
//   (1) THE DYLIB ARM WAS PINNED BY READING THE SCHEMA, NOT THE IMAGE.
//       `DylibSchemasDeclareTheSameRawByteCount` loads the two dylib documents
//       and asserts `addrAlign == 16` — a fact about the JSON. It emits
//       nothing and reads no byte. Its own comment argues the dylib arm "rides
//       encodeExecDynamic — the SAME textAlignLog2", which is true of today's
//       code and is exactly the kind of claim an instrument is supposed to
//       stop depending on: it is a statement about the WRITER, offered in
//       place of a statement about the OUTPUT.
//
//   (2) NEITHER EXEC CASE ABOVE REACHES `encodeExec`. `makeModule` carries an
//       extern import, and `macho::encode` routes a module with a non-empty
//       `externImports` to `encodeExecDynamic`. So the two shipped exec rows
//       exercise the DYNAMIC arm twice; the static arm's own
//       `sectionAlignLog2` call site is never driven. Three arms were claimed,
//       two were measured.
//
// Both gaps are the same shape as the defect the parent anchor closed — a
// second site quietly reaching its own conclusion about one field — so they are
// closed by MEASUREMENT here rather than by an argument about code shape.

TEST(MachOTextSectionAlign, DylibIMAGESWriteLog2NotJustTheirSchemas) {
    struct Case { std::string_view target, format; };
    std::vector<Case> const cases{
        {"arm64",  "macho64-arm64-darwin-dylib"},
        {"x86_64", "macho64-x86_64-darwin-dylib"},
    };
    for (auto const& c : cases) {
        SCOPED_TRACE(std::string{c.format});
        auto target = TargetSchema::loadShipped(c.target);
        ASSERT_TRUE(target.has_value());
        auto fmt = ObjectFormatSchema::loadShipped(c.format);
        ASSERT_TRUE(fmt.has_value());
        auto const* row = (*fmt)->sectionByKind(SectionKind::Text);
        ASSERT_NE(row, nullptr);

        // ⚠ NOT `makeModule`: that one is EXEC-shaped -- it carries an extern
        // import and an `imageEntryOverride`, and a dylib has neither a LC_MAIN
        // to override nor (in the zero-import witness shape this mirrors) any
        // imports. ✔MEASURED: handing the exec module to a dylib schema is
        // refused with one diagnostic, so the first cut of this test failed
        // before it ever read a byte. The shape below is the c150/c152
        // zero-import dylib witness `test_macho_dylib_writer.cpp` uses.
        AssembledModule mod;
        mod.expectedFuncCount = 1;
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        fn.bytes.assign(16, 0x90);
        mod.functions.push_back(std::move(fn));
        mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "_dss_a",
                                           SymbolBinding::Global,
                                           SymbolVisibility::Default});

        DiagnosticReporter rep;
        auto const bytes = macho::encode(mod, **target, **fmt, rep);
        // The diagnostics are SPELLED OUT on failure: an encode that refuses
        // for an unrelated reason would otherwise read as an align defect.
        std::string why;
        for (auto const& d : rep.all()) { why += d.actual; why += '\n'; }
        ASSERT_EQ(rep.errorCount(), 0u) << why;
        ASSERT_FALSE(bytes.empty()) << why;

        auto const align = textSectionAlignField(bytes, "__TEXT");
        ASSERT_TRUE(align.has_value()) << "no __text section_64 record";
        ASSERT_TRUE(std::has_single_bit(row->addrAlign));
        EXPECT_EQ(*align,
                  static_cast<std::uint32_t>(std::countr_zero(row->addrAlign)))
            << "section_64.align must be log2(addrAlign) in the EMITTED dylib, "
               "not merely in the schema the dylib was emitted from";
        EXPECT_EQ(*align, 4u) << "log2(16) = 4 — 16-byte code alignment";
        EXPECT_NE(*align, 16u)
            << "16 in this field means 2^16 = 65536-byte alignment, the exact "
               "value the exec and dylib arms used to ship";
    }
}

TEST(MachOTextSectionAlign, TheSTATICExecArmWritesLog2Too) {
    // ★ THE MODULE CARRIES NO EXTERN IMPORTS, AND THAT IS THE WHOLE POINT:
    // `macho::encode` routes on `externImports.empty()`, so this is the only
    // shape that reaches `encodeExec`. The synthetic schema is used rather than
    // a shipped one because every shipped Darwin exec document requests a code
    // signature, and `macho::encode` REFUSES that combination outright (the
    // static arm emits no __LINKEDIT to host the signature) — so the static arm
    // is unreachable from a shipped exec schema by construction, and saying so
    // here is cheaper than the next reader re-deriving it.
    //
    // ⚠ CORRECTED under D-LK-MACHO-ADHOC-SIGNATURE-DROPPED-ON-STATIC-ARM. This
    // sentence used to say the shipped documents declare "a non-zero
    // `image.codeSignatureSize`". ✔MEASURED: not one of them does — they
    // declare `image.codeSignature`, the ad-hoc block. The CONCLUSION was right
    // and had been right all along; the REASON was false, and it was false in
    // the exact direction that mattered, because the gate it described tested
    // `codeSignatureSize` alone and therefore did NOT refuse the shipped
    // documents. A comment stating a premise no instrument checks is how a
    // guard gets believed for a rule it does not enforce.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(execSchemaJson());
    ASSERT_TRUE(fmt.has_value());
    auto const* row = (*fmt)->sectionByKind(SectionKind::Text);
    ASSERT_NE(row, nullptr);
    ASSERT_EQ(row->addrAlign, 16u)
        << "the synthetic schema no longer declares __text's addrAlign as 16 "
           "raw bytes; this test would otherwise assert the wrong constant";

    AssembledModule mod = makeModule(**target, /*withCfi=*/false);
    mod.externImports.clear();   // force the STATIC arm
    ASSERT_TRUE(mod.externImports.empty());

    DiagnosticReporter rep;
    auto const bytes = macho::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    auto const align = textSectionAlignField(bytes, "__TEXT");
    ASSERT_TRUE(align.has_value());
    EXPECT_EQ(*align,
              static_cast<std::uint32_t>(std::countr_zero(row->addrAlign)));
    EXPECT_EQ(*align, 4u);
    EXPECT_NE(*align, 16u);
}
