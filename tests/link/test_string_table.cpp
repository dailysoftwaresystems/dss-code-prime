// StringTable substrate unit tests — plan 14 D-LK4-9.
//
// The three walker tests (test_elf_writer, test_pe_writer,
// test_macho_writer) exercise the substrate via golden bytes, but
// don't directly assert:
//   * Dedup behavior — same name added twice returns the same offset.
//   * Empty-name sentinel for `Init::NulByte` (returns 0).
//   * `release() &&` rvalue-qualification — drain semantics.
//   * `Init::U32SizePrefix` stamps an inclusive size at release time.
//   * Offset arithmetic: NulByte starts at 1, U32SizePrefix at 4.
//
// These tests pin the API contract directly, independent of any
// walker's byte-encoding semantics.

#include "link/format/string_table.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

using namespace dss::link::format::detail;

// ── NulByte init (ELF + Mach-O semantics) ───────────────────────

TEST(StringTableNulByte, ByteZeroIsNulSentinel) {
    StringTable t{StringTable::Init::NulByte};
    auto const v = t.view();
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0], 0u);
}

TEST(StringTableNulByte, EmptyNameReturnsOffsetZero) {
    StringTable t{StringTable::Init::NulByte};
    EXPECT_EQ(t.add(""), 0u);
    // Empty-name lookup must NOT grow the table.
    EXPECT_EQ(t.size(), 1u);
}

TEST(StringTableNulByte, FirstNonEmptyAddReturnsOffsetOne) {
    StringTable t{StringTable::Init::NulByte};
    EXPECT_EQ(t.add(".text"), 1u);
    // Table now holds: '\0' + ".text\0" = 7 bytes.
    EXPECT_EQ(t.size(), 7u);
}

TEST(StringTableNulByte, DedupReturnsExistingOffset) {
    StringTable t{StringTable::Init::NulByte};
    auto const first = t.add("sym_42");
    auto const second = t.add("sym_42");
    EXPECT_EQ(first, second);
    // Table should hold only one copy.
    EXPECT_EQ(t.size(), 1u + 6u + 1u);  // NUL + "sym_42" + NUL
}

TEST(StringTableNulByte, ReleasePassesBytesThroughUnchanged) {
    StringTable t{StringTable::Init::NulByte};
    (void)t.add(".text");
    auto const expectedSize = t.size();
    auto const bytes = std::move(t).release();
    EXPECT_EQ(bytes.size(), expectedSize);
    EXPECT_EQ(bytes[0], 0u);
    EXPECT_EQ(bytes[1], '.');
    EXPECT_EQ(bytes[5], 't');
    EXPECT_EQ(bytes[6], 0u);
}

// ── U32SizePrefix init (PE semantics) ───────────────────────────

TEST(StringTableU32SizePrefix, FirstFourBytesAreSizePlaceholder) {
    StringTable t{StringTable::Init::U32SizePrefix};
    // Before release(), the first 4 bytes are zero placeholders;
    // size() returns the in-progress byte count (including the
    // reserved prefix).
    EXPECT_EQ(t.size(), 4u);
    auto const v = t.view();
    ASSERT_EQ(v.size(), 4u);
    for (auto b : v) EXPECT_EQ(b, 0u);
}

TEST(StringTableU32SizePrefix, EmptyNameAppendedAsConcreteString) {
    // PE/COFF strtab has no sentinel meaning at offset 0 (offset 0
    // is the size prefix byte). Empty name MUST be appended like
    // any other string so the offset is well-defined.
    StringTable t{StringTable::Init::U32SizePrefix};
    auto const offset = t.add("");
    EXPECT_EQ(offset, 4u)
        << "first add returns offset just past the size prefix";
    // Table grew by 1 byte (the appended NUL terminator).
    EXPECT_EQ(t.size(), 5u);
}

TEST(StringTableU32SizePrefix, ReleaseStampsInclusiveSize) {
    StringTable t{StringTable::Init::U32SizePrefix};
    (void)t.add("foo");
    // Expected: size = 4 (prefix) + 3 ("foo") + 1 (NUL) = 8.
    auto const expectedSize = t.size();
    ASSERT_EQ(expectedSize, 8u);
    auto const bytes = std::move(t).release();
    ASSERT_EQ(bytes.size(), 8u);
    // Size prefix (LE u32) at bytes[0..3] should now read 8.
    std::uint32_t const stampedSize =
        static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
    EXPECT_EQ(stampedSize, 8u);
    EXPECT_EQ(bytes[4], 'f');
    EXPECT_EQ(bytes[5], 'o');
    EXPECT_EQ(bytes[6], 'o');
    EXPECT_EQ(bytes[7], 0u);
}

TEST(StringTableU32SizePrefix, DedupReturnsExistingOffset) {
    StringTable t{StringTable::Init::U32SizePrefix};
    auto const first = t.add("symbol");
    auto const second = t.add("symbol");
    EXPECT_EQ(first, second);
    EXPECT_GE(first, 4u);
}

TEST(StringTableU32SizePrefix, ReleaseOnEmptyTableStampsFour) {
    // Smallest legal PE/COFF strtab is just the size prefix
    // reading 4 (the size includes itself).
    StringTable t{StringTable::Init::U32SizePrefix};
    auto const bytes = std::move(t).release();
    ASSERT_EQ(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 4u);
    EXPECT_EQ(bytes[1], 0u);
    EXPECT_EQ(bytes[2], 0u);
    EXPECT_EQ(bytes[3], 0u);
}
