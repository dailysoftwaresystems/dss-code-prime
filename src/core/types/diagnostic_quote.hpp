#pragma once

#include "core/export.hpp"

#include <cstddef>
#include <string>
#include <string_view>

// ══ THE ONE RENDERING OF SOURCE TEXT INSIDE A DIAGNOSTIC ══════════════════════
//
// A diagnostic that names an operand, a declarator, an attribute clause or a token by
// its spelling quotes it through `quotedSourceText` — never `tree.text(n)` pasted into
// its sentence. Every tier that quotes calls it: the tokenizer and the tree builder for
// a lexeme, the semantic analyzer for an operand, the assembler's lowering for an
// operand of a directive. A repo-guard test holds every message statement of those
// files to it (tests/core/test_diagnostic_quote_guard.cpp).
//
// THE RENDERING. The quote is ONE LINE: a run of white space that holds a line break
// becomes one space, and white space before the first or after the last character goes;
// any other white space is the source's, byte for byte. Past the limit the text is cut
// at a character boundary (a UTF-8 sequence is never split) and ends in `…` (U+2026, a
// character no C source spells, so a cut cannot read as the `...` token). Text at or
// under the limit is quoted WHOLE: a short operand is still named in full.
//
// WHY A LIMIT ([[D-DIAG-A-DIAGNOSTIC-QUOTES-ITS-OPERAND-WHOLE]]). A quote used to be
// the operand's whole text, so one diagnostic grew with the expression it named, and a
// nest of N conditionals printed O(N²) bytes, where both working references stay
// linear: gcc prints the caret's line and clang names the operand's TYPES. The snippet
// under the header carries the location; the quote only names what is there.
//
// THE LIMIT is the configuration's: `quotedSourceTextLimit` of the config tree's
// `configure-time/diagnostics.json`, read when the build is configured and compiled in —
// no default in code stands in for it.

namespace dss {

// The most bytes one quote renders before its ellipsis, as the configuration states it.
[[nodiscard]] DSS_EXPORT std::size_t diagnosticQuoteLimit() noexcept;

// `text` as a diagnostic quotes it under `limit` bytes. Empty text quotes as empty.
[[nodiscard]] DSS_EXPORT std::string quotedSourceText(std::string_view text, std::size_t limit);

// The same under the configured limit — what every diagnostic site calls.
[[nodiscard]] DSS_EXPORT std::string quotedSourceText(std::string_view text);

} // namespace dss
