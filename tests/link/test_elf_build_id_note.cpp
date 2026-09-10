// D-LK-ELF-EMITS-NO-BUILD-ID-NOTE — the ELF image's `.note.gnu.build-id`.
//
// ✔MEASURED 2026-09-07 (WSL Ubuntu 24.04, GNU ld 2.42), each reference probed
// SEPARATELY and each with the CONTROL that says the note is a per-link POLICY
// rather than an unconditional property of the target:
//   * gcc 13.3.0 `-O2 a.c -o a1.out` and a rebuild to `a2.out` both stamp
//     `df4a8b78d56207b6df0e681bb4c1b48d54f6d124` — REPRODUCIBLE;
//     `-O2 b.c` (one character of source different) stamps
//     `703e81b32e5595635a80b3f9186895a42dcb51ab` — DISTINCT.
//   * clang 18.1.3 stamps `49138c0b580bd392b24f1d5ca445d45b51f1788e` twice.
//   * CONTROL, both compilers: `-Wl,--build-id=none` REMOVES the section
//     (3 NOTE sections become 2). Without this arm, "gcc emits a build id" is
//     equally consistent with "this target always has one", which decides
//     nothing about whether presence belongs in a config document.
//   * Both `.so` forms carry one; `gcc -c` and `clang -c` carry NONE, so the
//     relocatable flavour is correctly excluded.
// ⇒ two working references at DEFAULT flags ⇒ the union makes it REQUIRED, and
// the `--build-id=none` knob makes PRESENCE a declaration rather than a
// constant in the walker.
//
// ── WHAT EACH TEST HERE IS FOR ────────────────────────────────────────────
//
//  1. `StaticExecCarriesAMappedBuildIdNoteOfTheDeclaredShape`
//     The wire record, read back off the emitted bytes, and the proof it is
//     INSIDE PT_LOAD #1 — an unmapped identity is invisible to the crash
//     reporter that reads it.
//  2. `TheDescriptorIsDerivedFromTheImageItIdentifies`
//     The fixed point: SHA-256 over the emitted image with the descriptor
//     zeroed reproduces the stamped descriptor exactly. This is what makes the
//     id a function of the CONTENT rather than of the clock or a counter.
//  3. `RebuildingTheSameModuleReproducesTheSameId` / `4. ...DistinctIds`
//     Reproducible and distinguishing — the two halves that make an id useful,
//     and the two the references were measured on above.
//  5. `AFormatDeclaringNoNoteRowEmitsNoNote`
//     THE CONTROL, and it is the `--build-id=none` arm: the SAME module and
//     the SAME walker, with the `note` row REMOVED from a document derived
//     from the shipped one, emits no SHT_NOTE section at all. Derived at run
//     time from the shipped document rather than transcribed, so it keeps
//     saying the same thing after the shipped document changes.
//  6. `TheDynamicArmCarriesTheSameNote`
//     One document, two walkers. `elf::encode` routes to the dynamic walker the
//     moment a module carries an extern import, so a build id that appeared only
//     on freestanding programs would be one declaration with two behaviours.

#include "asm/asm.hpp"
#include "core/crypto/sha256.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace dss;

namespace {

constexpr std::uint32_t kShtNote        = 7;
constexpr std::uint32_t kNtGnuBuildId   = 3;
constexpr std::uint64_t kShfAlloc       = 2;
constexpr std::size_t   kDescBytes      = 32;   // a full SHA-256
constexpr std::size_t   kDescOffset     = 16;   // 12-byte Nhdr + "GNU\0"
constexpr std::size_t   kNoteBytes      = kDescOffset + kDescBytes;
constexpr char const*   kNoteName       = ".note.gnu.build-id";

[[nodiscard]] std::uint16_t readU16(std::vector<std::uint8_t> const& b,
                                    std::uint64_t off) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(b.at(off))
        | static_cast<std::uint16_t>(static_cast<std::uint16_t>(b.at(off + 1))
                                     << 8));
}
[[nodiscard]] std::uint32_t readU32(std::vector<std::uint8_t> const& b,
                                    std::uint64_t off) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(b.at(off + i)) << (i * 8);
    return v;
}
[[nodiscard]] std::uint64_t readU64(std::vector<std::uint8_t> const& b,
                                    std::uint64_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(b.at(off + i)) << (i * 8);
    return v;
}
[[nodiscard]] std::string readCStr(std::vector<std::uint8_t> const& b,
                                   std::uint64_t off) {
    std::string s;
    for (std::uint64_t p = off; p < b.size() && b[p] != 0; ++p)
        s.push_back(static_cast<char>(b[p]));
    return s;
}

struct Shdr {
    std::string   name;
    std::uint32_t type  = 0;
    std::uint64_t flags = 0;
    std::uint64_t addr = 0, offset = 0, size = 0, align = 0, entSize = 0;
};

[[nodiscard]] std::vector<Shdr> readSections(std::vector<std::uint8_t> const& b) {
    std::vector<Shdr> out;
    if (b.size() < 64) return out;
    std::uint64_t const shoff = readU64(b, 40);
    std::uint16_t const shnum = readU16(b, 60);
    std::uint16_t const shstrndx = readU16(b, 62);
    std::uint64_t const shstrOff = readU64(b, shoff + shstrndx * 64ull + 24);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const h = shoff + i * 64ull;
        Shdr s;
        s.name    = readCStr(b, shstrOff + readU32(b, h + 0));
        s.type    = readU32(b, h + 4);
        s.flags   = readU64(b, h + 8);
        s.addr    = readU64(b, h + 16);
        s.offset  = readU64(b, h + 24);
        s.size    = readU64(b, h + 32);
        s.align   = readU64(b, h + 48);
        s.entSize = readU64(b, h + 56);
        out.push_back(std::move(s));
    }
    return out;
}

[[nodiscard]] Shdr const* find(std::vector<Shdr> const& v,
                               std::string const& name) {
    for (auto const& s : v) {
        if (s.name == name) return &s;
    }
    return nullptr;
}
// ⚠⚠ THE RVALUE OVERLOAD IS DELETED, AND THIS IS THE THIRD TIME THIS REPOSITORY
// HAS PAID FOR THE CLASS. `find(readSections(img), name)` returns a pointer INTO A
// TEMPORARY that dies at the end of the full expression, so every later `*note`
// reads freed memory. ✔MEASURED 2026-09-08 on the macOS carriage: two images built
// from DIFFERENT code both read `note->offset == 0`, so `descriptorOf` returned
// bytes 16..47 of the image — the ELF header (`e_type=2`, `e_machine=0x3E`,
// `e_entry=0x401000`, `e_phoff=0x40`) — identically for both, and
// `TwoDistinctImagesGetDistinctIds` failed. On Linux and Windows the freed bytes
// still held the old header, so the SAME undefined behaviour passed on two of three
// carriages.
// ★★ AND THE SIBLING TEST PASSED **VACUOUSLY**: `RebuildingTheSameModuleReproduces-
// TheSameId` asserts EQUALITY, and two dangling reads that both land on offset 0 are
// equal. It was green on every host while proving nothing. A test that cannot fail is
// worse than one that does.
// ⇒ Both prior occurrences (`D-TEST-SCHEMA-TEMPORARY-DANGLING-REFERENCE`, and the
// `named(arrayOf(...), ...)` copy in `tests/ffi/test_pe_abort_behavior_binding.cpp`)
// were fixed by ADOPTING THE NAMED-LOCAL CONVENTION. A convention has no teeth at the
// moment of the decision, which is exactly why the class came back. This deletion
// makes the mistake a COMPILE ERROR instead: bind the sections to a named local first.
Shdr const* find(std::vector<Shdr>&&, std::string const&) = delete;

[[nodiscard]] std::size_t countNoteSections(std::vector<std::uint8_t> const& b) {
    std::size_t n = 0;
    for (auto const& s : readSections(b)) {
        if (s.type == kShtNote) ++n;
    }
    return n;
}

[[nodiscard]] std::vector<std::uint8_t>
descriptorOf(std::vector<std::uint8_t> const& image, Shdr const& note) {
    std::vector<std::uint8_t> d;
    for (std::size_t i = 0; i < kDescBytes; ++i) {
        d.push_back(image.at(note.offset + kDescOffset + i));
    }
    return d;
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
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(elf64-x86_64-linux-exec) failed";
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

// The shipped exec document MINUS its `note` row — the `--build-id=none` arm.
// ⚠ DERIVED, NEVER TRANSCRIBED. A transcribed copy is a second owner of the
// document and goes stale silently; deriving means this control keeps saying
// the same thing on both sides of the day the shipped document changes.
// ★ AND THE DIRECTION IS REMOVE, NOT ADD: a fixture that ADDED the row would
// stay green on the day the shipped document LOST it, which is the failure this
// control exists to catch.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema> formatWithNoNoteRow() {
    auto const path = dss::test::configRoot()
                    / "object-formats/elf64-x86_64-linux-exec.format.json";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "cannot read " << path.string();
        return nullptr;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    auto doc = nlohmann::json::parse(buf.str(), nullptr, false);
    if (doc.is_discarded()) {
        ADD_FAILURE() << "the shipped exec document did not parse as JSON";
        return nullptr;
    }
    nlohmann::json kept = nlohmann::json::array();
    bool removed = false;
    for (auto const& row : doc.at("sections")) {
        if (row.contains("kind") && row.at("kind") == "note") {
            removed = true;
            continue;
        }
        kept.push_back(row);
    }
    if (!removed) {
        ADD_FAILURE() << "premise: the shipped exec document must declare a "
                         "`note` row for this control to remove one";
        return nullptr;
    }
    doc["sections"] = std::move(kept);
    auto f = ObjectFormatSchema::loadFromText(doc.dump(), "<exec, no note>");
    if (!f.has_value()) {
        std::string why;
        for (auto const& d : f.error()) why += d.message + "\n";
        ADD_FAILURE() << "derived document did not load: " << why;
        return nullptr;
    }
    return std::move(f).value();
}

[[nodiscard]] AssembledModule trivialModule(std::vector<std::uint8_t> code) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{7};
    fn.bytes  = std::move(code);
    mod.functions.push_back(std::move(fn));
    mod.imageEntryOverride = 0u;
    return mod;
}

[[nodiscard]] std::vector<std::uint8_t> encodeTrivial(
        std::vector<std::uint8_t>  code,
        Loaded const&              loaded,
        ObjectFormatSchema const*  formatOverride = nullptr) {
    AssembledModule mod = trivialModule(std::move(code));
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target,
                             formatOverride != nullptr ? *formatOverride
                                                       : *loaded.format,
                             rep);
    if (rep.errorCount() != 0) {
        for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    }
    return bytes;
}

}  // namespace

// ── 1. THE WIRE RECORD, AND THAT IT IS MAPPED ────────────────────────────
TEST(ElfBuildIdNote, StaticExecCarriesAMappedBuildIdNoteOfTheDeclaredShape) {
    auto const loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const image = encodeTrivial({0x90, 0x90, 0xC3}, loaded);
    ASSERT_FALSE(image.empty());

    auto const sections = readSections(image);
    auto const* note = find(sections, kNoteName);
    ASSERT_NE(note, nullptr) << "the image must carry `" << kNoteName << "`";
    EXPECT_EQ(note->type, kShtNote)  << "sh_type must be SHT_NOTE";
    EXPECT_EQ(note->flags & kShfAlloc, kShfAlloc)
        << "sh_flags must carry SHF_ALLOC — both references map the note, and "
           "an unmapped identity is invisible to a crash reporter";
    EXPECT_EQ(note->align, 4u) << "the gABI note record aligns to 4";
    EXPECT_EQ(note->entSize, 0u) << "a note section is not an array of records";
    ASSERT_EQ(note->size, kNoteBytes);

    EXPECT_EQ(readU32(image, note->offset + 0), 4u)   << "n_namesz = |\"GNU\\0\"|";
    EXPECT_EQ(readU32(image, note->offset + 4), kDescBytes)
        << "n_descsz — DSS publishes the full SHA-256 it computed rather than "
           "truncating to claim ld's default 20-byte sha1 width";
    EXPECT_EQ(readU32(image, note->offset + 8), kNtGnuBuildId)
        << "n_type = NT_GNU_BUILD_ID";
    EXPECT_EQ(image.at(note->offset + 12), 'G');
    EXPECT_EQ(image.at(note->offset + 13), 'N');
    EXPECT_EQ(image.at(note->offset + 14), 'U');
    EXPECT_EQ(image.at(note->offset + 15), 0u) << "the owner name is NUL-terminated";

    // Inside PT_LOAD #1 — the program header table starts at byte 64.
    ASSERT_EQ(readU32(image, 64 + 0), 1u) << "first phdr must be PT_LOAD";
    std::uint64_t const pVaddr = readU64(image, 64 + 16);
    std::uint64_t const pMemsz = readU64(image, 64 + 40);
    EXPECT_GE(note->addr, pVaddr);
    EXPECT_LE(note->addr + note->size, pVaddr + pMemsz)
        << "the note must be MAPPED — it is SHF_ALLOC, and a section outside "
           "every PT_LOAD is not in the process image at all";
    // File/VA congruence: the walker asserts it, and so does this.
    std::uint64_t const pOffset = readU64(image, 64 + 8);
    EXPECT_EQ(note->offset - pOffset, note->addr - pVaddr)
        << "the note's file offset and VA must advance in lock-step with the "
           "segment, or the loader maps different bytes than `readelf -n` reads";
}

// ── 2. THE FIXED POINT ───────────────────────────────────────────────────
TEST(ElfBuildIdNote, TheDescriptorIsDerivedFromTheImageItIdentifies) {
    auto const loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const image = encodeTrivial({0x90, 0x90, 0xC3}, loaded);
    ASSERT_FALSE(image.empty());
    auto const sections = readSections(image);
    auto const* note = find(sections, kNoteName);
    ASSERT_NE(note, nullptr);

    auto const stamped = descriptorOf(image, *note);
    // Re-derive: zero the descriptor and hash the whole image. The payload sits
    // INSIDE the hashed region and is zero when it is hashed, so the derivation
    // is a fixed point rather than a self-reference — the same arrangement
    // Mach-O's `stampImageUuid` uses, and ld64's.
    std::vector<std::uint8_t> zeroed = image;
    for (std::size_t i = 0; i < kDescBytes; ++i) {
        zeroed.at(note->offset + kDescOffset + i) = 0;
    }
    auto const digest = dss::crypto::sha256(
        std::span<std::uint8_t const>{zeroed.data(), zeroed.size()});
    std::vector<std::uint8_t> expected(digest.begin(), digest.end());
    EXPECT_EQ(stamped, expected)
        << "the descriptor must be SHA-256 of the finished image with the "
           "descriptor zeroed — anything else (a clock, a counter, a random "
           "value) would make a `--config=release` corpus artifact differ from "
           "itself, which several examples compare byte-for-byte";
    bool anyNonZero = false;
    for (auto b : stamped) anyNonZero = anyNonZero || (b != 0);
    EXPECT_TRUE(anyNonZero) << "an all-zero descriptor means nothing stamped it";
}

// ── 3. REPRODUCIBLE ──────────────────────────────────────────────────────
TEST(ElfBuildIdNote, RebuildingTheSameModuleReproducesTheSameId) {
    auto const loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const a = encodeTrivial({0x90, 0x90, 0xC3}, loaded);
    auto const b = encodeTrivial({0x90, 0x90, 0xC3}, loaded);
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a, b)
        << "the whole image must be byte-identical across two encodes — a "
           "non-reproducible build id would make every release artifact differ "
           "from itself";
    auto const sa = readSections(a);
    auto const sb = readSections(b);
    auto const* na = find(sa, kNoteName);
    auto const* nb = find(sb, kNoteName);
    ASSERT_NE(na, nullptr);
    ASSERT_NE(nb, nullptr);
    // ⚠ Until 2026-09-08 both pointers dangled, so this EXPECT_EQ compared two reads
    // of freed memory and passed for that reason on every host. See the deleted
    // rvalue overload of `find`.
    EXPECT_EQ(descriptorOf(a, *na), descriptorOf(b, *nb));
}

// ── 4. DISTINGUISHING ────────────────────────────────────────────────────
TEST(ElfBuildIdNote, TwoDistinctImagesGetDistinctIds) {
    auto const loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    // One instruction byte apart — `nop nop ret` vs `nop nop nop ret`.
    auto const a = encodeTrivial({0x90, 0x90, 0xC3}, loaded);
    auto const b = encodeTrivial({0x90, 0x90, 0x90, 0xC3}, loaded);
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());
    auto const sa = readSections(a);
    auto const sb = readSections(b);
    auto const* na = find(sa, kNoteName);
    auto const* nb = find(sb, kNoteName);
    ASSERT_NE(na, nullptr);
    ASSERT_NE(nb, nullptr);
    EXPECT_NE(descriptorOf(a, *na), descriptorOf(b, *nb))
        << "two images with different content must get different ids — an id "
           "that did not move is not identifying anything";
}

// ── 5. THE CONTROL — presence is DECLARED, not hardcoded ────────────────
TEST(ElfBuildIdNote, AFormatDeclaringNoNoteRowEmitsNoNote) {
    auto const loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    auto const withNote = encodeTrivial({0x90, 0x90, 0xC3}, loaded);
    ASSERT_FALSE(withNote.empty());
    ASSERT_EQ(countNoteSections(withNote), 1u)
        << "premise: the shipped document produces exactly one SHT_NOTE";

    auto const noNoteFormat = formatWithNoNoteRow();
    ASSERT_NE(noNoteFormat, nullptr);
    auto const without = encodeTrivial({0x90, 0x90, 0xC3}, loaded,
                                       noNoteFormat.get());
    ASSERT_FALSE(without.empty());
    EXPECT_EQ(countNoteSections(without), 0u)
        << "removing the declared row must remove the note — this is the "
           "`-Wl,--build-id=none` arm both references offer, and it is what "
           "makes presence a per-link policy rather than a constant in the "
           "walker";
    auto const withoutSections = readSections(without);
    EXPECT_EQ(find(withoutSections, kNoteName), nullptr);
    EXPECT_LT(without.size(), withNote.size())
        << "the note's bytes must actually be gone, not merely unnamed";
    EXPECT_EQ(readU16(without, 60) + 1u, readU16(withNote, 60))
        << "exactly ONE section header separates the two images";
}

// ── 6. ONE DOCUMENT, BOTH WALKERS ────────────────────────────────────────
TEST(ElfBuildIdNote, TheDynamicArmCarriesTheSameNote) {
    auto const loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    // An extern import is what routes `elf::encode` to the DYNAMIC walker, so
    // this is the same document reaching a different writer.
    AssembledModule mod = trivialModule({0x90, 0x90, 0xC3});
    ExternImport imp;
    imp.symbol      = SymbolId{81};
    imp.mangledName = "puts";
    imp.libraryPath = "libc.so.6";
    imp.isData      = false;
    mod.externImports.push_back(std::move(imp));
    mod.symbols.push_back(ModuleSymbol{SymbolId{7}, "dss_main",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    DiagnosticReporter rep;
    auto const image = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(image.empty()) << [&] {
        std::string all;
        for (auto const& d : rep.all()) all += d.actual + "\n";
        return all;
    }();

    auto const sections = readSections(image);
    auto const* note = find(sections, kNoteName);
    ASSERT_NE(note, nullptr)
        << "the DYNAMIC arm must emit the note too — the same document reaches "
           "this walker the moment a module carries an extern import, and one "
           "declaration with two behaviours is the silent-inconsistency class";
    EXPECT_EQ(note->type, kShtNote);
    EXPECT_EQ(note->size, kNoteBytes);
    EXPECT_EQ(readU32(image, note->offset + 8), kNtGnuBuildId);

    // Same fixed point as the static arm.
    std::vector<std::uint8_t> zeroed = image;
    for (std::size_t i = 0; i < kDescBytes; ++i) {
        zeroed.at(note->offset + kDescOffset + i) = 0;
    }
    auto const digest = dss::crypto::sha256(
        std::span<std::uint8_t const>{zeroed.data(), zeroed.size()});
    std::vector<std::uint8_t> const expected(digest.begin(), digest.end());
    EXPECT_EQ(descriptorOf(image, *note), expected);

    // Mapped, like the static arm's — PT_PHDR is first here, so scan the
    // program headers for the PT_LOAD that covers it rather than assuming one.
    std::uint64_t const phoff = readU64(image, 32);
    std::uint16_t const phnum = readU16(image, 56);
    bool mapped = false;
    for (std::uint16_t i = 0; i < phnum; ++i) {
        std::uint64_t const h = phoff + i * 56ull;
        if (readU32(image, h + 0) != 1u) continue;   // PT_LOAD
        std::uint64_t const va = readU64(image, h + 16);
        std::uint64_t const sz = readU64(image, h + 40);
        if (note->addr >= va && note->addr + note->size <= va + sz) {
            mapped = true;
        }
    }
    EXPECT_TRUE(mapped) << "the note must sit inside a PT_LOAD";
}
