#include "core/types/source_buffer.hpp"
#include "tokenizer/source_reader.hpp"

#include <gtest/gtest.h>

using namespace dss;

namespace {

[[nodiscard]] std::shared_ptr<SourceBuffer> buf(std::string text) {
    return SourceBuffer::fromString(std::move(text), "<test>");
}

} // namespace

TEST(SourceReader, EmptyBufferIsImmediatelyAtEnd) {
    auto src = buf("");
    SourceReader r{*src};
    EXPECT_TRUE(r.isAtEnd());
    EXPECT_EQ(r.position(), 0u);
    EXPECT_EQ(r.size(), 0u);
    EXPECT_EQ(r.peek(), '\0');
    EXPECT_EQ(r.peek(5), '\0');     // past-end lookahead returns sentinel
    EXPECT_TRUE(r.remaining().empty());
}

TEST(SourceReader, PeekDoesNotConsume) {
    auto src = buf("abc");
    SourceReader r{*src};
    EXPECT_EQ(r.peek(0), 'a');
    EXPECT_EQ(r.peek(1), 'b');
    EXPECT_EQ(r.peek(2), 'c');
    EXPECT_EQ(r.peek(3), '\0');     // past-end
    EXPECT_EQ(r.position(), 0u);
    EXPECT_FALSE(r.isAtEnd());
}

TEST(SourceReader, AdvanceMovesByExactlyN) {
    auto src = buf("abcdef");
    SourceReader r{*src};
    r.advance(2);
    EXPECT_EQ(r.position(), 2u);
    EXPECT_EQ(r.peek(), 'c');
    r.advance(3);
    EXPECT_EQ(r.position(), 5u);
    EXPECT_EQ(r.peek(), 'f');
    r.advance(1);
    EXPECT_EQ(r.position(), 6u);
    EXPECT_TRUE(r.isAtEnd());
    EXPECT_EQ(r.peek(), '\0');
}

TEST(SourceReader, AdvancePastEndClampsToSize) {
    auto src = buf("ab");
    SourceReader r{*src};
    r.advance(100);
    EXPECT_EQ(r.position(), 2u);
    EXPECT_TRUE(r.isAtEnd());
}

TEST(SourceReader, SliceReturnsExactRange) {
    auto src = buf("hello world");
    SourceReader r{*src};
    EXPECT_EQ(r.slice(0, 5), "hello");
    EXPECT_EQ(r.slice(6, 11), "world");
    EXPECT_EQ(r.slice(0, 11), "hello world");
}

TEST(SourceReader, SliceClampsToSize) {
    auto src = buf("abc");
    SourceReader r{*src};
    EXPECT_EQ(r.slice(0, 100), "abc");
    EXPECT_EQ(r.slice(1, 100), "bc");
    EXPECT_TRUE(r.slice(100, 200).empty());
    EXPECT_TRUE(r.slice(2, 1).empty());   // inverted range
}

TEST(SourceReader, RemainingReflectsPosition) {
    auto src = buf("abcdef");
    SourceReader r{*src};
    EXPECT_EQ(r.remaining(), "abcdef");
    r.advance(2);
    EXPECT_EQ(r.remaining(), "cdef");
    r.advance(4);
    EXPECT_TRUE(r.remaining().empty());
}

TEST(SourceReader, BufferAccessorReturnsSameInstance) {
    auto src = buf("x");
    SourceReader r{*src};
    EXPECT_EQ(&r.buffer(), src.get());
    EXPECT_EQ(r.buffer().id(), src->id());
}

TEST(SourceReader, Utf8BytesPassThroughTransparently) {
    // A 2-byte UTF-8 sequence (é = C3 A9). The reader is byte-level;
    // the schema's lexeme keys are byte strings, so multi-byte UTF-8
    // chars in identifiers fall out of the byte loop naturally.
    auto src = buf("\xC3\xA9x");
    SourceReader r{*src};
    EXPECT_EQ(r.size(), 3u);
    EXPECT_EQ(static_cast<unsigned char>(r.peek(0)), 0xC3u);
    EXPECT_EQ(static_cast<unsigned char>(r.peek(1)), 0xA9u);
    EXPECT_EQ(r.peek(2), 'x');
    r.advance(3);
    EXPECT_TRUE(r.isAtEnd());
}
