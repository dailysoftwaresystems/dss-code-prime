#pragma once

#include "core/types/grammar_schema.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

// LOAD A SHIPPED GRAMMAR, OR FAIL THE ONE TEST THAT ASKED FOR IT.
//
// D-TEST-A-TORN-SHIPPED-CONFIG-CRASHES-A-SUITE-INSTEAD-OF-REDDING-IT
//
// ★★★ THE DEFECT THIS REPLACES, ✔MEASURED 2026-08-24 by running both binaries
// against a PRIVATE config root whose `sources/c.lang.json` had been emptied:
//
//   `test_hir_lowering_c`, in `analyzeC`:
//     Failure / Failed / loadShipped(c) failed
//     hir rc=-1073740791  hex=0xC0000409
//
//   `test_tokenizer`, in `loadC`:
//     Value of: loaded.has_value()  Actual: false  Expected: true
//     c.lang.json load failed
//     dss::Tokenizer fatal: schema is null
//     tokenizer rc=-1073740791  hex=0xC0000409
//
// (The two failures are quoted by the FUNCTION they came out of. A `path:line`
// citation is a claim nothing rechecks: insert one line above it and it points
// at unrelated prose while still reading as evidence.)
//
// Both died at `0xC0000409` (STATUS_STACK_BUFFER_OVERRUN — what Windows reports
// for `__fastfail`, which is where UCRT's `abort()` ends up), and the two got
// there by DIFFERENT routes that this one helper closes:
//
//   * `test_hir_lowering_c` called `std::abort()` ITSELF, right after
//     `ADD_FAILURE()`. Abort unwinds nothing, so GoogleTest never ran its
//     reporter: no `[  FAILED  ]` line, no case name, no summary, and under
//     `ctest` an abnormal-termination class rather than a test failure.
//   * `test_tokenizer` checked with the NON-FATAL `EXPECT_TRUE`, then carried
//     on and handed a `nullptr` schema to `Tokenizer`, whose
//     `tokenizerFatal("schema is null")` correctly refuses — and refuses by
//     aborting. The PRODUCT guard is right; the fixture drove it there.
//
// ⇒ NEITHER was an exception escaping a `noexcept` in `src/**`, and neither is a
//   product defect. Both are the same test-side fault: a `LoadResult` that was
//   not allowed to stop the one test that needed it.
//
// ★ THROW, and specifically do not `ASSERT_*` here: a fatal GoogleTest assertion
// only returns from the function it appears in, so a helper that returns a
// SCHEMA cannot use one — it would have to invent a null to return, which is
// exactly the second route above. GoogleTest reports an escaping exception as a
// failure of the one running test, names it, and lets the other cases run.

namespace dss::test_support {

// The shipped grammar for `name`, or a `std::runtime_error` naming the language
// and every diagnostic the loader produced. Never returns null.
[[nodiscard]] inline std::shared_ptr<GrammarSchema const>
shippedSchemaOrThrow(std::string_view name) {
    auto loaded = GrammarSchema::loadShipped(name);
    if (!loaded) {
        std::string message = "loadShipped(\"";
        message += name;
        message += "\") failed";
        // The diagnostics are the whole point: "load failed" sends the reader
        // hunting, "version skew" / "torn read of …" / "parse error at line N"
        // does not.
        for (auto const& d : loaded.error()) {
            message += "\n    ";
            message += d.path;
            message += ": ";
            message += d.message;
        }
        throw std::runtime_error(std::move(message));
    }
    // `loadShipped` never yields an engaged-but-null result; assert it anyway,
    // because a null reaching a consumer is how the tokenizer suite died.
    if (*loaded == nullptr) {
        std::string message = "loadShipped(\"";
        message += name;
        message += "\") returned a null schema";
        throw std::runtime_error(std::move(message));
    }
    return *loaded;
}

} // namespace dss::test_support
