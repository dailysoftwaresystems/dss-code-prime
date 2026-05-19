#include "core/types/source_buffer.hpp"

#include <gtest/gtest.h>

using namespace dss;

TEST(SourceBuffer, FromStringBasic) {
    auto buf = SourceBuffer::fromString("hello, world", "<test>");
    EXPECT_EQ(buf->text(), "hello, world");
    EXPECT_EQ(buf->name(), "<test>");
    EXPECT_EQ(buf->size(), 12u);
    EXPECT_TRUE(buf->id().valid());
}

TEST(SourceBuffer, BufferIdsAreUnique) {
    auto a = SourceBuffer::fromString("a");
    auto b = SourceBuffer::fromString("b");
    auto c = SourceBuffer::fromString("c");
    EXPECT_NE(a->id(), b->id());
    EXPECT_NE(b->id(), c->id());
    EXPECT_NE(a->id(), c->id());
}

TEST(SourceBuffer, SliceBasic) {
    auto buf = SourceBuffer::fromString("0123456789");
    EXPECT_EQ(buf->slice(SourceSpan::of(2, 7)), "23456");
    EXPECT_EQ(buf->slice(2, 7), "23456");
    EXPECT_EQ(buf->slice(SourceSpan::empty(5)), "");
}

TEST(SourceBuffer, SliceClampsPastEnd) {
    auto buf = SourceBuffer::fromString("abc");
    // Past-end is clamped, not UB.
    EXPECT_EQ(buf->slice(1, 100), "bc");
    EXPECT_EQ(buf->slice(50, 100), "");
}

TEST(SourceBuffer, LineColSingleLine) {
    auto buf = SourceBuffer::fromString("hello world");
    EXPECT_EQ(buf->lineCol(0), (LineCol{1, 1}));
    EXPECT_EQ(buf->lineCol(6), (LineCol{1, 7}));
}

TEST(SourceBuffer, LineColWithLf) {
    auto buf = SourceBuffer::fromString("abc\ndef\nghi");
    EXPECT_EQ(buf->lineCol(0), (LineCol{1, 1}));   // 'a'
    EXPECT_EQ(buf->lineCol(3), (LineCol{1, 4}));   // '\n' is end of line 1
    EXPECT_EQ(buf->lineCol(4), (LineCol{2, 1}));   // 'd'
    EXPECT_EQ(buf->lineCol(7), (LineCol{2, 4}));   // '\n'
    EXPECT_EQ(buf->lineCol(8), (LineCol{3, 1}));   // 'g'
    EXPECT_EQ(buf->lineCol(10), (LineCol{3, 3})); // 'i'
}

TEST(SourceBuffer, LineColWithCrLf) {
    auto buf = SourceBuffer::fromString("abc\r\ndef");
    EXPECT_EQ(buf->lineCol(0), (LineCol{1, 1}));   // 'a'
    EXPECT_EQ(buf->lineCol(3), (LineCol{1, 4}));   // '\r' — line 1
    EXPECT_EQ(buf->lineCol(4), (LineCol{1, 5}));   // '\n' — still line 1 (CRLF is one line break)
    EXPECT_EQ(buf->lineCol(5), (LineCol{2, 1}));   // 'd'
}

TEST(SourceBuffer, LineColWithLoneCr) {
    auto buf = SourceBuffer::fromString("abc\rdef");
    EXPECT_EQ(buf->lineCol(3), (LineCol{1, 4}));   // '\r'
    EXPECT_EQ(buf->lineCol(4), (LineCol{2, 1}));   // 'd' — lone \r is a line break
}

TEST(SourceBuffer, LineColPastEndClamps) {
    auto buf = SourceBuffer::fromString("ab");
    // Off-by-one for span.end() should not crash. Result is past-last-column.
    auto lc = buf->lineCol(99);
    EXPECT_EQ(lc.line, 1u);
}

TEST(SourceBuffer, EmptyBuffer) {
    auto buf = SourceBuffer::fromString("");
    EXPECT_EQ(buf->size(), 0u);
    EXPECT_EQ(buf->lineCol(0), (LineCol{1, 1}));
    EXPECT_EQ(buf->slice(0, 0), "");
}
