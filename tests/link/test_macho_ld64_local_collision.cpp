// D-LK-MACHO-STB-LOCAL-LD64-RUNTIME-WITNESS — the ld64 half of
// D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION (TF-C54).
//
// WHAT THE EXISTING PIN CANNOT PROVE. `test_macho_writer.cpp`'s
// `ObjectNlistCouplesNameAndBindingStaticLocalDropsNExt` pins the emitted BYTE:
// an internal-linkage defined function's nlist_64 carries bare `N_SECT` (0x0E)
// instead of `N_SECT|N_EXT` (0x0F). That is a statement about DSS's own writer
// agreeing with DSS's own reading of the Mach-O ABI. It says nothing about the
// only consumer that matters — Apple's linker. The claim TF-C54 actually makes
// is "two separately-compiled DSS translation units, each restarting the
// SymbolId counter and therefore each defining `_sym_7`, LINK TOGETHER", and
// the only instrument that can answer that is ld64 itself: with the fix
// reverted both definitions carry N_EXT, both enter the global namespace under
// the same name, and the link dies with `duplicate symbol '_sym_7'`.
//
// WHY IT TOOK UNTIL NOW. The row was named 2026-07-23 and carried ONE blocker
// for the whole time it stood open — "no macOS linker on this Windows/WSL
// host". The ELF sibling has qemu and the PE sibling has link.exe; a Mach-O has
// no off-Mac consumer at all, so this witness could not be written, only
// deferred. It is written here because the operator's Apple Silicon machine
// became scriptable (scripts/ssh-macos/ssh-macos.ps1, SKILL.md §3.1), which is the
// prerequisite the row named and nothing else about the fix has changed.
//
// TWO TIERS, AND NEITHER REPLACES THE OTHER.
//
//   * TIER 1 (`MachOLd64LocalCollision`) — HOST-INDEPENDENT. Builds the two
//     objects and asserts, against the emitted bytes, the full nlist sequence
//     and the full __text relocation table of BOTH. It runs on Windows, on
//     Linux and on the Mac, so a regression in the binding decision reddens on
//     every leg of the matrix rather than only when a Mac is reachable. This is
//     the always-on guard.
//   * TIER 2 (`MachOLd64LocalCollisionNative`) — the RUNTIME witness. Hands the
//     SAME two byte blobs to ld64, links, spawns, and asserts exit 42. This is
//     the end-to-end proof, and it is the only tier that can observe ld64's
//     duplicate-symbol verdict. It also asserts what ld64 does NOT say: the
//     link log must be free of `no platform load command found`, which is the
//     only place in the tree where Apple's linker — rather than DSS's own
//     reading of the ABI — judges the LC_BUILD_VERSION the MH_OBJECT walker
//     emits (D-LK10-ENTRY-MACHO-EXIT, MH_OBJECT arm).
//
// The two tiers share ONE object builder (`buildTwoTus`) on purpose: a
// structural pin that guards different bytes than the ones the linker consumes
// guards nothing.
//
// ★ THE 42 IS A SUM, NOT A CONSTANT, and that is the whole design. `_main`
// (TU A) calls its own `_sym_7` → 20 and then `_second` (TU B), whose `_sym_7`
// → 22. If ld64 ever folded the two colliding definitions into one — the
// failure this witness exists to detect, in the form where it does NOT error —
// the program would return 40 or 44, not 42. So the exit code discriminates
// "kept module-private" from "silently merged", which a single-call witness
// returning a literal 42 could not.
//
// ★ NO PRODUCT CODE SHELLS OUT TO A LINKER. The hermetic-compiler thesis
// forbids `ld`/`clang` invocations in `src/`; this is a TEST deliberately
// invoking the foreign linker it is written to be judged by, and the invocation
// lives entirely in this file.
//
// ✔FIRST MEASURED 2026-08-13 on the operator's Apple Silicon machine —
// macOS 26.5.2 (Darwin 25.5.0, T8132), Apple `ld` PROJECT:ld-1267 — where the
// link succeeded and the binary exited 42, and where reverting the binding
// decision produced `duplicate symbol '_sym_7'`. Both halves are recorded in
// the per-tier comments below.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/macho.hpp"
#include "link/object_format_schema.hpp"
#include "macho_test_support.hpp"

// The `ArmVerdict` vocabulary + its strict-mode reader. A witness that cannot
// run must land in a NAMED class, never in an unrecorded green — see the
// verdict block above the native tier.
#include "arm_verdict_ledger.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

// `captureCmd` / `tailOf` — the SHARED shell-capture spellings. Included by
// relative path for the reason `test_coff_object_reader.cpp` includes the same
// header that way: `tests/core` is not on this target's include path, and a
// quoted include resolves against the includer's own directory first. Reused
// rather than re-spelled because the cmd.exe-vs-POSIX redirection difference in
// there is a MEASURED defect that cost two cycles of silently-inert oracles
// (D-TEST-NATIVE-ORACLE-INERT-ON-POSIX); a second copy would be free to make it
// again.
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>   // strnlen — the fixed-width Mach-O name fields are not NUL-terminated when full
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

using dss::macho::test::findLoadCommand;
using dss::macho::test::readU32LE;

constexpr std::uint32_t kLcSegment64 = 0x19u;
constexpr std::uint32_t kLcBuildVersion = 0x32u;
constexpr std::size_t   kSection64Size = 80;

// Locate a `section_64` record by the (segment, section) pair written INTO THE
// SECTION ITSELF.
//
// ★ NOT `macho_test_support.hpp`'s `findSection`, and the difference is the
// whole reason this exists. That one resolves the segment by the NAME ON THE
// LC_SEGMENT_64 COMMAND, which is right for an image (`__TEXT`, `__DATA_CONST`,
// …) and cannot work for a relocatable object: an MH_OBJECT carries ONE
// anonymous catch-all segment whose segname is the empty string
// (`macho.cpp` — `appendName16(bytes, "")  // segname empty for MH_OBJECT`),
// with the real two-level naming living in each section_64's own `segname`
// field. Reaching for the shared helper here returns "no such section" for an
// object that has it, which is exactly what it did on first run.
//
// section_64: sectname[16] segname[16] addr(8) size(8) offset(4) align(4)
//             reloff(4) nreloc(4) flags(4) reserved1(4) reserved2(4) reserved3(4)
[[nodiscard]] std::optional<std::size_t>
findObjectSection(std::span<std::uint8_t const> obj, std::string_view segment,
                  std::string_view section) {
    auto readName16 = [&](std::size_t at) {
        auto const* chars = reinterpret_cast<char const*>(&obj[at]);
        return std::string{chars, ::strnlen(chars, 16)};
    };
    if (obj.size() < 32) return std::nullopt;
    std::uint32_t const ncmds = readU32LE(obj, 16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (off + 8 > obj.size()) return std::nullopt;
        std::uint32_t const cmd     = readU32LE(obj, off);
        std::uint32_t const cmdsize = readU32LE(obj, off + 4);
        if (cmdsize == 0) return std::nullopt;
        if (cmd == kLcSegment64 && off + 72 <= obj.size()) {
            std::uint32_t const nsects = readU32LE(obj, off + 64);
            for (std::uint32_t s = 0; s < nsects; ++s) {
                std::size_t const secOff = off + 72 + s * kSection64Size;
                if (secOff + kSection64Size > obj.size()) return std::nullopt;
                if (readName16(secOff) == section
                    && readName16(secOff + 16) == segment) {
                    return secOff;
                }
            }
        }
        off += cmdsize;
    }
    return std::nullopt;
}

// ── The two translation units ──────────────────────────────────────────────
//
// AArch64, because the reachable Mac is Apple Silicon and Rosetta is not a
// dependency this witness may take (a machine without it would turn the run
// leg into an unexplained crash rather than a skip).
//
// HAND-ENCODED, not produced by DSS's assembler, and deliberately so: this test
// must control the exact bytes ld64 is handed, and routing them through the
// component under test would make a codegen change able to silently alter what
// the linker is being asked about. Nothing here is taken on trust either — the
// linked image RUNS and returns 20+22, which exercises every one of these words
// (a wrong `str`/`ldr` offset or a wrong `add` operand yields some other number
// or a crash, not a pass).
constexpr std::uint32_t kStpFpLrPre32  = 0xA9BE7BFDu;  // stp x29,x30,[sp,#-32]!
constexpr std::uint32_t kStpFpLrPre16  = 0xA9BF7BFDu;  // stp x29,x30,[sp,#-16]!
constexpr std::uint32_t kLdpFpLrPost32 = 0xA8C27BFDu;  // ldp x29,x30,[sp],#32
constexpr std::uint32_t kLdpFpLrPost16 = 0xA8C17BFDu;  // ldp x29,x30,[sp],#16
constexpr std::uint32_t kStrW0Sp16     = 0xB90013E0u;  // str w0,[sp,#16]
constexpr std::uint32_t kLdrW1Sp16     = 0xB94013E1u;  // ldr w1,[sp,#16]
constexpr std::uint32_t kAddW0W0W1     = 0x0B010000u;  // add w0,w0,w1
constexpr std::uint32_t kRet           = 0xD65F03C0u;  // ret
constexpr std::uint32_t kBlPlaceholder = 0x94000000u;  // bl #0 — imm26 patched
                                                       // by the BRANCH26 reloc

// `movz w0, #imm16` — the whole reason the two helpers are distinguishable at
// runtime. Derived, not tabulated: a second hand-written constant would be one
// more place for 20 and 22 to drift out of agreement with the expected 42.
[[nodiscard]] constexpr std::uint32_t movzW0(std::uint32_t imm16) {
    return 0x52800000u | (imm16 << 5);
}

void appendWord(std::vector<std::uint8_t>& out, std::uint32_t word) {
    out.push_back(static_cast<std::uint8_t>(word & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((word >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((word >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((word >> 24) & 0xFFu));
}

[[nodiscard]] std::vector<std::uint8_t> words(std::initializer_list<std::uint32_t> ws) {
    std::vector<std::uint8_t> out;
    out.reserve(ws.size() * 4u);
    for (std::uint32_t w : ws) appendWord(out, w);
    return out;
}

// The COLLIDING SymbolId. Both TUs mint it because a per-CU SymbolId counter
// restarts at every translation unit — that restart IS the defect TF-C54 fixed,
// so the witness reproduces it rather than working around it.
constexpr std::uint32_t kCollidingSymId = 7u;
constexpr std::uint32_t kHelperAResult  = 20u;
constexpr std::uint32_t kHelperBResult  = 22u;
constexpr std::uint32_t kExpectedExit   = kHelperAResult + kHelperBResult;  // 42

// `kind` 1 is ARM64_RELOC_BRANCH26 in macho64-arm64-darwin.format.json, which
// is the SAME numbering arm64.target.json gives `call26`. Read as a named
// constant so a schema renumbering shows up here as a mismatch rather than as
// an unexplained relocation type in the emitted table.
constexpr RelocationKind kBranch26{1};

// TU A: `_main` (Global) + a nameless-in-`symbols` helper that therefore emits
// as internal `_sym_7` with LOCAL binding, plus an undefined reference to
// TU B's `_second`.
//
//   _main:  stp / bl _sym_7 / str w0 / bl _second / ldr w1 / add / ldp / ret
//   _sym_7: movz w0,#20 / ret
//
// The helper deliberately carries NO `ModuleSymbol` row. That is exactly how
// the compile pipeline presents a `static` function and a synthesized helper
// (`definedName` falls through to `_sym_<id>`, `definedBinding` to Local), and
// it is the shape the hermetic ELF sibling `TfC54TwoTusCollidingStaticsRead
// BackLocalMergeClean` uses for the same reason.
[[nodiscard]] AssembledModule buildTuA() {
    AssembledModule mod;
    mod.cuId = CompilationUnitId{1};
    mod.expectedFuncCount = 2;

    AssembledFunction entry;
    entry.symbol = SymbolId{1};
    entry.bytes  = words({kStpFpLrPre32, kBlPlaceholder, kStrW0Sp16, kBlPlaceholder,
                          kLdrW1Sp16, kAddW0W0W1, kLdpFpLrPost32, kRet});
    entry.relocations.push_back(Relocation{/*offset=*/4,
                                           /*target=*/SymbolId{kCollidingSymId},
                                           /*kind=*/kBranch26, /*addend=*/0});
    entry.relocations.push_back(Relocation{/*offset=*/12, /*target=*/SymbolId{99},
                                           /*kind=*/kBranch26, /*addend=*/0});
    mod.functions.push_back(std::move(entry));

    AssembledFunction helper;
    helper.symbol = SymbolId{kCollidingSymId};
    helper.bytes  = words({movzW0(kHelperAResult), kRet});
    mod.functions.push_back(std::move(helper));

    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "_main", SymbolBinding::Global,
                                       SymbolVisibility::Default});
    ExternImport second;
    second.symbol      = SymbolId{99};
    second.mangledName = "_second";
    mod.externImports.push_back(std::move(second));
    return mod;
}

// TU B: `_second` (Global) + ITS OWN `_sym_7`, minted by a counter that
// restarted. Same id, different body, no coordination with TU A.
//
//   _second: stp / bl _sym_7 / ldp / ret
//   _sym_7:  movz w0,#22 / ret
[[nodiscard]] AssembledModule buildTuB() {
    AssembledModule mod;
    mod.cuId = CompilationUnitId{2};
    mod.expectedFuncCount = 2;

    AssembledFunction second;
    second.symbol = SymbolId{1};
    second.bytes  = words({kStpFpLrPre16, kBlPlaceholder, kLdpFpLrPost16, kRet});
    second.relocations.push_back(Relocation{/*offset=*/4,
                                            /*target=*/SymbolId{kCollidingSymId},
                                            /*kind=*/kBranch26, /*addend=*/0});
    mod.functions.push_back(std::move(second));

    AssembledFunction helper;
    helper.symbol = SymbolId{kCollidingSymId};
    helper.bytes  = words({movzW0(kHelperBResult), kRet});
    mod.functions.push_back(std::move(helper));

    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "_second", SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

struct TwoObjects {
    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
};

// Encode both TUs through the production `macho::encode` MH_OBJECT path. ONE
// producer for both tiers — see the file header.
[[nodiscard]] TwoObjects buildTwoTus() {
    TwoObjects out;
    auto target = TargetSchema::loadShipped("arm64");
    if (!target.has_value()) {
        ADD_FAILURE() << "loadShipped(arm64) failed";
        for (auto const& d : target.error()) ADD_FAILURE() << "  " << d.message;
        return out;
    }
    auto format = ObjectFormatSchema::loadShipped("macho64-arm64-darwin");
    if (!format.has_value()) {
        ADD_FAILURE() << "loadShipped(macho64-arm64-darwin) failed";
        for (auto const& d : format.error()) ADD_FAILURE() << "  " << d.message;
        return out;
    }

    auto encodeOne = [&](AssembledModule const& mod) {
        DiagnosticReporter rep;
        auto bytes = macho::encode(mod, **target, **format, rep);
        if (rep.errorCount() != 0u) {
            ADD_FAILURE() << "the MH_OBJECT writer must accept a two-function TU";
            for (auto const& d : rep.all()) ADD_FAILURE() << "  " << d.actual;
        }
        return bytes;
    };
    out.a = encodeOne(buildTuA());
    out.b = encodeOne(buildTuB());
    return out;
}

// ── nlist_64 / relocation_info readback ────────────────────────────────────

// One symbol table row, in the three fields this witness makes claims about.
struct NlistRow {
    std::string   name;
    std::uint8_t  type  = 0;   // n_type: 0x0E N_SECT, 0x0F N_SECT|N_EXT, 0x01 N_UNDF|N_EXT
    std::uint8_t  sect  = 0;   // n_sect: 1-based section ordinal, 0 for undefined
    friend bool operator==(NlistRow const&, NlistRow const&) = default;
};

// Printed on a full-sequence mismatch. gtest cannot render a vector of structs
// usefully without this, and a diff that says only "sequences differ" would
// leave the reader to hexdump the object by hand.
std::ostream& operator<<(std::ostream& os, NlistRow const& r) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%02X", static_cast<unsigned>(r.type));
    return os << "{\"" << r.name << "\", n_type=" << buf
              << ", n_sect=" << static_cast<unsigned>(r.sect) << '}';
}

constexpr std::uint32_t kLcSymtab = 0x02u;

[[nodiscard]] std::vector<NlistRow> readSymbolTable(std::span<std::uint8_t const> obj) {
    std::vector<NlistRow> rows;
    auto const lc = findLoadCommand(obj, kLcSymtab);
    if (!lc) {
        ADD_FAILURE() << "the emitted object carries no LC_SYMTAB";
        return rows;
    }
    std::uint32_t const symoff = readU32LE(obj, *lc + 8);
    std::uint32_t const nsyms  = readU32LE(obj, *lc + 12);
    std::uint32_t const stroff = readU32LE(obj, *lc + 16);
    for (std::uint32_t i = 0; i < nsyms; ++i) {
        std::size_t const at = symoff + static_cast<std::size_t>(i) * 16u;
        if (at + 16u > obj.size()) {
            ADD_FAILURE() << "nlist_64 #" << i << " runs past the end of the object";
            return rows;
        }
        NlistRow row;
        std::uint32_t const strx = readU32LE(obj, at);
        for (std::size_t p = stroff + strx; p < obj.size() && obj[p] != 0; ++p) {
            row.name.push_back(static_cast<char>(obj[p]));
        }
        row.type = obj[at + 4];
        row.sect = obj[at + 5];
        rows.push_back(std::move(row));
    }
    return rows;
}

// One __text relocation_info row, unpacked. `symbolNum` is an INDEX into the
// nlist sequence above, which is what makes "this TU's call binds to this TU's
// own helper" a checkable structural claim rather than a hope.
struct TextReloc {
    std::uint32_t address   = 0;
    std::uint32_t symbolNum = 0;
    std::uint32_t type      = 0;   // r_type, bits 28..31
    bool          isExtern  = false;
    bool          pcRel     = false;
    std::uint32_t length    = 0;
    friend bool operator==(TextReloc const&, TextReloc const&) = default;
};

std::ostream& operator<<(std::ostream& os, TextReloc const& r) {
    return os << "{r_address=" << r.address << ", r_symbolnum=" << r.symbolNum
              << ", r_type=" << r.type << ", r_extern=" << r.isExtern
              << ", r_pcrel=" << r.pcRel << ", r_length=" << r.length << '}';
}

[[nodiscard]] std::vector<TextReloc> readTextRelocs(std::span<std::uint8_t const> obj) {
    std::vector<TextReloc> rows;
    auto const sec = findObjectSection(obj, "__TEXT", "__text");
    if (!sec) {
        ADD_FAILURE() << "the emitted object carries no (__TEXT,__text) section";
        return rows;
    }
    std::uint32_t const reloff = readU32LE(obj, *sec + 56);
    std::uint32_t const nreloc = readU32LE(obj, *sec + 60);
    for (std::uint32_t i = 0; i < nreloc; ++i) {
        std::size_t const at = reloff + static_cast<std::size_t>(i) * 8u;
        if (at + 8u > obj.size()) {
            ADD_FAILURE() << "relocation_info #" << i << " runs past the end";
            return rows;
        }
        std::uint32_t const info = readU32LE(obj, at + 4);
        rows.push_back(TextReloc{readU32LE(obj, at),
                                 info & 0x00FFFFFFu,
                                 (info >> 28) & 0xFu,
                                 ((info >> 27) & 1u) != 0u,
                                 ((info >> 24) & 1u) != 0u,
                                 (info >> 25) & 0x3u});
    }
    return rows;
}

// One load command, in the two fields that decide whether ld64 can walk the
// list at all.
struct LoadCommand {
    std::uint32_t cmd     = 0;
    std::uint32_t cmdsize = 0;
    friend bool operator==(LoadCommand const&, LoadCommand const&) = default;
};

std::ostream& operator<<(std::ostream& os, LoadCommand const& c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%02X", static_cast<unsigned>(c.cmd));
    return os << "{cmd=" << buf << ", cmdsize=" << c.cmdsize << '}';
}

// Walk `ncmds` commands and return the sequence, having first checked that
// the walk lands EXACTLY on `sizeofcmds` — the coherence the mach_header_64
// asserts and that nothing else in this tree verifies on every leg. A command
// the header does not count, or a sizeofcmds that does not match the commands
// emitted, produces a malformed object: ld64 finds section data by adding
// sizeofcmds to the header, so a miscount does not crash the writer, it ships
// a `.o` whose section_64.offset lands inside a load command.
[[nodiscard]] std::vector<LoadCommand>
readLoadCommands(std::span<std::uint8_t const> obj) {
    std::vector<LoadCommand> out;
    if (obj.size() < 32) {
        ADD_FAILURE() << "object is too small to carry a mach_header_64";
        return out;
    }
    std::uint32_t const ncmds      = readU32LE(obj, 16);
    std::uint32_t const sizeofcmds = readU32LE(obj, 20);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (off + 8 > obj.size()) {
            ADD_FAILURE() << "load command #" << i << " runs past the end";
            return out;
        }
        out.push_back(LoadCommand{readU32LE(obj, off), readU32LE(obj, off + 4)});
        if (out.back().cmdsize == 0) {
            ADD_FAILURE() << "load command #" << i << " has cmdsize 0";
            return out;
        }
        off += out.back().cmdsize;
    }
    EXPECT_EQ(off, 32u + sizeofcmds)
        << "mach_header_64.sizeofcmds (" << sizeofcmds << ") must equal the "
           "bytes the ncmds (" << ncmds << ") commands actually occupy; ld64 "
           "locates the section data by adding it to the header";
    return out;
}

}  // namespace

// ── TIER 1: the host-independent structural guard ──────────────────────────
//
// RED-ON-DISABLE, and the recipe is exact: make `ObjectSymbolNames::defined
// Binding` return `SymbolBinding::Global` for the not-externally-visible case
// (or make macho.cpp's two defined-symbol loops OR in N_EXT unconditionally),
// and the `_sym_7` rows below come back 0x0F. It fires on EVERY leg, which is
// the property the ld64 tier below cannot have.
TEST(MachOLd64LocalCollision, BothTusEmitCollidingHelperAsLocalWithoutNExt) {
    TwoObjects const objs = buildTwoTus();
    ASSERT_FALSE(objs.a.empty());
    ASSERT_FALSE(objs.b.empty());

    // FULL SEQUENCE, not a lookup. The emission order (defined functions, then
    // undefined externs) is what the relocation r_symbolnum indices below are
    // resolved against, so a reordering is a real defect and must not be
    // absorbed by a find-by-name.
    std::vector<NlistRow> const expectedA{
        {"_main",   0x0Fu, 1u},   // externally visible: N_SECT|N_EXT, __text
        {"_sym_7",  0x0Eu, 1u},   // internal linkage: N_SECT, NO N_EXT
        {"_second", 0x01u, 0u},   // N_UNDF|N_EXT — resolved by ld64 from TU B
    };
    std::vector<NlistRow> const expectedB{
        {"_second", 0x0Fu, 1u},
        {"_sym_7",  0x0Eu, 1u},
    };
    EXPECT_EQ(readSymbolTable(objs.a), expectedA)
        << "TU A's colliding helper must be internal (`_sym_7`) AND Local "
           "(bare N_SECT) — the name and the binding are one decision";
    EXPECT_EQ(readSymbolTable(objs.b), expectedB)
        << "TU B's colliding helper carries the SAME name as TU A's and must "
           "likewise drop N_EXT, or ld64 sees two global definitions of one name";

    // Each TU's branch binds to ITS OWN nlist index for `_sym_7` (index 1 in
    // both), which is the structural form of the claim the exit code proves at
    // runtime. TU A's second branch targets index 2, the undefined `_second`.
    // r_type 2 = ARM64_RELOC_BRANCH26, r_length 2 = 4 bytes, r_pcrel = 1.
    std::vector<TextReloc> const expectedRelocsA{
        {/*address=*/4u,  /*symbolNum=*/1u, /*type=*/2u, true, true, 2u},
        {/*address=*/12u, /*symbolNum=*/2u, /*type=*/2u, true, true, 2u},
    };
    std::vector<TextReloc> const expectedRelocsB{
        {/*address=*/4u, /*symbolNum=*/1u, /*type=*/2u, true, true, 2u},
    };
    EXPECT_EQ(readTextRelocs(objs.a), expectedRelocsA);
    EXPECT_EQ(readTextRelocs(objs.b), expectedRelocsB);
}

// The load-command list ld64 is handed, as a FULL SEQUENCE — presence, order,
// sizes and the header counters that describe them, on EVERY leg.
//
// This is the always-on half of the platform claim whose runtime half lives in
// the ld64 tier below: that tier can prove Apple's linker stops warning, but
// only on a Mac, and only when one is reachable. Here the same property is a
// structural fact about the bytes, so a regression reddens on Windows and
// Linux too.
//
// ORDER IS PART OF IT. LC_BUILD_VERSION sits between LC_SEGMENT_64 and
// LC_SYMTAB because that is where Apple's own `clang -c` puts it
// (✔MEASURED 2026-08-13 with `otool -l` on a clang-produced arm64 `.o`:
// LC_SEGMENT_64 / LC_BUILD_VERSION / LC_SYMTAB / LC_DYSYMTAB). ld64 imposes no
// order, so matching clang is the whole point of a fidelity fix — a lookup
// that only asked "is it present?" would let the command drift anywhere.
//
// RED-ON-DISABLE, two independent levers, both exercised: remove the
// `image.buildVersion` block from `macho64-arm64-darwin.format.json` and the
// sequence comes back two commands with sizeofcmds 176; leave the emission in
// but stop counting it in ncmds/sizeofcmds and the `off == 32 + sizeofcmds`
// check inside `readLoadCommands` fires.
TEST(MachOLd64LocalCollision, BothTusDeclareTheirPlatformInLoadCommandOrder) {
    TwoObjects const objs = buildTwoTus();
    ASSERT_FALSE(objs.a.empty());
    ASSERT_FALSE(objs.b.empty());

    // segment_command_64 (72) + one section_64 (80) = 152; build_version 24;
    // symtab 24. Both TUs are text-only, so both carry the identical list.
    std::vector<LoadCommand> const expected{
        {kLcSegment64,     152u},
        {kLcBuildVersion,   24u},
        {kLcSymtab,         24u},
    };
    EXPECT_EQ(readLoadCommands(objs.a), expected);
    EXPECT_EQ(readLoadCommands(objs.b), expected);

    // The header's own counters, stated exactly rather than inferred from the
    // walk above (the walk checks they AGREE; these check they are RIGHT).
    for (auto const* obj : {&objs.a, &objs.b}) {
        EXPECT_EQ(readU32LE(*obj, 16), 3u);    // ncmds
        EXPECT_EQ(readU32LE(*obj, 20), 200u);  // sizeofcmds = 152 + 24 + 24
    }

    // The platform each object declares: PLATFORM_MACOS, min-OS 11.0, SDK
    // 11.0, ntools 0 — read from the command itself, so a schema that changed
    // platform without anyone noticing shows up here.
    for (auto const* obj : {&objs.a, &objs.b}) {
        auto const at = findLoadCommand(*obj, kLcBuildVersion);
        ASSERT_TRUE(at.has_value());
        EXPECT_EQ(readU32LE(*obj, *at + 8),  1u);           // PLATFORM_MACOS
        EXPECT_EQ(readU32LE(*obj, *at + 12), 0x000B0000u);  // minos 11.0
        EXPECT_EQ(readU32LE(*obj, *at + 16), 0x000B0000u);  // sdk 11.0
        EXPECT_EQ(readU32LE(*obj, *at + 20), 0u);           // ntools
    }
}

// ── TIER 2: the ld64 runtime witness ───────────────────────────────────────

namespace {

// WHY THIS TIER CARRIES A VERDICT INSTEAD OF A BARE `GTEST_SKIP`.
//
// A skipped gtest exits 0 and ctest paints the entry green, so "the linker was
// not there" and "the linker linked it" are indistinguishable from the gate's
// summary line. That is the masked-coverage shape
// D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT was opened for (449 of 557 manifests
// silently skipped and counted as passes) and the shape
// D-TEST-NATIVE-ORACLE-INERT-ON-POSIX was opened for (a native oracle that
// skips on error is a broken oracle that reports success). This tier therefore
// lands in the SAME `ArmVerdict` vocabulary the corpus harnesses use, prints
// the ledger line whatever the outcome, and honours DSS_STRICT_ARM_VERDICTS.
//
// THE THREE OUTCOMES, AND WHY EACH GETS THE CLASS IT GETS:
//
//   * NOT DARWIN → `SkippedByRunOn`, the STRUCTURAL class. Predictable from the
//     host OS alone and unchangeable by anything installed on it; ld64 is a
//     Darwin-hosted linker and there is no emulator for the Mach-O it produces.
//     Counted, printed, never failed — including under strict mode, because a
//     strict mode that reddens every Windows and Linux leg is a strict mode
//     nobody can turn on, and Tier 1 is what covers those legs.
//   * DARWIN, NO LINKER/SDK → `SkippedBuildInputMissing`, the ENVIRONMENTAL
//     class. The machine could have supplied it and did not (no Xcode Command
//     Line Tools, an `xcrun` shim with nothing behind it). Strict mode promotes
//     it to a RED, which is exactly the lever the macos gate needs so this
//     witness cannot quietly stop running there.
//   * ANYTHING THAT BREAKS → `Poisoned` plus a hard assertion. A link that
//     fails, a binary that will not spawn, an exit code that is not 42 — none
//     of those is a skip, and the ld64 stderr is folded into the message
//     because the duplicate-symbol line IS the diagnosis.
//
// ✔THE STRICT LEVER IS NOT DECORATIVE, MEASURED 2026-08-13 rather than asserted:
// running this binary on the Mac with `xcrun` made unreachable (PATH pointed at
// an empty directory) reports `sh: xcrun: command not found`, lands
// `skipped-build-input-missing`, and exits 0 as a SKIP by default — while the
// same run under `DSS_STRICT_ARM_VERDICTS=1` FAILS the test and exits 1. Both
// arms were observed; a promotion path that has never been seen to fire is a
// gate nobody can rely on.
struct WitnessVerdict {
    test_support::ArmVerdict verdict = test_support::ArmVerdict::Poisoned;
    std::string              detail;
};

constexpr char const* kWitnessExample = "link/macho_ld64_local_collision";
constexpr char const* kWitnessSpec    = "arm64:macho64-arm64-darwin";
constexpr char const* kWitnessArm     = "ld64-two-tu-link-run";

// Record the verdict, print the accounting, and enforce strict mode.
//
// Returns the message the CALLER must pass to `GTEST_SKIP()`, or "" when the
// caller must NOT skip. The skip is issued at the call site rather than here
// on purpose: `GTEST_SKIP()` inside a helper marks the result but does not
// return from the test body, so a helper that "skipped" would go on to run the
// very assertions it meant to stand in for — the same subroutine trap that
// makes `ASSERT_*` unsafe outside a `void` test body.
[[nodiscard]] std::string recordWitness(WitnessVerdict const& v) {
    test_support::ArmVerdictLedger ledger;
    ledger.record(kWitnessExample, kWitnessSpec, kWitnessArm, v.verdict, v.detail);
    // Printed unconditionally. The counts line is the artifact a reader greps
    // to answer "did the ld64 leg actually run on this host?", and it must be
    // there on the green path too — a ledger that only speaks up on failure
    // re-creates the silence it exists to break.
    std::string const name{test_support::armVerdictName(v.verdict)};
    std::fprintf(stdout, "[ld64-witness] %s\n", ledger.renderCountsLine().c_str());
    // `renderSkipDetail` omits VERIFIED arms by design — a corpus caller does
    // not want 557 success lines buried under the handful it must act on. Here
    // there is exactly ONE arm and its detail IS the evidence (the resolved ld64
    // command and the observed exit code), so the verified case prints directly.
    if (test_support::armVerdictIsVerified(v.verdict)) {
        std::fprintf(stdout, "[ld64-witness]   [%s] %s\n", name.c_str(),
                     v.detail.c_str());
    } else {
        std::fputs(ledger.renderSkipDetail("[ld64-witness]   ").c_str(), stdout);
    }
    std::fflush(stdout);

    // `Ran` needs no skip; `Poisoned` is about to be FAILed at the call site,
    // and skipping on top of a failure would only obscure which one is real.
    if (!test_support::armVerdictIsSkip(v.verdict)) return {};

    auto const strict = test_support::readStrictArmVerdicts();
    if (strict.malformed) {
        ADD_FAILURE()
            << test_support::kStrictArmVerdictsEnv << "='" << strict.raw
            << "' is not a recognised value. An unrecognised setting must never "
               "quietly mean 'off' — that is how a gate stops gating without "
               "anyone noticing.";
    }
    if (strict.on && test_support::armVerdictIsEnvironmentalSkip(v.verdict)) {
        ADD_FAILURE()
            << "STRICT: the ld64 runtime witness was environmentally skipped ("
            << test_support::armVerdictName(v.verdict) << ") — " << v.detail
            << ". On a Darwin gate leg this witness must RUN; a green here would "
               "mean D-LK-MACHO-STB-LOCAL-LD64-RUNTIME-WITNESS is unwitnessed "
               "while reporting as covered.";
        return {};
    }
    return "[" + name + "] " + v.detail;
}

// These two are CALLED only from the Darwin arm but COMPILED on every host, and
// the missing `#if defined(__APPLE__)` around them is deliberate — the same rule
// `native_c_probe.hpp` states for its Windows-only `locateMsvcToolchain`: a seam
// that only one leg's compiler ever parses is a seam that rots unseen between
// runs on that leg. Nothing in either is platform-specific, so the guard would
// buy nothing and cost the type-checking.

// Run `cmd`, capturing stdout+stderr into `log`. Returns the shell's exit
// status. `captureCmd` is the shared spelling — see the include note.
[[nodiscard]] int runCapturing(std::string const& cmd, fs::path const& log) {
    return std::system(
        test_support::native_probe::captureCmd(cmd, log).c_str());
}

// The first line of a command's output, trimmed — how `xcrun` answers a query.
// Empty means "no answer", which the callers treat as ABSENT rather than broken
// (an `xcrun` that cannot find a tool also exits non-zero and prints its
// complaint, and the caller reports both).
[[nodiscard]] std::string firstLine(fs::path const& log) {
    std::ifstream in{log};
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

}  // namespace

// THE WITNESS. Two DSS-emitted Mach-O relocatable objects, each defining
// `_sym_7`, linked by ld64 into one executable that runs and exits 42.
//
// RED-ON-REVERT — the half that proves the witness is not vacuous.
// ✔MEASURED 2026-08-13, macOS 26.5.2 / arm64 / Apple ld-1267, by changing
// `ObjectSymbolNames::definedBinding`'s not-externally-visible fall-through
// from `SymbolBinding::Local` to `SymbolBinding::Global` (that ONE line is the
// whole pre-TF-C54 behaviour: the name stays `_sym_7`, only the binding
// changes) and rebuilding. ld64 then refuses the link, rc=256 from the shell:
//
//     duplicate symbol '_sym_7' in:
//         …/tu_a.o
//         …/tu_b.o
//     ld: 1 duplicate symbols
//
// BOTH tiers went red on that mutation — Tier 1 on the n_type (0x0F where
// 0x0E is required, in both objects) and Tier 2 on the refused link.
TEST(MachOLd64LocalCollisionNative, Ld64LinksCollidingLocalsAndBinaryExits42) {
#if !defined(__APPLE__)
    if (auto const skip = recordWitness(
            {test_support::ArmVerdict::SkippedByRunOn,
             "ld64 is a Darwin-hosted linker and a Mach-O executable has no "
             "off-Mac runner, so this host can produce no verdict on the link. "
             "The always-on structural guard for the same property is "
             "MachOLd64LocalCollision.BothTusEmitCollidingHelperAsLocalWithoutNExt"
             " in this file, and it DID run here."});
        !skip.empty()) {
        GTEST_SKIP() << skip;
    }
    return;
#else
    test_support::ScratchDir scratch{test_support::Location::Temp, "macho-ld64"};
    auto const dir = scratch.path();

    // Locate the linker and the SDK. `xcrun` is present on every macOS install
    // INCLUDING ones with no Command Line Tools behind it, so its exit status —
    // not its mere existence — is what decides absent-vs-present. This is the
    // same shim trap `findCompiler`'s Unix arm documents for `/usr/bin/cc`.
    fs::path const toolLog = dir / "xcrun.txt";
    if (runCapturing("xcrun -f ld", toolLog) != 0 || firstLine(toolLog).empty()) {
        if (auto const skip = recordWitness(
                {test_support::ArmVerdict::SkippedBuildInputMissing,
                 "`xcrun -f ld` could not name a linker on this Darwin host — no "
                 "Xcode Command Line Tools behind the xcrun shim. Output: "
                     + test_support::native_probe::tailOf(toolLog, 10)});
            !skip.empty()) {
            GTEST_SKIP() << skip;
        }
        return;
    }
    std::string const ldPath = firstLine(toolLog);

    fs::path const sdkLog = dir / "sdk.txt";
    if (runCapturing("xcrun --show-sdk-path", sdkLog) != 0
        || firstLine(sdkLog).empty()) {
        if (auto const skip = recordWitness(
                {test_support::ArmVerdict::SkippedBuildInputMissing,
                 "`xcrun --show-sdk-path` named no SDK, so libSystem cannot be "
                 "resolved for the link. Output: "
                     + test_support::native_probe::tailOf(sdkLog, 10)});
            !skip.empty()) {
            GTEST_SKIP() << skip;
        }
        return;
    }
    std::string const sdkPath = firstLine(sdkLog);

    TwoObjects const objs = buildTwoTus();
    ASSERT_FALSE(objs.a.empty());
    ASSERT_FALSE(objs.b.empty());

    fs::path const objA = dir / "tu_a.o";
    fs::path const objB = dir / "tu_b.o";
    fs::path const exe  = dir / "two_tu_witness";
    // Checked, not fire-and-forget. A failed write would reach ld64 as "file not
    // found" and be reported as a LINK failure — i.e. as the exact defect this
    // witness exists to detect, from a cause that has nothing to do with it.
    auto writeObject = [](fs::path const& p,
                          std::vector<std::uint8_t> const& bytes) {
        std::ofstream out{p, std::ios::binary};
        out.write(reinterpret_cast<char const*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        return out.good();
    };
    ASSERT_TRUE(writeObject(objA, objs.a)) << "could not stage " << objA;
    ASSERT_TRUE(writeObject(objB, objs.b)) << "could not stage " << objB;

    // The link.
    //
    // `-platform_version` states the OUTPUT image's deployment target, which
    // ld64 otherwise has to invent for a link whose inputs declare none. It
    // says nothing about the INPUTS — each object answers for its own platform
    // through its LC_BUILD_VERSION, and the assertion below is that every DSS
    // object now does.
    //
    // Explicit `-e _main` rather than relying on ld64's default entry name, so
    // the entry contract is stated in the command a reader reproduces by hand.
    // dyld enters `_main` through libSystem and calls `exit()` with its return
    // value, which is how the 20+22 sum becomes the process exit code.
    fs::path const linkLog = dir / "ld.txt";
    std::string const linkCmd =
        "\"" + ldPath + "\" -arch arm64 -platform_version macos 12.0 12.0"
        " -syslibroot \"" + sdkPath + "\" -lSystem -e _main -o \""
        + exe.string() + "\" \"" + objA.string() + "\" \"" + objB.string() + "\"";
    int const linkRc = runCapturing(linkCmd, linkLog);

    // ★ ld64's OWN OUTPUT IS PART OF THE VERDICT, not just its exit status.
    //
    // WHY THIS TIER OWNS THE ASSERTION. Every byte pin in the tree can prove
    // DSS emitted an LC_BUILD_VERSION; only ld64 can say whether ld64 is
    // satisfied by it, and the whole content of "the object declares its
    // platform" is that Apple's linker stops guessing. So the claim is made
    // where the instrument is.
    //
    // ✔MEASURED 2026-08-13 on the operator's Apple Silicon Mac (macOS 26.5.2,
    // Apple `ld` PROJECT:ld-1267), both states of the same machine:
    //   * BEFORE — with the MH_OBJECT walker emitting no platform command, the
    //     log opened with one `ld: warning: no platform load command found in
    //     '<…>/tu_a.o', assuming: macOS` per object, and this assertion FAILED
    //     while the link still succeeded and the binary still exited 42. That
    //     is exactly the shape being fixed: a defect an exit-status-only
    //     witness cannot see.
    //   * AFTER — the same command against objects carrying LC_BUILD_VERSION
    //     produces NO output at all; the ledger detail below prints ld64's
    //     words on the green path, and it printed `(nothing)`.
    // The log is echoed on the failing path too, so a reader gets ld64's words
    // rather than a bare boolean whichever way this goes.
    std::string linkOutput;
    {
        std::ifstream in{linkLog, std::ios::binary};
        linkOutput.assign(std::istreambuf_iterator<char>{in},
                          std::istreambuf_iterator<char>{});
    }
    EXPECT_EQ(linkOutput.find("no platform load command"), std::string::npos)
        << "ld64 must not have to GUESS a platform for a DSS object: every "
           "MH_OBJECT the darwin formats produce carries LC_BUILD_VERSION. A "
           "per-object warning trains readers to ignore linker warnings and "
           "can bury a real one.\n  command: " << linkCmd
        << "\n  ld64 said:\n" << linkOutput;

    if (linkRc != 0 || !fs::exists(exe)) {
        (void)recordWitness({test_support::ArmVerdict::Poisoned,
                             "the ld64 link failed"});
        FAIL() << "ld64 REFUSED the two-TU link (rc=" << linkRc << ").\n"
               << "  command: " << linkCmd << "\n"
               << "  A `duplicate symbol '_sym_7'` here means the internal-"
                  "linkage binding regressed to N_SECT|N_EXT and both TUs' "
                  "helpers entered the global namespace "
                  "(D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION)."
               << test_support::native_probe::tailOf(linkLog, 40);
    }

    auto const run = test_support::runBinary(exe, test_support::kRunBudget,
                                             /*captureStdout=*/true);
    if (!run.spawned || run.timedOut) {
        (void)recordWitness({test_support::ArmVerdict::Poisoned,
                             "the linked binary did not run"});
        FAIL() << "the ld64-linked binary did not run: " << run.diagnostic
               << "\n  stdout: " << run.capturedStdout;
    }

    // 42 = 20 + 22. 40 or 44 would mean ld64 collapsed the two `_sym_7`
    // definitions into one and both call sites reached the same body — the
    // silent form of the failure this witness exists to catch.
    EXPECT_EQ(run.exitCode, kExpectedExit)
        << "each TU's call must reach ITS OWN `_sym_7` (" << kHelperAResult
        << " + " << kHelperBResult << "); " << run.exitCode
        << " means the two module-private definitions were not kept apart.\n"
        << "  command: " << linkCmd << "\n  stdout: " << run.capturedStdout;

    // The `Ran` detail carries the RESOLVED command and the observed exit code,
    // not just the word "ran". The ledger line is printed on the green path too
    // (see `recordWitness`), so the gate log of a passing macOS leg contains the
    // exact invocation a reader can paste into a shell to re-derive the result —
    // which is what "witnessed" has to mean for a row whose whole content is an
    // external tool's verdict.
    //
    // ★ WHAT ld64 SAID is part of that detail, verbatim, INCLUDING when it said
    // nothing. The assertion above is deliberately scoped to the one warning
    // this change is about — asserting an EMPTY log would red the gate for any
    // future unrelated ld64 note, which is not a DSS defect — but that leaves
    // "the log is otherwise clean" unobserved unless it is printed. So it is
    // printed: a reader of a green gate log can see for themselves that the
    // platform warning is gone AND that nothing replaced it, without the test
    // having to predict what ld64 might add next.
    (void)recordWitness(
        {test_support::ArmVerdict::Ran,
         "ld64 linked two DSS Mach-O objects that BOTH define `_sym_7`; the "
         "binary exited " + std::to_string(run.exitCode) + " (expected "
             + std::to_string(kExpectedExit) + " = " + std::to_string(kHelperAResult)
             + " from TU A's helper + " + std::to_string(kHelperBResult)
             + " from TU B's). ld64 output: "
             + (linkOutput.empty() ? std::string{"(nothing)"} : linkOutput)
             + " link: " + linkCmd});
#endif
}
