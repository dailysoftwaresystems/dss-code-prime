// D-LK-LIBRARY-FUNCTION-ADDRESS-IS-THE-IMAGE-STUB — the STRUCTURE that gives a library function ONE address across
// the process, read off images the production driver builds from C source (the subject's real input path).
//
// ✔MEASURED 2026-09-30 — what every reference writes, and what DSS wrote:
//   * gcc -no-pie, clang -no-pie (GNU ld) and clang -fuse-ld=lld -no-pie AND -pie: an address-taken import is
//     STT_FUNC in `.dynsym` with st_value = its PLT stub, and the stub's own slot is a JUMP_SLOT (x86_64
//     `R_X86_64_JUMP_SLOT ... 401060 puts`, aarch64 `4005c0`, lld -pie `17c0`); every `&puts == dlsym(...)`.
//   * DSS before P69: every import NOTYPE / 0 with a GLOB_DAT slot, while code and initializers named the stub —
//     `&puts != dlsym` in an executable, and `sp != &puts` INSIDE one PIE.
//
// ★ THE ONE ASSERTION THAT DOES NOT NEED A RUN: a canonical stub's own slot must be a JUMP_SLOT, never a GLOB_DAT.
// A GLOB_DAT there resolves through the non-PLT lookup, which FINDS this image's undefined-with-value definition —
// so the slot would hold the stub's own address and the first call through it would loop forever. That failure is
// a hang, not an exit code; this file pins the structure that prevents it on every host.
//
// Host-independent: it only compiles and reads bytes, so it runs on every leg. The run is
// `examples/c/library_function_address_equals_dlsym`.

#include "asm/asm.hpp"                        // AssembledModule — the hand-built belt case
#include "core/types/alignment.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf.hpp"               // elf::encode — the writer's own belt
#include "link/object_format_schema.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

// puts: address taken IN CODE. atoi: address taken by a static INITIALIZER. printf: only CALLED.
constexpr char const* kSubject =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "typedef int (*text_fn)(const char *);\n"
    "static text_fn from_initializer = atoi;\n"
    "__attribute__((noinline)) static text_fn code_address(void) { return puts; }\n"
    "int main(void) {\n"
    "    printf(\"%d\\n\", 7);\n"
    "    return from_initializer(\"2\") + (code_address() != 0 ? 40 : 0);\n"
    "}\n";

[[nodiscard]] std::vector<std::uint8_t> readFile(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Compile the subject for one `<target>:<format>` spec; returns the image bytes (empty on failure; callers assert).
[[nodiscard]] std::vector<std::uint8_t> buildImage(fs::path const& dir, std::string const& spec,
                                                   std::string const& artifact, DiagnosticReporter& rep) {
    auto const src = dir / "subject.c";
    std::ofstream(src, std::ios::binary) << kSubject;
    auto const out = dir / ("out-" + std::to_string(std::hash<std::string>{}(spec)));
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    int const rc = p.compileFiles(std::vector<std::string>{src.string()}, "c",
                                  std::vector<std::string>{spec}, rep);
    if (rc != 0) return {};
    auto const img = out / artifact;
    return fs::exists(img) ? readFile(img) : std::vector<std::uint8_t>{};
}

// ── The smallest ELF64 reader these pins need (a wire layout fixed by the gABI) ──
[[nodiscard]] std::uint16_t u16(std::vector<std::uint8_t> const& b, std::uint64_t o) {
    return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}
[[nodiscard]] std::uint32_t u32(std::vector<std::uint8_t> const& b, std::uint64_t o) {
    return static_cast<std::uint32_t>(u16(b, o)) | (static_cast<std::uint32_t>(u16(b, o + 2)) << 16);
}
[[nodiscard]] std::uint64_t u64(std::vector<std::uint8_t> const& b, std::uint64_t o) {
    return static_cast<std::uint64_t>(u32(b, o)) | (static_cast<std::uint64_t>(u32(b, o + 4)) << 32);
}

struct Sec {
    std::string   name;
    std::uint64_t addr = 0, offset = 0, size = 0;
};
struct Sym {
    std::string   name;
    std::uint8_t  type = 0;
    std::uint16_t shndx = 0;
    std::uint64_t value = 0;
};
struct Rela {
    std::uint64_t offset = 0;
    std::uint32_t sym = 0, type = 0;
};
struct Image {
    std::vector<Sec>  secs;
    std::vector<Sym>  dynsyms;
    std::vector<Rela> relaDyn;
    [[nodiscard]] Sec const* sec(std::string const& n) const {
        for (auto const& s : secs) if (s.name == n) return &s;
        return nullptr;
    }
    [[nodiscard]] std::optional<std::uint32_t> symIndex(std::string const& n) const {
        for (std::uint32_t i = 0; i < dynsyms.size(); ++i) if (dynsyms[i].name == n) return i;
        return std::nullopt;
    }
};

[[nodiscard]] std::string cstr(std::vector<std::uint8_t> const& b, std::uint64_t o) {
    std::string s;
    while (o < b.size() && b[o] != 0) s.push_back(static_cast<char>(b[o++]));
    return s;
}

[[nodiscard]] Image readImage(std::vector<std::uint8_t> const& b) {
    Image im;
    std::uint64_t const shoff = u64(b, 40);
    std::uint16_t const shnum = u16(b, 60), shstrndx = u16(b, 62);
    std::uint64_t const strOff = u64(b, shoff + shstrndx * 64u + 24);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const h = shoff + static_cast<std::uint64_t>(i) * 64u;
        im.secs.push_back(Sec{cstr(b, strOff + u32(b, h)), u64(b, h + 16), u64(b, h + 24), u64(b, h + 32)});
    }
    Sec const* ds = im.sec(".dynsym");
    Sec const* dstr = im.sec(".dynstr");
    if (ds && dstr) {
        for (std::uint64_t p = 0; p + 24 <= ds->size; p += 24) {
            std::uint64_t const o = ds->offset + p;
            im.dynsyms.push_back(Sym{cstr(b, dstr->offset + u32(b, o)),
                                     static_cast<std::uint8_t>(b[o + 4] & 0xF), u16(b, o + 6), u64(b, o + 8)});
        }
    }
    if (Sec const* ra = im.sec(".rela.dyn")) {
        for (std::uint64_t p = 0; p + 24 <= ra->size; p += 24) {
            std::uint64_t const info = u64(b, ra->offset + p + 8);
            im.relaDyn.push_back(Rela{u64(b, ra->offset + p), static_cast<std::uint32_t>(info >> 32),
                                      static_cast<std::uint32_t>(info & 0xFFFFFFFFu)});
        }
    }
    return im;
}

constexpr std::uint8_t kSttNotype = 0;
constexpr std::uint8_t kSttFunc   = 2;

struct Arm {
    char const*   spec;
    char const*   artifact;
    std::uint32_t globDat;
    std::uint32_t jumpSlot;
    bool          decodesX86Stub;
};

// The rows naming `sym`, by type: type -> HOW MANY rows of that type.
[[nodiscard]] std::map<std::uint32_t, int> rowTypesFor(Image const& im, std::uint32_t sym) {
    std::map<std::uint32_t, int> out;
    for (auto const& r : im.relaDyn) if (r.sym == sym) ++out[r.type];
    return out;
}
// The NUMBER of rows of `type` — not whether the type occurs (`std::map::count` answers only that, so a
// duplicated JUMP_SLOT would have read as one; P69 review NIT).
[[nodiscard]] int rowsOfType(std::map<std::uint32_t, int> const& types, std::uint32_t type) {
    auto const it = types.find(type);
    return it == types.end() ? 0 : it->second;
}

}  // namespace

TEST(LibraryFunctionAddress, AnAddressTakenImportIsCanonicalInEveryExecutableAndItsStubsSlotIsAJumpSlot) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "library-fn-address"};
    Arm const arms[] = {
        {"x86_64:elf64-x86_64-linux-exec", "subject", 6, 7, true},
        {"x86_64:elf64-x86_64-linux-pie", "subject", 6, 7, true},
        {"arm64:elf64-aarch64-linux-exec", "subject", 1025, 1026, false},
        {"arm64:elf64-aarch64-linux-pie", "subject", 1025, 1026, false},
    };
    // Every arm is judged on its own: a failure in one never hides the others, and nothing below reads a byte
    // whose position a failed check left undefined.
    for (Arm const& arm : arms) {
        SCOPED_TRACE(arm.spec);
        DiagnosticReporter rep;
        auto const bytes = buildImage(scratch.path(), arm.spec, arm.artifact, rep);
        bool const isElf64 = bytes.size() >= 64 && bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' &&
                             bytes[3] == 'F' && bytes[4] == 2;
        EXPECT_TRUE(isElf64) << "no ELF64 image was built; errs=" << rep.errorCount();
        if (!isElf64) continue;
        Image const im = readImage(bytes);
        Sec const* plt = im.sec(".plt");
        EXPECT_NE(plt, nullptr) << "an executable calling an import owns a `.plt`";
        if (plt == nullptr) continue;
        for (char const* taken : {"puts", "atoi"}) {
            SCOPED_TRACE(taken);
            auto const idx = im.symIndex(taken);
            EXPECT_TRUE(idx.has_value()) << "the import must be in `.dynsym`";
            if (!idx) continue;
            Sym const& s = im.dynsyms[*idx];
            EXPECT_EQ(s.type, kSttFunc) << "an address-taken import is stated STT_FUNC";
            EXPECT_EQ(s.shndx, 0u) << "it stays SHN_UNDEF: only its ADDRESS is defined here";
            bool const onAStub = s.value >= plt->addr && s.value + 6 <= plt->addr + plt->size;
            EXPECT_TRUE(onAStub) << "st_value must be the image's own stub: st_value 0x" << std::hex << s.value
                                 << ", `.plt` [0x" << plt->addr << ", 0x" << plt->addr + plt->size << ")";
            auto const types = rowTypesFor(im, *idx);
            EXPECT_EQ(rowsOfType(types, arm.globDat), 0)
                << "a GLOB_DAT naming a canonical import resolves to this image's own stub — the stub's slot "
                   "would hold the stub and the first call through it would loop";
            EXPECT_EQ(rowsOfType(types, arm.jumpSlot), 1) << "the stub's own slot is the ONE JUMP_SLOT";
            if (!arm.decodesX86Stub || !onAStub) continue;
            // `FF 25 disp32`: the stub jumps through [rip + disp32] — exactly the JUMP_SLOT's r_offset.
            std::uint64_t const fileOff = plt->offset + (s.value - plt->addr);
            EXPECT_LE(fileOff + 6, bytes.size()) << "`.plt` lies outside the file";
            if (fileOff + 6 > bytes.size()) continue;
            EXPECT_EQ(bytes[fileOff], 0xFFu) << "st_value is not the start of an `FF 25` stub";
            EXPECT_EQ(bytes[fileOff + 1], 0x25u) << "st_value is not the start of an `FF 25` stub";
            if (bytes[fileOff] != 0xFF || bytes[fileOff + 1] != 0x25) continue;
            std::int32_t const disp = static_cast<std::int32_t>(u32(bytes, fileOff + 2));
            std::uint64_t const slot = s.value + 6 + static_cast<std::int64_t>(disp);
            bool found = false;
            for (auto const& r : im.relaDyn) {
                if (r.offset != slot) continue;
                found = true;
                EXPECT_EQ(r.type, arm.jumpSlot) << "the slot the stub jumps through";
                EXPECT_EQ(r.sym, *idx);
            }
            EXPECT_TRUE(found) << "no `.rela.dyn` row fills the slot the canonical stub jumps through";
        }
        // The CALLED-only import is untouched: NOTYPE, value 0, a GLOB_DAT slot (both reference linkers too).
        auto const pf = im.symIndex("printf");
        EXPECT_TRUE(pf.has_value());
        if (!pf) continue;
        EXPECT_EQ(im.dynsyms[*pf].type, kSttNotype);
        EXPECT_EQ(im.dynsyms[*pf].value, 0u);
        auto const pfTypes = rowTypesFor(im, *pf);
        EXPECT_EQ(rowsOfType(pfTypes, arm.jumpSlot), 0);
        EXPECT_EQ(rowsOfType(pfTypes, arm.globDat), 1);
    }
}

TEST(LibraryFunctionAddress, ASharedObjectNeverDefinesACanonicalStub) {
    // A DSO's definition never interposes, so it must not state one: its code LOADS the address from a slot
    // (`externAddrBinding: got`) and its initializer names the symbol (a symbolic row), both bound to the
    // executable's canonical address or the library's function by the loader.
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "library-fn-address"};
    struct DsoArm { char const* spec; std::uint32_t abs64; };
    // R_X86_64_64 = 1, R_AARCH64_ABS64 = 257: the symbol-based row that fills a slot with the symbol's address.
    for (DsoArm const arm : {DsoArm{"x86_64:elf64-x86_64-linux-dyn", 1}, DsoArm{"arm64:elf64-aarch64-linux-dyn", 257}}) {
        SCOPED_TRACE(arm.spec);
        DiagnosticReporter rep;
        auto const bytes = buildImage(scratch.path(), arm.spec, "subject.so", rep);
        // Every arm is judged on its own (P69 review NIT): an EXPECT and a `continue`, never an ASSERT that
        // would end the loop at the first arm that fails.
        EXPECT_FALSE(bytes.empty()) << "errs=" << rep.errorCount();
        if (bytes.empty()) continue;
        Image const im = readImage(bytes);
        for (auto const& s : im.dynsyms) {
            if (s.shndx != 0) continue;   // exports
            EXPECT_EQ(s.value, 0u) << s.name << ": an import of a shared object never carries a value";
            EXPECT_NE(s.type, kSttFunc) << s.name << ": a shared object states no canonical stub";
        }
        // `code_address()` returns &puts: under `externAddrBinding: got` the code LOADS it from a slot the link
        // mints, and that slot is filled by a SYMBOL-BASED abs64 row naming `puts` (not by the stub's GLOB_DAT).
        auto const puts = im.symIndex("puts");
        EXPECT_TRUE(puts.has_value());
        if (!puts.has_value()) continue;
        // EXACTLY one (P69 review NIT): the subject takes `&puts` only in code (its initializer is `atoi`'s), so
        // the link mints one slot for it; a second abs64 row naming `puts` would be a second slot.
        EXPECT_EQ(rowsOfType(rowTypesFor(im, *puts), arm.abs64), 1)
            << "the address of `puts` taken in the DSO's code must be ONE slot the loader fills with `puts` itself";
    }
}

TEST(LibraryFunctionAddress, AnImportIsNeverBothCanonicalAndHoldingAnAddressSlotOfItsOwn) {
    // P69 review NIT: `encodeElfExecDynamic` makes an address-taken import's stub CANONICAL; an import that also
    // carried an address slot of its own (`ExternImport::addressSlotSymbol`, a slot the loader fills with the
    // library's address) would hand out a SECOND address for the one function. No shipped path builds both (a
    // slot is minted for a preemptible definition, which an executable never routes through the loader), so the
    // writer's refusal is reached here on a hand-built module — and must stay loud.
    auto const t = TargetSchema::loadShipped("x86_64");
    auto const f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(t.has_value() && f.has_value());
    auto const* abs64 = (*t)->relocationByName("abs64");
    ASSERT_NE(abs64, nullptr);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(fn);
    ExternImport puts{SymbolId{99}, "puts", "libc.so.6"};
    puts.addressSlotSymbol = SymbolId{98};
    mod.externImports.push_back(puts);
    AssembledData slot;   // `static int (*p)(const char *) = puts;` — an address-taking reference
    slot.symbol    = SymbolId{50};
    slot.section   = DataSectionKind::Data;
    slot.bytes.assign(8, 0);
    slot.alignment = Alignment::ofRuntimePow2(8);
    slot.relocations.push_back(Relocation{0, SymbolId{99}, abs64->kind, 0});
    mod.dataItems.push_back(slot);
    mod.imageEntryOverride = 0u;   // drive the writer directly (no trampoline)
    DiagnosticReporter rep;
    auto const bytes = elf::encode(mod, **t, **f, rep);
    EXPECT_TRUE(bytes.empty());
    bool named = false;
    for (auto const& d : rep.all()) {
        named = named || (d.actual.find("one function would have two addresses in one image") != std::string::npos
                          && d.actual.find("'puts'") != std::string::npos);
    }
    EXPECT_TRUE(named) << "the refusal names the import and says why";
}
