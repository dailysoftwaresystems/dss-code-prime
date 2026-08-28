// Plan 11 FF1 -- the BSD `ar` archive variant. Anchor D-FF1-AR-BSD-VARIANT.
//
// WHY THESE TESTS READ REAL FILES INSTEAD OF SYNTHESISING BYTES.
// Its sibling `test_binary_reader_ar.cpp` synthesises GNU archives in-code,
// which is right for THAT reader: the GNU layout was already witnessed
// against real `ar` 2.42 output when it shipped (c161), and the synthetic
// fixtures pin corruption shapes no real producer emits. Here the whole
// question is "what does a real BSD producer actually write", and a
// hand-authored BSD archive would only test this reader against the test
// author's reading of the format -- the two would agree by construction and
// prove nothing. So the fixtures are REAL, produced by REAL tools, and
// their provenance is recorded beside them in
// `tests/ffi/data/README-bsd-archives.md`:
//
//   libbsdapple.a  Apple `ar` (Xcode toolchain) on macOS 26.5.2 arm64.
//                  "__.SYMDEF SORTED", Mach-O members, leading-'_' symbols.
//   libbsdllvm.a   `llvm-ar-19 --format=bsd` on Ubuntu. Plain "__.SYMDEF",
//                  ELF members, no leading underscore.
//
// TWO PRODUCERS ON PURPOSE. They disagree in every way the format permits
// -- symbol-table name, inline-name padding, member object format, symbol
// spelling, and entry ORDER (Apple sorts by name, so its ranlib[0] is the
// SECOND member) -- so a reader that accidentally hardcodes one producer's
// choices fails against the other. And neither is the host running these
// tests: the Mach-O fixture is parsed on Windows and Linux exactly as it is
// on a Mac, which is the agnosticism claim made executable.
//
// ★ THE LOAD-BEARING PIN is `MemberPayloadSkipsTheInlineName`. A BSD
// "#1/N" member's name lives INSIDE its payload, so the logical object
// starts N bytes in and is N bytes shorter. Get that subtraction wrong and
// the reader still "succeeds" -- it just hands the linker a member whose
// first bytes are "s.o\0" instead of a format magic, and whose tail is
// silently truncated. That is a silent miscompile, not a crash, so it is
// pinned on the magic bytes of the payload the reader actually reports.
//
// The 64-bit symbol tables ("/SYM64/", "__.SYMDEF_64", "__.SYMDEF SORTED
// _64") stay REFUSED -- they need a >4 GiB archive to exercise honestly.
// Their refusal is pinned here too, because a refusal that quietly turns
// into a best-effort parse is the regression this row exists to prevent.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "ffi/binary_reader.hpp"
#include "ffi/binary_readers/ar_reader.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::ffi;

namespace {

[[nodiscard]] std::vector<std::uint8_t> readFixture(std::string_view name) {
    auto const path = dss::test::repoRoot()
                    / "tests" / "ffi" / "data" / std::string{name};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

[[nodiscard]] std::vector<std::uint8_t> appleFixture() {
    return readFixture("libbsdapple.a");
}
[[nodiscard]] std::vector<std::uint8_t> llvmFixture() {
    return readFixture("libbsdllvm.a");
}

// Patch a little-endian u32 in place -- used to mutate a REAL fixture into
// each corruption shape. Mutating real bytes beats synthesising them: the
// mutant differs from a known-good archive in exactly one field, so a green
// refusal cannot be an accident of some other malformed byte.
void patchU32LE(std::vector<std::uint8_t>& b, std::size_t off,
                std::uint32_t v) {
    ASSERT_LT(off + 3, b.size());
    b[off + 0] = static_cast<std::uint8_t>( v        & 0xFF);
    b[off + 1] = static_cast<std::uint8_t>((v >>  8) & 0xFF);
    b[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    b[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

// Offsets into the REAL fixtures, taken from an INDEPENDENT dump of the
// files (a Python decoder written from the format description, not from
// ar_reader.cpp -- so agreement between the two is evidence rather than an
// echo). The fixtures are committed and frozen, so these are stable.
constexpr std::size_t kAppleSymdefData  = 88;   // past the "#1/20" inline name
constexpr std::size_t kLlvmSymdefData   = 80;   // past the "#1/12" inline name

[[nodiscard]] ArSymbol const* findSym(ArArchive const& a,
                                      std::string_view name) {
    for (auto const& s : a.symbols) if (s.name == name) return &s;
    return nullptr;
}

} // namespace

// ── The two real archives parse ────────────────────────────────────

TEST(BinaryReaderArBsd, AppleArchiveMembersAndArmap) {
    auto const bytes = appleFixture();
    ASSERT_FALSE(bytes.empty()) << "fixture tests/ffi/data/libbsdapple.a missing";
    ASSERT_EQ(bytes.size(), 1352u) << "fixture changed; re-derive the pins";

    DiagnosticReporter rep;
    auto r = readArArchive(bytes, "libbsdapple.a", rep);
    ASSERT_TRUE(r.has_value())
        << "Apple BSD archive refused: " << r.error().detail;

    // `ar t` on the Mac lists "__.SYMDEF SORTED", s.o, a_very_long_member_
    // name.o. The symbol table is ARCHIVE METADATA, not a linkable member,
    // so it is excluded here -- the same rule the GNU reader applies to "/"
    // and "//", and what `ArMember` documents.
    ASSERT_EQ(r->members.size(), 2u);
    EXPECT_EQ(r->members[0].name, "s.o");
    EXPECT_EQ(r->members[1].name, "a_very_long_member_name.o");

    // Symbols, and the member each one resolves to -- verbatim from
    // `nm -g libbsdapple.a` on the producing Mac (Mach-O symbols carry the
    // leading underscore; the reader must NOT strip it).
    ASSERT_EQ(r->symbols.size(), 2u);
    auto const* shortSym = findSym(*r, "_dss_bsd_short");
    auto const* longSym  = findSym(*r, "_dss_bsd_long");
    ASSERT_NE(shortSym, nullptr);
    ASSERT_NE(longSym,  nullptr);
    EXPECT_EQ(r->members[shortSym->memberIndex].name, "s.o");
    EXPECT_EQ(r->members[longSym->memberIndex].name, "a_very_long_member_name.o");

    // ★ Apple's table is SORTED BY NAME, so entry 0 is the SECOND member.
    // Pinned because a reader that assumed armap order == member order
    // would pass every other assertion in this test.
    EXPECT_EQ(r->symbols[0].name, "_dss_bsd_long");
    EXPECT_EQ(r->symbols[0].memberOffset, 736u);
    EXPECT_EQ(r->symbols[1].name, "_dss_bsd_short");
    EXPECT_EQ(r->symbols[1].memberOffset, 144u);
}

TEST(BinaryReaderArBsd, LlvmArchiveMembersAndArmap) {
    auto const bytes = llvmFixture();
    ASSERT_FALSE(bytes.empty()) << "fixture tests/ffi/data/libbsdllvm.a missing";
    ASSERT_EQ(bytes.size(), 2768u) << "fixture changed; re-derive the pins";

    DiagnosticReporter rep;
    auto r = readArArchive(bytes, "libbsdllvm.a", rep);
    ASSERT_TRUE(r.has_value())
        << "llvm-ar BSD archive refused: " << r.error().detail;

    ASSERT_EQ(r->members.size(), 2u);
    EXPECT_EQ(r->members[0].name, "s.o");
    EXPECT_EQ(r->members[1].name, "a_very_long_member_name.o");

    // `llvm-nm --print-armap`: dss_bsd_short in s.o, dss_bsd_long in
    // a_very_long_member_name.o. NO leading underscore -- these are ELF
    // members, and the same reader must not invent one.
    ASSERT_EQ(r->symbols.size(), 2u);
    auto const* shortSym = findSym(*r, "dss_bsd_short");
    auto const* longSym  = findSym(*r, "dss_bsd_long");
    ASSERT_NE(shortSym, nullptr);
    ASSERT_NE(longSym,  nullptr);
    EXPECT_EQ(r->members[shortSym->memberIndex].name, "s.o");
    EXPECT_EQ(r->members[longSym->memberIndex].name, "a_very_long_member_name.o");

    // Unsorted: llvm-ar emits members in order, the mirror of Apple above.
    EXPECT_EQ(r->symbols[0].name, "dss_bsd_short");
    EXPECT_EQ(r->symbols[1].name, "dss_bsd_long");
}

// ★★ THE LOAD-BEARING PIN. See the file header.
TEST(BinaryReaderArBsd, MemberPayloadSkipsTheInlineName) {
    // -- Apple: Mach-O members (magic CF FA ED FE, MH_MAGIC_64 stored LE) --
    {
        auto const bytes = appleFixture();
        ASSERT_FALSE(bytes.empty());
        DiagnosticReporter rep;
        auto r = readArArchive(bytes, "libbsdapple.a", rep);
        ASSERT_TRUE(r.has_value()) << r.error().detail;

        for (ArMember const& m : r->members) {
            // The reported payload must START at a format magic. If the
            // inline name were left in, this reads "s.o\0" / "a_ve" instead.
            ASSERT_LE(m.dataOffset + m.size, bytes.size());
            std::span<std::uint8_t const> const payload{
                bytes.data() + static_cast<std::size_t>(m.dataOffset),
                static_cast<std::size_t>(m.size)};
            ASSERT_GE(payload.size(), 4u);
            EXPECT_EQ(payload[0], 0xCFu) << "member " << m.name;
            EXPECT_EQ(payload[1], 0xFAu) << "member " << m.name;
            EXPECT_EQ(payload[2], 0xEDu) << "member " << m.name;
            EXPECT_EQ(payload[3], 0xFEu) << "member " << m.name;
            // ...and it must start strictly past the 60-byte header, which
            // is only true if N was actually subtracted.
            EXPECT_GT(m.dataOffset, m.headerOffset + 60u) << "member " << m.name;
        }
        // Exact geometry, from the independent dump.
        EXPECT_EQ(r->members[0].headerOffset, 144u);
        EXPECT_EQ(r->members[0].dataOffset,   216u);   // 144 + 60 + 12
        EXPECT_EQ(r->members[0].size,         520u);   // 532 - 12
        EXPECT_EQ(r->members[1].headerOffset, 736u);
        EXPECT_EQ(r->members[1].dataOffset,   832u);   // 736 + 60 + 36
        EXPECT_EQ(r->members[1].size,         520u);   // 556 - 36
    }

    // -- llvm-ar: ELF members (magic 7F 'E' 'L' 'F') --
    {
        auto const bytes = llvmFixture();
        ASSERT_FALSE(bytes.empty());
        DiagnosticReporter rep;
        auto r = readArArchive(bytes, "libbsdllvm.a", rep);
        ASSERT_TRUE(r.has_value()) << r.error().detail;

        for (ArMember const& m : r->members) {
            ASSERT_LE(m.dataOffset + m.size, bytes.size());
            std::span<std::uint8_t const> const payload{
                bytes.data() + static_cast<std::size_t>(m.dataOffset),
                static_cast<std::size_t>(m.size)};
            ASSERT_GE(payload.size(), 4u);
            EXPECT_EQ(payload[0], 0x7Fu) << "member " << m.name;
            EXPECT_EQ(payload[1], 'E')   << "member " << m.name;
            EXPECT_EQ(payload[2], 'L')   << "member " << m.name;
            EXPECT_EQ(payload[3], 'F')   << "member " << m.name;
            EXPECT_GT(m.dataOffset, m.headerOffset + 60u) << "member " << m.name;
        }
        EXPECT_EQ(r->members[0].headerOffset, 136u);
        EXPECT_EQ(r->members[0].dataOffset,   200u);   // 136 + 60 + 4
        // ★ CROSS-VARIANT PIN: s.o is byte-for-byte the same gcc object that
        // `ar rcs` packaged into the GNU control archive, where its member
        // size is also 1232. The SAME object packaged by two different `ar`
        // variants must yield the SAME logical size -- an off-by-N in the
        // inline-name subtraction breaks this equality and nothing else.
        EXPECT_EQ(r->members[0].size,         1232u);  // 1236 - 4
        EXPECT_EQ(r->members[1].headerOffset, 1432u);
        EXPECT_EQ(r->members[1].dataOffset,   1520u);  // 1432 + 60 + 28
        EXPECT_EQ(r->members[1].size,         1248u);  // 1276 - 28
    }
}

// The format-blind dispatch must route BSD bytes to the ar reader and
// project the armap, exactly as it does for GNU. Nothing about the
// dispatch is variant-aware -- both share the "!<arch>\n" magic.
TEST(BinaryReaderArBsd, DispatchProjectsBsdArmapToImportSurface) {
    auto const bytes = appleFixture();
    ASSERT_FALSE(bytes.empty());
    DiagnosticReporter rep;
    auto surf = readImportsFromBytes(bytes, "libbsdapple.a", rep);
    ASSERT_TRUE(surf.has_value()) << surf.error().detail;
    ASSERT_EQ(surf->size(), 2u);

    bool sawShort = false;
    for (ImportSurface const& row : *surf) {
        EXPECT_EQ(row.kind,       SymbolKind::NoType);
        EXPECT_EQ(row.visibility, SymbolVisibility::Default);
        EXPECT_EQ(row.linkage,    SymbolLinkage::External);
        if (row.mangledName == "_dss_bsd_short") {
            sawShort = true;
            EXPECT_EQ(row.libraryPath, "libbsdapple.a(s.o)");
        }
    }
    EXPECT_TRUE(sawShort);
}

// ── Corruption shapes: each mutates ONE field of a REAL archive ────

TEST(BinaryReaderArBsd, RanlibSizeNotMultipleOfEightRejected) {
    // The endian/width guard: 8 bytes per (strx, offset) entry, so a count
    // that is not a multiple of 8 means these bytes are a byte-swapped or
    // 64-bit table. Refusing beats sniffing.
    auto bytes = appleFixture();
    ASSERT_FALSE(bytes.empty());
    patchU32LE(bytes, kAppleSymdefData, 17u);   // was 16
    DiagnosticReporter rep;
    auto r = readArArchive(bytes, "mutant.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("not a multiple of the 8-byte"),
              std::string::npos) << r.error().detail;
    EXPECT_NE(r.error().detail.find("D-FF1-AR-BSD-VARIANT"), std::string::npos)
        << r.error().detail;
}

TEST(BinaryReaderArBsd, RanlibArrayPastTableEndRejected) {
    auto bytes = appleFixture();
    ASSERT_FALSE(bytes.empty());
    patchU32LE(bytes, kAppleSymdefData, 8000u);  // a valid multiple of 8...
    DiagnosticReporter rep;
    auto r = readArArchive(bytes, "mutant.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("runs past the table end"),
              std::string::npos) << r.error().detail;
}

TEST(BinaryReaderArBsd, StringTableIndexPastTableRejected) {
    auto bytes = llvmFixture();
    ASSERT_FALSE(bytes.empty());
    // entry[0].strx lives immediately after the 4-byte ranlib-size field.
    patchU32LE(bytes, kLlvmSymdefData + 4, 9999u);
    DiagnosticReporter rep;
    auto r = readArArchive(bytes, "mutant.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("past the"), std::string::npos)
        << r.error().detail;
    EXPECT_NE(r.error().detail.find("string table"), std::string::npos)
        << r.error().detail;
}

TEST(BinaryReaderArBsd, ArmapOffsetMatchingNoMemberRejected) {
    auto bytes = llvmFixture();
    ASSERT_FALSE(bytes.empty());
    // entry[0].offset sits 4 bytes after entry[0].strx.
    patchU32LE(bytes, kLlvmSymdefData + 8, 999u);   // no member header there
    DiagnosticReporter rep;
    auto r = readArArchive(bytes, "mutant.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("matches no archive member"),
              std::string::npos) << r.error().detail;
}

TEST(BinaryReaderArBsd, InlineNameLongerThanMemberRejected) {
    // Widen s.o's "#1/12" to "#1/9999" while leaving its 532-byte payload
    // alone. Without the guard the logical size underflows to ~UINT64_MAX
    // and the member spans the rest of memory.
    auto bytes = appleFixture();
    ASSERT_FALSE(bytes.empty());
    std::string const field = "#1/9999";
    for (std::size_t i = 0; i < 16; ++i) {
        bytes[144 + i] = static_cast<std::uint8_t>(
            i < field.size() ? field[i] : ' ');
    }
    DiagnosticReporter rep;
    auto r = readArArchive(bytes, "mutant.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("more than the member's"),
              std::string::npos) << r.error().detail;
}

// ── The 64-bit shapes stay REFUSED, by name ───────────────────────
//
// Rewriting the symbol table's inline name in place is the cheapest honest
// way to reach these arms: the resulting archive is a real BSD archive that
// merely CLAIMS a table shape this reader has not been shown. That is
// precisely the situation the refusal exists for.

namespace {
// Overwrite the Apple fixture's inline symbol-table name (20 bytes at
// offset 68, NUL-padded) with `name`, keeping the width.
[[nodiscard]] std::vector<std::uint8_t> appleWithSymdefNamed(std::string_view name) {
    auto bytes = appleFixture();
    if (bytes.empty()) return bytes;
    for (std::size_t i = 0; i < 20; ++i) {
        bytes[68 + i] = static_cast<std::uint8_t>(
            i < name.size() ? name[i] : '\0');
    }
    return bytes;
}
} // namespace

TEST(BinaryReaderArBsd, Symdef64StaysRefusedByName) {
    auto const bytes = appleWithSymdefNamed("__.SYMDEF_64");
    ASSERT_FALSE(bytes.empty());
    DiagnosticReporter rep;
    auto r = readArArchive(bytes, "sym64.a", rep);
    ASSERT_FALSE(r.has_value()) << "a 64-bit ranlib table must NOT be parsed "
                                  "blind -- no >4 GiB fixture exercises it";
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("64-bit"), std::string::npos)
        << r.error().detail;
    EXPECT_NE(r.error().detail.find("D-FF1-AR-BSD-VARIANT"), std::string::npos)
        << r.error().detail;
}

TEST(BinaryReaderArBsd, SortedSymdef64StaysRefusedByName) {
    auto const bytes = appleWithSymdefNamed("__.SYMDEF SORTED_64");
    ASSERT_FALSE(bytes.empty());
    DiagnosticReporter rep;
    auto r = readArArchive(bytes, "sym64s.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("64-bit"), std::string::npos)
        << r.error().detail;
}

TEST(BinaryReaderArBsd, UnknownSymdefDialectRefusedNotGuessed) {
    // ★ The anti-fall-through pin. A "__.SYMDEF" spelling this reader has
    // never seen must REFUSE BY NAME, never degrade to a best-effort parse
    // of whatever follows -- that trade (a loud refusal for a silent
    // misparse) is the one this reader never makes.
    auto const bytes = appleWithSymdefNamed("__.SYMDEF FUTURE");
    ASSERT_FALSE(bytes.empty());
    DiagnosticReporter rep;
    auto r = readArArchive(bytes, "future.a", rep);
    ASSERT_FALSE(r.has_value())
        << "an unrecognised __.SYMDEF dialect was parsed anyway";
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("unrecognised"), std::string::npos)
        << r.error().detail;
    EXPECT_NE(r.error().detail.find("D-FF1-AR-BSD-VARIANT"), std::string::npos)
        << r.error().detail;
}
