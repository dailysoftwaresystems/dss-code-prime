// core/test_diagnostic_quote — THE ONE RENDERING OF SOURCE TEXT INSIDE A DIAGNOSTIC
// (core/types/diagnostic_quote.hpp; [[D-DIAG-A-DIAGNOSTIC-QUOTES-ITS-OPERAND-WHOLE]], cycle P69 f26d, lane `cs`).
//
// The rule, cell by cell: text at or under the limit is the source's own bytes (the negative cell — a short
// operand is still named in full); a run of white space that holds a line break is one space, so a quote is one
// line; past the limit the text is cut at a character boundary and ends in `…`; the limit is the configuration's,
// the value `configure-time/diagnostics.json` states.

#include "core/types/diagnostic_quote.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace dss {
namespace {

std::string const kEllipsis = "…";

TEST(DiagnosticQuote, TextAtOrUnderTheLimitIsTheSourcesOwnBytes) {
    EXPECT_EQ(quotedSourceText("n", 8), "n");
    EXPECT_EQ(quotedSourceText("abcdefgh", 8), "abcdefgh");        // exactly the limit
    EXPECT_EQ(quotedSourceText("a  +\tb", 8), "a  +\tb");           // a run with no line break is kept
    EXPECT_EQ(quotedSourceText("\"x  y\"", 8), "\"x  y\"");          // a literal's spaces are its own
}

TEST(DiagnosticQuote, ARunHoldingALineBreakIsOneSpace) {
    EXPECT_EQ(quotedSourceText("(c\n   ? p\n   : 1)", 64), "(c ? p : 1)");
    EXPECT_EQ(quotedSourceText("a \r\n\t b", 64), "a b");
    EXPECT_EQ(quotedSourceText("\n  x  \n", 64), "x");              // the ends go
}

TEST(DiagnosticQuote, PastTheLimitTheQuoteIsCutAndSaysSo) {
    EXPECT_EQ(quotedSourceText("abcdefghi", 8), "abcdefgh" + kEllipsis);
    EXPECT_EQ(quotedSourceText("abcd efgh", 5), "abcd" + kEllipsis);  // a space before the cut goes
    // The bound holds whatever the text's size: 10000 lines quote as one line of at most the limit.
    std::string big;
    for (int i = 0; i < 10000; ++i) big += "n +\n";
    std::string const q = quotedSourceText(big, 48);
    EXPECT_LE(q.size(), 48 + kEllipsis.size());
    EXPECT_EQ(q.find('\n'), std::string::npos);
    EXPECT_TRUE(q.ends_with(kEllipsis)) << q;
}

TEST(DiagnosticQuote, ACutNeverSplitsACharacter) {
    std::string const s = "ab\xC3\xA9" "c";   // "ab", U+00E9 (two bytes), "c"
    EXPECT_EQ(quotedSourceText(s, 3), "ab" + kEllipsis);             // the limit falls inside U+00E9
    EXPECT_EQ(quotedSourceText(s, 4), "ab\xC3\xA9" + kEllipsis);
    EXPECT_EQ(quotedSourceText(s, 5), s);
    // A sequence the text truncates is quoted byte by byte, never read past the text's end.
    EXPECT_EQ(quotedSourceText("ab\xE2\x80", 8), "ab\xE2\x80");
}

TEST(DiagnosticQuote, EmptyTextQuotesAsEmpty) {
    EXPECT_EQ(quotedSourceText("", 8), "");
    EXPECT_EQ(quotedSourceText(" \n\t", 8), "");
}

TEST(DiagnosticQuote, TheLimitIsTheConfigurationsValue) {
    std::ifstream in{std::string{DSS_TEST_REPO_ROOT} + "/src/dss-config/configure-time/diagnostics.json",
                     std::ios::binary};
    ASSERT_TRUE(in) << "configure-time/diagnostics.json is not readable";
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string const doc = ss.str();
    std::string_view constexpr kKey = "\"quotedSourceTextLimit\"";
    std::size_t p = doc.find(kKey);
    ASSERT_NE(p, std::string::npos) << "the document states no quotedSourceTextLimit";
    p = doc.find(':', p + kKey.size());
    ASSERT_NE(p, std::string::npos);
    ++p;
    while (p < doc.size() && (doc[p] == ' ' || doc[p] == '\t' || doc[p] == '\n' || doc[p] == '\r')) ++p;
    std::size_t stated = 0;
    std::size_t digits = 0;
    for (; p < doc.size() && doc[p] >= '0' && doc[p] <= '9'; ++p, ++digits)
        stated = stated * 10 + static_cast<std::size_t>(doc[p] - '0');
    ASSERT_GT(digits, 0u) << "quotedSourceTextLimit is not a whole number";
    EXPECT_EQ(diagnosticQuoteLimit(), stated);
    // ... and the one-argument rendering is the rule under it.
    std::string const longText(diagnosticQuoteLimit() + 10, 'x');
    EXPECT_EQ(quotedSourceText(longText), quotedSourceText(longText, diagnosticQuoteLimit()));
    EXPECT_EQ(quotedSourceText(longText), std::string(diagnosticQuoteLimit(), 'x') + kEllipsis);
}

} // namespace
} // namespace dss
