// Mach-O 64-bit .o writer tests — plan 14 LK3 cycle 1.
//
// Pins golden byte-level invariants of the emitted MH_OBJECT:
//   * mach_header_64 magic = 0xFEEDFACF (MH_MAGIC_64).
//   * cputype = 0x01000007 (CPU_TYPE_X86_64); filetype = MH_OBJECT (1).
//   * ncmds = 3 (LC_SEGMENT_64 + LC_BUILD_VERSION + LC_SYMTAB) for a
//     shipped darwin object format, 2 for one that declares no
//     `image.buildVersion` — the LC_BUILD_VERSION emission is gated on
//     the schema, so both counts are pinned (D-LK10-ENTRY-MACHO-EXIT).
//   * LC_SEGMENT_64 at byte 32; section_64 for `__text` immediately
//     follows the segment command.
//   * section_64.sectname = "__text"; segname = "__TEXT" (two-level
//     naming — D-LK3-1 closure).
//   * section_64.flags = 0x80000400 (S_REGULAR | S_ATTR_PURE_INSTRUCTIONS
//     | S_ATTR_SOME_INSTRUCTIONS).
//   * nlist_64 records are 16 bytes packed.
//   * relocation_info records are 8 bytes packed; r_info high 4 bits =
//     r_type (BRANCH=2 for rel32 → call sym).
//   * String table NUL-seeded (n_strx=0 means "no name") — same shape
//     as ELF (D-LK4-9 substrate consumer).
//
// Also pins that the shipped `macho64-x86_64-darwin.format.json`
// loads cleanly via `loadShipped`.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "format_reject_support.hpp"   // countAtPath / countWithMessage / rejectSummary
#include "link/format/macho.hpp"
#include "link/format/macho_object_reader.hpp"   // the round-trip acceptance test
#include "link/format/macho_chained_fixups.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link_test_support.hpp"
#include "macho_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace dss;
using dss::link_format::test::countAtPath;
using dss::link_format::test::errorCount;
using dss::link_format::test::countWithMessage;
using dss::link_format::test::rejectSummary;

namespace {
// Every diagnostic a reporter collected, so a failure names the cause rather
// than only the count. `rejectSummary` reads a schema LOAD result, not a
// writer's reporter, so the two are not interchangeable.
[[nodiscard]] std::string diagSummary(dss::DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        out += "\n  [" + std::to_string(static_cast<int>(d.code))
               + "] " + d.actual;
    }
    return out.empty() ? std::string{"(no diagnostics)"} : out;
}
}  // namespace

namespace {

// D-TEST-LE-READ-HELPERS CLOSED at 8aabc04 audit fold; local LE
// readers harmonized to the shared substrate at
// `tests/link/link_test_support.hpp`. The fold left local copies
// in this file dead-but-present; the 5ac97ae audit fold (4-agent
// convergence: code-reviewer HIGH-1 + type-design Q5 + simplifier
// S1 + comment-analyzer) drops them.
using dss::link_format::test::readU16LE;
using dss::link_format::test::readU32LE;
using dss::link_format::test::readU64LE;

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
    auto f = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(macho64-x86_64-darwin) failed";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

// The arm64 sibling loader (D-LK3-MACHO-ARM64-OBJECT): arm64 target + the new
// macho64-arm64-darwin relocatable-object format.
[[nodiscard]] Loaded loadShippedArm64() {
    Loaded out;
    auto t = TargetSchema::loadShipped("arm64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(arm64) failed";
        for (auto const& d : t.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped("macho64-arm64-darwin");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(macho64-arm64-darwin) failed";
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

// ── THE ONE DOOR TO THE WRITER (D-LK10-ENTRY 2.13 gate 6) ────────
//
// `resolveEntryFnIdx` (src/link/format/exec_reloc_apply.hpp) treats a
// format that declares `processExit` as having CONTRACTED that its
// image entry is the DSS-synthesized `_start` trampoline — and only
// `linker::link` injects one (`injectEntryTrampoline` in
// src/link/entry_trampoline.cpp — the file's ONLY assignment to that field —
// sets `imageEntryOverride = 0` on every successful injection). Reaching the
// walker's
// `entryPoint`-empty default with no override therefore means the
// module never came through the linker, and the walker now fails loud
// instead of silently making the user's first function the process
// entry.
//
// EVERY test in this file drives the writer DIRECTLY: these are
// byte-level writer pins, not link pins. So they all deliberately want
// an UNTRAMPOLINED image, and each must say so. Rather than repeat
// `mod.imageEntryOverride = 0` at ~70 call sites — the duplication that
// breeds the missed site — every direct-writer call in this file goes
// through this ONE helper, so the contract holds by construction and a
// test added later inherits it for free.
//
// Setting index 0 is semantically a NO-OP: 0 is exactly what the
// pre-gate default handed back, so not a single emitted byte, offset,
// count or size pinned below changes. The condition is the format's own
// `processExit` — mirroring the gate's own predicate — so the MH_OBJECT
// fixtures (which have no entry at all, and whose walker never consults
// the field) and any non-Mach-O format passed here route through
// untouched, and no library-flavored walker ever sees an override it
// would rightly reject.
[[nodiscard]] std::vector<std::uint8_t>
encodeUntrampolined(AssembledModule&          module,
                    TargetSchema const&       target,
                    ObjectFormatSchema const& format,
                    DiagnosticReporter&       reporter) {
    if (format.processExit().has_value()) {
        module.imageEntryOverride = 0u;
    }
    return macho::encode(module, target, format, reporter);
}

} // namespace

// ── Shipped JSON loads ───────────────────────────────────────────

TEST(MachOFormatJson, ShippedFileLoadsCleanly) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.format);
    EXPECT_EQ(loaded.format->kind(), ObjectFormatKind::MachO);
    EXPECT_EQ(loaded.format->name(), "macho64-x86_64-darwin");
    EXPECT_EQ(loaded.format->macho().cputype, 0x01000007u);
    EXPECT_EQ(loaded.format->macho().cpusubtype, 3u);
    EXPECT_TRUE(loaded.format->macho().filetype == MachOObjectType::Object);
    auto const* textRow = loaded.format->sectionByKind(SectionKind::Text);
    ASSERT_NE(textRow, nullptr);
    EXPECT_EQ(textRow->name, "__text");
    EXPECT_EQ(textRow->segment, "__TEXT");
    // D-MACHO-TEXT-SECTION-ALIGN-RAW-BYTES-INTO-LOG2-FIELD: this row used to
    // read `4u // log2(16)` — the object schemas alone spelled `addrAlign` as
    // a log2 exponent while every exec/dylib schema and every other section
    // row spelled it in RAW BYTES. The intent asserted here is unchanged
    // ("`__text` is 16-byte aligned"); only the unit is, now that one key has
    // one meaning. The writer converts to `section_64.align` with
    // `countr_zero`, and `MachOTextSectionAlign` pins that conversion
    // end-to-end on every arm.
    EXPECT_EQ(textRow->addrAlign, 16u);  // RAW BYTES; log2 is the writer's job
    EXPECT_EQ(textRow->type, 0x80000400u);
    // Mach-O has NO section headers for symtab/strtab.
    EXPECT_EQ(loaded.format->sectionByKind(SectionKind::Symtab), nullptr);
    EXPECT_EQ(loaded.format->sectionByKind(SectionKind::Strtab), nullptr);
}

// ── mach_header_64 golden bytes ─────────────────────────────────

TEST(MachOWriter, MachHeader64IdentityBytesMatchAppleAbi) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 42);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_GE(bytes.size(), 32u);
    EXPECT_EQ(rep.errorCount(), 0u);

    // magic = MH_MAGIC_64 = 0xFEEDFACF
    EXPECT_EQ(readU32LE(bytes, 0), 0xFEEDFACFu);
    // cputype = CPU_TYPE_X86_64 = 0x01000007
    EXPECT_EQ(readU32LE(bytes, 4), 0x01000007u);
    // cpusubtype = CPU_SUBTYPE_X86_64_ALL = 3
    EXPECT_EQ(readU32LE(bytes, 8), 3u);
    // filetype = MH_OBJECT = 1
    EXPECT_EQ(readU32LE(bytes, 12), 1u);
    // ncmds = 3 (LC_SEGMENT_64 + LC_BUILD_VERSION + LC_SYMTAB)
    EXPECT_EQ(readU32LE(bytes, 16), 3u);
    // sizeofcmds = 72 + 80*1 (segment+section) + 24 (build_version)
    //            + 24 (symtab) = 200
    EXPECT_EQ(readU32LE(bytes, 20), 200u);
    // flags = 0x2000 = MH_SUBSECTIONS_VIA_SYMBOLS.
    //
    // ★ THIS PIN IS REARGUED, NOT MERELY UPDATED, and the history it replaces
    // is kept because it was RIGHT. It used to assert 0 and said so: "cycle 1
    // emits a flat __text without subsection markers, anchored as D-LK3-2 for
    // the future subsection-emit cycle" — i.e. the flag was withheld because
    // setting it would have LIED to ld64 about function-granular dead-strip
    // safety. That precondition is now SATISFIED rather than overturned: the
    // MH_OBJECT writer stamps N_ALT_ENTRY (n_desc 0x0200) on every synthetic
    // per-block label, so each remaining defined symbol genuinely does start
    // its own subsection and the declaration is true
    // (D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM). The old
    // deferral named plan 14 §3.1 D-LK3-2, which had closed as the
    // MH_EXECUTE/LC_MAIN row and therefore owned this for nobody.
    //
    // The value is NOT hardcoded in the writer — it is `MachOIdentity::flags`
    // copied verbatim from the shipped format schema, which is exactly why the
    // ld64 dead-strip evidence can be revisited by editing one JSON number and
    // this one expectation, with no C++ change.
    EXPECT_EQ(readU32LE(bytes, 24), 0x2000u)
        << "MH_OBJECT must declare MH_SUBSECTIONS_VIA_SYMBOLS — without it a "
           "reader cannot tell a file-local `static` body from an interior "
           "block label and drops the body's bytes";
    // reserved = 0
    EXPECT_EQ(readU32LE(bytes, 28), 0u);
}

// ── LC_SEGMENT_64 + section_64 two-level naming ─────────────────

TEST(MachOWriter, LcSegment64ContainsOneSectionWithTwoLevelNaming) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 42);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 32u + 72u + 80u);

    // LC_SEGMENT_64 at byte 32
    EXPECT_EQ(readU32LE(bytes, 32), 0x19u);  // LC_SEGMENT_64
    EXPECT_EQ(readU32LE(bytes, 36), 72u + 80u);  // cmdsize
    // segname (16 bytes) starts at 40 — empty for MH_OBJECT
    for (std::size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(bytes[40 + i], 0u) << "LC_SEGMENT_64 segname must be empty";
    }
    // nsects @ +72+64 = +136 (segment_command_64 fields:
    // cmd(4)+cmdsize(4)+segname(16)+vmaddr(8)+vmsize(8)+fileoff(8)+
    // filesize(8)+maxprot(4)+initprot(4)+nsects(4)+flags(4)).
    EXPECT_EQ(readU32LE(bytes, 32 + 64), 1u);  // nsects = 1

    // section_64 starts at byte 32 + 72 = 104
    // sectname[16] = "__text\0\0\0\0\0\0\0\0\0\0"
    EXPECT_EQ(bytes[104 + 0], '_');
    EXPECT_EQ(bytes[104 + 1], '_');
    EXPECT_EQ(bytes[104 + 2], 't');
    EXPECT_EQ(bytes[104 + 3], 'e');
    EXPECT_EQ(bytes[104 + 4], 'x');
    EXPECT_EQ(bytes[104 + 5], 't');
    EXPECT_EQ(bytes[104 + 6], 0u);
    // segname[16] = "__TEXT\0\0\0\0\0\0\0\0\0\0" at offset 104+16=120
    EXPECT_EQ(bytes[120 + 0], '_');
    EXPECT_EQ(bytes[120 + 1], '_');
    EXPECT_EQ(bytes[120 + 2], 'T');
    EXPECT_EQ(bytes[120 + 3], 'E');
    EXPECT_EQ(bytes[120 + 4], 'X');
    EXPECT_EQ(bytes[120 + 5], 'T');
    EXPECT_EQ(bytes[120 + 6], 0u);
    // section_64.flags @ offset 104 + 16 + 16 + 8 + 8 + 4 + 4 + 4 + 4 = 168
    // (sectname[16] + segname[16] + addr(8) + size(8) + offset(4) +
    //  align(4) + reloff(4) + nreloc(4) → flags)
    EXPECT_EQ(readU32LE(bytes, 168), 0x80000400u);
}

// ── LC_SYMTAB locates symbol + string tables ───────────────────

TEST(MachOWriter, LcSymtabReferencesNlist64AndStringTable) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 7);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // LC_SYMTAB starts at byte 32 + 72 + 80 + 24 = 208 — the trailing +24 is
    // LC_BUILD_VERSION, which the walker writes between the segment command
    // (with its section_64 tail) and the symtab, matching clang's own `.o`
    // command order (D-LK10-ENTRY-MACHO-EXIT).
    EXPECT_EQ(readU32LE(bytes, 208), 0x02u);  // LC_SYMTAB
    EXPECT_EQ(readU32LE(bytes, 212), 24u);    // cmdsize
    std::uint32_t const symoff = readU32LE(bytes, 216);
    std::uint32_t const nsyms = readU32LE(bytes, 220);
    std::uint32_t const stroff = readU32LE(bytes, 224);
    std::uint32_t const strsize = readU32LE(bytes, 228);
    EXPECT_EQ(nsyms, 1u);
    EXPECT_GT(symoff, 0u);
    EXPECT_LE(symoff + 16u, bytes.size());

    // nlist_64 record: n_strx(u32) + n_type(u8) + n_sect(u8) +
    // n_desc(u16) + n_value(u64). Total 16 bytes.
    EXPECT_EQ(readU32LE(bytes, symoff + 0), 1u)
        << "n_strx points 1 byte past the leading NUL "
           "('_sym_7' lives at offset 1 in the strtab)";
    // n_type = N_SECT = 0x0E — the function has no ModuleSymbol row, so it is
    // not externally visible → `definedBinding` = Local → bare N_SECT, NO N_EXT
    // (TF-C54, D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION;
    // pre-fix it was N_SECT|N_EXT = 0x0F).
    EXPECT_EQ(bytes[symoff + 4], 0x0Eu);
    // n_sect = 1 (1-based)
    EXPECT_EQ(bytes[symoff + 5], 1u);
    // n_value = 0 (function offset 0 in .text)
    EXPECT_EQ(readU64LE(bytes, symoff + 8), 0u);

    // String table starts with NUL (n_strx=0 = "no name")
    EXPECT_EQ(bytes[stroff], 0u);
    // Then "_sym_7\0" at offset 1
    EXPECT_EQ(bytes[stroff + 1], '_');
    EXPECT_EQ(bytes[stroff + 2], 's');
    EXPECT_EQ(bytes[stroff + 3], 'y');
    EXPECT_EQ(bytes[stroff + 4], 'm');
    EXPECT_EQ(bytes[stroff + 5], '_');
    EXPECT_EQ(bytes[stroff + 6], '7');
    EXPECT_EQ(bytes[stroff + 7], 0u);
    EXPECT_GE(strsize, 8u);
}

// ── D-LK-OBJECT-EXTERN-SYMBOL-NAMES: real (pre-mangled) name verbatim ──

TEST(MachOWriter, ObjectSymtabEmitsPipelineMangledNameVerbatimNoDoubleUnderscore) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{10};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    // The compile pipeline pre-mangles a Mach-O defined name with a leading
    // `_` (applyCMangling). The writer must emit it VERBATIM — a writer-side
    // re-mangle would DOUBLE it to `__public_fn`. Seeding the already-`_`-
    // prefixed name is what makes a double-mangle detectable.
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "_public_fn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // Single-symbol layout matches the LcSymtab test: LC_SYMTAB at 208,
    // stroff at 224; the name sits at strtab offset 1 (past the leading NUL).
    std::uint32_t const stroff = readU32LE(bytes, 224);
    std::string name;
    for (std::size_t p = stroff + 1; p < bytes.size() && bytes[p] != 0; ++p) {
        name.push_back(static_cast<char>(bytes[p]));
    }
    EXPECT_EQ(name, "_public_fn")
        << "Mach-O writer must emit the pipeline-mangled name verbatim — "
           "exactly one leading underscore, never a re-mangled `__public_fn`";
}

// ── D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION (TF-C54) ──

TEST(MachOWriter, ObjectNlistCouplesNameAndBindingStaticLocalDropsNExt) {
    // Red-on-disable pin: two defined functions, A global + B static (Local).
    // NAME and BINDING are coupled (`definedName`/`definedBinding`): A keeps its
    // real name + N_SECT|N_EXT (0x0F); B stays internal `_sym_11` + bare N_SECT
    // (0x0E, NO N_EXT), so a sibling `.o`'s unrelated `_sym_<id>` can never bind
    // to it at a FOREIGN link. Mach-O relocatable objects carry no LC_DYSYMTAB,
    // so there is no local-first reordering — only the N_EXT bit flips.
    // Reverting `definedBinding` → B emits N_SECT|N_EXT and this pin goes red.
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction a;
    a.symbol = SymbolId{10};
    a.bytes  = {0xC3};
    mod.functions.push_back(std::move(a));
    AssembledFunction b;
    b.symbol = SymbolId{11};
    b.bytes  = {0xC3};
    mod.functions.push_back(std::move(b));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "_realfn",
                                       SymbolBinding::Global, SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{11}, "_statfn",
                                       SymbolBinding::Local, SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // LC_SYMTAB at byte 208; symoff @ +8, nsyms @ +12, stroff @ +16.
    // nlist_64 = n_strx(u32,+0) n_type(u8,+4) n_sect(u8,+5) n_desc(u16,+6)
    //            n_value(u64,+8) = 16 bytes.
    std::uint32_t const symoff = readU32LE(bytes, 208 + 8);
    std::uint32_t const stroff = readU32LE(bytes, 208 + 16);
    ASSERT_EQ(readU32LE(bytes, 208 + 12), 2u);

    auto nameAt = [&](std::uint32_t i) {
        std::uint32_t const strx = readU32LE(bytes, symoff + i * 16 + 0);
        std::string s;
        for (std::size_t p = stroff + strx; p < bytes.size() && bytes[p] != 0; ++p)
            s.push_back(static_cast<char>(bytes[p]));
        return s;
    };

    auto descAt = [&](std::uint32_t i) {
        return static_cast<std::uint16_t>(
            bytes[symoff + i * 16 + 6]
            | (static_cast<std::uint16_t>(bytes[symoff + i * 16 + 7]) << 8));
    };

    // sym[0] = fn A (global) → real name, N_SECT|N_EXT (0x0F).
    EXPECT_EQ(nameAt(0), "_realfn");
    EXPECT_EQ(bytes[symoff + 0 * 16 + 4], 0x0Fu)
        << "externally-visible fn keeps N_SECT|N_EXT";
    // sym[1] = fn B (static) → internal `_sym_11`, bare N_SECT (0x0E).
    EXPECT_EQ(nameAt(1), "_sym_11");
    EXPECT_EQ(bytes[symoff + 1 * 16 + 4], 0x0Eu)
        << "static (Local) fn drops N_EXT — the TF-C54 fix";

    // ── n_desc: NEITHER function is an alternate entry ──────────────
    //
    // D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM. This is
    // the OTHER half of the byte that used to make a `static` function and an
    // interior block label indistinguishable: with n_type coupled to binding
    // (above) but n_desc left 0 on BOTH, fn B's nlist was byte-identical to a
    // synthetic `_sym_<id>` block label, so the archive-member reader demoted
    // it to a bodiless symbol and its bytes never entered the image. A whole
    // function is never an alternate entry — it must carry n_desc = 0 while
    // `Arm64ObjectJumpTableBlockSymbolIsLocalDefinedNotUndef` pins the block
    // label at N_ALT_ENTRY (0x0200). The two pins are the discriminating PAIR;
    // either one alone proves nothing.
    // RED-ON-DISABLE: stamp N_ALT_ENTRY on the defined-function loop (or on the
    // Local arm of it) and these two go red.
    EXPECT_EQ(descAt(0), 0x0000u)
        << "an externally-visible whole function is an atom, never an "
           "N_ALT_ENTRY interior label";
    EXPECT_EQ(descAt(1) & 0x0200u, 0x0000u)
        << "a file-local (`static`) whole function must NOT carry N_ALT_ENTRY "
           "(0x0200) — that is the bit that says 'I am interior to the atom "
           "before me', and setting it here is exactly the misread that drops "
           "a static function's body on archive-member read-back";
    EXPECT_EQ(descAt(1), 0x0000u)
        << "no other n_desc bit is claimed for a defined function either";
}

// ── D-LK-OBJECT-WEAK-DEF-RELOCATABLE: N_WEAK_DEF, and NOTHING ELSE ──
//
// Mach-O expresses a weak DEFINITION with one n_desc bit, N_WEAK_DEF (0x0080),
// on an otherwise ordinary N_SECT|N_EXT symbol. There is no second bit and no
// header flag to go with it AT THE RELOCATABLE TIER.
//
// ⚠ THE ANCHOR ROW SAID "N_WEAK_DEF + MH_WEAK_DEFINES" AND THAT IS WRONG FOR
// AN OBJECT. ✔MEASURED 2026-08-20 on the operator's Apple Silicon host:
// `/usr/bin/clang -c` on `__attribute__((weak)) int wk(void)` emits n_desc
// 0x0080 for `_wk` and a mach_header whose flags are 0x00002000 —
// MH_SUBSECTIONS_VIA_SYMBOLS ONLY. MH_WEAK_DEFINES (0x8000) shows up only
// after LINKING (the exec from that object reads 0x00218085; a weak-free
// control exec reads 0x00200085), and `<mach-o/loader.h>` documents it as a
// property of "the final linked image". So this pin asserts the bit IS set on
// the symbol and IS NOT set on the object header — the second half is the one
// that keeps a future reader of the anchor row from "fixing" the writer into
// emitting a flag no reference encoder emits on a `.o`.
//
// RED-ON-DISABLE: drop N_WEAK_DEF from the n_desc decision → the weak symbol
// becomes byte-identical to the strong one and every assertion below reds.
TEST(MachOWriter, ObjectWeakDefinedFunctionEmitsNWeakDefAndNoHeaderFlag) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction st;
    st.symbol = SymbolId{10};
    st.bytes  = {0xC3};
    mod.functions.push_back(std::move(st));
    AssembledFunction w;
    w.symbol = SymbolId{11};
    w.bytes  = {0x90, 0xC3};
    mod.functions.push_back(std::move(w));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "_strongfn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    // The `__attribute__((weak))` shape c-subset.lang.json produces;
    // `definedBinding` returns Weak for it.
    mod.symbols.push_back(ModuleSymbol{SymbolId{11}, "_weakfn",
                                       SymbolBinding::Weak,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // mach_header_64.flags sits at offset 24.
    EXPECT_EQ(readU32LE(bytes, 24) & 0x8000u, 0u)
        << "MH_WEAK_DEFINES is a FINAL-IMAGE flag; an MH_OBJECT that claims it "
           "asserts something about a linked image it is not, and no reference "
           "encoder sets it on a `.o`";

    std::uint32_t const symoff = readU32LE(bytes, 216);
    std::uint32_t const nsyms  = readU32LE(bytes, 220);
    ASSERT_EQ(nsyms, 2u);
    // Emission order is module order: [0] strong, [1] weak.
    EXPECT_EQ(bytes[symoff + 4], 0x0Fu) << "strong: N_SECT|N_EXT";
    EXPECT_EQ(readU16LE(bytes, symoff + 6), 0u)
        << "a STRONG definition carries no n_desc bits";
    EXPECT_EQ(bytes[symoff + 16 + 4], 0x0Fu)
        << "a weak definition is still N_SECT|N_EXT - the weakness is in "
           "n_desc, and demoting n_type would make it undefined or local";
    EXPECT_EQ(readU16LE(bytes, symoff + 16 + 6), 0x0080u)
        << "N_WEAK_DEF (0x0080). NOT 0x0040 (N_WEAK_REF, a weak REFERENCE), "
           "NOT 0x0020 (N_NO_DEAD_STRIP) and NOT 0x0200 (N_ALT_ENTRY, which "
           "would mark the body an interior label and lose it)";
}

// ★★★ The Mach-O half of the round-trip acceptance test: the DSS writer
// emits N_WEAK_DEF and DSS'S OWN Mach-O reader reads the binding back as Weak.
// The writer bit and the reader lift are one mechanism; a bit nobody reads is
// indistinguishable from a bit never written.
//
// RED-ON-DISABLE: drop either half → the read-back binding is Global.
TEST(MachOWriter, ObjectWeakDefinitionRoundTripsBackToWeakThroughDssReader) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction st;
    st.symbol = SymbolId{10};
    st.bytes  = {0xC3};
    mod.functions.push_back(std::move(st));
    AssembledFunction w;
    w.symbol = SymbolId{11};
    w.bytes  = {0x90, 0xC3};
    mod.functions.push_back(std::move(w));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "_strongfn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{11}, "_weakfn",
                                       SymbolBinding::Weak,
                                       SymbolVisibility::Default});

    DiagnosticReporter wrep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, wrep);
    ASSERT_EQ(wrep.errorCount(), 0u);

    DiagnosticReporter rrep;
    auto back = macho::readRelocatableObject(bytes, *loaded.target,
                                             *loaded.format, rrep);
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(rrep.errorCount(), 0u);

    auto bindingOf = [&](std::string const& name)
        -> std::optional<SymbolBinding> {
        for (auto const& sym : back->symbols) {
            if (sym.name == name) return sym.binding;
        }
        return std::nullopt;
    };
    auto const weak = bindingOf("_weakfn");
    ASSERT_TRUE(weak.has_value());
    EXPECT_EQ(*weak, SymbolBinding::Weak)
        << "N_WEAK_DEF must read back as Weak";
    auto const strong = bindingOf("_strongfn");
    ASSERT_TRUE(strong.has_value());
    EXPECT_EQ(*strong, SymbolBinding::Global)
        << "the strong sibling must not be dragged weak by its neighbour";
}

// ── D-LK-ALIAS-NAME-ABSENT-FROM-REEMITTED-OBJECT-SYMTAB, Mach-O arm ──
//
// Several names on one atom, each with ITS OWN binding — Mach-O says that per
// SYMBOL (N_EXT plus the N_WEAK_DEF n_desc bit), so a weak alias of a strong
// definition needs nothing special here. Two things must hold together:
// the alias record must be PRESENT at the canonical's n_sect/n_value, and the
// relocation against the atom must still name the CANONICAL index. The second
// is the one that fails silently: this writer REGISTERS indices in one pass and
// EMITS records in another, so an alias the emission adds and the registration
// does not count shifts every later record while the index map keeps the old
// values — a well-formed object whose relocations name the wrong symbols.
//
// ★ Emitting an alias at all is safe under MH_SUBSECTIONS_VIA_SYMBOLS, and that
// was MEASURED, not assumed: scripts/macho-alias-ld64-matrix, 8 cells on real
// Apple Silicon (plain second label / `.alt_entry` / a clang `.globl`+`.set`
// control, each × with and without `-dead_strip`, linked against a caller
// referencing ONLY the alias, plus a canonical-only control). Every cell: both
// names present at the SAME address, the program RAN and returned 42, `__text`
// unchanged at 0x28 bytes. No zero-length atom, no stripped body.
//
// RED-ON-DISABLE: drop the alias emission → nsyms falls and `_othername` is
// absent; drop the registration-side alias accounting → the r_symbolnum
// assertion reds (and the writer's own index tripwire fires first).
TEST(MachOWriter, ObjectSymtabCarriesEveryAliasNameAndRelocKeepsTheCanonical) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction target;
    target.symbol = SymbolId{10};
    target.bytes  = {0x90, 0xC3};
    mod.functions.push_back(std::move(target));
    AssembledFunction caller;
    caller.symbol = SymbolId{11};
    caller.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3};
    caller.relocations.push_back(
        Relocation{/*offset=*/1u, /*target=*/SymbolId{10},
                   /*kind=*/RelocationKind{1}, /*addend=*/0});
    mod.functions.push_back(std::move(caller));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "_realname",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "_weakalias",
                                       SymbolBinding::Weak,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{11}, "_callerfn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u) << diagSummary(rep);
    ASSERT_FALSE(bytes.empty());

    std::uint32_t const symoff = readU32LE(bytes, 216);
    std::uint32_t const nsyms  = readU32LE(bytes, 220);
    std::uint32_t const stroff = readU32LE(bytes, 224);
    ASSERT_EQ(nsyms, 3u) << "canonical + alias + caller";
    auto nameAt = [&](std::uint32_t i) {
        std::uint32_t const strx = readU32LE(bytes, symoff + i * 16);
        std::string name;
        for (std::size_t q = stroff + strx;
             q < bytes.size() && bytes[q] != 0; ++q) {
            name.push_back(static_cast<char>(bytes[q]));
        }
        return name;
    };
    EXPECT_EQ(nameAt(0), "_realname");
    EXPECT_EQ(nameAt(1), "_weakalias")
        << "the alias NAME must reach the re-emitted symbol table";
    EXPECT_EQ(readU64LE(bytes, symoff + 1 * 16 + 8),
              readU64LE(bytes, symoff + 0 * 16 + 8))
        << "both names must resolve to ONE address";
    EXPECT_EQ(bytes[symoff + 1 * 16 + 5], bytes[symoff + 0 * 16 + 5])
        << "same n_sect";
    EXPECT_EQ(readU16LE(bytes, symoff + 1 * 16 + 6), 0x0080u)
        << "the alias carries ITS OWN binding: N_WEAK_DEF, not the "
           "canonical's n_desc of 0";
    EXPECT_EQ(readU16LE(bytes, symoff + 0 * 16 + 6), 0u)
        << "and the canonical is NOT dragged weak by its alias";

    // The caller's relocation lives in __text's relocation_info table.
    // section_64 for __text starts at 32 + 72 = 104. Its layout:
    // sectname[16] segname[16] addr[8] size[8] offset[4] align[4]
    // reloff[4] nreloc[4] flags[4] ... - so reloff @ +56, nreloc @ +60.
    std::uint32_t const reloff = readU32LE(bytes, 104 + 56);
    ASSERT_EQ(readU32LE(bytes, 104 + 60), 1u);
    std::uint32_t const rinfo = readU32LE(bytes, reloff + 4);
    EXPECT_EQ(nameAt(rinfo & 0x00FFFFFFu), "_realname")
        << "the relocation must still name the CANONICAL record - an alias "
           "counted in emission but not in registration points every later "
           "relocation at the wrong symbol";
}

// ★ The Mach-O half of the read/re-emit round trip the alias anchor's closing
// work names. Building an `AssembledModule` by hand proves the writer but not
// that the READER hands the writer a module carrying both names, and the two
// halves are one mechanism. This drives the real reader over a real object.
//
// RED-ON-DISABLE: drop the alias emission → the first re-emission already loses
// the name, so the second read finds one name where there were two.
TEST(MachOWriter, AliasedObjectSurvivesReadThenReEmitThroughTheRealReader) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction f;
    f.symbol = SymbolId{10};
    f.bytes  = {0x90, 0x90, 0xC3};
    mod.functions.push_back(std::move(f));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "_realname",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "_weakalias",
                                       SymbolBinding::Weak,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep1;
    auto first = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep1);
    ASSERT_EQ(rep1.errorCount(), 0u) << diagSummary(rep1);

    DiagnosticReporter rrep;
    auto read = macho::readRelocatableObject(first, *loaded.target,
                                             *loaded.format, rrep);
    ASSERT_TRUE(read.has_value()) << diagSummary(rrep);
    ASSERT_EQ(read->functions.size(), 1u)
        << "two names at one address are ONE atom, not two";
    int named = 0;
    for (auto const& sym : read->symbols) {
        if (sym.name == "_realname" || sym.name == "_weakalias") ++named;
    }
    EXPECT_EQ(named, 2) << "both names must come back off the wire";

    DiagnosticReporter rep2;
    auto second =
        encodeUntrampolined(*read, *loaded.target, *loaded.format, rep2);
    ASSERT_EQ(rep2.errorCount(), 0u) << diagSummary(rep2);

    std::uint32_t const symoff = readU32LE(second, 216);
    std::uint32_t const nsyms  = readU32LE(second, 220);
    std::uint32_t const stroff = readU32LE(second, 224);
    auto nameAt = [&](std::uint32_t i) {
        std::uint32_t const strx = readU32LE(second, symoff + i * 16);
        std::string name;
        for (std::size_t q = stroff + strx;
             q < second.size() && second[q] != 0; ++q) {
            name.push_back(static_cast<char>(second[q]));
        }
        return name;
    };
    std::optional<std::uint32_t> a;
    std::optional<std::uint32_t> b;
    for (std::uint32_t i = 0; i < nsyms; ++i) {
        if (nameAt(i) == "_realname")  a = i;
        if (nameAt(i) == "_weakalias") b = i;
    }
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value())
        << "the alias name must survive a full read/re-emit cycle";
    EXPECT_EQ(readU64LE(second, symoff + *a * 16 + 8),
              readU64LE(second, symoff + *b * 16 + 8))
        << "and both must still resolve to ONE address";
    EXPECT_EQ(readU16LE(second, symoff + *b * 16 + 6), 0x0080u)
        << "the alias must still be WEAK after the round trip - the reader "
           "lifted N_WEAK_DEF and the writer put it back";
}

// ── relocation_info r_info packing ─────────────────────────────

TEST(MachOWriter, RelocationInfoPacksTypeLengthPcrelExternSymbolnum) {
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    caller.symbol = SymbolId{1};
    caller.bytes = {0xE8, 0x00, 0x00, 0x00, 0x00};  // call rel32
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};        // extern
    rel.kind   = RelocationKind{1};  // → BRANCH
    rel.addend = 0;                  // Mach-O convention
    caller.relocations.push_back(rel);
    mod.functions.push_back(std::move(caller));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // section_64.reloff @ offset 104 + 16 + 16 + 8 + 8 + 4 + 4 = 160
    std::uint32_t const relocOff = readU32LE(bytes, 160);
    std::uint32_t const relocCount = readU32LE(bytes, 164);
    ASSERT_EQ(relocCount, 1u);
    ASSERT_GT(relocOff, 0u);
    ASSERT_LE(relocOff + 8u, bytes.size());

    // r_address = 1 (patch site within .text)
    EXPECT_EQ(readU32LE(bytes, relocOff + 0), 1u);
    // r_info bits: type(28..31)=2(BRANCH); extern(27)=1; length(25..26)=2;
    // pcrel(24)=1; symbolnum(0..23) = symtab index of target.
    std::uint32_t const rInfo = readU32LE(bytes, relocOff + 4);
    EXPECT_EQ((rInfo >> 28) & 0xFu, 2u);          // r_type = BRANCH
    EXPECT_EQ((rInfo >> 27) & 0x1u, 1u);          // r_extern = 1
    EXPECT_EQ((rInfo >> 25) & 0x3u, 2u);          // r_length = 2 (4 bytes)
    EXPECT_EQ((rInfo >> 24) & 0x1u, 1u);          // r_pcrel = 1
    // symtab index: 0 = caller (defined), 1 = extern target.
    EXPECT_EQ(rInfo & 0x00FFFFFFu, 1u);
}

// ── D-LK3-MACHO-ARM64-OBJECT: the arm64 sibling object format ───

// The shipped macho64-arm64-darwin.format.json loads with the arm64 identity —
// the arm64 mirror of ShippedFileLoadsCleanly. RED-on-disable: delete the file →
// loadShipped fails.
TEST(MachOFormatJson, ShippedArm64FileLoadsCleanly) {
    auto loaded = loadShippedArm64();
    ASSERT_TRUE(loaded.format);
    EXPECT_EQ(loaded.format->kind(), ObjectFormatKind::MachO);
    EXPECT_EQ(loaded.format->name(), "macho64-arm64-darwin");
    // CPU_TYPE_ARM64 = (12 | CPU_ARCH_ABI64 0x01000000) = 0x0100000C.
    EXPECT_EQ(loaded.format->macho().cputype, 0x0100000Cu);
    EXPECT_EQ(loaded.format->macho().cpusubtype, 0u);   // CPU_SUBTYPE_ARM64_ALL
    EXPECT_TRUE(loaded.format->macho().filetype == MachOObjectType::Object);
    auto const* textRow = loaded.format->sectionByKind(SectionKind::Text);
    ASSERT_NE(textRow, nullptr);
    EXPECT_EQ(textRow->name, "__text");
    EXPECT_EQ(textRow->segment, "__TEXT");
    EXPECT_EQ(textRow->type, 0x80000400u);
    // The arm64 relocation vocabulary: BRANCH26(1)/PAGE21(2)/PAGEOFF12(3)/UNSIGNED(4)
    // — the walker validates emitted reloc kinds against these.
    EXPECT_EQ(loaded.format->relocationByKind(RelocationKind{2})->nativeId, 0x35000000u); // PAGE21
    EXPECT_EQ(loaded.format->relocationByKind(RelocationKind{3})->nativeId, 0x44000000u); // PAGEOFF12
}

// mach_header_64 golden bytes for arm64 — the arm64 mirror of the x86_64 header
// pin. The cputype byte (0x0100000C) is the distinguishing field a foreign
// toolchain reads to accept the .o as arm64 (proven live: `file` reports "Mach-O
// 64-bit object arm64" and system clang links it → exit 42, this cycle's witness).
TEST(MachOWriter, MachHeader64Arm64IdentityBytesMatchAppleAbi) {
    auto loaded = loadShippedArm64();
    AssembledModule mod = makeTrivialModule({0xC0, 0x03, 0x5F, 0xD6}, 42); // arm64 RET
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_GE(bytes.size(), 32u);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(readU32LE(bytes, 0), 0xFEEDFACFu);    // MH_MAGIC_64
    EXPECT_EQ(readU32LE(bytes, 4), 0x0100000Cu);    // cputype = CPU_TYPE_ARM64
    EXPECT_EQ(readU32LE(bytes, 8), 0u);             // cpusubtype = CPU_SUBTYPE_ARM64_ALL
    EXPECT_EQ(readU32LE(bytes, 12), 1u);            // filetype = MH_OBJECT
    // flags = MH_SUBSECTIONS_VIA_SYMBOLS — the arm64 mirror of the x86_64 pin,
    // where the rearguing of this expectation is written out in full
    // (D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM). Pinned
    // on BOTH arches deliberately: the value comes from each format schema
    // separately, so one arch declaring it and the other not is a real and
    // otherwise-invisible way for the two `.o` families to diverge.
    EXPECT_EQ(readU32LE(bytes, 24), 0x2000u);       // MH_SUBSECTIONS_VIA_SYMBOLS
}

// ── LC_BUILD_VERSION in a RELOCATABLE object (D-LK10-ENTRY-MACHO-EXIT) ──
//
// ✔MEASURED 2026-08-13, macOS 26.5.2 arm64, Apple `ld` PROJECT:ld-1267: an
// MH_OBJECT with no platform load command makes ld64 print `ld: warning: no
// platform load command found in '<tu>.o', assuming: macOS` for EVERY such
// object, and Apple `clang -c` emits the command into every `.o` it writes
// (LC_SEGMENT_64 / LC_BUILD_VERSION / LC_SYMTAB / LC_DYSYMTAB, in that order).

namespace {
constexpr std::uint32_t kLcBuildVersion = 0x32u;
// PLATFORM_MACOS, and 11.0 in the on-wire (major<<16)|(minor<<8)|patch form
// the schema loader produces from the JSON string "11.0".
constexpr std::uint32_t kPlatformMacOs  = 1u;
constexpr std::uint32_t kVersion11_0    = 0x000B0000u;
} // namespace

// ALL FOUR object-flavored darwin formats declare it, and the two `-staticlib`
// rows are the ones that matter most: an `ar` archive carries no load commands
// of its own, so the platform can only live in each MEMBER — and members are
// written by this same MH_OBJECT walker reading the STATICLIB schema
// (`linkAndWriteStaticArchive` hands it that format, not the bare relocatable
// sibling). Omit the key there and every member of a `.a` draws the warning.
// RED-ON-DISABLE: delete the `image` block from any one of the four files and
// exactly that row fails.
TEST(MachOFormatJson, AllRelocatableDarwinFormatsDeclareBuildVersion) {
    for (char const* name : {"macho64-arm64-darwin",
                             "macho64-x86_64-darwin",
                             "macho64-arm64-darwin-staticlib",
                             "macho64-x86_64-darwin-staticlib"}) {
        auto f = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(f.has_value()) << name;
        auto const& fmt = **f;
        ASSERT_TRUE(fmt.macho().filetype == MachOObjectType::Object) << name;
        auto const& bv = fmt.machoImage().buildVersion;
        ASSERT_TRUE(bv.has_value())
            << name << ": a darwin relocatable object must declare "
                       "image.buildVersion or ld64 warns on every link";
        EXPECT_EQ(static_cast<std::uint32_t>(bv->platform), kPlatformMacOs)
            << name;
        EXPECT_EQ(bv->minOs, kVersion11_0) << name;
        EXPECT_EQ(bv->sdk, kVersion11_0) << name;
    }
}

// The 24 emitted bytes, verbatim, AND the two header counters that must move
// with them. ncmds/sizeofcmds are the single most likely way to get this
// wrong: a load command the header does not count leaves ld64 walking off the
// end of the command list, and a sizeofcmds that disagrees makes every
// section_64.offset point into the wrong place.
TEST(MachOWriter, Arm64ObjectEmitsBuildVersionAndCorrectedHeaderCounts) {
    auto loaded = loadShippedArm64();
    AssembledModule mod = makeTrivialModule({0xC0, 0x03, 0x5F, 0xD6}, 42);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // Header counters. 3 = LC_SEGMENT_64 + LC_BUILD_VERSION + LC_SYMTAB;
    // 200 = 72 + 80 (segment + one section_64) + 24 + 24.
    EXPECT_EQ(readU32LE(bytes, 16), 3u);
    EXPECT_EQ(readU32LE(bytes, 20), 200u);

    // POSITION is part of the claim, not just presence: clang writes the
    // command between the segment and the symtab, so pin the literal offset
    // rather than only what `findLoadCommand` walks to.
    auto const at = dss::macho::test::findLoadCommand(bytes, kLcBuildVersion);
    ASSERT_TRUE(at.has_value())
        << "the MH_OBJECT walker must emit LC_BUILD_VERSION when the schema "
           "declares image.buildVersion";
    EXPECT_EQ(*at, 32u + 72u + 80u)   // header + segment cmd + one section_64
        << "LC_BUILD_VERSION sits after LC_SEGMENT_64 (+ its section_64 tail) "
           "and before LC_SYMTAB, matching clang's own `.o` command order";

    // build_version_command = 6 x u32: cmd / cmdsize / platform / minos /
    // sdk / ntools. All six pinned exactly.
    ASSERT_LE(*at + 24u, bytes.size());
    EXPECT_EQ(readU32LE(bytes, *at + 0),  kLcBuildVersion);
    EXPECT_EQ(readU32LE(bytes, *at + 4),  24u);
    EXPECT_EQ(readU32LE(bytes, *at + 8),  kPlatformMacOs);
    EXPECT_EQ(readU32LE(bytes, *at + 12), kVersion11_0);
    EXPECT_EQ(readU32LE(bytes, *at + 16), kVersion11_0);
    EXPECT_EQ(readU32LE(bytes, *at + 20), 0u)   // ntools
        << "no trailing build_tool_version records are emitted";

    // The counters are not merely plausible — they are CONSISTENT with the
    // bytes. sizeofcmds must land exactly on the first section byte, which is
    // where LC_SEGMENT_64.fileoff says the flat `.o` space begins.
    EXPECT_EQ(readU64LE(bytes, 72), 32u + readU32LE(bytes, 20));
}

// THE GATE, not just the emission: a Mach-O object format that declares NO
// `image.buildVersion` emits no LC_BUILD_VERSION and keeps the pre-change
// layout byte for byte. Without this pin the walker could ignore the schema
// and stamp the command unconditionally — which would be a hardcoded platform
// wearing a config key's name.
TEST(MachOWriter, ObjectWithoutSchemaBuildVersionEmitsNoLoadCommand) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
      "dataModel": "LP64",
      "headerNameMatching": "case-insensitive",
      "format": {"name":"macho-obj-no-buildversion","kind":"macho"},
      "macho": { "cputype": 16777228, "cpusubtype": 0, "filetype": "object", "flags": 0 },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":4,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_TRUE(format.has_value()) << rejectSummary(format);
    ASSERT_FALSE((*format)->machoImage().buildVersion.has_value());

    AssembledModule mod = makeTrivialModule({0xC0, 0x03, 0x5F, 0xD6}, 42);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, **format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    EXPECT_EQ(dss::macho::test::findLoadCommand(bytes, kLcBuildVersion),
              std::nullopt)
        << "no schema declaration ⇒ no LC_BUILD_VERSION";
    EXPECT_EQ(readU32LE(bytes, 16), 2u);     // ncmds = segment + symtab
    EXPECT_EQ(readU32LE(bytes, 20), 176u);   // sizeofcmds = 72 + 80 + 24
    EXPECT_EQ(readU64LE(bytes, 72), 208u);   // fileoff = 32 + 176
    EXPECT_EQ(readU32LE(bytes, 184), 0x02u); // LC_SYMTAB back at 184
}

// ★ THE PLATFORM IS DECLARED, NEVER ASSUMED. Same walker, same shipped arm64
// target, an object schema that says `ios` — and the emitted u32 is
// PLATFORM_IOS (2), not PLATFORM_MACOS (1). This is the assertion that would
// go red if anything in the object path hardcoded macOS or treated it as an
// implicit default, and it is the reason the vocabulary carries Apple's whole
// PLATFORM_* set instead of one row.
TEST(MachOWriter, ObjectBuildVersionPlatformComesFromTheSchemaNotAMacOsDefault) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto format = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
      "dataModel": "LP64",
      "headerNameMatching": "case-insensitive",
      "format": {"name":"macho-obj-ios","kind":"macho"},
      "macho": { "cputype": 16777228, "cpusubtype": 0, "filetype": "object", "flags": 0 },
      "image": { "buildVersion": { "platform": "ios", "minOs": "17.4", "sdk": "17.4" } },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":4,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_TRUE(format.has_value()) << rejectSummary(format);

    AssembledModule mod = makeTrivialModule({0xC0, 0x03, 0x5F, 0xD6}, 42);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, **format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    auto const at = dss::macho::test::findLoadCommand(bytes, kLcBuildVersion);
    ASSERT_TRUE(at.has_value());
    EXPECT_EQ(readU32LE(bytes, *at + 8), 2u)     // PLATFORM_IOS
        << "the emitted platform must be whatever the format DECLARED";
    EXPECT_EQ(readU32LE(bytes, *at + 12), 0x00110400u)   // 17.4.0
        << "17.4 encodes as (17<<16)|(4<<8)|0";
    EXPECT_EQ(readU32LE(bytes, *at + 16), 0x00110400u);
}

// The vocabulary itself. ✔MEASURED 2026-08-13 against the real header on the
// operator's Mac — `grep 'define PLATFORM_' "$(xcrun --show-sdk-path)/usr/
// include/mach-o/loader.h"` documents 1..24 with no conditional guard — and
// every spelling below was fed to `ld -platform_version <name> 1.0 1.0` and
// ACCEPTED by Apple `ld` PROJECT:ld-1267. The hyphens are Apple's, not this
// project's: `iossimulator` is REJECTED by that same ld64 while
// `ios-simulator` is accepted.
TEST(MachOBuildVersionPlatform, TableIsAppleSPlatformSetAndRoundTrips) {
    using Platform = MachOBuildVersion::Platform;
    struct Row { std::uint32_t value; char const* name; };
    // Apple's numbering, in Apple's order. The VALUE is written verbatim into
    // build_version_command.platform, so a wrong number here is a wrong byte
    // in a shipped file — this table is the ABI, not a convenience.
    constexpr Row kApple[] = {
        { 1,  "macos"                }, { 2,  "ios"                   },
        { 3,  "tvos"                 }, { 4,  "watchos"               },
        { 5,  "bridgeos"             }, { 6,  "mac-catalyst"          },
        { 7,  "ios-simulator"        }, { 8,  "tvos-simulator"        },
        { 9,  "watchos-simulator"    }, { 10, "driverkit"             },
        { 11, "visionos"             }, { 12, "visionos-simulator"    },
        { 13, "firmware"             }, { 14, "sepos"                 },
        { 15, "macos-exclavecore"    }, { 16, "macos-exclavekit"      },
        { 17, "ios-exclavecore"      }, { 18, "ios-exclavekit"        },
        { 19, "tvos-exclavecore"     }, { 20, "tvos-exclavekit"       },
        { 21, "watchos-exclavecore"  }, { 22, "watchos-exclavekit"    },
        { 23, "visionos-exclavecore" }, { 24, "visionos-exclavekit"   },
    };
    ASSERT_EQ(kMachOBuildVersionPlatformTable.rows.size(),
              std::size(kApple))
        << "the shipped table must cover Apple's whole PLATFORM_* range";
    for (auto const& row : kApple) {
        auto const decoded = machoBuildVersionPlatformFromName(row.name);
        ASSERT_TRUE(decoded.has_value()) << row.name;
        EXPECT_EQ(static_cast<std::uint32_t>(*decoded), row.value) << row.name;
        // Round-trip: the name a config author writes is the name the engine
        // reports back, so a diagnostic can never rename a platform.
        EXPECT_EQ(machoBuildVersionPlatformName(*decoded), row.name);
    }
    // PLATFORM_UNKNOWN (0) and PLATFORM_ANY (0xFFFFFFFF) are real macros and
    // deliberately UNLISTED — a file that declares "unknown" is worse than one
    // that declares nothing, and "any" is a matching wildcard, not a value a
    // producer stamps. Their absence must be a decision, not an oversight, so
    // it is pinned.
    EXPECT_EQ(machoBuildVersionPlatformFromName("unknown"), std::nullopt);
    EXPECT_EQ(machoBuildVersionPlatformFromName("any"), std::nullopt);
    // And a typo still fails loud rather than silently picking row 0.
    EXPECT_EQ(machoBuildVersionPlatformFromName("macosx"), std::nullopt);
    static_assert(static_cast<std::uint32_t>(Platform::Ios) == 2u,
                  "PLATFORM_IOS is 2 in <mach-o/loader.h>");
}

// The loader's MH_OBJECT rule changed shape: `image.buildVersion` is now
// ACCEPTED on a relocatable object while every other `image` key stays
// rejected. Both halves pinned in one place, because a change that only
// relaxed the first half would be indistinguishable from one that deleted the
// whole rule.
TEST(MachOFormatJsonValidate, ObjAcceptsBuildVersionButStillRejectsOtherImageKeys) {
    auto ok = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
      "dataModel": "LP64",
      "headerNameMatching": "case-insensitive",
      "format": {"name":"macho-obj-bv-only","kind":"macho"},
      "macho": { "cputype": 16777228, "cpusubtype": 0, "filetype": "object", "flags": 0 },
      "image": { "buildVersion": { "platform": "macos", "minOs": "11.0", "sdk": "11.0" } },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":4,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_TRUE(ok.has_value()) << rejectSummary(ok);
    ASSERT_TRUE((*ok)->machoImage().buildVersion.has_value());

    // The same schema plus ONE forbidden key is refused, and the `/image`
    // diagnostic is the sole reason — so the rejection is still driven by that
    // key and not by the buildVersion sitting next to it.
    auto bad = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
      "dataModel": "LP64",
      "headerNameMatching": "case-insensitive",
      "format": {"name":"macho-obj-bv-plus-pagezero","kind":"macho"},
      "macho": { "cputype": 16777228, "cpusubtype": 0, "filetype": "object", "flags": 0 },
      "image": { "buildVersion": { "platform": "macos", "minOs": "11.0", "sdk": "11.0" }, "pageZeroSize": 4294967296 },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":4,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(errorCount(bad), 1u) << rejectSummary(bad);
    EXPECT_EQ(countAtPath(bad, "/image"), 1u) << rejectSummary(bad);

    // An unknown platform name still fails loud — the vocabulary grew, it did
    // not open.
    auto typo = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
      "dataModel": "LP64",
      "headerNameMatching": "case-insensitive",
      "format": {"name":"macho-obj-bad-platform","kind":"macho"},
      "macho": { "cputype": 16777228, "cpusubtype": 0, "filetype": "object", "flags": 0 },
      "image": { "buildVersion": { "platform": "macosx", "minOs": "11.0", "sdk": "11.0" } },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":4,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(typo.has_value());
    EXPECT_EQ(countAtPath(typo, "/image/buildVersion/platform"), 1u)
        << rejectSummary(typo);
}

// ── D-LK-OBJECT-EXTERN-CALL-MACHO: undefined extern carries its REAL name ──

// The Mach-O analog of ElfWriter.ObjectExternCallEmitsUndefImportNameAndPlt32
// Reloc (c141): an arm64 MH_OBJECT with a BL to an extern import must emit the
// undefined symbol under its REAL pipeline-mangled import name (`_libc_fn`) as
// N_UNDF|N_EXT, with the BRANCH26 relocation's r_extern/r_symbolnum pointing at
// that symtab entry — so a FOREIGN linker (ld64/clang) resolves it against libc
// or a sibling object. Before this cycle the name was the internal `_sym_<id>`
// (the exact blocker the D-LK3-MACHO-ARM64-OBJECT cycle documented: ld64 cannot
// resolve `_sym_7`). RED-ON-DISABLE: revert the macho.cpp undefined-extern loop
// to the `_sym_` spelling → the name assertion fails (`_sym_20` != `_libc_fn`).
// The shipped format also declares `externCallDispatch: "direct-plt"` (pinned
// here) so the LOWERING tier accepts the extern call — DSS never builds a stub
// in a `.o`; ld64 synthesizes it (the ELF c141 direct-plt contract).
TEST(MachOWriter, Arm64ObjectExternCallEmitsUndefImportRealName) {
    auto loaded = loadShippedArm64();
    ASSERT_TRUE(loaded.target && loaded.format);
    ASSERT_TRUE(loaded.format->externCallDispatch().has_value())
        << "macho64-arm64-darwin must declare externCallDispatch so extern "
           "calls lower (D-LK-OBJECT-EXTERN-CALL-MACHO)";

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    caller.symbol = SymbolId{10};
    caller.bytes  = {0x00, 0x00, 0x00, 0x94};   // BL #0 (imm26 unresolved)
    caller.relocations.push_back(Relocation{/*offset=*/0u, /*target=*/SymbolId{20},
                                            /*kind=*/RelocationKind{1},  // BRANCH26
                                            /*addend=*/0});
    mod.functions.push_back(std::move(caller));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "_caller",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    ExternImport ext;
    ext.symbol      = SymbolId{20};
    ext.mangledName = "_libc_fn";     // pipeline-mangled (leading `_`, macho)
    ext.isData      = false;
    mod.externImports.push_back(std::move(ext));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "an MH_OBJECT with an extern-call import must encode";

    // LC_SYMTAB fields (single-segment/single-section MH_OBJECT layout, same
    // as the LcSymtab pin): cmd@208, symoff@216, nsyms@220, stroff@224.
    std::uint32_t const symoff = readU32LE(bytes, 216);
    std::uint32_t const nsyms  = readU32LE(bytes, 220);
    std::uint32_t const stroff = readU32LE(bytes, 224);
    ASSERT_EQ(nsyms, 2u) << "defined caller + undefined extern";

    // nlist_64[1] = the undefined extern (defined-then-undefined order).
    std::size_t const n1 = symoff + 16;
    std::uint32_t const nStrx = readU32LE(bytes, n1 + 0);
    std::string name;
    for (std::size_t p = stroff + nStrx; p < bytes.size() && bytes[p] != 0; ++p) {
        name.push_back(static_cast<char>(bytes[p]));
    }
    EXPECT_EQ(name, "_libc_fn")
        << "undefined extern must carry its REAL import name (verbatim, one "
           "leading underscore), never the internal _sym_<id> fallback";
    EXPECT_EQ(bytes[n1 + 4], 0x01u)      // n_type = N_UNDF|N_EXT
        << "extern is N_UNDF|N_EXT";
    EXPECT_EQ(bytes[n1 + 5], 0u)         // n_sect = NO_SECT
        << "undefined symbol carries no section";

    // The BRANCH26 relocation targets that symtab entry: r_extern=1,
    // r_symbolnum=1 (index of the extern nlist).
    std::uint32_t const relocOff = readU32LE(bytes, 160);
    std::uint32_t const rInfo    = readU32LE(bytes, relocOff + 4);
    EXPECT_EQ((rInfo >> 27) & 0x1u, 1u)  << "r_extern = 1 (symbol-relative)";
    EXPECT_EQ(rInfo & 0x00FFFFFFu, 1u)   << "r_symbolnum = the extern's index";
}

// An ARM64-DISTINCT relocation (PAGE21, kind=2 → r_type=3) packs correctly — proves
// the arm64 format's reloc table is loaded, NOT x86_64's (whose kind=2 is UNSIGNED,
// r_type=0). RED-on-disable: a wrong nativeId in the format flips the packed r_type.
TEST(MachOWriter, Arm64RelocationInfoPacksPage21) {
    auto loaded = loadShippedArm64();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    caller.symbol = SymbolId{1};
    caller.bytes = {0x00, 0x00, 0x00, 0x90, 0xC0, 0x03, 0x5F, 0xD6}; // adrp x0,#0 ; ret
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{2};        // extern
    rel.kind   = RelocationKind{2};  // → ARM64_RELOC_PAGE21
    rel.addend = 0;
    caller.relocations.push_back(rel);
    mod.functions.push_back(std::move(caller));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    std::uint32_t const relocOff = readU32LE(bytes, 160);
    ASSERT_EQ(readU32LE(bytes, 164), 1u);           // one reloc
    ASSERT_LE(relocOff + 8u, bytes.size());
    std::uint32_t const rInfo = readU32LE(bytes, relocOff + 4);
    EXPECT_EQ((rInfo >> 28) & 0xFu, 3u);            // r_type = 3 (PAGE21) — NOT x86's 0
    EXPECT_EQ((rInfo >> 27) & 0x1u, 1u);            // r_extern
    EXPECT_EQ((rInfo >> 25) & 0x3u, 2u);            // r_length = 2 (4 bytes)
    EXPECT_EQ((rInfo >> 24) & 0x1u, 1u);            // r_pcrel = 1
    EXPECT_EQ(rInfo & 0x00FFFFFFu, 1u);             // symtab index of the extern target
}

// ── D-LK-OBJECT-DATA-SECTION-RELOCATABLE (Mach-O MH_OBJECT arm, c147) ──
//
// The arm64 MH_OBJECT writer emits DATA sections — the Mach-O analog of
// the ELF c142 ET_REL arm (+ the c145 data-relocation emission). Layout
// constants for a TWO-section .o (used by the pins below):
//   header 32 | LC_SEGMENT_64 @32 (segname@40, vmaddr@56, vmsize@64,
//   fileoff@72, filesize@80, nsects@96) | section_64[0] __text @104
//   (reloff@160, nreloc@164) | section_64[1] @184 (sectname@184,
//   segname@200, addr@216, size@224, offset@232, align@236, reloff@240,
//   nreloc@244, flags@248) | LC_BUILD_VERSION @264 | LC_SYMTAB @288
//   (symoff@296, nsyms@300, stroff@304) | section bytes @312
//   (= 32 + 72 + 2*80 + 24 + 24).
//
// ⚠ The section_64 offsets above are UNSHIFTED by LC_BUILD_VERSION and that
// is not an oversight: the section records are the LC_SEGMENT_64 command's
// own tail, so they precede every later load command. Only what follows the
// segment command moved (D-LK10-ENTRY-MACHO-EXIT, MH_OBJECT arm).

namespace {
// Read a NUL-terminated name from the string table.
[[nodiscard]] std::string readStrtabName(std::vector<std::uint8_t> const& bytes,
                                          std::size_t stroff,
                                          std::uint32_t nStrx) {
    std::string name;
    for (std::size_t p = stroff + nStrx; p < bytes.size() && bytes[p] != 0; ++p)
        name.push_back(static_cast<char>(bytes[p]));
    return name;
}
// Compare a 16-byte section_64 name field with an expected string.
[[nodiscard]] bool name16Equals(std::vector<std::uint8_t> const& bytes,
                                 std::size_t off, std::string_view expect) {
    char buf[17] = {};
    std::memcpy(buf, bytes.data() + off, 16);
    return std::string_view{buf} == expect;
}
} // namespace

// (1) A function + one rodata item: the section count grows to 2, the
// `__TEXT,__const` section_64 carries the right sectname/segname/addr/
// offset/size/flags, the data symbol's nlist is N_SECT|N_EXT with
// n_sect = 2 and n_value = the FLAT address (section addr + item offset —
// Mach-O n_value is an address, not ELF's section-relative st_value), and
// the name is the real pre-mangled `_msg` (definedName). RED-on-disable:
// revert the data-section emission → nsects stays 1 and nsyms stays 1.
TEST(MachOWriter, Arm64ObjectRodataItemEmitsConstSectionAndDataSymbol) {
    auto loaded = loadShippedArm64();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC0, 0x03, 0x5F, 0xD6};   // arm64 RET (4 bytes)
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "_greet",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::Rodata;
    d.bytes     = {'h', 'i', 0};
    d.alignment = Alignment::of<1>();
    mod.dataItems.push_back(std::move(d));
    mod.symbols.push_back(ModuleSymbol{SymbolId{42}, "_msg",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // Two sections: sizeofcmds = 72 + 2*80 + 24 (build_version) + 24 = 280;
    // nsects = 2, ncmds = 3.
    EXPECT_EQ(readU32LE(bytes, 16), 3u);     // ncmds
    EXPECT_EQ(readU32LE(bytes, 20), 280u);   // sizeofcmds
    EXPECT_EQ(readU32LE(bytes, 96), 2u);     // nsects
    // Flat layout: text 4 bytes at addr 0; rodata at alignUp(4, 8) = 8
    // (schema floor 8 raw bytes), span 3. vmsize = filesize = 11.
    EXPECT_EQ(readU64LE(bytes, 64), 11u);    // vmsize
    EXPECT_EQ(readU64LE(bytes, 72), 312u);   // fileoff (header+cmds)
    EXPECT_EQ(readU64LE(bytes, 80), 11u);    // filesize (all file-backed)

    // section_64[1] = __TEXT,__const. UNMOVED by LC_BUILD_VERSION — the
    // section records are the segment command's own tail.
    EXPECT_TRUE(name16Equals(bytes, 184, "__const"));
    EXPECT_TRUE(name16Equals(bytes, 200, "__TEXT"));
    EXPECT_EQ(readU64LE(bytes, 216), 8u);    // addr (flat space)
    EXPECT_EQ(readU64LE(bytes, 224), 3u);    // size
    EXPECT_EQ(readU32LE(bytes, 232), 320u);  // offset = 312 + addr 8
    EXPECT_EQ(readU32LE(bytes, 236), 3u);    // align = log2(8)
    EXPECT_EQ(readU32LE(bytes, 240), 0u);    // reloff (no relocs)
    EXPECT_EQ(readU32LE(bytes, 244), 0u);    // nreloc
    EXPECT_EQ(readU32LE(bytes, 248), 0u);    // flags = S_REGULAR
    // The item's bytes land at the section's file offset.
    ASSERT_GE(bytes.size(), 323u);
    EXPECT_EQ(bytes[320], 'h');
    EXPECT_EQ(bytes[321], 'i');
    EXPECT_EQ(bytes[322], 0u);

    // LC_SYMTAB @288: symoff = 312 + 11 (no relocs), nsyms = 2.
    std::uint32_t const symoff = readU32LE(bytes, 296);
    std::uint32_t const nsyms  = readU32LE(bytes, 300);
    std::uint32_t const stroff = readU32LE(bytes, 304);
    EXPECT_EQ(symoff, 323u);
    ASSERT_EQ(nsyms, 2u);
    // nlist[1] = the data symbol: real name, N_SECT|N_EXT, n_sect=2,
    // n_value = the FLAT address 8.
    std::size_t const n1 = symoff + 16;
    EXPECT_EQ(readStrtabName(bytes, stroff, readU32LE(bytes, n1)), "_msg");
    EXPECT_EQ(bytes[n1 + 4], 0x0Fu);         // N_SECT | N_EXT
    EXPECT_EQ(bytes[n1 + 5], 2u);            // n_sect = __const ordinal
    EXPECT_EQ(readU64LE(bytes, n1 + 8), 8u); // n_value = flat address
}

// ── D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION (TF-C54):
//    the DATA emit site drops N_EXT for a static item, as the function site ──
//
// The n_type binding lives at a SECOND, fully-duplicated emit site (the data
// loop), NOT shared with the function loop — the design-audit flagged this data
// twin as unpinned. A static (Local) DATA item carves to `_sym_<id>` + bare
// N_SECT (0x0E, NO N_EXT); the Global half is pinned by
// Arm64ObjectRodataItemEmitsConstSectionAndDataSymbol above (0x0F). A
// string-literal / `static const` rodata is exactly this Local shape, and
// pre-fix it collided across TUs as N_SECT|N_EXT `_sym_<id>`. RED-ON-DISABLE:
// reverting the data-site ternary (macho.cpp) to a hardcoded N_SECT|N_EXT makes
// this read 0x0F → the ==0x0E assertion goes red.
TEST(MachOWriter, Arm64ObjectStaticDataItemDropsNExt) {
    auto loaded = loadShippedArm64();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC0, 0x03, 0x5F, 0xD6};   // arm64 RET
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "_greet",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::Rodata;
    d.bytes     = {'h', 'i', 0};
    d.alignment = Alignment::of<1>();
    mod.dataItems.push_back(std::move(d));
    // STATIC (Local) data → carved `_sym_42` + bare N_SECT (NO N_EXT). Same flat
    // layout as the Global test above, so the LC_SYMTAB constants match; only
    // the name and the N_EXT bit change.
    mod.symbols.push_back(ModuleSymbol{SymbolId{42}, "_msg",
                                       SymbolBinding::Local,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    std::uint32_t const symoff = readU32LE(bytes, 296);
    std::uint32_t const stroff = readU32LE(bytes, 304);
    ASSERT_EQ(readU32LE(bytes, 300), 2u);   // nsyms = fn + data
    std::size_t const n1 = symoff + 16;     // nlist[1] = the data symbol
    EXPECT_EQ(readStrtabName(bytes, stroff, readU32LE(bytes, n1)), "_sym_42")
        << "a static (Local) data item stays internal `_sym_<id>`";
    EXPECT_EQ(bytes[n1 + 4], 0x0Eu)   // bare N_SECT — THE FIX
        << "static data drops N_EXT (0x0E), not the pre-fix N_SECT|N_EXT (0x0F)";
}

// D-LK-OBJECT-WEAK-DEF-RELOCATABLE, the DATA twin, on the arm64 leg. The data
// emit site is separate code from the function site and the two drifting apart
// is the exact defect D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION
// was, so it gets its own pin. Asserts the bit AND the round trip: the bit
// alone would pass over a reader that ignores it, and the round trip alone
// would pass over a writer that set the wrong bit if the reader read the same
// wrong bit.
//
// RED-ON-DISABLE: route the data site around `definedNDesc` → n_desc reads 0
// and the read-back binding falls to Global.
TEST(MachOWriter, Arm64ObjectWeakDefinedDataEmitsNWeakDefAndRoundTrips) {
    auto loaded = loadShippedArm64();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;   // a benign anchor fn so only the DATA is weak
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC0, 0x03, 0x5F, 0xD6};   // arm64 RET
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "_anchor",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    AssembledData sd;
    sd.symbol    = SymbolId{9};
    sd.section   = DataSectionKind::Data;
    sd.bytes     = {9, 9, 9, 9};
    sd.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(sd));
    mod.symbols.push_back(ModuleSymbol{SymbolId{9}, "_strongdat",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    AssembledData d;
    d.symbol    = SymbolId{10};
    d.section   = DataSectionKind::Data;
    d.bytes     = {1, 2, 3, 4};
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "_weakdat",
                                       SymbolBinding::Weak,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(readU32LE(bytes, 24) & 0x8000u, 0u)
        << "MH_WEAK_DEFINES stays off an MH_OBJECT on the data path too";

    DiagnosticReporter rrep;
    auto back = macho::readRelocatableObject(bytes, *loaded.target,
                                             *loaded.format, rrep);
    ASSERT_TRUE(back.has_value());
    auto bindingOf = [&](std::string const& name)
        -> std::optional<SymbolBinding> {
        for (auto const& sym : back->symbols) {
            if (sym.name == name) return sym.binding;
        }
        return std::nullopt;
    };
    EXPECT_EQ(bindingOf("_weakdat").value_or(SymbolBinding::Global),
              SymbolBinding::Weak)
        << "a weak DATA definition must round-trip back to Weak";
    EXPECT_EQ(bindingOf("_strongdat").value_or(SymbolBinding::Weak),
              SymbolBinding::Global);
    // ★ THE COUNTERS ARE LOAD-BEARING, not bookkeeping. Both byte comparisons
    // below sit inside a NESTED MATCH LOOP with no floor on how often the match
    // succeeds: if the reader handed back no data items at all -- or items whose
    // SymbolIds matched no `symbols` row -- neither `EXPECT_EQ` would execute
    // and this test would pass having compared NOTHING. The bytes are the half
    // that separates "the right binding" from "the right binding over the wrong
    // bytes", which is the silent miscompile this cell exists for, so the pin
    // must state that it reached them. EXACTLY once each: a duplicate row would
    // mean the round-trip invented a second item for one atom.
    unsigned weakByteChecks = 0;
    unsigned strongByteChecks = 0;
    for (auto const& item : back->dataItems) {
        for (auto const& sym : back->symbols) {
            if (sym.symbol != item.symbol) continue;
            if (sym.name == "_weakdat") {
                ++weakByteChecks;
                EXPECT_EQ(item.bytes, (std::vector<std::uint8_t>{1, 2, 3, 4}))
                    << "the weak item's bytes must survive intact - the right "
                       "binding over the wrong bytes is a silent miscompile";
            } else if (sym.name == "_strongdat") {
                ++strongByteChecks;
                EXPECT_EQ(item.bytes, (std::vector<std::uint8_t>{9, 9, 9, 9}));
            }
        }
    }
    EXPECT_EQ(weakByteChecks, 1u)
        << "the weak item's bytes must have been COMPARED exactly once - zero "
           "means the loop above never matched and this cell asserted nothing "
           "about the bytes";
    EXPECT_EQ(strongByteChecks, 1u)
        << "and so must the strong item's, for the same reason";
}

// (2) A RelRoConst item carrying an abs64 reloc to a DEFINED function (a
// const fn-ptr table slot): the `__DATA,__const` section carries its OWN
// relocation_info table (the c145 `.rela.data.rel.ro` analog) — nreloc=1,
// r_extern=1 / r_pcrel=0 / r_length=3 / r_type=0 (ARM64_RELOC_UNSIGNED),
// r_symbolnum = the function's symtab index, r_address = the slot's
// SECTION-relative offset. The relocation's addend is written INTO the
// 8-byte slot (Mach-O's in-place-addend convention — relocation_info has
// no addend column; ld64 computes S + slot). RED-on-disable: drop the
// data-reloc emission → nreloc reads 0; drop the in-slot addend → the
// slot reads 0.
TEST(MachOWriter, Arm64ObjectRelRoFnPtrSlotEmitsUnsignedRelocAndInSlotAddend) {
    auto loaded = loadShippedArm64();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction f1;
    f1.symbol = SymbolId{1};
    f1.bytes  = {0xC0, 0x03, 0x5F, 0xD6};   // RET
    mod.functions.push_back(std::move(f1));
    AssembledData tab;
    tab.symbol    = SymbolId{7};
    tab.section   = DataSectionKind::RelRoConst;
    tab.bytes.assign(8, 0);                  // one pointer slot
    tab.alignment = Alignment::of<8>();
    tab.relocations.push_back(Relocation{/*offset=*/0u,
                                         /*target=*/SymbolId{1},
                                         /*kind=*/RelocationKind{4},  // UNSIGNED
                                         /*addend=*/8});
    mod.dataItems.push_back(std::move(tab));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // Two sections; relro at alignUp(4, 8) = 8, span 8 → filesize 16.
    EXPECT_EQ(readU32LE(bytes, 96), 2u);     // nsects
    EXPECT_TRUE(name16Equals(bytes, 184, "__const"));
    EXPECT_TRUE(name16Equals(bytes, 200, "__DATA"));
    EXPECT_EQ(readU64LE(bytes, 216), 8u);    // addr
    EXPECT_EQ(readU64LE(bytes, 224), 8u);    // size
    EXPECT_EQ(readU32LE(bytes, 232), 320u);  // offset = 312 + 8
    std::uint32_t const reloff = readU32LE(bytes, 240);
    std::uint32_t const nreloc = readU32LE(bytes, 244);
    ASSERT_EQ(nreloc, 1u)
        << "the relro section must carry its own relocation_info table "
           "(the .rela.data.rel.ro analog)";
    // Reloc tables follow ALL file-backed section bytes: 312 + 16.
    EXPECT_EQ(reloff, 328u);
    ASSERT_LE(reloff + 8u, bytes.size());
    // r_address = the slot's offset WITHIN its section.
    EXPECT_EQ(readU32LE(bytes, reloff + 0), 0u);
    std::uint32_t const rInfo = readU32LE(bytes, reloff + 4);
    EXPECT_EQ((rInfo >> 28) & 0xFu, 0u);     // r_type = ARM64_RELOC_UNSIGNED
    EXPECT_EQ((rInfo >> 27) & 0x1u, 1u);     // r_extern = 1
    EXPECT_EQ((rInfo >> 25) & 0x3u, 3u);     // r_length = 3 (8 bytes)
    EXPECT_EQ((rInfo >> 24) & 0x1u, 0u);     // r_pcrel = 0
    EXPECT_EQ(rInfo & 0x00FFFFFFu, 0u)       // r_symbolnum = f1's index
        << "the slot must target the DEFINED function's symtab entry";
    // In-place addend: the 8-byte slot at file offset 320 carries 8.
    EXPECT_EQ(readU64LE(bytes, 320), 8u)
        << "Mach-O has no RELA addend column — rel.addend must be written "
           "into the slot bytes (ld64 computes S + slot)";
    // The table's own symbol: nlist[1], n_sect=2, n_value=8.
    std::uint32_t const symoff = readU32LE(bytes, 296);
    ASSERT_EQ(readU32LE(bytes, 300), 2u);    // nsyms = f1 + tab
    EXPECT_EQ(bytes[symoff + 16 + 5], 2u);
    EXPECT_EQ(readU64LE(bytes, symoff + 16 + 8), 8u);
}

// A jump-table data slot targeting a SYNTHETIC PER-BLOCK symbol (the dense-
// switch / computed-goto shape: `AssembledData` abs64 reloc → an interior
// `__text` offset recorded in `fn.blockSymbols`) must resolve to a DEFINED
// LOCAL nlist — N_SECT with NO N_EXT, n_sect=1, n_value = the flat text
// address — never the undefined-extern fallback. Before the c147 review fold
// the MH_OBJECT writer skipped block-symbol registration, so the extern scan
// fabricated an N_UNDF|N_EXT `_sym_<id>` nothing could ever define: a
// cleanly-emitted but unlinkable .o (ld64 "undefined symbol '_sym_77'"), or
// — worse — a silent wrong-control-flow bind against a sibling object's
// unrelated `_sym_77` export. The ELF ET_REL writer's STB_LOCAL block-symbol
// leg is the mirror. RED-ON-DISABLE: drop the block-symbol registration →
// nlist[1] flips to the data symbol and the block lands N_UNDF|N_EXT at the
// tail → the n_type/n_value assertions fail.
TEST(MachOWriter, Arm64ObjectJumpTableBlockSymbolIsLocalDefinedNotUndef) {
    auto loaded = loadShippedArm64();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{10};
    fn.bytes  = {0xC0, 0x03, 0x5F, 0xD6,     // RET (block 0)
                 0xC0, 0x03, 0x5F, 0xD6};    // RET (block 1, offset 4)
    fn.blockSymbols.push_back({SymbolId{77}, /*blockByteOffset=*/4u});
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "_dispatch",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    AssembledData tab;                        // one jump-table slot
    tab.symbol    = SymbolId{7};
    tab.section   = DataSectionKind::Data;
    tab.bytes.assign(8, 0);
    tab.alignment = Alignment::of<8>();
    tab.relocations.push_back(Relocation{/*offset=*/0u,
                                         /*target=*/SymbolId{77},   // the BLOCK
                                         /*kind=*/RelocationKind{4}, // UNSIGNED
                                         /*addend=*/0});
    mod.dataItems.push_back(std::move(tab));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "a jump-table slot targeting a block symbol must encode";

    // Symbol order: func(0) → BLOCK local(1) → data(2). No undefined extern.
    std::uint32_t const symoff = readU32LE(bytes, 296);
    std::uint32_t const nsyms  = readU32LE(bytes, 300);
    std::uint32_t const stroff = readU32LE(bytes, 304);
    ASSERT_EQ(nsyms, 3u) << "func + block local + data symbol - and NO "
                            "fabricated undefined extern for the block";
    std::size_t const n1 = symoff + 16;      // the block symbol's nlist
    std::uint32_t const nStrx = readU32LE(bytes, n1 + 0);
    std::string name;
    for (std::size_t p = stroff + nStrx; p < bytes.size() && bytes[p] != 0; ++p) {
        name.push_back(static_cast<char>(bytes[p]));
    }
    EXPECT_EQ(name, "_sym_77");
    EXPECT_EQ(bytes[n1 + 4], 0x0Eu)
        << "block symbol must be N_SECT LOCAL (0x0E) - N_EXT would let a "
           "sibling object's unrelated _sym_<id> bind to an interior block "
           "address (silent wrong-control-flow); N_UNDF (0x01) is the "
           "fabricated-extern break this pin guards";
    EXPECT_EQ(bytes[n1 + 5], 1u)             // n_sect = __text
        << "block symbol lives in __text";
    // n_desc = N_ALT_ENTRY (0x0200) — D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS
    // -BLOCK-LABEL-NOT-ATOM. n_type/n_sect/n_value above are byte-for-byte what
    // a file-local (`static`) whole FUNCTION also carries, so before this bit
    // existed nothing on the wire distinguished the two and the archive-member
    // reader had to guess from N_EXT — guessing wrong for every `static`
    // function and dropping its body. N_ALT_ENTRY is the format's own word for
    // "an alternate entry INTO the atom before me, not an atom", and it is what
    // makes the header's MH_SUBSECTIONS_VIA_SYMBOLS declaration honest.
    // ⚠ 0x0200, NOT 0x0020: ✔MEASURED against clang 19 targeting
    // arm64-apple-macos, an explicit `.alt_entry` emits n_desc=0x0200 while
    // `__attribute__((used))` emits 0x0020 (N_NO_DEAD_STRIP) — a different bit
    // with a different meaning. The paired pin is
    // `ObjectNlistCouplesNameAndBindingStaticLocalDropsNExt`, which asserts a
    // whole function carries n_desc = 0.
    // RED-ON-DISABLE: revert the block-symbol loop to n_desc = 0 and this fails.
    EXPECT_EQ(static_cast<std::uint16_t>(
                  bytes[n1 + 6] | (static_cast<std::uint16_t>(bytes[n1 + 7]) << 8)),
              0x0200u)
        << "synthetic block label must carry N_ALT_ENTRY (0x0200) — it is the "
           "ONLY thing distinguishing it from a file-local whole function, "
           "which has identical n_type/n_sect and no size field";
    EXPECT_EQ(readU64LE(bytes, n1 + 8), 4u)  // n_value = flat text addr
        << "n_value = funcTextStart + blockByteOffset (flat address)";

    // The data section's reloc resolves r_symbolnum to the block (index 1).
    std::uint32_t const reloff = readU32LE(bytes, 240);
    std::uint32_t const nreloc = readU32LE(bytes, 244);
    ASSERT_EQ(nreloc, 1u);
    std::uint32_t const rInfo = readU32LE(bytes, reloff + 4);
    EXPECT_EQ((rInfo >> 27) & 0x1u, 1u);     // r_extern = 1 (symbol-relative)
    EXPECT_EQ(rInfo & 0x00FFFFFFu, 1u)
        << "the jump-table slot targets the DEFINED block local, not a "
           "fabricated undefined extern";
}

// (3) A bss item: S_ZEROFILL flags from the schema row, offset = 0, size =
// reservedSize, NO file bytes — vmsize covers the zero-fill tail, filesize
// does not. RED-on-disable: emitting bss as file-backed flips offset/
// filesize; dropping it flips nsects.
TEST(MachOWriter, Arm64ObjectBssItemIsZeroFillWithVmsizeButNoFileBytes) {
    auto loaded = loadShippedArm64();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC0, 0x03, 0x5F, 0xD6};
    mod.functions.push_back(std::move(fn));
    AssembledData g;
    g.symbol       = SymbolId{9};
    g.section      = DataSectionKind::Bss;
    g.reservedSize = 4;                      // int g; — no file bytes
    g.alignment    = Alignment::of<4>();
    mod.dataItems.push_back(std::move(g));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    EXPECT_EQ(readU32LE(bytes, 96), 2u);     // nsects
    // bss at alignUp(4, 8) = 8 (schema floor 8), span 4.
    EXPECT_EQ(readU64LE(bytes, 64), 12u)     // vmsize INCLUDES zero-fill
        << "LC_SEGMENT_64.vmsize must cover the zero-fill tail";
    EXPECT_EQ(readU64LE(bytes, 80), 4u)      // filesize EXCLUDES it
        << "LC_SEGMENT_64.filesize must cover only file-backed bytes";
    EXPECT_TRUE(name16Equals(bytes, 184, "__bss"));
    EXPECT_TRUE(name16Equals(bytes, 200, "__DATA"));
    EXPECT_EQ(readU64LE(bytes, 216), 8u);    // addr still advances
    EXPECT_EQ(readU64LE(bytes, 224), 4u);    // size = reservedSize
    EXPECT_EQ(readU32LE(bytes, 232), 0u);    // offset = 0 (S_ZEROFILL)
    EXPECT_EQ(readU32LE(bytes, 248), 1u);    // flags = S_ZEROFILL (schema)
    // symtab directly after text bytes (312 + 4) — bss stored nothing.
    EXPECT_EQ(readU32LE(bytes, 296), 316u);
    // The bss symbol's n_value is its flat address.
    std::uint32_t const symoff = readU32LE(bytes, 296);
    EXPECT_EQ(bytes[symoff + 16 + 5], 2u);
    EXPECT_EQ(readU64LE(bytes, symoff + 16 + 8), 8u);
}

// (4) A DATA-FREE module keeps the exact SINGLE-SECTION layout:
// ncmds/sizeofcmds/nsects/vmsize/filesize/symoff/stroff are what one
// `__text` section derives, so the data-section machinery is a no-op when no
// data items exist (every LcSymtab/reloc pin above hardcodes this layout).
// ⚠ The figures are the SINGLE-SECTION ones, not the pre-c147 ones — the
// data machinery still adds nothing here, but LC_BUILD_VERSION does, and
// conflating "no data sections" with "unchanged since c147" is how a stale
// constant survives a real layout change.
TEST(MachOWriter, Arm64ObjectDataFreeModuleKeepsSingleSectionLayout) {
    auto loaded = loadShippedArm64();
    AssembledModule mod = makeTrivialModule({0xC0, 0x03, 0x5F, 0xD6}, 42);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(readU32LE(bytes, 16), 3u);     // ncmds (+ LC_BUILD_VERSION)
    EXPECT_EQ(readU32LE(bytes, 20), 200u);   // sizeofcmds = 72 + 80 + 24 + 24
    EXPECT_EQ(readU32LE(bytes, 96), 1u);     // nsects = 1 (__text only)
    EXPECT_EQ(readU64LE(bytes, 64), 4u);     // vmsize = text only
    EXPECT_EQ(readU64LE(bytes, 72), 232u);   // fileoff = 32 + 72 + 80 + 24 + 24
    EXPECT_EQ(readU64LE(bytes, 80), 4u);     // filesize = text only
    // LC_SYMTAB at 208; symtab right after text (232 + 4); strtab after
    // the single 16-byte nlist.
    EXPECT_EQ(readU32LE(bytes, 208), 0x02u);
    EXPECT_EQ(readU32LE(bytes, 216), 236u);  // symoff
    EXPECT_EQ(readU32LE(bytes, 220), 1u);    // nsyms
    EXPECT_EQ(readU32LE(bytes, 224), 252u);  // stroff
}

// (5) The shipped arm64 object format declares the four data-section rows
// + supportedDataSections (the linker's acceptsDataSection gate) — and
// does NOT declare TLS (tdata/tbss stay gate-rejected). RED-on-disable:
// remove a JSON row → the corresponding assertion fails.
TEST(MachOFormatJson, Arm64ObjectDeclaresDataSectionRows) {
    auto loaded = loadShippedArm64();
    ASSERT_TRUE(loaded.format);
    auto const& fmt = *loaded.format;
    EXPECT_TRUE(fmt.acceptsDataSection(DataSectionKind::Rodata));
    EXPECT_TRUE(fmt.acceptsDataSection(DataSectionKind::Data));
    EXPECT_TRUE(fmt.acceptsDataSection(DataSectionKind::Bss));
    EXPECT_TRUE(fmt.acceptsDataSection(DataSectionKind::RelRoConst));
    EXPECT_FALSE(fmt.acceptsDataSection(DataSectionKind::Tdata))
        << "TLS stays undeclared on the MH_OBJECT schema (fail-loud gate)";
    EXPECT_FALSE(fmt.acceptsDataSection(DataSectionKind::Tbss));

    auto const* rodata = fmt.sectionByKind(SectionKind::Rodata);
    ASSERT_NE(rodata, nullptr);
    EXPECT_EQ(rodata->name, "__const");
    EXPECT_EQ(rodata->segment, "__TEXT");
    EXPECT_EQ(rodata->type, 0u);             // S_REGULAR
    auto const* data = fmt.sectionByKind(SectionKind::Data);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->name, "__data");
    EXPECT_EQ(data->segment, "__DATA");
    EXPECT_EQ(data->type, 0u);
    auto const* relro = fmt.sectionByKind(SectionKind::RelRoConst);
    ASSERT_NE(relro, nullptr);
    EXPECT_EQ(relro->name, "__const")
        << "relro is __DATA,__const — the .o precursor of __DATA_CONST";
    EXPECT_EQ(relro->segment, "__DATA");
    EXPECT_EQ(relro->type, 0u);
    auto const* bss = fmt.sectionByKind(SectionKind::Bss);
    ASSERT_NE(bss, nullptr);
    EXPECT_EQ(bss->name, "__bss");
    EXPECT_EQ(bss->segment, "__DATA");
    EXPECT_EQ(bss->type, 1u);                // S_ZEROFILL
}

// (6) A reloc-bearing RODATA item stays fail-loud (allow=false — the
// RodataDataItemWithRelocationFailsLoud discipline shared with ELF): a
// relocated slot cannot live in never-written `__TEXT,__const`.
TEST(MachOWriter, Arm64ObjectRodataItemWithRelocationFailsLoud) {
    auto loaded = loadShippedArm64();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC0, 0x03, 0x5F, 0xD6};
    mod.functions.push_back(std::move(fn));
    AssembledData d;
    d.symbol    = SymbolId{3};
    d.section   = DataSectionKind::Rodata;
    d.bytes.assign(8, 0);
    d.alignment = Alignment::of<8>();
    d.relocations.push_back(Relocation{0u, SymbolId{1}, RelocationKind{4}, 0});
    mod.dataItems.push_back(std::move(d));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    EXPECT_GT(rep.errorCount(), 0u);
}

// (7) The c145 extern-coverage mirror: a target referenced ONLY by a
// DATA-item relocation (a const table of libc fn pointers) still gets its
// N_UNDF|N_EXT nlist under the REAL import name, and the data reloc's
// r_symbolnum points at it. RED-on-disable: scan only function relocs →
// the extern nlist is never emitted (K_SymbolUndefined aborts the encode).
TEST(MachOWriter, Arm64ObjectDataRelocOnlyExternGetsUndefNlist) {
    auto loaded = loadShippedArm64();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC0, 0x03, 0x5F, 0xD6};   // RET — NO relocations
    mod.functions.push_back(std::move(fn));
    AssembledData tab;
    tab.symbol    = SymbolId{7};
    tab.section   = DataSectionKind::RelRoConst;
    tab.bytes.assign(8, 0);
    tab.alignment = Alignment::of<8>();
    tab.relocations.push_back(Relocation{0u, SymbolId{20},
                                         RelocationKind{4}, 0});
    mod.dataItems.push_back(std::move(tab));
    ExternImport ext;
    ext.symbol      = SymbolId{20};
    ext.mangledName = "_puts";
    ext.isData      = false;
    mod.externImports.push_back(std::move(ext));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "a data-reloc-only extern must be covered by the undefined-"
           "extern scan (the ELF c145 mirror)";
    std::uint32_t const symoff = readU32LE(bytes, 296);
    std::uint32_t const nsyms  = readU32LE(bytes, 300);
    std::uint32_t const stroff = readU32LE(bytes, 304);
    ASSERT_EQ(nsyms, 3u) << "f1 + table + undefined _puts";
    std::size_t const n2 = symoff + 2 * 16;
    EXPECT_EQ(readStrtabName(bytes, stroff, readU32LE(bytes, n2)), "_puts");
    EXPECT_EQ(bytes[n2 + 4], 0x01u);         // N_UNDF | N_EXT
    EXPECT_EQ(bytes[n2 + 5], 0u);            // NO_SECT
    // The relro reloc targets the extern's index (2).
    std::uint32_t const reloff = readU32LE(bytes, 240);
    std::uint32_t const rInfo  = readU32LE(bytes, reloff + 4);
    EXPECT_EQ(rInfo & 0x00FFFFFFu, 2u);
}

// (8) Thread-local items fail LOUD on the direct walker call (the linker's
// acceptsDataSection gate fires first in the shipped pipeline; this is the
// anti-silent-drop belt behind it, mirroring the ELF static-arm reject).
TEST(MachOWriter, Arm64ObjectThreadLocalItemFailsLoud) {
    auto loaded = loadShippedArm64();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC0, 0x03, 0x5F, 0xD6};
    mod.functions.push_back(std::move(fn));
    AssembledData t;
    t.symbol    = SymbolId{4};
    t.section   = DataSectionKind::Tdata;
    t.bytes     = {7, 0, 0, 0};
    t.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(t));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksThreadLocalSupport)
            saw = true;
    }
    EXPECT_TRUE(saw)
        << "a Tdata item must be rejected loud, never silently dropped";
}

// (9) A data SymbolId colliding with a function SymbolId is a producer-
// contract breach — fail loud (the K_DuplicateDataSymbol mirror of the
// ELF ET_REL / exec addDataSymbolVas discipline).
TEST(MachOWriter, Arm64ObjectDuplicateDataSymbolFailsLoud) {
    auto loaded = loadShippedArm64();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC0, 0x03, 0x5F, 0xD6};
    mod.functions.push_back(std::move(fn));
    AssembledData d;
    d.symbol    = SymbolId{1};               // collides with the function
    d.section   = DataSectionKind::Rodata;
    d.bytes     = {1, 2, 3, 4};
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& diag : rep.all()) {
        if (diag.code == DiagnosticCode::K_DuplicateDataSymbol) saw = true;
    }
    EXPECT_TRUE(saw);
}

// (10) All four data sections at once: the full section order (__text →
// __TEXT,__const → __DATA,__data → __DATA,__const → __DATA,__bss LAST,
// the zerofill-last convention), cumulative flat addrs, and 1-based
// n_sect ordinals across every symbol. Pins the multi-section cursor
// arithmetic a single-data-section module cannot exercise.
TEST(MachOWriter, Arm64ObjectAllFourDataSectionsOrderAndOrdinals) {
    auto loaded = loadShippedArm64();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC0, 0x03, 0x5F, 0xD6};
    mod.functions.push_back(std::move(fn));
    auto addItem = [&](std::uint32_t sym, DataSectionKind k,
                       std::vector<std::uint8_t> b, std::uint64_t reserved) {
        AssembledData d;
        d.symbol       = SymbolId{sym};
        d.section      = k;
        d.bytes        = std::move(b);
        d.reservedSize = reserved;
        d.alignment    = Alignment::of<8>();
        mod.dataItems.push_back(std::move(d));
    };
    addItem(10, DataSectionKind::Rodata, {1, 2, 3, 4, 5, 6, 7, 8}, 0);
    addItem(11, DataSectionKind::Data, {9, 9, 9, 9, 9, 9, 9, 9}, 0);
    addItem(12, DataSectionKind::RelRoConst, {0, 0, 0, 0, 0, 0, 0, 0}, 0);
    addItem(13, DataSectionKind::Bss, {}, 8);

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // 5 sections: sizeofcmds = 72 + 5*80 + 24 (build_version) + 24 = 520;
    // header+cmds = 552.
    EXPECT_EQ(readU32LE(bytes, 20), 520u);
    EXPECT_EQ(readU32LE(bytes, 96), 5u);
    // Flat addrs: text [0,4) → rodata [8,16) → data [16,24) → relro
    // [24,32) → bss [32,40). filesize = 32, vmsize = 40.
    EXPECT_EQ(readU64LE(bytes, 64), 40u);    // vmsize
    EXPECT_EQ(readU64LE(bytes, 80), 32u);    // filesize
    struct Expect {
        char const* sect; char const* seg;
        std::uint64_t addr; std::uint32_t offset;
    };
    // section_64[i] starts at 104 + i*80 (inside the segment command, so
    // LC_BUILD_VERSION does not move them); fileoff base = 552.
    Expect const rows[] = {
        {"__const", "__TEXT", 8, 560},
        {"__data",  "__DATA", 16, 568},
        {"__const", "__DATA", 24, 576},
        {"__bss",   "__DATA", 32, 0},
    };
    for (std::size_t i = 0; i < 4; ++i) {
        std::size_t const base = 104 + (i + 1) * 80;
        EXPECT_TRUE(name16Equals(bytes, base, rows[i].sect)) << i;
        EXPECT_TRUE(name16Equals(bytes, base + 16, rows[i].seg)) << i;
        EXPECT_EQ(readU64LE(bytes, base + 32), rows[i].addr) << i;
        EXPECT_EQ(readU32LE(bytes, base + 48), rows[i].offset) << i;
    }
    // nlist n_sect ordinals: fn=1, rodata=2, data=3, relro=4, bss=5;
    // n_value = each item's flat address.
    std::uint32_t const symoff = readU32LE(bytes, 32 + 72 + 5 * 80 + 24 + 8);
    ASSERT_EQ(readU32LE(bytes, 32 + 72 + 5 * 80 + 24 + 12), 5u);   // nsyms
    std::uint8_t const expectSect[] = {1, 2, 3, 4, 5};
    std::uint64_t const expectValue[] = {0, 8, 16, 24, 32};
    for (std::size_t s = 0; s < 5; ++s) {
        EXPECT_EQ(bytes[symoff + s * 16 + 5], expectSect[s]) << s;
        EXPECT_EQ(readU64LE(bytes, symoff + s * 16 + 8), expectValue[s]) << s;
    }
}

// ── Wrong-format kind rejection ────────────────────────────────

TEST(MachOWriter, NonMachOFormatKindEmitsK_NoMatchingObjectFormat) {
    auto loaded = loadShipped();
    auto elf = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(elf.has_value());

    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, **elf, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_NoMatchingObjectFormat) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

// ── macho.cputype = 0 validate rejection ───────────────────────

TEST(MachOFormatJson, ZeroCputypeRejectedByValidate) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bad-macho","kind":"macho"},
      "macho": { "cputype": 0, "filetype": 1 }
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: no sections/image block to trip any
    // other rule, so cputype==0 is the only diagnostic.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/macho/cputype"), 1u) << rejectSummary(r);
}

// ── Mach-O section row missing `segment` rejected ──────────────

TEST(MachOFormatJson, EmptySegmentRejectedByValidate) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bad-macho-seg","kind":"macho"},
      "macho": { "cputype": 16777223, "filetype": 1 },
      "sections":[{"kind":"text","name":"__text","type":0,"flags":0,"addrAlign":4,"entrySize":0}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: cputype is valid and virtualAddress
    // defaults to 0, so the empty 'segment' is the only diagnostic.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/sections/0/segment"), 1u) << rejectSummary(r);
}

// ── ELF/PE section row with `segment` set rejected ─────────────

TEST(ElfFormatJson, SegmentFieldRejectedOnElfSection) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bad-elf","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62 },
      "sections":[{"kind":"text","name":".text","segment":"__TEXT","type":1,"flags":6,"addrAlign":16,"entrySize":0}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: class/data/machine are all valid and
    // virtualAddress defaults to 0 (ET_REL default), so the non-empty
    // 'segment' is the only diagnostic.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/sections/0/segment"), 1u) << rejectSummary(r);
}

// ── Mach-O MH_OBJECT section with non-zero virtualAddress rejected ──

TEST(MachOFormatJson, NonZeroVirtualAddressRejectedOnMhObject) {
    // Mach-O MH_OBJECT relocatable files use section_64.addr = 0
    // (vmaddr assignment happens at exec build time via
    // LC_SEGMENT_64). The MH_EXECUTE path will use virtualAddress
    // — anchored at D-LK3-2. Pin the cycle-1 validate-rejection so
    // a future MachO-row edit can't silently no-op.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bad-macho-va","kind":"macho"},
      "macho": { "cputype": 16777223, "filetype": 1 },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":4,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: cputype is valid and 'segment' is
    // non-empty, so the non-zero virtualAddress on this MH_OBJECT row
    // is the only diagnostic.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/sections/0/virtualAddress"), 1u)
        << rejectSummary(r);
}

// ── D-LK10-ENTRY-MACHO-SECTIONVA-COMPUTED: schema __text VA inconsistent
//    with the computed textFileOff fails loud ───────────────────────────

TEST(MachOExecWriter, SchemaTextVaInconsistentWithTextFileOffFailsLoud) {
    // The exec walker derives textFileOff = alignUp(headerAndCmds,
    // segmentPageSize) but trusts the schema's __text.virtualAddress to equal
    // pageZeroSize + textFileOff. This synthetic exec format declares a VA of
    // pageZeroSize + 0x2000 (TWO segmentPageSizes — congruent, so validate()
    // accepts it: validate only checks `>= pageZeroSize` + `% segmentPageSize`,
    // NOT this stronger equality), but a trivial module's header+load-commands
    // fit in ONE page so the real textFileOff is 0x1000. The walker must FAIL
    // LOUD on the mismatch rather than emit a section_64.addr dyld would
    // mis-map. RED-on-disable: without the `textSegmentVaMatchesFileOff` check
    // the encode SUCCEEDS and `bytes.empty()` flips to false.
    // The `processExit` + `entryCallingConvention` pair below is what
    // keeps this synthetic MH_EXECUTE schema LOADABLE at all: an
    // exec-flavored format must declare both or validate() rejects it
    // (D-LK10-ENTRY 2.13). Values are the shipped
    // macho64-x86_64-darwin-exec ones, matching this fixture's x86_64
    // cputype; nothing here builds a trampoline, so they emit nothing.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
      "format": { "name": "macho-va-inconsistent-test", "version": "1.0", "kind": "macho" },
      "dataModel": "LP64",
      "headerNameMatching": "case-sensitive",
      "bitFieldStrategy": "gnu_packed",
      "entryPoint": "",
      "externCallDispatch": "direct-plt",
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "bindNow": true
      },
      "sections": [
        { "kind": "text", "name": "__text", "segment": "__TEXT", "type": 2147484672, "flags": 0, "addrAlign": 16, "entrySize": 0, "virtualAddress": 4294975488 }
      ],
      "relocations": [
        { "name": "X86_64_RELOC_BRANCH", "kind": 1, "nativeId": 369098752 },
        { "name": "X86_64_RELOC_UNSIGNED_8", "kind": 2, "nativeId": 100663296 },
        { "name": "X86_64_RELOC_UNSIGNED_4", "kind": 3, "nativeId": 33554432 }
      ]
    })");
    ASSERT_TRUE(r.has_value())
        << "the synthetic exec format must PASS validate (VA 0x100002000 is "
           "congruent + >= pageZeroSize); the textFileOff inequality is exactly "
           "what validate does NOT check";
    auto const& fmt = *r.value();
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, fmt, rep);
    EXPECT_TRUE(bytes.empty())
        << "an inconsistent schema VA must abort the encode, not emit a "
           "mis-mapped binary";
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_NoMatchingObjectFormat) sawCode = true;
    }
    EXPECT_TRUE(sawCode)
        << "must fail loud with the __text VA / textFileOff inconsistency "
           "diagnostic";
}

// ── Non-zero addend fails loud (Mach-O has no Rela addend) ─────

TEST(MachOWriter, NonZeroAddendFailsLoud) {
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    caller.symbol = SymbolId{1};
    caller.bytes = {0xE8, 0x00, 0x00, 0x00, 0x00};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};
    rel.addend = -4;
    caller.relocations.push_back(rel);
    mod.functions.push_back(std::move(caller));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    EXPECT_GT(rep.errorCount(), 0u);
}

// ── Multi-function module exercises running-offset arithmetic ──

TEST(MachOWriter, MultiFunctionModuleEmitsSequentialTextBytesAndIndices) {
    // Two functions back-to-back; the second's symbol must have
    // n_value = len(first.bytes). Pins the running-offset
    // accumulator across functions (test-analyzer convergence —
    // single-function tests cannot exercise the multi-function
    // index/offset arithmetic).
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction a;
    a.symbol = SymbolId{1};
    a.bytes = {0x90, 0x90, 0xC3};   // nop nop ret = 3 bytes
    mod.functions.push_back(std::move(a));
    AssembledFunction b;
    b.symbol = SymbolId{2};
    b.bytes = {0xC3};
    mod.functions.push_back(std::move(b));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // symoff via LC_SYMTAB at byte 208; symoff field at +8 = 216.
    std::uint32_t const symoff = readU32LE(bytes, 208 + 8);
    std::uint32_t const nsyms  = readU32LE(bytes, 208 + 12);
    ASSERT_EQ(nsyms, 2u);

    // Sym[0] = function `a`: n_value = 0.
    EXPECT_EQ(readU64LE(bytes, symoff + 0 * 16 + 8), 0u);
    // Sym[1] = function `b`: n_value = 3 (right after `a`'s bytes).
    EXPECT_EQ(readU64LE(bytes, symoff + 1 * 16 + 8), 3u);
    // Both functions have no ModuleSymbol row → not externally visible →
    // `definedBinding` = Local → bare N_SECT = 0x0E, NO N_EXT (TF-C54; pre-fix
    // both were N_SECT|N_EXT = 0x0F).
    EXPECT_EQ(bytes[symoff + 0 * 16 + 4], 0x0Eu);
    EXPECT_EQ(bytes[symoff + 1 * 16 + 4], 0x0Eu);
}

// ── End-to-end via the format-blind linker::link() dispatch ────────────

TEST(LinkerEndToEnd, MachODispatchProducesNonEmptyBytes) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(image.ok());
    EXPECT_EQ(image.format, ObjectFormatKind::MachO);
    EXPECT_FALSE(image.bytes.empty());
    EXPECT_EQ(rep.errorCount(), 0u);
    // Magic bytes 0xFEEDFACF (little-endian)
    ASSERT_GE(image.bytes.size(), 4u);
    EXPECT_EQ(image.bytes[0], 0xCFu);
    EXPECT_EQ(image.bytes[1], 0xFAu);
    EXPECT_EQ(image.bytes[2], 0xEDu);
    EXPECT_EQ(image.bytes[3], 0xFEu);
}

// ── LK3 cycle 2: MH_EXECUTE writer ────────────────────────────

namespace {
[[nodiscard]] Loaded loadShippedExec() {
    Loaded out;
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(x86_64) failed";
        for (auto const& d : t.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(macho64-x86_64-darwin-exec) failed";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.format = std::move(f).value();
    }
    return out;
}
} // namespace

TEST(MachOExecFormatJson, ShippedFileLoadsCleanly) {
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.format);
    EXPECT_EQ(loaded.format->kind(), ObjectFormatKind::MachO);
    EXPECT_TRUE(loaded.format->macho().filetype == MachOObjectType::Execute);
    auto const& im = loaded.format->machoImage();
    EXPECT_EQ(im.pageZeroSize, 0x100000000ull);
    EXPECT_EQ(im.dylinkerPath, "/usr/lib/dyld");
    ASSERT_EQ(im.loadDylibs.size(), 1u);
    EXPECT_EQ(im.loadDylibs[0].path, "/usr/lib/libSystem.B.dylib");
}

TEST(MachOExecWriter, MachHeaderFiletypeEqualsMhExecute) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 32u);
    // mach_header_64.filetype @ +12 = MH_EXECUTE = 2.
    EXPECT_EQ(readU32LE(bytes, 12), 2u);
    // flags @ +24 contains MH_PIE (0x200000) bit.
    EXPECT_NE(readU32LE(bytes, 24) & 0x200000u, 0u);
}

TEST(MachOExecWriter, PageZeroSegmentEmittedFirst) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // First load command at offset 32 is LC_SEGMENT_64 (0x19).
    ASSERT_GE(bytes.size(), 32u + 72u);
    EXPECT_EQ(readU32LE(bytes, 32), 0x19u);
    // segname @ +40 = "__PAGEZERO"
    EXPECT_EQ(bytes[40], '_');
    EXPECT_EQ(bytes[41], '_');
    EXPECT_EQ(bytes[42], 'P');
    EXPECT_EQ(bytes[43], 'A');
    // vmsize @ +64 = pageZeroSize
    EXPECT_EQ(readU64LE(bytes, 64), 0x100000000ull);
}

TEST(MachOExecWriter, LcMainEntryOffPointsToFirstFunction) {
    auto loaded = loadShippedExec();
    // 2 functions: f[0] is some prelude (0x90 NOP + 0xC3 ret), f[1] is the entry.
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction a;
    a.symbol = SymbolId{1};
    a.bytes  = {0x90, 0xC3};
    mod.functions.push_back(std::move(a));
    AssembledFunction b;
    b.symbol = SymbolId{42};
    b.bytes  = {0xC3};
    mod.functions.push_back(std::move(b));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // ★ WHAT THIS PINS, RESTATED SINCE D-LK10-ENTRY 2.13 gate 6.
    // The entry is functions[0] — entryoff = textFileOff + 0, and
    // textFileOff is headerAndCmds page-aligned, so entryoff is a
    // multiple of the page size. f[1] would land at textFileOff + 2,
    // which is NOT, so the assertion below still discriminates index 0
    // from index 1 (that is why the fixture carries TWO functions).
    // What CHANGED is only where index 0 comes from: it used to be the
    // walker's silent `entryPoint`-empty default, and that default is
    // now a fail-loud on a `processExit`-declaring format. The helper
    // supplies `imageEntryOverride = 0` — the same value, now STATED —
    // so this pin covers the override path, and the old default path no
    // longer exists to cover.
    // Locate LC_MAIN by scanning load commands.
    std::size_t off = 32;  // start of load commands
    bool sawLcMain = false;
    while (off + 8 <= bytes.size()) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x80000028u) {       // LC_MAIN
            std::uint64_t const entryOff = readU64LE(bytes, off + 8);
            EXPECT_EQ(entryOff % 0x1000u, 0u);
            sawLcMain = true;
            break;
        }
        off += cmdsize;
    }
    EXPECT_TRUE(sawLcMain);
}

// F5 (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): a symbol-address DATA global must emit a
// dyld REBASE opcode stream, so a PIE image's absolute pointer is slid at load.
// HOST-INDEPENDENT structural pin — red-on-disable on EVERY leg: revert the
// rebase wiring (rebaseOff/Size in LC_DYLD_INFO_ONLY) and rebase_size returns to
// 0, failing this test even on the Windows/Linux legs that cannot RUN a Mach-O.
// The macOS CI leg is the runtime witness; this is the always-on guard the dss
// cross-target bar pairs with it.
TEST(MachOExecWriter, SymbolAddressDataGlobalEmitsDyldRebaseStream) {
    // DSS's Mach-O exec target is arm64-only (the x86_64 darwin exec format
    // declares no __data section row); mirror the arm64-exit dynamic recipe.
    auto targetR = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(targetR.has_value());
    auto target = std::move(targetR).value();
    auto formatR = ObjectFormatSchema::loadShipped("macho64-arm64-darwin-exec");
    ASSERT_TRUE(formatR.has_value());
    auto format = std::move(formatR).value();

    // The abs64 reloc kind, found by the SAME agnostic formula the pipeline uses
    // (widthBytes==8 && !pcRelative) — never a hardcoded kind id.
    RelocationKind abs64{0};
    bool foundAbs64 = false;
    for (auto const& r : target->relocations())
        if (r.widthBytes == 8 && !r.pcRelative) { abs64 = r.kind; foundAbs64 = true; break; }
    ASSERT_TRUE(foundAbs64)
        << "arm64 target must declare an 8-byte non-pc-relative reloc (ARM64_RELOC_UNSIGNED)";

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};  // BL _abs ; RET
    // Reference the extern (call26) so it gets a __stubs/__got slot — an
    // unreferenced import has no binding site. kind 1 = ARM64_RELOC_BRANCH26.
    fn.relocations.push_back(Relocation{0u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    // An extern import forces the dynamic writer (encodeExecDynamic) — the only
    // path that lays out __DATA and emits LC_DYLD_INFO_ONLY.
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_abs", "/usr/lib/libSystem.B.dylib"});
    // `target` — a plain 8-byte mutable data global (NO reloc): must be SKIPPED
    // by the rebase stream (only pointer slots are rebased).
    AssembledData targetData;
    targetData.symbol    = SymbolId{50};
    targetData.section   = DataSectionKind::Data;
    targetData.bytes.assign(8, 0);
    targetData.alignment = Alignment::ofRuntimePow2(8);
    mod.dataItems.push_back(std::move(targetData));
    // `p` — a symbol-address pointer: 8-byte slot + abs64 reloc → `target`.
    AssembledData p;
    p.symbol    = SymbolId{51};
    p.section   = DataSectionKind::Data;
    p.bytes.assign(8, 0);
    p.alignment = Alignment::ofRuntimePow2(8);
    p.relocations.push_back(Relocation{0u, SymbolId{50}, abs64, 0});
    mod.dataItems.push_back(std::move(p));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *target, *format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    std::uint32_t const ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    bool sawDyldInfo = false;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x80000022u) {  // LC_DYLD_INFO_ONLY
            std::uint32_t const rebaseOff  = readU32LE(bytes, off + 8);
            std::uint32_t const rebaseSize = readU32LE(bytes, off + 12);
            // THE PIN: a symbol-address data global must yield a non-empty rebase
            // stream. rebase_size==0 means the wiring regressed (red-on-disable).
            ASSERT_GT(rebaseSize, 0u)
                << "symbol-address data global must emit a dyld REBASE stream; "
                   "rebase_size==0 means the PIE-rebase wiring regressed.";
            ASSERT_LE(static_cast<std::size_t>(rebaseOff) + rebaseSize, bytes.size());
            // Decode: SET_TYPE_IMM|POINTER (0x11), ≥1 SET_SEGMENT_AND_OFFSET (0x2X)
            // + DO_REBASE (0x5X), terminated by DONE (0x00).
            EXPECT_EQ(bytes[rebaseOff], 0x11u);  // SET_TYPE_IMM | REBASE_TYPE_POINTER
            bool sawSetSeg = false, sawDoRebase = false, sawDone = false;
            std::size_t bi = rebaseOff + 1;
            std::size_t const end = rebaseOff + rebaseSize;
            while (bi < end) {
                std::uint8_t const opHi = bytes[bi] & 0xF0u;
                bi++;
                if (opHi == 0x20u) {            // SET_SEGMENT_AND_OFFSET_ULEB
                    sawSetSeg = true;
                    while (bi < end && (bytes[bi] & 0x80u)) bi++;  // skip ULEB cont.
                    if (bi < end) bi++;                            // last ULEB byte
                } else if (opHi == 0x50u) {     // DO_REBASE_IMM_TIMES
                    sawDoRebase = true;
                } else if (opHi == 0x00u) {     // DONE
                    sawDone = true; break;
                }
            }
            EXPECT_TRUE(sawSetSeg);
            EXPECT_TRUE(sawDoRebase);
            EXPECT_TRUE(sawDone);
            sawDyldInfo = true;
            break;
        }
        off += cmdsize;
    }
    EXPECT_TRUE(sawDyldInfo)
        << "LC_DYLD_INFO_ONLY must be present on the legacy darwin exec path.";
}

// c145 (D-LK-RELRO-CONST-DATA-RELOCATABLE): a CONST symbol-address global (relro)
// is FOLDED into the writable `__DATA,__data` section in the MH_EXECUTE image and
// dyld rebases its slot via the __DATA rebase stream — the task-blessed "treat
// relro like data" placement (over a separate __DATA_CONST). No fail-loud.
// RED-on-disable: without the relro→__data merge the item is dropped/rejected and
// rebase_size returns to 0. Mirrors the F5 symbol-address pin above, relro-routed.
TEST(MachOExecWriter, RelRoConstItemFoldsIntoDataAndEmitsDyldRebase) {
    auto targetR = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(targetR.has_value());
    auto target = std::move(targetR).value();
    auto formatR = ObjectFormatSchema::loadShipped("macho64-arm64-darwin-exec");
    ASSERT_TRUE(formatR.has_value());
    auto format = std::move(formatR).value();
    RelocationKind abs64{0};
    bool foundAbs64 = false;
    for (auto const& r : target->relocations())
        if (r.widthBytes == 8 && !r.pcRelative) { abs64 = r.kind; foundAbs64 = true; break; }
    ASSERT_TRUE(foundAbs64);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};  // BL _abs ; RET
    fn.relocations.push_back(Relocation{0u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    // An extern import forces the dynamic writer (the only path that lays __DATA).
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_abs", "/usr/lib/libSystem.B.dylib"});
    // A plain data target (no reloc — must NOT be rebased).
    AssembledData targetData;
    targetData.symbol    = SymbolId{50};
    targetData.section   = DataSectionKind::Data;
    targetData.bytes.assign(8, 0);
    targetData.alignment = Alignment::ofRuntimePow2(8);
    mod.dataItems.push_back(std::move(targetData));
    // A CONST pointer table → relro (the c145 routing): folds into __data + rebased.
    AssembledData p;
    p.symbol    = SymbolId{51};
    p.section   = DataSectionKind::RelRoConst;
    p.bytes.assign(8, 0);
    p.alignment = Alignment::ofRuntimePow2(8);
    p.relocations.push_back(Relocation{0u, SymbolId{50}, abs64, 0});
    mod.dataItems.push_back(std::move(p));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *target, *format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "a relro item must NOT fail loud in a Mach-O exec image";
    ASSERT_FALSE(bytes.empty());

    std::uint32_t const ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    bool sawRebase = false;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x80000022u) {  // LC_DYLD_INFO_ONLY
            std::uint32_t const rebaseSize = readU32LE(bytes, off + 12);
            ASSERT_GT(rebaseSize, 0u)
                << "the relro pointer folded into __data must get a dyld REBASE; "
                   "rebase_size==0 means the relro merge/rebase wiring regressed.";
            sawRebase = true;
            break;
        }
        off += cmdsize;
    }
    EXPECT_TRUE(sawRebase) << "LC_DYLD_INFO_ONLY with a rebase for the relro slot.";
}

// D-LK-MACHO-DATA-EXTERN-DEAD-STUB (c119): the __stubs band is COMPACTED to the
// FUNCTION externs — a DATA extern (got-indirect) is never called, so it gets a
// __got slot but NO __stubs stub. A module with 1 func + 1 data extern must emit
// exactly ONE stub (numFuncExterns), not two. RED-ON-DISABLE: the pre-c119 lockstep
// layout emitted a dead stub per data extern → __stubs size would be 2×stubSize.
TEST(MachOExecWriter, DataExternGetsGotSlotButNoStub) {
    auto targetR = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(targetR.has_value());
    auto target = std::move(targetR).value();
    auto formatR = ObjectFormatSchema::loadShipped("macho64-arm64-darwin-exec");
    ASSERT_TRUE(formatR.has_value());
    auto format = std::move(formatR).value();

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};  // BL _func_ext ; RET
    // A FUNCTION extern (#99) is CALLED (BRANCH26) → it gets a __stubs stub + __got
    // slot. A DATA extern (#98, isData) just needs to be in externImports — the
    // walker builds a __got slot per extern (dyld-bound) but NO stub for data.
    fn.relocations.push_back(Relocation{0u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_func_ext", "/usr/lib/libSystem.B.dylib"});
    ExternImport dataExt{SymbolId{98}, "_data_ext", "/usr/lib/libSystem.B.dylib"};
    dataExt.isData = true;
    mod.externImports.push_back(std::move(dataExt));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *target, *format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // Walk LC_SEGMENT_64 sections → find __stubs → read its `size` (section_64 @ +40).
    constexpr std::uint32_t kLcSegment64 = 0x19u;
    std::uint32_t const ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    bool sawStubs = false;
    std::uint32_t stubsSize = 0, gotSize = 0;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == kLcSegment64) {
            std::uint32_t const nsects = readU32LE(bytes, off + 64);
            std::size_t sec = off + 72;
            for (std::uint32_t s = 0; s < nsects; ++s, sec += 80) {
                char const* nm = reinterpret_cast<char const*>(&bytes[sec]);
                // sectname is a NUL-padded char[16]; compare INCLUDING the NUL
                // terminator (the string literals carry it) so "__got" can't match
                // a hypothetical "__got_more".
                if (std::memcmp(nm, "__stubs", 8) == 0) { stubsSize = readU32LE(bytes, sec + 40); sawStubs = true; }
                if (std::memcmp(nm, "__got", 6) == 0)   { gotSize   = readU32LE(bytes, sec + 40); }
            }
        }
        off += cmdsize;
    }
    ASSERT_TRUE(sawStubs) << "the __stubs section must be present";
    // arm64 stub = 12 bytes (ADRP x16 + LDR x16 + BR x16). ONE func extern → 12.
    // The dead-stub bug (a stub per data extern too) would give 2×12 = 24.
    EXPECT_EQ(stubsSize, 12u)
        << "a DATA extern must NOT get a __stubs stub — expected 1 func-extern stub "
           "(12 B); 24 B means the data extern got a dead stub (pre-c119 lockstep).";
    // BOTH externs still get a __got slot (8 B each): the func's stub jumps through
    // it, the data extern's got slot holds the dyld-bound object address.
    EXPECT_EQ(gotSize, 16u)
        << "every extern (func + data) keeps a __got slot — 2 × 8 B.";
}

TEST(MachOExecWriter, IntraModuleBranchAppliedByteForByte) {
    // Branch (rel32, kind 1) from fn[0] to fn[1].
    // sectionVa = pageZeroSize + 0x1000 = 0x100001000.
    // P = sectionVa + 1, S = sectionVa + 6, A = 0 → value = 1.
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));
    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3};
    mod.functions.push_back(std::move(f1));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // Locate __text file offset by parsing the __TEXT segment's
    // __text section_64 record. __TEXT segment is the 2nd LC_SEGMENT_64.
    std::size_t off = 32;
    std::uint32_t textFileOff = 0;
    int segIdx = 0;
    while (off + 8 <= bytes.size()) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            ++segIdx;
            if (segIdx == 2) {
                // section_64 starts at off + 72; section.offset @ +96.
                textFileOff = readU32LE(bytes, off + 72 + 48);
                break;
            }
        }
        off += cmdsize;
    }
    ASSERT_NE(textFileOff, 0u);
    EXPECT_EQ(bytes[textFileOff + 0], 0xE8u);
    EXPECT_EQ(readU32LE(bytes, textFileOff + 1), 1u);
    EXPECT_EQ(bytes[textFileOff + 5], 0xC3u);
}

TEST(MachOExecWriter, ExternTargetFailsLoudAsUndefined) {
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{99};
    rel.kind   = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(MachOExecFormatJsonValidate, ObjWithImageBlockRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"obj-with-image","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": 1, "flags": 0 },
      "image": { "pageZeroSize": 4294967296, "dylinkerPath": "/usr/lib/dyld", "loadDylibs": ["/usr/lib/libSystem.B.dylib"] },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: the single `fail("/image", ...)` call
    // site fires once regardless of which image field triggered its
    // `anySet` check (here pageZeroSize/dylinkerPath/loadDylibs); cputype
    // is valid and the section row is otherwise clean, so it is the only
    // diagnostic.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/image"), 1u) << rejectSummary(r);
}

TEST(MachOExecFormatJsonValidate, ObjWithBindNowFalseRejected) {
    // Symmetric reject: MH_OBJECT must not set image.bindNow=false.
    // Eager-vs-lazy is an exec-image concept; .o files do not bind
    // at all (the linker resolves at exec build time). Without this
    // rule a JSON typo would silently load. (Type-design HIGH fold,
    // LK6 cycle 2c post-fold review.)
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"obj-with-bindnow-false","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": 1, "flags": 0 },
      "image": { "bindNow": false },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: same `fail("/image", ...)` call site as
    // ObjWithImageBlockRejected, this time tripped by `bindNow: false`
    // alone; cputype is valid and the section row is otherwise clean.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/image"), 1u) << rejectSummary(r);
}

TEST(MachOExecFormatJsonValidate, ExecMissingLoadDylibsRejected) {
    // The entry cluster (`processExit` + `entryCallingConvention`) is
    // present so the ONLY reason this fixture is rejected is its own
    // defect — the missing `image.loadDylibs`. Without it D-LK10-ENTRY
    // 2.13 rejects any MH_EXECUTE schema that declares no `processExit`,
    // which would confound the pin (validate() accumulates; the loader
    // rejects on ANY error). Values copied VERBATIM from the shipped
    // src/dss-config/object-formats/macho64-x86_64-darwin-exec.format.json,
    // matching this fixture's x86_64 cputype (`sysv_amd64`, NOT the arm64
    // sibling's `apple_arm64`). Inert here: the load IS the test, so no
    // trampoline is built and no cc name is resolved.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"exec-no-dylibs","kind":"macho"},
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 0 },
      "image": { "pageZeroSize": 4294967296, "dylinkerPath": "/usr/lib/dyld" },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/image/loadDylibs"), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

// ── New tests folded from 7-agent review of LK3 cycle 2 ────────

TEST(MachOExecFormatJsonValidate, DylibWithoutDylibImageShapeRejected) {
    // c153 (D-LK3-3): MH_DYLIB is a SUPPORTED filetype now, but a
    // dylib schema missing its image identity (installName, a
    // loadDylibs entry, text VA == segmentPageSize) is still
    // rejected by the dylib shape rules — the bare-minimum JSON that
    // pre-c153 rejected on the filetype itself keeps failing, for
    // the precise per-field reasons. (The ACCEPTED full dylib shape
    // is pinned in test_macho_dylib_writer.cpp.)
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"a-dylib","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "dylib", "flags": 0 },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED: this bare-minimum dylib fixture is missing THREE
    // independent parts of the dylib shape at once -- loadDylibs and
    // installName default to empty, and __text's virtualAddress
    // defaults to 0 while segmentPageSize defaults to 0x1000, so the
    // "must equal segmentPageSize" rule fires too. Each is its own
    // `fail()` call in validate(); the path pin below targets
    // installName (this test's namesake), and errorCount documents
    // that the other two legitimately co-fire so a future edit can't
    // silently drop one without this test noticing.
    EXPECT_EQ(errorCount(r), 3u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/image/installName"), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/image/loadDylibs"), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/sections/<text>/virtualAddress"), 1u)
        << rejectSummary(r);
}

TEST(MachOExecFormatJsonValidate, SectionVaBelowPageZeroRejected) {
    // silent-failure H4 + code-reviewer C2: __text virtualAddress
    // must be >= pageZeroSize, else sectionVa - pageZeroSize
    // underflows.
    //
    // The entry cluster is present so the below-__PAGEZERO VA is the ONLY
    // reason this fixture is rejected (D-LK10-ENTRY 2.13 rejects an
    // MH_EXECUTE schema declaring no `processExit`, which would confound
    // the pin). Values VERBATIM from the shipped
    // src/dss-config/object-formats/macho64-x86_64-darwin-exec.format.json,
    // matching this fixture's x86_64 cputype; inert (no trampoline is
    // built — the load IS the test).
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"underflow","kind":"macho"},
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 0 },
      "image": { "pageZeroSize": 4294967296, "dylinkerPath": "/usr/lib/dyld", "loadDylibs": ["/usr/lib/libSystem.B.dylib"] },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/sections/<text>/virtualAddress"), 1u)
        << rejectSummary(r);
    // Two DIFFERENT rules emit at that pointer (below-__PAGEZERO vs the
    // segmentPageSize mmap-congruence check), so pin the message unique
    // to the underflow rule under test.
    EXPECT_EQ(countWithMessage(r, "is below __PAGEZERO end"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(MachOExecFormatJsonValidate, MissingDylinkerPathRejected) {
    // Entry cluster present so the missing `image.dylinkerPath` is the
    // ONLY rejection reason (D-LK10-ENTRY 2.13 would otherwise reject
    // this MH_EXECUTE schema for declaring no `processExit` too). Values
    // VERBATIM from the shipped
    // src/dss-config/object-formats/macho64-x86_64-darwin-exec.format.json,
    // matching this fixture's x86_64 cputype; inert (no trampoline).
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"no-dyld","kind":"macho"},
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 0 },
      "image": { "pageZeroSize": 4294967296, "loadDylibs": ["/usr/lib/libSystem.B.dylib"] },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/image/dylinkerPath"), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(MachOExecWriter, EmptyTextFailsLoud) {
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(MachOExecWriter, RelocOffsetPastFunctionBytesFailsLoud) {
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xC3};
    Relocation rel;
    rel.offset = 4;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));
    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3, 0xC3, 0xC3, 0xC3};
    mod.functions.push_back(std::move(f1));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(MachOExecWriter, TextSegmentVmaddrEqualsPageZeroEnd) {
    // Load-bearing invariant: __TEXT.vmaddr must equal pageZeroSize
    // (otherwise __TEXT either overlaps __PAGEZERO or leaves a gap;
    // dyld rejects both). The walker computes it; a future
    // refactor that drifts this would silently produce a non-
    // loadable image.
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Walk load commands until __TEXT (the 2nd LC_SEGMENT_64).
    std::size_t off = 32;  // start of load commands
    int segIdx = 0;
    bool sawText = false;
    while (off + 8 <= bytes.size()) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            ++segIdx;
            if (segIdx == 2) {
                // segname @ +8 must be "__TEXT"
                EXPECT_EQ(bytes[off + 8], '_');
                EXPECT_EQ(bytes[off + 9], '_');
                EXPECT_EQ(bytes[off + 10], 'T');
                // vmaddr @ +24 must equal pageZeroSize (4 GiB)
                EXPECT_EQ(readU64LE(bytes, off + 24), 0x100000000ull);
                // fileoff @ +40 must equal 0 (mach header is in __TEXT)
                EXPECT_EQ(readU64LE(bytes, off + 40), 0u);
                // nsects @ +64 == 1 (just __text this cycle)
                EXPECT_EQ(readU32LE(bytes, off + 64), 1u);
                sawText = true;
                break;
            }
        }
        off += cmdsize;
    }
    EXPECT_TRUE(sawText);
}

TEST(MachOExecWriter, LcLoadDylibStructurePinnedByteForByte) {
    // Load-bearing dyld invariant: LC_LOAD_DYLIB.name offset must
    // be 24 (cmd+24 points at the dylib path string). If a future
    // refactor changes the field layout, dyld silently looks for
    // the path at the wrong offset, fails to find libSystem, and
    // the process never starts. Pin the layout byte-for-byte.
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    std::size_t off = 32;
    bool sawDylib = false;
    while (off + 8 <= bytes.size()) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x0Cu) {  // LC_LOAD_DYLIB
            // name offset @ +8 must be 24 (cmd+24)
            EXPECT_EQ(readU32LE(bytes, off + 8), 24u);
            // timestamp / current_version / compat_version @ +12/+16/+20
            // are 0 today (reserved for future cycle).
            EXPECT_EQ(readU32LE(bytes, off + 12), 0u);
            EXPECT_EQ(readU32LE(bytes, off + 16), 0u);
            EXPECT_EQ(readU32LE(bytes, off + 20), 0u);
            // path bytes @ +24 begin with "/usr/lib/libSystem"
            EXPECT_EQ(bytes[off + 24], '/');
            EXPECT_EQ(bytes[off + 25], 'u');
            EXPECT_EQ(bytes[off + 26], 's');
            EXPECT_EQ(bytes[off + 27], 'r');
            sawDylib = true;
            break;
        }
        off += cmdsize;
    }
    EXPECT_TRUE(sawDylib);
}

TEST(IsImageFlavorAccessor, ConsistentAcrossThreeFormats) {
    // type-design O1 fold-in: the isImageFlavor() accessor exposes
    // the cross-format triplet check beyond validate(). Pin it
    // matches the shipped JSONs.
    auto objE = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(objE.has_value());
    EXPECT_FALSE((**objE).isImageFlavor());

    auto execE = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(execE.has_value());
    EXPECT_TRUE((**execE).isImageFlavor());

    auto objP = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(objP.has_value());
    EXPECT_FALSE((**objP).isImageFlavor());

    auto execP = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(execP.has_value());
    EXPECT_TRUE((**execP).isImageFlavor());

    auto objM = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    ASSERT_TRUE(objM.has_value());
    EXPECT_FALSE((**objM).isImageFlavor());

    auto execM = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(execM.has_value());
    EXPECT_TRUE((**execM).isImageFlavor());
}

TEST(MachOExecWriter, DisplacementOverflowFailsLoud) {
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{1};
    rel.kind   = RelocationKind{1};
    rel.addend = std::numeric_limits<std::int64_t>::max() / 2;
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

// ── LK6 cycle 2c: extern imports produce a dynamic Mach-O image ─

TEST(MachOExecWriter, ExternImportsProduceDynamicImage) {
    // Cycle 2c walker has landed: extern imports now produce a
    // real Mach-O dynamic image (parallel to ELF cycle 2b.2). Pins
    // MH_EXECUTE byte + non-empty bytes + dylib/symbol strings.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
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
    imp.mangledName = "_printf";
    imp.libraryPath = "/usr/lib/libSystem.B.dylib";
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    // filetype @ +12 = MH_EXECUTE (2)
    EXPECT_EQ(static_cast<std::uint32_t>(bytes[12]) |
              (static_cast<std::uint32_t>(bytes[13]) << 8) |
              (static_cast<std::uint32_t>(bytes[14]) << 16) |
              (static_cast<std::uint32_t>(bytes[15]) << 24), 2u);
    std::string_view fileView{
        reinterpret_cast<char const*>(bytes.data()), bytes.size()};
    EXPECT_NE(fileView.find("/usr/lib/libSystem.B.dylib"),
              std::string_view::npos);
    EXPECT_NE(fileView.find("_printf"), std::string_view::npos);
}

// D-FFI-MACHO-NONDEFAULT-DYLIB-LOAD (the Mach-O sibling of the ELF
// DT_NEEDED-per-referenced-library walker D-FFI-MATH-LIBM-DT-NEEDED):
// an extern import that references a NON-libSystem library (the shipped
// exec schema declares only libSystem) must AUTO-EMIT its own
// LC_LOAD_DYLIB — before this walker the writer instead REJECTED it
// (K_SymbolUndefined "not declared in image.loadDylibs"), so a program
// importing `/usr/lib/libz.1.dylib` (zlib.json) could not link. This is
// the host-independent structural guard behind the arm64 zlib_roundtrip
// runtime witness: it pins the emitted LC_LOAD_DYLIB set (schema first,
// then the referenced lib) AND that each import's bind opcode targets
// the ordinal of ITS OWN library — the libz import must bind to the libz
// ordinal (2), NOT libSystem's (1). RED-ON-DISABLE: revert the walker to
// the validate-or-reject loop → `macho::encode` fails loud (errorCount
// > 0, empty bytes) → every ASSERT/EXPECT here fails.
TEST(MachOExecWriter, NonLibSystemImportAutoEmitsLcLoadDylibAndBindsToItsOrdinal) {
    auto loaded = loadShippedExec();  // arm64 + macho64-arm64-darwin-exec
    ASSERT_TRUE(loaded.target);
    ASSERT_TRUE(loaded.format);

    // A single function that BL-branches to BOTH externs (so each gets a
    // __stubs/__got slot + a bind site), then RET.
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x00, 0x00, 0x00, 0x94,   // BL  _printf   (sym 99)
                 0x00, 0x00, 0x00, 0x94,   // BL  _deflate  (sym 100)
                 0xC0, 0x03, 0x5F, 0xD6};  // RET
    fn.relocations.push_back(Relocation{0u, SymbolId{99},  RelocationKind{1}, 0});
    fn.relocations.push_back(Relocation{4u, SymbolId{100}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    // Import #1 lives in the schema-declared libSystem (ordinal 1);
    // import #2 lives in a NON-schema library — the case the walker now
    // handles. Declaration order deliberately puts the non-libSystem
    // import LAST to prove the emission is set-driven, not order-lucky.
    mod.externImports.push_back(
        ExternImport{SymbolId{99},  "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    mod.externImports.push_back(
        ExternImport{SymbolId{100}, "_deflate",
                     "/usr/lib/libz.1.dylib"});

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "a referenced non-libSystem import must AUTO-EMIT LC_LOAD_DYLIB, "
           "not be rejected (D-FFI-MACHO-NONDEFAULT-DYLIB-LOAD).";
    ASSERT_FALSE(bytes.empty());

    // ── Walk the load commands: collect LC_LOAD_DYLIB paths in order and
    //    capture the LC_DYLD_INFO_ONLY bind stream.
    std::uint32_t const ncmds = readU32LE(bytes, 16);
    std::vector<std::string> dylibPaths;
    std::size_t bindOff = 0, bindSize = 0;
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        ASSERT_NE(cmdsize, 0u);
        if (cmd == 0x0Cu) {  // LC_LOAD_DYLIB
            std::uint32_t const nameOff = readU32LE(bytes, off + 8);
            std::string path(
                reinterpret_cast<char const*>(bytes.data()) + off + nameOff);
            dylibPaths.push_back(std::move(path));
        } else if (cmd == 0x80000022u) {  // LC_DYLD_INFO_ONLY
            bindOff  = readU32LE(bytes, off + 16);
            bindSize = readU32LE(bytes, off + 20);
        }
        off += cmdsize;
    }

    // THE PIN #1: exactly two LC_LOAD_DYLIB — the schema's libSystem
    // FIRST (fixed ordinal 1), then the referenced libz.
    ASSERT_EQ(dylibPaths.size(), 2u)
        << "libSystem (schema) + libz (referenced) → two LC_LOAD_DYLIB.";
    EXPECT_EQ(dylibPaths[0], "/usr/lib/libSystem.B.dylib");
    EXPECT_EQ(dylibPaths[1], "/usr/lib/libz.1.dylib");

    // ── Decode the bind opcode stream into name → dylib-ordinal.
    ASSERT_GT(bindSize, 0u);
    ASSERT_LE(bindOff + bindSize, bytes.size());
    std::unordered_map<std::string, std::uint32_t> ordinalOf;
    std::uint32_t curOrd = 0;
    std::size_t bi = bindOff;
    std::size_t const bend = bindOff + bindSize;
    auto skipUleb = [&]() {
        while (bi < bend && (bytes[bi] & 0x80u)) ++bi;
        if (bi < bend) ++bi;
    };
    while (bi < bend) {
        std::uint8_t const opcode = bytes[bi] & 0xF0u;
        std::uint8_t const imm    = bytes[bi] & 0x0Fu;
        ++bi;
        switch (opcode) {
            case 0x00u: bi = bend; break;               // BIND_OPCODE_DONE
            case 0x10u: curOrd = imm; break;            // SET_DYLIB_ORDINAL_IMM
            case 0x20u: {                               // SET_DYLIB_ORDINAL_ULEB
                std::uint32_t v = 0; int sh = 0;
                while (bi < bend) {
                    std::uint8_t b = bytes[bi++];
                    v |= static_cast<std::uint32_t>(b & 0x7Fu) << sh;
                    if (!(b & 0x80u)) break;
                    sh += 7;
                }
                curOrd = v;
                break;
            }
            case 0x40u: {                               // SET_SYMBOL_TRAILING_FLAGS_IMM
                std::string name(
                    reinterpret_cast<char const*>(bytes.data()) + bi);
                bi += name.size() + 1;                  // name + NUL
                ordinalOf[name] = curOrd;
                break;
            }
            case 0x50u: break;                          // SET_TYPE_IMM
            case 0x60u: skipUleb(); break;              // SET_ADDEND_SLEB
            case 0x70u: skipUleb(); break;              // SET_SEGMENT_AND_OFFSET_ULEB
            case 0x80u: skipUleb(); break;              // ADD_ADDR_ULEB
            case 0x90u: break;                          // DO_BIND
            default: break;
        }
    }

    // THE PIN #2: each import binds to the ordinal of ITS OWN library.
    // _printf → libSystem (ordinal 1); _deflate → libz (ordinal 2). A
    // regression that pinned every import to libSystem would bind
    // _deflate to ordinal 1 and dyld would search libSystem for a symbol
    // that lives in libz → load failure.
    ASSERT_TRUE(ordinalOf.count("_printf"));
    ASSERT_TRUE(ordinalOf.count("_deflate"));
    EXPECT_EQ(ordinalOf["_printf"],  1u);
    EXPECT_EQ(ordinalOf["_deflate"], 2u)
        << "the libz import must bind to the libz dylib ordinal (2), "
           "NOT libSystem's (1).";

    // ── THE PIN #3: ncmds/sizeofcmds account for the added command.
    // Re-encode an otherwise-identical module whose SECOND import also
    // lives in libSystem (so exactly ONE dylib is emitted) — same extern
    // count, same got slots, same binds; the ONLY difference is the extra
    // referenced dylib. ncmds must grow by exactly 1 and sizeofcmds by
    // exactly the libz LC_LOAD_DYLIB size.
    AssembledModule baseMod;
    baseMod.expectedFuncCount = 1;
    AssembledFunction bfn;
    bfn.symbol = SymbolId{1};
    bfn.bytes  = {0x00, 0x00, 0x00, 0x94, 0x00, 0x00, 0x00, 0x94,
                  0xC0, 0x03, 0x5F, 0xD6};
    bfn.relocations.push_back(Relocation{0u, SymbolId{99},  RelocationKind{1}, 0});
    bfn.relocations.push_back(Relocation{4u, SymbolId{100}, RelocationKind{1}, 0});
    baseMod.functions.push_back(std::move(bfn));
    baseMod.externImports.push_back(
        ExternImport{SymbolId{99},  "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    baseMod.externImports.push_back(
        ExternImport{SymbolId{100}, "_puts",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter baseRep;
    auto baseBytes = encodeUntrampolined(baseMod, *loaded.target, *loaded.format,
                                   baseRep);
    ASSERT_EQ(baseRep.errorCount(), 0u);
    ASSERT_FALSE(baseBytes.empty());
    std::uint32_t const baseNcmds      = readU32LE(baseBytes, 16);
    std::uint32_t const baseSizeofcmds = readU32LE(baseBytes, 20);
    std::uint32_t const thisSizeofcmds = readU32LE(bytes, 20);
    EXPECT_EQ(ncmds, baseNcmds + 1u)
        << "the extra referenced dylib adds exactly one LC_LOAD_DYLIB.";
    // LC_LOAD_DYLIB size = 24-byte dylib_command + path + NUL, padded to 8.
    std::string const libz = "/usr/lib/libz.1.dylib";
    std::uint32_t const libzCmdSize =
        static_cast<std::uint32_t>(((24 + libz.size() + 1 + 7) / 8) * 8);
    EXPECT_EQ(thisSizeofcmds, baseSizeofcmds + libzCmdSize);
}

// D-FFI-MACHO-NONDEFAULT-DYLIB-LOAD, fail-loud belt: an extern with an
// EMPTY libraryPath is unresolvable — Mach-O's two-level namespace binds
// every import against a NAMED dylib ordinal, so there is no library for
// dyld to search. The walker rejects it (the ELF library-less guard's
// Mach-O analog; Mach-O grants no `.so`-style library-less exemption).
TEST(MachOExecWriter, EmptyLibraryPathImportFailsLoud) {
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};  // BL ; RET
    fn.relocations.push_back(Relocation{0u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_mystery", ""});  // no library
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    EXPECT_GT(rep.errorCount(), 0u)
        << "a library-less import must fail loud, not ship a bindless binary.";
}

TEST(MachOExecWriter, DynamicImageEmitsExpectedSegments) {
    // Dynamic Mach-O carries 4 segments: __PAGEZERO + __TEXT (with
    // __text + __stubs) + __DATA_CONST (with __got) + __LINKEDIT.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
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
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    std::string_view fv{
        reinterpret_cast<char const*>(bytes.data()), bytes.size()};
    EXPECT_NE(fv.find("__PAGEZERO"),   std::string_view::npos);
    EXPECT_NE(fv.find("__TEXT"),       std::string_view::npos);
    EXPECT_NE(fv.find("__stubs"),      std::string_view::npos);
    EXPECT_NE(fv.find("__DATA_CONST"), std::string_view::npos);
    EXPECT_NE(fv.find("__got"),        std::string_view::npos);
    EXPECT_NE(fv.find("__LINKEDIT"),   std::string_view::npos);
}

TEST(MachOExecWriter, BindNowFalseFailsLoudCitingDLK613) {
    // The lazy-binding upgrade is anchored at D-LK6-13. Until it
    // lands, the walker must fail loud on `image.bindNow = false`.
    // The entry cluster in the JSON is required by validate() on any
    // MH_EXECUTE schema (D-LK10-ENTRY 2.13) — shipped
    // macho64-x86_64-darwin-exec values, this fixture's cputype.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-lazy-pending","kind":"macho"},
      "entryPoint": "",
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "bindNow": false
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
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawAnchor = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport
         && d.actual.find("D-LK6-13") != std::string::npos) {
            sawAnchor = true;
        }
    }
    EXPECT_TRUE(sawAnchor);
}

// D-LK6-14 substrate (e4508b9 → next 2026-06-01): schema JSON
// accepts the `useChainedFixups` flag. Defaults to false (legacy
// LC_DYLD_INFO_ONLY opcode stream path stays fully supported).
TEST(MachOExecFormatJson, UseChainedFixupsDefaultsToFalse) {
    // Entry cluster required by validate() on an MH_EXECUTE schema
    // (D-LK10-ENTRY 2.13) — shipped macho64-x86_64-darwin-exec values.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-cfx-default","kind":"macho"},
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
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
    EXPECT_FALSE((*r)->machoImage().useChainedFixups);
}

TEST(MachOExecFormatJson, UseChainedFixupsAcceptsTrue) {
    // Entry cluster required by validate() on an MH_EXECUTE schema
    // (D-LK10-ENTRY 2.13) — shipped macho64-x86_64-darwin-exec values.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-cfx-on","kind":"macho"},
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "useChainedFixups": true
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ]
    })");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE((*r)->machoImage().useChainedFixups);
}

TEST(MachOExecFormatJson, UseChainedFixupsRejectsNonBoolean) {
    // Entry cluster present so the non-boolean `useChainedFixups` is the
    // ONLY rejection reason (D-LK10-ENTRY 2.13 would otherwise reject
    // this MH_EXECUTE schema for declaring no `processExit` too). Values
    // VERBATIM from the shipped
    // src/dss-config/object-formats/macho64-x86_64-darwin-exec.format.json,
    // matching this fixture's x86_64 cputype; inert (no trampoline).
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-cfx-bad","kind":"macho"},
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "useChainedFixups": "yes"
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
    EXPECT_EQ(countAtPath(r, "/image/useChainedFixups"), 1u) << rejectSummary(r);
    // Three DIFFERENT rules emit at that pointer (this loader type check,
    // plus the MH_OBJECT and MH_DYLIB validate() rejects), so pin the
    // message unique to the type check under test.
    EXPECT_EQ(countWithMessage(r, "'useChainedFixups' must be a boolean"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

// D-LK6-14-INTEGRATION-PAYLOAD closed 2026-06-01: wire
// `dss::macho::detail::buildChainedFixupsPayload` into
// encodeExecDynamic — when `useChainedFixups=true`, replace the
// legacy LC_DYLD_INFO_ONLY emission with LC_DYLD_CHAINED_FIXUPS
// pointing at the payload in __LINKEDIT. Companion
// D-LK6-14-INTEGRATION-GOT-SLOTS (open) populates __got slots
// with DYLD_CHAINED_PTR_64 bitfields + drops LC_DYSYMTAB.
// Byte-level tests pin LC structure; runtime loadability is FF6
// territory.
namespace {
// Inline-test fixture used by the 4 chained-fixups integration
// pins below. Same shape as the legacy BindNowFalse fixture —
// one extern, one function, one BRANCH relocation — but with
// `image.useChainedFixups = true`. Its `processExit` +
// `entryCallingConvention` pair is required by validate() on any
// MH_EXECUTE schema (D-LK10-ENTRY 2.13); the values are the shipped
// macho64-x86_64-darwin-exec ones, matching the fixture's cputype.
// They emit nothing here — the trampoline they describe is injected
// only by `linker::link`, and these pins drive the writer directly.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const>
loadChainedFixupsExecFormat() {
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-cfx-integration","kind":"macho"},
      "entryPoint": "",
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "useChainedFixups": true
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
    if (!fmt.has_value()) return nullptr;
    return *fmt;
}
[[nodiscard]] AssembledModule chainedFixupsTestModule() {
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
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    return mod;
}
// LC_DYLD_CHAINED_FIXUPS = 0x80000034 (LC_REQ_DYLD bit set).
constexpr std::uint32_t kLcDyldChainedFixups = 0x80000034u;
constexpr std::uint32_t kLcDyldInfoOnly      = 0x80000022u;
} // namespace

TEST(MachOExecWriter, ChainedFixupsLcPresent) {
    // Primary mutual-exclusion pin: useChainedFixups=true emits
    // LC_DYLD_CHAINED_FIXUPS AND no LC_DYLD_INFO_ONLY. A regression
    // that re-introduces both LCs (or the wrong one) fails this.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    EXPECT_TRUE(dss::macho::test::findLoadCommand(bytes, kLcDyldChainedFixups).has_value())
        << "LC_DYLD_CHAINED_FIXUPS must be emitted on the chained "
           "path";
    EXPECT_FALSE(dss::macho::test::findLoadCommand(bytes, kLcDyldInfoOnly).has_value())
        << "LC_DYLD_INFO_ONLY must NOT be emitted when "
           "useChainedFixups=true — legacy + modern are mutually "
           "exclusive";
}

TEST(MachOExecWriter, ChainedFixupsLcCmdsizeIs16Bytes) {
    // LC_DYLD_CHAINED_FIXUPS uses the linkedit_data_command shape:
    // cmd / cmdsize / dataoff / datasize = 4+4+4+4 = 16 bytes.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const lcOff = dss::macho::test::findLoadCommand(bytes, kLcDyldChainedFixups);
    ASSERT_TRUE(lcOff.has_value());
    std::uint32_t const cmdsize =
        readU32LE(bytes, static_cast<std::size_t>(*lcOff + 4));
    EXPECT_EQ(cmdsize, 16u)
        << "linkedit_data_command shape: cmd+cmdsize+dataoff+datasize "
           "= 4+4+4+4 = 16 bytes";
    // dataoff must lie within the buffer + datasize must be non-zero
    // (we have 1 import → at least 1 byte name + NUL + header + starts
    // + import row).
    std::uint32_t const dataoff =
        readU32LE(bytes, static_cast<std::size_t>(*lcOff + 8));
    std::uint32_t const datasize =
        readU32LE(bytes, static_cast<std::size_t>(*lcOff + 12));
    EXPECT_GT(datasize, 0u);
    EXPECT_LE(static_cast<std::uint64_t>(dataoff) +
              static_cast<std::uint64_t>(datasize),
              bytes.size())
        << "dataoff + datasize must lie within the emitted binary";
}

TEST(MachOExecWriter, ChainedFixupsPayloadImportsCountMatchesExterns) {
    // Read the payload's dyld_chained_fixups_header.imports_count
    // field (at payload offset 16) and assert it equals the number
    // of externImports (1 in the fixture).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const lcOff = dss::macho::test::findLoadCommand(bytes, kLcDyldChainedFixups);
    ASSERT_TRUE(lcOff.has_value());
    std::uint32_t const dataoff =
        readU32LE(bytes, static_cast<std::size_t>(*lcOff + 8));
    // dyld_chained_fixups_header layout:
    //   [ 0.. 3] fixups_version
    //   [ 4.. 7] starts_offset
    //   [ 8..11] imports_offset
    //   [12..15] symbols_offset
    //   [16..19] imports_count
    std::uint32_t const importsCount =
        readU32LE(bytes, static_cast<std::size_t>(dataoff + 16));
    EXPECT_EQ(importsCount, mod.externImports.size())
        << "payload imports_count must equal module.externImports.size()";
}

TEST(MachOExecWriter, ChainedFixupsSymbolsPoolContainsExternName) {
    // Read symbols_offset from the payload header (at payload+12),
    // then walk into the symbols pool past the NUL sentinel and
    // assert the first name is "_printf" (the fixture's mangledName).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const lcOff = dss::macho::test::findLoadCommand(bytes, kLcDyldChainedFixups);
    ASSERT_TRUE(lcOff.has_value());
    std::uint32_t const dataoff =
        readU32LE(bytes, static_cast<std::size_t>(*lcOff + 8));
    std::uint32_t const symbolsOffset =
        readU32LE(bytes, static_cast<std::size_t>(dataoff + 12));
    // Symbols pool: leading NUL sentinel at relative offset 0; first
    // import's name at offset 1.
    std::size_t const firstNameOff =
        static_cast<std::size_t>(dataoff) +
        static_cast<std::size_t>(symbolsOffset) + 1u;
    ASSERT_LT(firstNameOff, bytes.size());
    std::string firstName;
    for (std::size_t i = firstNameOff;
         i < bytes.size() && bytes[i] != 0u; ++i) {
        firstName.push_back(static_cast<char>(bytes[i]));
    }
    EXPECT_EQ(firstName, "_printf")
        << "symbols pool's first NUL-terminated name must be the "
           "fixture's extern mangledName ('_printf'); this pins the "
           "end-to-end ExternImport.mangledName → ChainedFixupImport.name "
           "→ payload pool flow";
}

// 8aabc04 audit fold (test-analyzer + test-analyzer-dim-2 HIGH):
// pin LC_DYSYMTAB stays emitted on the chained path. The companion
// D-LK6-14-INTEGRATION-GOT-SLOTS will drop it together with __got
// slot bitfield population — a premature regression that drops
// LC_DYSYMTAB here would produce structurally broken chained
// binaries with no failing test.
// D-LK6-14-INTEGRATION-GOT-SLOTS closed: LC_DYSYMTAB is DROPPED on
// the chained-fixups path (chained pointers in __got encode the
// import ordinal directly via DYLD_CHAINED_PTR_64 bits [0..23], so
// the indirect symbol table is redundant). The prior pin (which
// pinned PRESENCE during the substrate window) is now inverted.
TEST(MachOExecWriter, ChainedFixupsDropsLcDysymtab) {
    constexpr std::uint32_t kLcDysymtab = 0x0Bu;
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(
        dss::macho::test::findLoadCommand(bytes, kLcDysymtab).has_value())
        << "LC_DYSYMTAB MUST be absent on the chained-fixups path — "
           "D-LK6-14-INTEGRATION-GOT-SLOTS closed: chained pointers "
           "in __got encode the import ordinal directly so the "
           "indirect symbol table is redundant. A regression that "
           "re-emits LC_DYSYMTAB here would produce dyld-rejected "
           "binaries because ncmds/sizeofcmds arithmetic accounts "
           "for the absence.";
}

// D-LK6-14-INTEGRATION-GOT-SLOTS pin: each __got slot must hold a
// valid DYLD_CHAINED_PTR_64 bind bitfield (bit 63 set, ordinal in
// bits [0..23] matching the import index, next field forming a
// valid chain). Without this, dyld cannot resolve extern imports.
TEST(MachOExecWriter, ChainedFixupsGotSlotsHaveBindBitfield) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
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
        ExternImport{SymbolId{99}, "_a",
                     "/usr/lib/libSystem.B.dylib"});
    mod.externImports.push_back(
        ExternImport{SymbolId{100}, "_b",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Walk LC_SEGMENT_64s, find __DATA_CONST, read its fileoff so we
    // can read the 8-byte __got slots directly. segment_command_64
    // layout: cmd(4) + cmdsize(4) + segname[16] + vmaddr(8) +
    // vmsize(8) + fileoff(8) → fileoff at lcOff + 40.
    constexpr std::uint32_t kLcSegment64 = 0x19u;
    std::optional<std::size_t> dataConstFileOff;
    std::optional<std::size_t> dataConstLcOff;
    std::size_t lcOff = 32;  // past mach_header_64
    std::uint32_t const ncmds = readU32LE(bytes, 16);
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd = readU32LE(bytes, lcOff);
        std::uint32_t const cmdsize = readU32LE(bytes, lcOff + 4);
        if (cmd == kLcSegment64) {
            char name[17] = {};
            std::memcpy(name, &bytes[lcOff + 8], 16);
            if (std::string{name} == "__DATA_CONST") {
                dataConstFileOff = static_cast<std::size_t>(
                    readU64LE(bytes, lcOff + 40));
                dataConstLcOff = lcOff;
                break;
            }
        }
        lcOff += cmdsize;
    }
    ASSERT_TRUE(dataConstFileOff.has_value())
        << "__DATA_CONST LC_SEGMENT_64 must be present";
    // D-LK6-14-INTEGRATION-GOT-SLOTS: __got section_64.reserved1
    // MUST be 0 on the chained path (was numExterns on legacy as
    // an indirect-symtab index; on chained the indirect symtab is
    // dropped so the reference becomes invalid). section_64 starts
    // at segment_command_64 + 72 (cmd(4)+cmdsize(4)+segname[16]
    // +vmaddr(8)+vmsize(8)+fileoff(8)+filesize(8)+maxprot(4)
    // +initprot(4)+nsects(4)+flags(4)); reserved1 within
    // section_64 at offset 68 (sectname[16]+segname[16]+addr(8)
    // +size(8)+offset(4)+align(4)+reloff(4)+nreloc(4)+flags(4)
    // = 68). a4464fe audit-fold (dim-2 M1 + test-analyzer MEDIUM):
    // pin sectname == "__got" first so a __const-before-__got
    // reshape doesn't silently read a sibling section's reserved1.
    std::size_t const sect0Off = *dataConstLcOff + 72;
    char gotName[17] = {};
    std::memcpy(gotName, &bytes[sect0Off], 16);
    EXPECT_EQ(std::string{gotName}, "__got")
        << "expected __got at section[0] of __DATA_CONST — a sibling "
           "section reshape would silently shift offsets and read "
           "the wrong section's reserved1.";
    EXPECT_EQ(readU32LE(bytes, sect0Off + 68), 0u)
        << "section_64.__got.reserved1 must be 0 on the chained path "
           "(was numExterns as indirect-symtab index on legacy; the "
           "indirect symtab is gone so the reference would be stale)";
    // Read slot[0] and slot[1] (8 bytes each).
    std::uint64_t const slot0 = readU64LE(bytes, *dataConstFileOff);
    std::uint64_t const slot1 = readU64LE(bytes, *dataConstFileOff + 8);
    // bit 63 = bind (must be 1 on both slots — they're binds, not rebases).
    EXPECT_NE(slot0 & (1ull << 63), 0u)
        << "slot[0] bind bit (63) must be set";
    EXPECT_NE(slot1 & (1ull << 63), 0u)
        << "slot[1] bind bit (63) must be set";
    // bits [0..23] = ordinal: slot[i] → import row i.
    EXPECT_EQ(slot0 & 0xFFFFFFull, 0u)
        << "slot[0] ordinal must be 0 (first import)";
    EXPECT_EQ(slot1 & 0xFFFFFFull, 1u)
        << "slot[1] ordinal must be 1 (second import)";
    // bits [51..62] = next (12 bits, 4-byte units to next chain entry).
    // Slot[0] points at slot[1] (8 bytes away = 2 four-byte units).
    // Slot[1] is the last → next = 0.
    std::uint64_t const next0 = (slot0 >> 51) & 0xFFFu;
    std::uint64_t const next1 = (slot1 >> 51) & 0xFFFu;
    EXPECT_EQ(next0, 2u)
        << "slot[0] next field must be 2 (4-byte units to slot[1])";
    EXPECT_EQ(next1, 0u)
        << "slot[1] next field must be 0 (end of chain)";
}

// D-LK6-14-INTEGRATION-GOT-SLOTS pin: the chained-fixups payload's
// seg_info_offset[0] must be non-zero (= 8) and point at a
// dyld_chained_starts_in_segment struct with pointer_format=6 and
// non-zero segment_offset. A regression dropping the segInfo arg
// would leave seg_info_offset[0] = 0 (substrate behavior) and dyld
// would see "no chains in segment".
TEST(MachOExecWriter, ChainedFixupsPayloadHasStartsInSegment) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const lcOff =
        dss::macho::test::findLoadCommand(bytes, kLcDyldChainedFixups);
    ASSERT_TRUE(lcOff.has_value());
    std::uint32_t const dataoff =
        readU32LE(bytes, *lcOff + 8);
    // starts_offset is at payload+4 (header field).
    std::uint32_t const startsOff = readU32LE(bytes, dataoff + 4);
    // starts_in_image: seg_count (u32) at startsOff, seg_info_offset[0]
    // (u32) at startsOff+4.
    EXPECT_EQ(readU32LE(bytes, dataoff + startsOff), 1u)
        << "seg_count must be 1 (single __DATA_CONST segment)";
    std::uint32_t const segInfoOffset =
        readU32LE(bytes, dataoff + startsOff + 4);
    EXPECT_EQ(segInfoOffset, 8u)
        << "seg_info_offset[0] must be 8 (immediately after the "
           "starts_in_image header); a regression dropping segInfo "
           "would leave this 0 (substrate behavior) and dyld would "
           "see 'no chains in segment'";
    // dyld_chained_starts_in_segment at startsOff + 8:
    //   [ 0.. 3] size           [ 4.. 5] page_size
    //   [ 6.. 7] pointer_format [ 8..15] segment_offset
    //   [16..19] max_valid_ptr  [20..21] page_count
    std::size_t const segStructOff =
        dataoff + startsOff + segInfoOffset;
    // size field at struct+0: header bytes + 2 bytes per page_start
    // entry. v1 single-page → 22 + 2*1 = 24. Symbolized via
    // kDyldChainedStartsInSegmentHdrSz so a header-size regression
    // (someone adds a field) surfaces in this test alongside the
    // producer. FLIP-MARKER: when D-LK6-14-MULTI-PAGE-GOT closes,
    // expected size becomes `kDyldChainedStartsInSegmentHdrSz +
    // 2u * page_count` with page_count > 1.
    EXPECT_EQ(readU32LE(bytes, segStructOff + 0),
              static_cast<std::uint32_t>(
                  ::dss::macho::detail::kDyldChainedStartsInSegmentHdrSz
                  + 2u * 1u))
        << "dyld_chained_starts_in_segment.size must be header (22) "
           "+ 2*page_count (2) = 24 for the v1 single-page case";
    EXPECT_EQ(readU16LE(bytes, segStructOff + 6),
              6u)
        << "pointer_format must be 6 (DYLD_CHAINED_PTR_64)";
    EXPECT_NE(readU64LE(bytes, segStructOff + 8), 0u)
        << "segment_offset must be non-zero (= gotVa - "
           "__TEXT.vmaddr); a regression that leaves it 0 means dyld "
           "would resolve the chain at the wrong VM address";
    EXPECT_EQ(readU16LE(bytes, segStructOff + 20), 1u)
        << "page_count must be 1 (single-page __DATA_CONST)";
    // page_starts[0] at segStructOff + 22.
    EXPECT_EQ(readU16LE(bytes, segStructOff + 22), 0u)
        << "page_starts[0] must be 0 (first chained pointer at byte "
           "0 of the page — __got starts at __DATA_CONST start)";
}

// D-LK6-14-SIZEOFCMDS-DELTA-PIN: chained path's sizeofcmds must be
// exactly `kDysymtabCommandSize` (80) less than legacy path's (since
// LC_DYSYMTAB is dropped on chained). Pins the ncmds/sizeofcmds
// arithmetic against subtle drift.
TEST(MachOExecWriter, ChainedFixupsSizeofcmdsDelta) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    // Legacy fixture (useChainedFixups absent → defaults to false).
    // Its entry cluster mirrors the chained fixture's — required by
    // validate() on an MH_EXECUTE schema (D-LK10-ENTRY 2.13) and
    // IDENTICAL on both sides, so the sizeofcmds delta pinned below
    // isolates exactly the LC_DYLD_INFO_ONLY/LC_DYSYMTAB swap.
    auto fmtLegacy = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-legacy-for-delta","kind":"macho"},
      "entryPoint": "",
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
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
    ASSERT_TRUE(fmtLegacy.has_value());
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytesLegacy = encodeUntrampolined(mod, **target, **fmtLegacy, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto fmtChained = loadChainedFixupsExecFormat();
    ASSERT_NE(fmtChained, nullptr);
    auto bytesChained = encodeUntrampolined(mod, **target, *fmtChained, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // sizeofcmds field is at mach_header_64 offset 20 (u32).
    std::uint32_t const sizeofcmdsLegacy =
        readU32LE(bytesLegacy, 20);
    std::uint32_t const sizeofcmdsChained =
        readU32LE(bytesChained, 20);
    // Delta = LC_DYLD_INFO_ONLY (48) - LC_DYLD_CHAINED_FIXUPS (16)
    //       + LC_DYSYMTAB (80, dropped on chained) = 112.
    EXPECT_EQ(sizeofcmdsLegacy - sizeofcmdsChained, 112u)
        << "sizeofcmds delta: legacy emits LC_DYLD_INFO_ONLY (48) + "
           "LC_DYSYMTAB (80) = 128; chained emits LC_DYLD_CHAINED_"
           "FIXUPS (16) only = 16; delta = 112. Regression in any "
           "arm of the ternary or in the LC sizes would shift this.";
}

// D-LK6-14-MULTI-PAGE-GOT guard pin (2ba0489 audit fold, test-
// analyzer + dim-2 + simplifier convergence): >512 externs needs
// multi-page chains with per-page page_starts[i]. Until that lands,
// v1 fails loud K_NoMatchingObjectFormat. A regression dropping the
// guard would silently emit a single-page chain that dyld walks off
// the end of, producing load failures only at runtime on macOS.
TEST(MachOExecWriter, ChainedFixupsMultiPageGotFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    // Reloc targets the FIRST extern (SymbolId{100}) so symbol
    // resolution passes; the guard we're pinning fires later, in
    // section (l.5) after layout.
    fn.relocations.push_back(
        Relocation{1u, SymbolId{100u}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    // 4 KiB page / 8-byte __got slot = 512 externs fits; 513 spills
    // onto a second page and trips the guard.
    constexpr std::uint32_t kPageSize    = 4096u;
    constexpr std::uint32_t kSlotSize    = 8u;
    constexpr std::uint32_t kSpillCount  = (kPageSize / kSlotSize) + 1u;
    // FIXTURE-INVARIANT (D-TEST-MULTI-PAGE-FIXTURE-INVARIANT): all
    // 513 externs share libSystem (libOrdinal=1 << 127 ceiling) and
    // the symbols-pool stays under 8 MiB (D-LK6-14-NAME-OFFSET-OVERFLOW).
    // The SOLE failure surface this fixture probes is
    // the multi-page __DATA_CONST guard. A future refactor of the
    // fixture (multi-dylib, longer names) MUST re-verify these
    // boundaries or the test silently re-routes.
    for (std::uint32_t i = 0; i < kSpillCount; ++i) {
        mod.externImports.push_back(ExternImport{
            SymbolId{100u + i},
            "_x" + std::to_string(i),
            "/usr/lib/libSystem.B.dylib"});
    }
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, *fmt, rep);
    EXPECT_TRUE(bytes.empty())
        << "Multi-page __got must emit no bytes (loud-fail path).";
    EXPECT_EQ(rep.errorCount(), 1u)
        << "Exactly one K_NoMatchingObjectFormat must fire.";
    bool found = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_NoMatchingObjectFormat &&
            d.actual.find("D-LK6-14-MULTI-PAGE-GOT") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found)
        << "Diagnostic must cite D-LK6-14-MULTI-PAGE-GOT for "
           "future-grep navigability";
}

// 8aabc04 audit fold (test-analyzer-dim-2 HIGH): multi-import name
// ordering. Existing tests use N=1 so a regression that swaps the
// order of `push_back(0)` and `nameOffsets.push_back(...)` would
// shift every offset by 1 but pass N=1 (the lone import would still
// land at offset 1). Use N=2 and assert offsets advance correctly.
TEST(MachOExecWriter, ChainedFixupsTwoImportsHaveCorrectSymbolOrdering) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
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
        ExternImport{SymbolId{99}, "_alpha",
                     "/usr/lib/libSystem.B.dylib"});
    mod.externImports.push_back(
        ExternImport{SymbolId{100}, "_beta",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const lcOff = dss::macho::test::findLoadCommand(bytes, kLcDyldChainedFixups);
    ASSERT_TRUE(lcOff.has_value());
    std::uint32_t const dataoff =
        readU32LE(bytes, static_cast<std::size_t>(*lcOff + 8));
    // Pool layout for {"_alpha", "_beta"}:
    //   [0]NUL  [1..6]"_alpha"[0]  [8..12]"_beta"[0]
    // Symbols offset in payload from header[12].
    std::uint32_t const symbolsOffset =
        readU32LE(bytes, static_cast<std::size_t>(dataoff + 12));
    std::size_t const poolBase =
        static_cast<std::size_t>(dataoff) + symbolsOffset;
    EXPECT_EQ(bytes[poolBase + 0], 0u)         << "leading NUL sentinel";
    EXPECT_EQ(bytes[poolBase + 1], '_');
    EXPECT_EQ(bytes[poolBase + 6], 'a')        << "_alpha tail char";
    EXPECT_EQ(bytes[poolBase + 7], 0u)         << "NUL after _alpha";
    EXPECT_EQ(bytes[poolBase + 8], '_');
    EXPECT_EQ(bytes[poolBase + 12], 'a')       << "_beta tail char";
    EXPECT_EQ(bytes[poolBase + 13], 0u)        << "NUL after _beta";
    // 5ac97ae audit fold (test-analyzer + test-analyzer-dim-2 +
    // code-architect convergence): pin the packed import-row fields
    // for BOTH imports. Pool-position pin catches push-NUL swap;
    // packed-row pin catches libOrdinal miscast + weakImport
    // hardcode regression + dylibOrdinal multi-import mapping.
    std::uint32_t const importsOffset =
        readU32LE(bytes, static_cast<std::size_t>(dataoff + 8));
    std::uint32_t const row0 =
        readU32LE(bytes, dataoff + importsOffset + 0 * 4);
    std::uint32_t const row1 =
        readU32LE(bytes, dataoff + importsOffset + 1 * 4);
    EXPECT_EQ(row0 & 0xFFu, 1u)
        << "_alpha libOrdinal must be 1 (libSystem is loadDylibs[0])";
    EXPECT_EQ(row1 & 0xFFu, 1u)
        << "_beta libOrdinal must also be 1 (same dylib) — catches "
           "dylibOrdinal mis-mapping for multi-import path";
    EXPECT_EQ((row0 >> 8) & 0x1u, 0u)
        << "weakImport=false hardcode (D-LK6-14-MACHO-WEAK-DEF) "
           "must not flip on chained-fixups walker path";
    EXPECT_EQ((row1 >> 8) & 0x1u, 0u);
    EXPECT_EQ(row0 >> 9, 1u)
        << "_alpha name_offset = 1 (NUL sentinel at 0)";
    EXPECT_EQ(row1 >> 9, 8u)
        << "_beta name_offset = 8 (NUL + '_alpha' + NUL = 7 bytes)";
}

// 8aabc04 audit fold (test-analyzer + test-analyzer-dim-2 HIGH):
// pin the D-LK6-14-NAME-OFFSET-OVERFLOW guard. Encode with a
// cumulative symbols pool that exceeds the 23-bit name_offset
// field (8 MiB - 1) and assert the walker fails loud with
// K_SymbolUndefined citing the anchor.
TEST(MachOExecWriter, ChainedFixupsNameOffsetOverflowFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
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
    // Single extern with a mangledName at the field ceiling. Pool
    // accounting: 1 (NUL sentinel) + name.size() + 1 (NUL terminator).
    // Push the name to exactly (1 << 23) bytes — total pool = 1 + (1<<23) + 1
    // = 8 MiB + 2, which exceeds the (1 << 23) - 1 ceiling.
    std::string longName(static_cast<std::size_t>(1) << 23, 'x');
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, std::move(longName),
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, *fmt, rep);
    EXPECT_TRUE(bytes.empty())
        << "name-offset overflow must abort emission";
    bool sawAnchor = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined
         && d.actual.find("D-LK6-14-NAME-OFFSET-OVERFLOW") != std::string::npos) {
            sawAnchor = true;
        }
    }
    EXPECT_TRUE(sawAnchor)
        << "walker MUST emit K_SymbolUndefined citing "
           "D-LK6-14-NAME-OFFSET-OVERFLOW so operators can either "
           "reduce import-name length OR fall back to LC_DYLD_INFO_ONLY";
}

TEST(MachOExecFormatJson, BindNowDefaultsToTrue) {
    // Entry cluster required by validate() on an MH_EXECUTE schema
    // (D-LK10-ENTRY 2.13) — shipped macho64-x86_64-darwin-exec values.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-bindnow-default","kind":"macho"},
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
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
    EXPECT_TRUE((*r)->machoImage().bindNow);
}

TEST(MachOExecFormatJson, PageZeroSizeMustBePowerOfTwo) {
    // Walker depends on pageZeroSize being page-aligned (vmaddr =
    // pageZeroSize so mmap-congruence requires it). Validate must
    // reject non-power-of-two values. (pr-test-analyzer Gap 1 fold
    // — LK6 cycle 2c post-fold review.)
    //
    // Entry cluster present so the non-power-of-two pageZeroSize is the
    // ONLY rejection reason (D-LK10-ENTRY 2.13 would otherwise reject
    // this MH_EXECUTE schema for declaring no `processExit` too). Values
    // VERBATIM from the shipped
    // src/dss-config/object-formats/macho64-x86_64-darwin-exec.format.json,
    // matching this fixture's x86_64 cputype; inert (no trampoline).
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-bad-pagezero","kind":"macho"},
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 12884901888,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"]
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":17179869184}
      ]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/image/pageZeroSize"), 1u) << rejectSummary(r);
    // Three DIFFERENT rules emit at that pointer (the exec non-zero
    // requirement, the MH_DYLIB must-be-zero rule, and this power-of-two
    // check), so pin the message unique to the rule under test.
    EXPECT_EQ(countWithMessage(r, "must be a power of two"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(MachOExecWriter, TextSegmentFilesizeCoversStubsEnd) {
    // CRITICAL anti-regression for the __TEXT.filesize fix
    // (code-reviewer C1 fold). The buggy formula was
    // `stubsEnd - textFileOff` (would have truncated dyld's mmap
    // before reaching .text); the correct formula is `stubsEnd`
    // since __TEXT.fileoff = 0 by Apple convention.
    // (pr-test-analyzer Gap 3 fold — LK6 cycle 2c post-fold review.)
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back({1, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    // Walk to LC_SEGMENT_64 __TEXT; read filesize + fileoff +
    // section_64{__text/__stubs} to compute the expected
    // (stubsFileOff + stubsFileSize) value, then assert filesize
    // matches.
    std::uint32_t ncmds =
        static_cast<std::uint32_t>(bytes[16]) |
        (static_cast<std::uint32_t>(bytes[17]) << 8) |
        (static_cast<std::uint32_t>(bytes[18]) << 16) |
        (static_cast<std::uint32_t>(bytes[19]) << 24);
    std::size_t off = 32;
    bool foundText = false;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t cmd =
            static_cast<std::uint32_t>(bytes[off]) |
            (static_cast<std::uint32_t>(bytes[off+1]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+2]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+3]) << 24);
        std::uint32_t cmdsize =
            static_cast<std::uint32_t>(bytes[off+4]) |
            (static_cast<std::uint32_t>(bytes[off+5]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+6]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+7]) << 24);
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            std::string segName(
                reinterpret_cast<char const*>(&bytes[off + 8]),
                strnlen(reinterpret_cast<char const*>(
                            &bytes[off + 8]), 16));
            if (segName == "__TEXT") {
                std::uint64_t fileOff = 0, fileSize = 0;
                for (int b = 0; b < 8; ++b) {
                    fileOff |= static_cast<std::uint64_t>(
                                   bytes[off + 40 + b]) << (b * 8);
                    fileSize |= static_cast<std::uint64_t>(
                                    bytes[off + 48 + b]) << (b * 8);
                }
                // __TEXT.fileoff = 0 (Apple convention — mach
                // header lives inside __TEXT).
                EXPECT_EQ(fileOff, 0u);
                // Find __stubs section_64 to compute stubsEnd.
                std::uint32_t nsects =
                    static_cast<std::uint32_t>(bytes[off+64]) |
                    (static_cast<std::uint32_t>(bytes[off+65]) << 8) |
                    (static_cast<std::uint32_t>(bytes[off+66]) << 16) |
                    (static_cast<std::uint32_t>(bytes[off+67]) << 24);
                std::size_t secOff = off + 72;
                std::uint64_t stubsEnd = 0;
                for (std::uint32_t s = 0; s < nsects; ++s) {
                    std::string secName(
                        reinterpret_cast<char const*>(&bytes[secOff]),
                        strnlen(reinterpret_cast<char const*>(
                                    &bytes[secOff]), 16));
                    std::uint64_t secSize = 0;
                    for (int b = 0; b < 8; ++b)
                        secSize |= static_cast<std::uint64_t>(
                                       bytes[secOff + 40 + b]) << (b*8);
                    std::uint32_t secFileOff =
                        static_cast<std::uint32_t>(bytes[secOff + 48]) |
                        (static_cast<std::uint32_t>(bytes[secOff + 49]) << 8) |
                        (static_cast<std::uint32_t>(bytes[secOff + 50]) << 16) |
                        (static_cast<std::uint32_t>(bytes[secOff + 51]) << 24);
                    if (secName == "__stubs") {
                        stubsEnd = static_cast<std::uint64_t>(secFileOff) + secSize;
                    }
                    secOff += 80;
                }
                ASSERT_NE(stubsEnd, 0u);
                EXPECT_EQ(fileSize, stubsEnd)
                    << "__TEXT.filesize must equal stubsEnd "
                       "(buggy formula was stubsEnd - textFileOff "
                       "which would have truncated dyld's mmap)";
                foundText = true;
                break;
            }
        }
        off += cmdsize;
    }
    EXPECT_TRUE(foundText);
}

TEST(MachOExecFormatJson, BindNowTypeCheckRejectsNonBoolean) {
    // Entry cluster present so the non-boolean `bindNow` is the ONLY
    // rejection reason (D-LK10-ENTRY 2.13 would otherwise reject this
    // MH_EXECUTE schema for declaring no `processExit` too). Values
    // VERBATIM from the shipped
    // src/dss-config/object-formats/macho64-x86_64-darwin-exec.format.json,
    // matching this fixture's x86_64 cputype; inert (no trampoline).
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-bindnow-wrong","kind":"macho"},
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "bindNow": "true"
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
    EXPECT_EQ(countAtPath(r, "/image/bindNow"), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(MachOExecWriter, MultipleExternsInTwoLibrariesEmitTwoLcLoadDylibRefs) {
    // 2-extern × 2-library smoke test — both dylib paths appear
    // in the file (both LC_LOAD_DYLIB strings + both bind opcode
    // dylib ordinals). The JSON's entry cluster is required by
    // validate() on an MH_EXECUTE schema (D-LK10-ENTRY 2.13); its
    // libSystem importLibraryPath is already loadDylibs[0] here.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "leading-underscore" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"macho-two-libs","kind":"macho"},
      "entryPoint": "",
      "runtimeLibraries": [{"role":"cLibrary","image":"/usr/lib/libSystem.B.dylib"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary",
        "importMangledName": "_exit" },
      "entryCallingConvention": "sysv_amd64",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": [
          "/usr/lib/libSystem.B.dylib",
          "/usr/lib/libobjc.A.dylib"
        ]
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
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back(
        {1, SymbolId{99}, RelocationKind{1}, 0});
    fn.relocations.push_back(
        {6, SymbolId{100}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    mod.externImports.push_back(
        ExternImport{SymbolId{100}, "_objc_msgSend",
                     "/usr/lib/libobjc.A.dylib"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    std::string_view fv{
        reinterpret_cast<char const*>(bytes.data()), bytes.size()};
    EXPECT_NE(fv.find("/usr/lib/libSystem.B.dylib"),
              std::string_view::npos);
    EXPECT_NE(fv.find("/usr/lib/libobjc.A.dylib"),
              std::string_view::npos);
    EXPECT_NE(fv.find("_printf"), std::string_view::npos);
    EXPECT_NE(fv.find("_objc_msgSend"), std::string_view::npos);
}

TEST(MachOExecWriter, BindStreamEmitsExpectedOpcodeShape) {
    // Pin the bind opcode stream byte shape so a future regression
    // in IMM/ULEB threshold, trailing NUL, opcode ordering, or
    // segment-index miswiring shows up as a test failure rather
    // than a dyld load-time crash (pr-test-analyzer FOLD-NOW #1).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back({1, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    // Walk to LC_DYLD_INFO_ONLY: header at +32, scan load cmds.
    // Header layout: magic(4) cputype(4) cpusubtype(4) filetype(4)
    //                ncmds(4) sizeofcmds(4) flags(4) reserved(4)
    std::uint32_t ncmds =
        static_cast<std::uint32_t>(bytes[16]) |
        (static_cast<std::uint32_t>(bytes[17]) << 8) |
        (static_cast<std::uint32_t>(bytes[18]) << 16) |
        (static_cast<std::uint32_t>(bytes[19]) << 24);
    std::size_t off = 32;
    std::uint64_t bindOff = 0, bindSize = 0;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t cmd =
            static_cast<std::uint32_t>(bytes[off]) |
            (static_cast<std::uint32_t>(bytes[off+1]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+2]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+3]) << 24);
        std::uint32_t cmdsize =
            static_cast<std::uint32_t>(bytes[off+4]) |
            (static_cast<std::uint32_t>(bytes[off+5]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+6]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+7]) << 24);
        if (cmd == 0x80000022u) {
            // dyld_info_command: cmd(4) cmdsize(4) rebase_off(4)
            // rebase_size(4) bind_off(4) bind_size(4) ...
            bindOff =
                static_cast<std::uint64_t>(bytes[off+16]) |
                (static_cast<std::uint64_t>(bytes[off+17]) << 8) |
                (static_cast<std::uint64_t>(bytes[off+18]) << 16) |
                (static_cast<std::uint64_t>(bytes[off+19]) << 24);
            bindSize =
                static_cast<std::uint64_t>(bytes[off+20]) |
                (static_cast<std::uint64_t>(bytes[off+21]) << 8) |
                (static_cast<std::uint64_t>(bytes[off+22]) << 16) |
                (static_cast<std::uint64_t>(bytes[off+23]) << 24);
            break;
        }
        off += cmdsize;
    }
    ASSERT_GT(bindSize, 0u);
    ASSERT_LE(bindOff + bindSize, bytes.size());
    // Expected stream prefix: SET_DYLIB_ORDINAL_IMM | 1 (lib #1),
    // SET_SYMBOL_TRAILING_FLAGS_IMM | 0, "_printf\0",
    // SET_TYPE_IMM | BIND_TYPE_POINTER (1),
    // SET_SEGMENT_AND_OFFSET_ULEB | 2 (kSegIdxDataConst), ULEB(0),
    // DO_BIND, DONE.
    std::vector<std::uint8_t> expected = {
        0x10u | 1u,               // SET_DYLIB_ORDINAL_IMM | 1
        0x40u | 0u,               // SET_SYMBOL_TRAILING_FLAGS_IMM
        '_','p','r','i','n','t','f', 0,
        0x50u | 1u,               // SET_TYPE_IMM | BIND_TYPE_POINTER
        0x70u | 2u,               // SET_SEGMENT_AND_OFFSET_ULEB | 2
        0x00u,                    // ULEB128(0) — offset 0 into __got
        0x90u,                    // DO_BIND
        0x00u,                    // DONE
    };
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(bytes[bindOff + i], expected[i])
            << "bind-stream byte " << i << " mismatch";
    }
}

TEST(MachOExecWriter, StubDispPointsAtGotSlot) {
    // Pin the FF 25 disp32 → __got slot arithmetic. Cycle-2c
    // correctness depends on disp32 = gotSlotVa - (stubVa + 6).
    // (pr-test-analyzer FOLD-NOW #2.)
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back({1, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    // Walk LCs to find __stubs section (in __TEXT segment) and
    // __got section (in __DATA_CONST). Each section_64 carries
    // addr(u64) + size(u64) + offset(u32). For each LC_SEGMENT_64
    // we read nsects from segment_command_64 (at +64 from cmd
    // start). Section_64 records follow each LC_SEGMENT_64 header.
    std::uint32_t ncmds =
        static_cast<std::uint32_t>(bytes[16]) |
        (static_cast<std::uint32_t>(bytes[17]) << 8) |
        (static_cast<std::uint32_t>(bytes[18]) << 16) |
        (static_cast<std::uint32_t>(bytes[19]) << 24);
    std::size_t off = 32;
    std::uint64_t stubsAddr = 0, stubsFileOff = 0;
    std::uint64_t gotAddr = 0;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t cmd =
            static_cast<std::uint32_t>(bytes[off]) |
            (static_cast<std::uint32_t>(bytes[off+1]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+2]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+3]) << 24);
        std::uint32_t cmdsize =
            static_cast<std::uint32_t>(bytes[off+4]) |
            (static_cast<std::uint32_t>(bytes[off+5]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+6]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+7]) << 24);
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            std::uint32_t nsects =
                static_cast<std::uint32_t>(bytes[off+64]) |
                (static_cast<std::uint32_t>(bytes[off+65]) << 8) |
                (static_cast<std::uint32_t>(bytes[off+66]) << 16) |
                (static_cast<std::uint32_t>(bytes[off+67]) << 24);
            std::size_t secOff = off + 72;
            for (std::uint32_t s = 0; s < nsects; ++s) {
                std::string secName(
                    reinterpret_cast<char const*>(&bytes[secOff]),
                    strnlen(reinterpret_cast<char const*>(
                                &bytes[secOff]), 16));
                std::uint64_t addr = 0;
                for (int b = 0; b < 8; ++b)
                    addr |= static_cast<std::uint64_t>(
                                bytes[secOff + 32 + b]) << (b*8);
                std::uint32_t fileOff =
                    static_cast<std::uint32_t>(bytes[secOff + 48]) |
                    (static_cast<std::uint32_t>(bytes[secOff + 49]) << 8) |
                    (static_cast<std::uint32_t>(bytes[secOff + 50]) << 16) |
                    (static_cast<std::uint32_t>(bytes[secOff + 51]) << 24);
                if (secName == "__stubs") {
                    stubsAddr = addr; stubsFileOff = fileOff;
                } else if (secName == "__got") {
                    gotAddr = addr;
                }
                secOff += 80;
            }
        }
        off += cmdsize;
    }
    ASSERT_NE(stubsAddr, 0u);
    ASSERT_NE(gotAddr, 0u);
    ASSERT_NE(stubsFileOff, 0u);
    // Stub byte 0..1 = FF 25; bytes 2..5 = disp32 (LE).
    EXPECT_EQ(bytes[stubsFileOff], 0xFFu);
    EXPECT_EQ(bytes[stubsFileOff + 1], 0x25u);
    std::int32_t disp =
        static_cast<std::int32_t>(
            static_cast<std::uint32_t>(bytes[stubsFileOff + 2]) |
            (static_cast<std::uint32_t>(bytes[stubsFileOff + 3]) << 8) |
            (static_cast<std::uint32_t>(bytes[stubsFileOff + 4]) << 16) |
            (static_cast<std::uint32_t>(bytes[stubsFileOff + 5]) << 24));
    // (stubAddr + 6) + disp = gotAddr (slot #0).
    EXPECT_EQ(
        static_cast<std::uint64_t>(
            static_cast<std::int64_t>(stubsAddr) + 6 + disp),
        gotAddr);
}

TEST(MachOExecWriter, DysymtabIundefsymNundefsymCorrect) {
    // LC_DYSYMTAB.iundefsym / nundefsym must agree with nlist
    // layout: defined externs first, undefined externs next. If
    // these drift, dyld binds the wrong symbol slots.
    // (pr-test-analyzer FOLD-NOW #3.)
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back({1, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    std::uint32_t ncmds =
        static_cast<std::uint32_t>(bytes[16]) |
        (static_cast<std::uint32_t>(bytes[17]) << 8) |
        (static_cast<std::uint32_t>(bytes[18]) << 16) |
        (static_cast<std::uint32_t>(bytes[19]) << 24);
    std::size_t off = 32;
    std::uint32_t iundefsym = 0xFFFFFFFFu, nundefsym = 0;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t cmd =
            static_cast<std::uint32_t>(bytes[off]) |
            (static_cast<std::uint32_t>(bytes[off+1]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+2]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+3]) << 24);
        std::uint32_t cmdsize =
            static_cast<std::uint32_t>(bytes[off+4]) |
            (static_cast<std::uint32_t>(bytes[off+5]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+6]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+7]) << 24);
        if (cmd == 0x0Bu) {  // LC_DYSYMTAB
            // Field layout: cmd(4) cmdsize(4) ilocalsym(4)
            // nlocalsym(4) iextdefsym(4) nextdefsym(4)
            // iundefsym(4) nundefsym(4) ...
            iundefsym =
                static_cast<std::uint32_t>(bytes[off+24]) |
                (static_cast<std::uint32_t>(bytes[off+25]) << 8) |
                (static_cast<std::uint32_t>(bytes[off+26]) << 16) |
                (static_cast<std::uint32_t>(bytes[off+27]) << 24);
            nundefsym =
                static_cast<std::uint32_t>(bytes[off+28]) |
                (static_cast<std::uint32_t>(bytes[off+29]) << 8) |
                (static_cast<std::uint32_t>(bytes[off+30]) << 16) |
                (static_cast<std::uint32_t>(bytes[off+31]) << 24);
            break;
        }
        off += cmdsize;
    }
    // 1 defined function + 1 undefined extern.
    EXPECT_EQ(iundefsym, 1u);
    EXPECT_EQ(nundefsym, 1u);
}

TEST(MachOExecWriter, UndeclaredDylibInExternImportAutoEmitsLcLoadDylib) {
    // D-FFI-MACHO-NONDEFAULT-DYLIB-LOAD: a NAMED import library not
    // declared in the format schema's `image.loadDylibs` is no longer
    // REJECTED — it is AUTO-EMITTED as its own LC_LOAD_DYLIB (the Mach-O
    // sibling of the ELF DT_NEEDED-per-referenced-library walker). This
    // supersedes the pre-cycle "undeclared dylib fails loud" contract.
    // (An EMPTY libraryPath still fails loud — see
    // EmptyLibraryPathImportFailsLoud.)
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped(
        "macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back(
        {1, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_undeclared",
                     "/usr/lib/libNotDeclared.dylib"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "a named non-schema import library must AUTO-EMIT LC_LOAD_DYLIB, "
           "not be rejected.";
    ASSERT_FALSE(bytes.empty());
    // The undeclared library appears as its own LC_LOAD_DYLIB.
    std::uint32_t const ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    bool sawUndeclared = false, sawLibSystem = false;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        ASSERT_NE(cmdsize, 0u);
        if (cmd == 0x0Cu) {  // LC_LOAD_DYLIB
            std::string const path(
                reinterpret_cast<char const*>(bytes.data()) + off
                + readU32LE(bytes, off + 8));
            if (path == "/usr/lib/libNotDeclared.dylib") sawUndeclared = true;
            if (path == "/usr/lib/libSystem.B.dylib")     sawLibSystem = true;
        }
        off += cmdsize;
    }
    EXPECT_TRUE(sawLibSystem)  << "the schema's libSystem stays declared.";
    EXPECT_TRUE(sawUndeclared) << "the referenced non-schema library is "
                                  "auto-emitted as its own LC_LOAD_DYLIB.";
}

// ── D-CSUBSET-THREAD-LOCAL (TLS C4): Mach-O TLV structural pins ──────────
//
// THE HOST CANNOT EXECUTE MACH-O, so these byte-level structural pins are the
// SOLE automated guard on the TLV writer (the runtime witness is the user's
// Apple-Silicon Mac + the macos-latest CI leg). They decode a produced
// arm64-macho-exec image and assert the descriptor ABI (word0 bind / word1 key
// / word2 block offset), the 3 __thread_* sections + flags, the
// __tlv_bootstrap bind-per-descriptor, and the CRIT-1/M-3 fail-loud gates —
// each RED-ON-DISABLE (routing thread-locals through ordinary data, or dropping
// the bind, makes a pin fail while a single-thread runtime would still pass).
namespace {

struct MachoSecInfo {
    std::string name;
    std::uint64_t addr = 0, size = 0;
    std::uint32_t off = 0, flags = 0, segidx = 0;
};
struct MachoBind { std::string sym; std::uint32_t seg = 0; std::uint64_t off = 0; };
struct MachoDecoded {
    std::vector<std::uint64_t> segVmaddr;
    std::vector<MachoSecInfo>  sections;
    std::vector<MachoBind>     binds;
    [[nodiscard]] MachoSecInfo const* sec(std::string_view n) const {
        for (auto const& s : sections) if (s.name == n) return &s;
        return nullptr;
    }
};

[[nodiscard]] std::uint64_t readUleb(std::vector<std::uint8_t> const& b,
                                     std::size_t& i) {
    std::uint64_t r = 0; int s = 0;
    while (i < b.size()) {
        std::uint8_t c = b[i++]; r |= std::uint64_t(c & 0x7f) << s;
        if (!(c & 0x80)) break; s += 7;
    }
    return r;
}

[[nodiscard]] MachoDecoded decodeMacho(std::vector<std::uint8_t> const& d) {
    MachoDecoded out;
    std::uint32_t const ncmds = readU32LE(d, 16);
    std::size_t off = 32;
    for (std::uint32_t c = 0; c < ncmds; ++c) {
        std::uint32_t const cmd = readU32LE(d, off);
        std::uint32_t const cmdsize = readU32LE(d, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            std::uint64_t const vmaddr = readU64LE(d, off + 24);
            std::uint32_t const nsects = readU32LE(d, off + 64);
            std::uint32_t const segidx =
                static_cast<std::uint32_t>(out.segVmaddr.size());
            out.segVmaddr.push_back(vmaddr);
            std::size_t so = off + 72;
            for (std::uint32_t s = 0; s < nsects; ++s) {
                MachoSecInfo si;
                char nm[17] = {0};
                std::memcpy(nm, d.data() + so, 16);
                si.name = nm;
                si.addr = readU64LE(d, so + 32);
                si.size = readU64LE(d, so + 40);
                si.off = readU32LE(d, so + 48);
                si.flags = readU32LE(d, so + 64);
                si.segidx = segidx;
                out.sections.push_back(std::move(si));
                so += 80;
            }
        } else if (cmd == 0x80000022u) {  // LC_DYLD_INFO_ONLY
            // dyld_info_command: cmd/cmdsize/rebase(8,12)/bind_off(16)/bind_size(20).
            std::uint32_t const bindOff = readU32LE(d, off + 16);
            std::uint32_t const bindSize = readU32LE(d, off + 20);
            std::size_t i = bindOff;
            std::string sym; std::uint32_t seg = 0; std::uint64_t o = 0;
            while (i < std::size_t(bindOff) + bindSize) {
                std::uint8_t const op = d[i++]; std::uint8_t const hi = op & 0xF0u;
                if (hi == 0x10u) { /* ORD_IMM */ }
                else if (hi == 0x20u) { (void)readUleb(d, i); }
                else if (hi == 0x40u) {
                    sym.clear();
                    while (i < d.size() && d[i] != 0) sym.push_back(char(d[i++]));
                    ++i;  // NUL
                } else if (hi == 0x50u) { /* SET_TYPE */ }
                else if (hi == 0x70u) { seg = op & 0x0Fu; o = readUleb(d, i); }
                else if (hi == 0x90u) { out.binds.push_back({sym, seg, o}); }
                else if (op == 0x00u) { /* DONE / pad */ }
            }
        }
        off += cmdsize;
    }
    return out;
}

// Extern (_exit) forces encodeExecDynamic (the only __DATA/TLV path). Optional
// tbss int + a configurable tdata alignment for the M-3 over-align pin.
[[nodiscard]] AssembledModule buildMachoTlvModule(bool withTbss,
                                                  std::uint64_t tdataAlign = 4) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};  // BL _exit ; RET
    fn.relocations.push_back(Relocation{0u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_exit", "/usr/lib/libSystem.B.dylib"});
    AssembledData g;  // thread_local int g = 7;  → tdata, word2 = 0
    g.symbol = SymbolId{50};
    g.section = DataSectionKind::Tdata;
    g.bytes = {7, 0, 0, 0};
    g.alignment = Alignment::ofRuntimePow2(tdataAlign);
    mod.dataItems.push_back(std::move(g));
    if (withTbss) {
        AssembledData c;  // static thread_local int c; → tbss, word2 = tbssBlockBase
        c.symbol = SymbolId{51};
        c.section = DataSectionKind::Tbss;
        c.reservedSize = 4;
        c.alignment = Alignment::ofRuntimePow2(4);
        mod.dataItems.push_back(std::move(c));
    }
    return mod;
}

struct MachoTlvLoaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};
[[nodiscard]] MachoTlvLoaded loadArm64MachoExec() {
    MachoTlvLoaded out;
    auto t = TargetSchema::loadShipped("arm64");
    if (t.has_value()) out.target = std::move(t).value(); else ADD_FAILURE();
    auto f = ObjectFormatSchema::loadShipped("macho64-arm64-darwin-exec");
    if (f.has_value()) out.format = std::move(f).value(); else ADD_FAILURE();
    return out;
}

[[nodiscard]] bool sawDiag(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) if (d.code == code) return true;
    return false;
}

}  // namespace

TEST(MachOTlvWriter, ThreeThreadSectionsCarryStdThreadLocalFlags) {
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/true);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *L.target, *L.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    auto dec = decodeMacho(bytes);
    auto const* tv = dec.sec("__thread_vars");
    auto const* td = dec.sec("__thread_data");
    auto const* tb = dec.sec("__thread_bss");
    ASSERT_NE(tv, nullptr); ASSERT_NE(td, nullptr); ASSERT_NE(tb, nullptr);
    // dyld keys the TLV template on these EXACT section types.
    EXPECT_EQ(tv->flags & 0xffu, 0x13u);  // S_THREAD_LOCAL_VARIABLES
    EXPECT_EQ(td->flags & 0xffu, 0x11u);  // S_THREAD_LOCAL_REGULAR
    EXPECT_EQ(tb->flags & 0xffu, 0x12u);  // S_THREAD_LOCAL_ZEROFILL
    EXPECT_EQ(tb->off, 0u) << "__thread_bss is zero-fill: no file bytes";
    // __thread_data and __thread_bss must be CONTIGUOUS (one dyld TLV region).
    EXPECT_LE(td->addr + td->size, tb->addr);
    EXPECT_EQ(tb->addr, td->addr + 4u)
        << "tbss must sit at tdataVA + tbssBlockBase (CRIT-2 contiguity)";
}

TEST(MachOTlvWriter, DescriptorWord2IsZeroBasedBlockOffset) {
    // ★ CRIT-2 byte pin: a 2-var block (tdata int @0 + tbss int @4). word0/word1
    // are 0 (dyld binds/fills them); word2 is the 0-BASED block offset — NOT the
    // arm64-ELF Variant-I 16+off (which would put every word2 at +16 → garbage).
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/true);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *L.target, *L.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto dec = decodeMacho(bytes);
    auto const* tv = dec.sec("__thread_vars");
    ASSERT_NE(tv, nullptr);
    ASSERT_EQ(tv->size, 2u * 24u) << "2 descriptors × 24 bytes";
    // desc[0] = tdata g → word2 0; desc[1] = tbss c → word2 4 (tbssBlockBase).
    for (int i = 0; i < 2; ++i) {
        std::uint64_t const w0 = readU64LE(bytes, tv->off + std::size_t(i) * 24 + 0);
        std::uint64_t const w1 = readU64LE(bytes, tv->off + std::size_t(i) * 24 + 8);
        EXPECT_EQ(w0, 0u) << "desc[" << i << "].word0 must be 0 (dyld binds thunk)";
        EXPECT_EQ(w1, 0u) << "desc[" << i << "].word1 must be 0 (key)";
    }
    std::uint64_t const w2a = readU64LE(bytes, tv->off + 16);
    std::uint64_t const w2b = readU64LE(bytes, tv->off + 24 + 16);
    EXPECT_EQ(w2a, 0u) << "tdata var's block offset is 0";
    EXPECT_EQ(w2b, 4u) << "tbss var's block offset is tbssBlockBase=4, not 16+off";
}

TEST(MachOTlvWriter, BootstrapBindPerDescriptorTargetsWord0) {
    // ★ CRIT-3: exactly one bind of `__tlv_bootstrap` (TWO underscores — the
    // on-disk libSystem symbol) per descriptor, at that descriptor's word0
    // (offset i*24 in the __thread_vars-bearing __DATA segment). A missing bind
    // = a `blr` through garbage on the Mac. The `bind.seg == __thread_vars
    // segment index` check below is the TEST-side guard on the segment index;
    // the writer ALSO fails loud (audit FOLD-1: it cross-checks the ACTUAL
    // emitted __DATA index against the value the binds use) so a segment-layout
    // change can never SILENTLY bind to the wrong segment on this
    // can't-run-macho host.
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/true);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *L.target, *L.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto dec = decodeMacho(bytes);
    auto const* tv = dec.sec("__thread_vars");
    ASSERT_NE(tv, nullptr);
    std::vector<MachoBind> tlv;
    for (auto const& b : dec.binds)
        if (b.sym == "__tlv_bootstrap") tlv.push_back(b);
    ASSERT_EQ(tlv.size(), 2u) << "one __tlv_bootstrap bind per descriptor";
    std::uint64_t const segVa = dec.segVmaddr[tv->segidx];
    std::uint64_t const tvOffInSeg = tv->addr - segVa;
    for (std::size_t i = 0; i < tlv.size(); ++i) {
        EXPECT_EQ(tlv[i].seg, tv->segidx)
            << "bind targets the __thread_vars-bearing segment";
        EXPECT_EQ(tlv[i].off, tvOffInSeg + i * 24u)
            << "bind offset is descriptor i's word0 (i*24)";
    }
}

TEST(MachOTlvWriter, TlsImageHeaderAdvertisesTlvDescriptorsFlagRuntimeClosure) {
    // ★ TLS C4 runtime-closure red-on-disable (the host-independent guard for the
    // gap the arm64 witness caught): a TLS-bearing image's mach_header_64.flags
    // MUST carry MH_HAS_TLV_DESCRIPTORS (0x00800000). The correct S_THREAD_LOCAL_*
    // section types + per-descriptor __tlv_bootstrap binds (pinned above) are
    // NECESSARY BUT NOT SUFFICIENT — dyld runs the TLV setup that rewrites each
    // descriptor's word0 thunk to the real tlv_get_addr ONLY when the HEADER
    // advertises this bit. Without it, dyld leaves word0 = __tlv_bootstrap, whose
    // thunk aborts the instant a thread_local is read → runtime SIGABRT
    // (`_tlv_bootstrap_error`). That is invisible to a byte-pin over sections/binds
    // and to any non-Mac host; this pin fails on EVERY leg if the writer drops the
    // flag, so the regression can never re-hide behind green byte-pins again.
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/true);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *L.target, *L.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 28u);
    // mach_header_64.flags @ +24 (magic/cputype/cpusubtype/filetype/ncmds/sizeofcmds).
    std::uint32_t const flags = readU32LE(bytes, 24);
    EXPECT_NE(flags & 0x00800000u, 0u)
        << "TLS image must set MH_HAS_TLV_DESCRIPTORS or dyld skips TLV setup "
           "→ runtime SIGABRT via _tlv_bootstrap_error on native execution";
    // The base format flags (NOUNDEFS|DYLDLINK|TWOLEVEL|PIE = 0x200085) survive —
    // the TLV bit is ADDED to them, never replaces them.
    EXPECT_EQ(flags & 0x00200085u, 0x00200085u)
        << "base MH flags preserved alongside the TLV bit";
}

TEST(MachOTlvWriter, AddressOfThreadLocalInDataItemFailsLoudCrit1) {
    // ★ CRIT-1 arm-(a) red-on-disable: a DATA-item reloc targeting a thread-local
    // symbol embeds the descriptor VA as a process-shared pointer. The macho
    // walker backstop rejects it (the semantic tier's 0xE048 is the front line;
    // this is the writer belt). Disabling the tlsDataSymbols backstop makes this
    // link clean with a garbage pointer.
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/false);
    RelocationKind abs64{0}; bool found = false;
    for (auto const& r : L.target->relocations())
        if (r.widthBytes == 8 && !r.pcRelative) { abs64 = r.kind; found = true; break; }
    ASSERT_TRUE(found);
    // `int *p = &g;` — a __data pointer with an abs64 reloc → the tls sym 50.
    AssembledData p;
    p.symbol = SymbolId{60};
    p.section = DataSectionKind::Data;
    p.bytes.assign(8, 0);
    p.alignment = Alignment::ofRuntimePow2(8);
    p.relocations.push_back(Relocation{0u, SymbolId{50}, abs64, 0});
    mod.dataItems.push_back(std::move(p));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *L.target, *L.format, rep);
    EXPECT_TRUE(bytes.empty());
    EXPECT_TRUE(sawDiag(rep, DiagnosticCode::K_RelocationKindMismatch))
        << "a data-item reloc targeting a thread-local must fail loud (CRIT-1)";
}

TEST(MachOTlvWriter, PlainThreadLocalLinksCleanCrit1Positive) {
    // ★ CRIT-1 positive: the ordinary access reloc (adrp/add vs a TLS sym, kinds
    // 2/3) must NOT trip the backstop — a plain thread_local links clean. macho
    // does NOT register tls syms in a tlsSymbols set / run the reloc-XOR arm (b);
    // if it did, this legitimate access reloc would be false-rejected.
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/true);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *L.target, *L.format, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(bytes.empty());
}

TEST(MachOTlvWriter, OverAlignedThreadLocalFailsLoudM3) {
    // ★ M-3 red-on-disable: a thread-local needing > 16-byte alignment cannot be
    // guaranteed by dyld's malloc'd TLV block base; fail loud
    // (K_ThreadLocalOveralignedForFormat) rather than silently under-align.
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/false, /*tdataAlign=*/32);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *L.target, *L.format, rep);
    EXPECT_TRUE(bytes.empty());
    EXPECT_TRUE(sawDiag(rep, DiagnosticCode::K_ThreadLocalOveralignedForFormat));
}

TEST(MachOTlvWriter, SixteenByteAlignedThreadLocalLinksCleanM3) {
    // _Alignas(16) (== the block-base guarantee) must PASS — the gate bites only
    // strictly-over-16 alignment.
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/false, /*tdataAlign=*/16);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *L.target, *L.format, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(bytes.empty());
}

TEST(MachOTlvWriter, NoThreadLocalEmitsNoThreadSectionsSqliteDormant) {
    // The disable-and-red control: a module with NO thread-local items emits
    // ZERO __thread_* sections + ZERO __tlv_bootstrap binds — the pre-TLS image
    // is byte-untouched (sqlite / ordinary output unaffected).
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};
    fn.relocations.push_back(Relocation{0u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_exit", "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *L.target, *L.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto dec = decodeMacho(bytes);
    EXPECT_EQ(dec.sec("__thread_vars"), nullptr);
    EXPECT_EQ(dec.sec("__thread_data"), nullptr);
    EXPECT_EQ(dec.sec("__thread_bss"), nullptr);
    for (auto const& b : dec.binds)
        EXPECT_NE(b.sym, "__tlv_bootstrap");
    // MH_HAS_TLV_DESCRIPTORS is content-conditional (set iff the module carries
    // thread-locals — clang's rule), NOT a blanket format-JSON flag: a no-TLS
    // image must NOT advertise it. This is the negative twin of the runtime-
    // closure pin — it goes red if the TLV bit is ever made unconditional.
    EXPECT_EQ(readU32LE(bytes, 24) & 0x00800000u, 0u)
        << "no-TLS image must NOT set MH_HAS_TLV_DESCRIPTORS";
}
