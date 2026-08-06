// Plan 11 FF4 (C name mangling) tests — `dss::ffi::applyCMangling` /
// `unapplyCMangling` / `cFormatAddsLeadingUnderscore`.
//
// ★ RE-KEYED AT D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN STEP C4. These tests used
// to enumerate `ObjectFormatKind` variants, because the rule lived in a closed
// C++ table keyed on the format identity. It does not any more: the rule is a
// DECLARED VERB read out of `.format.json` (`cSymbolDecoration.scheme`), and the
// mangler is never told which format it is serving. So the pins below are keyed
// on the SCHEME, which is the only thing the mangler can now see.
//
// ★ THE PER-FORMAT ANSWER DID NOT STOP BEING TESTED — IT MOVED to where the fact
// now lives: `tests/link/test_c_symbol_decoration.cpp` enumerates all 24 shipped
// formats FROM DISK and asserts each declares the right scheme for its kind. The
// two files meet in `SchemeIsReadFromTheShippedSchemaNotACppTable` at the bottom
// of this file, which drives a REAL shipped schema end-to-end through the REAL
// mangler — the one test that would go red if C4 were reverted.
//
// Pins:
//   * Each CSymbolDecorationScheme has a deterministic decoration rule.
//   * applyCMangling + unapplyCMangling round-trip cleanly under every scheme.
//   * Empty input is preserved (no synthesis).
//   * Undecorated input through unapplyCMangling under a decorating scheme
//     passes through unchanged (conservative).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "ffi/mangling/c_mangle.hpp"
#include "link/object_format_schema.hpp"
#include "diagnostic_count.hpp"

#include <gtest/gtest.h>

#include <concepts>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::countCode;

// Every `CSymbolDecorationScheme` enumerator, in ordinal order. The loops below
// drive this so all schemes are exercised by one edit.
//
// ★ THE OLD LOOPS ITERATED `kObjectFormatKindTable.rows`, which meant a NEW
// format variant was exercised automatically. Keying on the scheme costs that
// property, because the scheme enum has no name table to walk (it is hand-rolled
// so the sentinel can stay unspellable — see `object_format_kind.hpp`). The
// guard below buys it back: a FOURTH enumerator takes ordinal 3, and the
// assertion that ordinal 3 names nothing goes red, pointing whoever added it
// straight at this list. Cheaper than a name table and it fails LOUD rather than
// silently under-testing.
inline constexpr CSymbolDecorationScheme kAllSchemes[] = {
    CSymbolDecorationScheme::Unspecified,
    CSymbolDecorationScheme::None,
    CSymbolDecorationScheme::LeadingUnderscore,
};

TEST(FfiCMangle, SchemeListAboveCoversEveryEnumerator) {
    EXPECT_EQ(std::size(kAllSchemes), 3u);
    // Ordinals are dense from 0 and stop at 2; a 4th scheme would be spellable
    // at ordinal 3 and this reds.
    EXPECT_TRUE(cSymbolDecorationSchemeName(
                    static_cast<CSymbolDecorationScheme>(3)).empty())
        << "a new CSymbolDecorationScheme enumerator was added -- add it to "
           "kAllSchemes above, or every loop in this file silently stops "
           "covering it";
    for (std::size_t i = 0; i < std::size(kAllSchemes); ++i) {
        EXPECT_EQ(static_cast<std::size_t>(kAllSchemes[i]), i)
            << "kAllSchemes must stay in ordinal order";
    }
}

// ── Per-SCHEME decoration rule pin ─────────────────────────

TEST(FfiCMangle, NoneSchemeAddsNothing) {
    EXPECT_FALSE(cFormatAddsLeadingUnderscore(CSymbolDecorationScheme::None));
}
TEST(FfiCMangle, LeadingUnderscoreSchemeDecorates) {
    // Apple's convention, and the only decorating scheme that ships. It has no
    // bitness axis (32- and 64-bit Mach-O agree) and no arch axis (the arm64 and
    // x86_64 slices of a universal libSystem decorate identically) — which is
    // precisely why it is declared per FORMAT and not per target.
    EXPECT_TRUE(cFormatAddsLeadingUnderscore(
        CSymbolDecorationScheme::LeadingUnderscore));
}
TEST(FfiCMangle, UnspecifiedSentinelDecoratesNothing) {
    // A DEFENSIVE FLOOR, not a reachable policy — and the distinction is worth
    // stating because a reader could mistake this for a supported third answer.
    // No LOADED schema can carry the sentinel: `ObjectFormatData::validate()`
    // requires a real scheme on every format, unconditionally, so the only way
    // to reach here is a hand-built value that bypassed validation. Decorating
    // nothing is the conservative floor (it degrades to the ELF/PE answer rather
    // than inventing a `_`), but the fail-loud that matters happens upstream.
    EXPECT_FALSE(
        cFormatAddsLeadingUnderscore(CSymbolDecorationScheme::Unspecified));
}

// ── applyCMangling ─────────────────────────────────────────

TEST(FfiCMangle, ApplyMachOAddsUnderscore) {
    EXPECT_EQ(applyCMangling("printf", CSymbolDecorationScheme::LeadingUnderscore), "_printf");
    EXPECT_EQ(applyCMangling("malloc", CSymbolDecorationScheme::LeadingUnderscore), "_malloc");
    // already-underscored identifier just gets ANOTHER underscore
    // — the function applies the format rule mechanically, not
    // smart-deduplication (operator must hand a canonical name).
    EXPECT_EQ(applyCMangling("_existing", CSymbolDecorationScheme::LeadingUnderscore),
              "__existing");
}

TEST(FfiCMangle, ApplyElfPassesThrough) {
    EXPECT_EQ(applyCMangling("printf", CSymbolDecorationScheme::None), "printf");
    EXPECT_EQ(applyCMangling("_explicit", CSymbolDecorationScheme::None), "_explicit");
}

TEST(FfiCMangle, ApplyPePassesThroughV1) {
    EXPECT_EQ(applyCMangling("printf", CSymbolDecorationScheme::None), "printf");
}

TEST(FfiCMangle, ApplyWasmPassesThrough) {
    EXPECT_EQ(applyCMangling("env.printf", CSymbolDecorationScheme::None), "env.printf");
}

TEST(FfiCMangle, ApplyEmptyInputReturnsEmpty) {
    EXPECT_EQ(applyCMangling("", CSymbolDecorationScheme::LeadingUnderscore), "");
    EXPECT_EQ(applyCMangling("", CSymbolDecorationScheme::None), "");
}

// ── unapplyCMangling ───────────────────────────────────────

TEST(FfiCMangle, UnapplyMachOStripsLeadingUnderscore) {
    EXPECT_EQ(unapplyCMangling("_printf", CSymbolDecorationScheme::LeadingUnderscore), "printf");
    EXPECT_EQ(unapplyCMangling("_malloc", CSymbolDecorationScheme::LeadingUnderscore), "malloc");
    // Only ONE underscore is stripped — `__main` (Apple's
    // entrypoint canonicalization) demangles to `_main`.
    EXPECT_EQ(unapplyCMangling("__main", CSymbolDecorationScheme::LeadingUnderscore), "_main");
}

TEST(FfiCMangle, UnapplyMachOPreservesMissingUnderscore) {
    // Conservative: a Mach-O symbol WITHOUT the expected leading
    // `_` (uncommon but possible — pre-mangled libraries, system
    // helpers) returns as-is rather than fabricating an error.
    EXPECT_EQ(unapplyCMangling("printf", CSymbolDecorationScheme::LeadingUnderscore), "printf");
}

TEST(FfiCMangle, UnapplyElfPassesThrough) {
    EXPECT_EQ(unapplyCMangling("printf", CSymbolDecorationScheme::None), "printf");
    // Even if the input HAS a leading `_` on ELF (uncommon — would
    // be a manually-decorated symbol), unapply does NOT strip it
    // because the format rule says ELF has no decoration.
    EXPECT_EQ(unapplyCMangling("_explicit", CSymbolDecorationScheme::None), "_explicit");
}

TEST(FfiCMangle, UnapplyEmptyInputReturnsEmpty) {
    EXPECT_EQ(unapplyCMangling("", CSymbolDecorationScheme::LeadingUnderscore), "");
    EXPECT_EQ(unapplyCMangling("", CSymbolDecorationScheme::None), "");
}

// ── Round-trip on every variant ────────────────────────────

TEST(FfiCMangle, ApplyUnapplyRoundTripPreservesCanonicalForm) {
    // For every format-kind variant, apply→unapply must yield the
    // original canonical name back. Iteration is driven by
    // `kAllSchemes` so a future scheme is exercised automatically (the
    // completeness guard above is what keeps that true).
    auto roundTrip = [](std::string_view canonical,
                        CSymbolDecorationScheme scheme) {
        std::string decorated = applyCMangling(canonical, scheme);
        return unapplyCMangling(decorated, scheme);
    };
    for (CSymbolDecorationScheme const k : kAllSchemes) {
        EXPECT_EQ(roundTrip("printf", k), "printf")
            << "scheme = " << static_cast<unsigned>(k);
        EXPECT_EQ(roundTrip("really_long_libc_function_name_123", k),
                  "really_long_libc_function_name_123");
    }
}

// ── D-FF4-3: strict unapply mode ──────────────────────────────
//
// No-decoration formats (Elf/Pe/Wasm/Spirv/Unknown) get
// strict-mode no-op coverage via `ApplyThenStrictUnapplyRoundTrip`
// below, which iterates every ObjectFormatKind variant. The
// MachO-decorated happy path is also covered by that loop but
// is pinned explicitly below for readability (the value
// `"_printf"` is spelled out, vs. table iteration).

TEST(FfiCMangleStrict, MachOStripsLeadingUnderscoreCleanly) {
    DiagnosticReporter rep;
    auto r = unapplyCManglingStrict("_printf", CSymbolDecorationScheme::LeadingUnderscore, rep);
    ASSERT_TRUE(r.has_value()) << mangleErrorKindName(r.error().kind);
    EXPECT_EQ(*r, "printf");
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(FfiCMangleStrict, MachOMissingPrefixFailsLoud) {
    // The core D-FF4-3 contract: a MachO input without leading `_`
    // is a structural anomaly. Strict mode rejects it loudly
    // instead of silently passing through (which is what
    // `unapplyCMangling` does).
    DiagnosticReporter rep;
    auto r = unapplyCManglingStrict("printf", CSymbolDecorationScheme::LeadingUnderscore, rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, MangleErrorKind::MissingExpectedPrefix);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_MangleMissingExpectedPrefix), 1u);
    // Detail must name the offending input so a log grep can locate it.
    EXPECT_NE(r.error().detail.find("printf"), std::string::npos);
}

TEST(FfiCMangleStrict, EmptyInputReturnsEmptySuccess) {
    DiagnosticReporter rep;
    auto r = unapplyCManglingStrict("", CSymbolDecorationScheme::LeadingUnderscore, rep);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "");
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(FfiCMangleStrict, ApplyThenStrictUnapplyRoundTrip) {
    // Apply + strict unapply must always succeed (the canonical
    // FF5 ingest path: produce a decorated name, ship it, ingest
    // it back through strict mode). Iterate over every
    // ObjectFormatKind variant via the canonical name table so a
    // future variant is automatically exercised.
    for (CSymbolDecorationScheme const k : kAllSchemes) {
        std::string const decorated = applyCMangling("printf", k);
        DiagnosticReporter rep;
        auto r = unapplyCManglingStrict(decorated, k, rep);
        ASSERT_TRUE(r.has_value())
            << "round-trip failed for format kind "
            << static_cast<unsigned>(k);
        EXPECT_EQ(*r, "printf")
            << "round-trip wrong result for format kind "
            << static_cast<unsigned>(k);
        EXPECT_EQ(rep.errorCount(), 0u);
    }
}

TEST(FfiCMangleStrict, MangleErrorKindNameRoundTrip) {
    EXPECT_EQ(mangleErrorKindName(MangleErrorKind::MissingExpectedPrefix),
              "MissingExpectedPrefix");
}

TEST(FfiCMangleStrict, DiagnosticCodeNameRoundTrip) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_MangleMissingExpectedPrefix),
              "F_MangleMissingExpectedPrefix");
}

TEST(FfiCMangle, EverySchemeHasAnExplicitRulePin) {
    // The per-scheme rule semantics need a pin so a maintainer editing the
    // predicate cannot silently regress the decoration policy. Exactly ONE
    // scheme decorates; the sentinel and `none` do not.
    auto expectRule = [](CSymbolDecorationScheme s) -> bool {
        switch (s) {
            case CSymbolDecorationScheme::Unspecified:       return false;
            case CSymbolDecorationScheme::None:              return false;
            case CSymbolDecorationScheme::LeadingUnderscore: return true;
        }
        return false;  // closed-enum exhaustive switch — fallthrough is dead
    };
    for (CSymbolDecorationScheme const s : kAllSchemes) {
        EXPECT_EQ(cFormatAddsLeadingUnderscore(s), expectRule(s))
            << "rule mismatch for scheme " << static_cast<unsigned>(s)
            << " (" << cSymbolDecorationSchemeName(s) << ")";
    }
}

// ── TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME): `linkNameFor`'s ──────
// ── per-target LINK BASE NAME, and its composition with the format rule ──────
//
// The field answers "which UNDECORATED base name does the shipped library
// export for this C identifier ON THIS TARGET" (Darwin's `fstat$INODE64` on
// x86_64, the plain name on arm64). It is the INPUT to the decoration, unlike
// `asmLabel` — a user's C `__asm("x")` — which REPLACES it.

// ★ THE PROPERTY THE WHOLE DESIGN RESTS ON: the OVERRIDE path and the DEFAULT
// path go through ONE decoration rule. Same format, same call, only the base
// differs — so the `_` appears on both macho arms and on neither elf arm. An
// implementation that pre-composed the underscore in config (or in the semantic
// injector) would satisfy the first EXPECT and fail the elf ones, which is
// exactly the "it works on the arch I tested" shape this cycle is closing.
TEST(FfiCMangleLinkName, OverrideAndDefaultShareOneDecorationRule) {
    // OVERRIDE path — macho decorates the declared base name.
    EXPECT_EQ(linkNameFor("fstat", /*asmLabel=*/"", CSymbolDecorationScheme::LeadingUnderscore,
                          /*linkBaseName=*/"fstat$INODE64"),
              "_fstat$INODE64");
    // DEFAULT path — SAME format, SAME function, no override.
    EXPECT_EQ(linkNameFor("fstat", "", CSymbolDecorationScheme::LeadingUnderscore, ""), "_fstat");
    // The elf arms of both, proving the `_` is the FORMAT's fact and not the
    // symbol's: an override on a format whose decoration is EMPTY composes to
    // the bare base name, with no underscore leaking across formats.
    EXPECT_EQ(linkNameFor("fstat", "", CSymbolDecorationScheme::None, "fstat$INODE64"),
              "fstat$INODE64");
    EXPECT_EQ(linkNameFor("fstat", "", CSymbolDecorationScheme::None, ""), "fstat");
    // …and pe, the third shipped decoration policy (also empty in v1).
    EXPECT_EQ(linkNameFor("f", "", CSymbolDecorationScheme::None, "f2"), "f2");
}

// PRECEDENCE, pinned rather than left to reading order: a user's explicit source
// `__asm("x")` outranks the descriptor's per-target answer for the same name,
// and it is still VERBATIM (never decorated on top). The plausible wrong
// implementation — decorating the asm label once a linkBaseName exists — would
// emit `_dss_user_label` here.
TEST(FfiCMangleLinkName, AsmLabelOutranksLinkBaseNameAndStaysVerbatim) {
    EXPECT_EQ(linkNameFor("fstat", "dss_user_label", CSymbolDecorationScheme::LeadingUnderscore,
                          "fstat$INODE64"),
              "dss_user_label");
    EXPECT_EQ(linkNameFor("fstat", "dss_user_label", CSymbolDecorationScheme::None,
                          "fstat$INODE64"),
              "dss_user_label");
}

// Empty canonical + empty label + empty base ⇒ empty: the `nameOf`
// "module-private" signal, unchanged by this cycle. A base name supplied over an
// EMPTY canonical still composes (an odd but well-defined input; the rule has no
// special case for it).
TEST(FfiCMangleLinkName, EmptyInputsPreserveThePreExistingContract) {
    EXPECT_EQ(linkNameFor("", "", CSymbolDecorationScheme::LeadingUnderscore, ""), "");
    EXPECT_EQ(linkNameFor("", "", CSymbolDecorationScheme::None, ""), "");
    EXPECT_EQ(linkNameFor("", "", CSymbolDecorationScheme::LeadingUnderscore, "base"), "_base");
}

// ★ THE FOUR-CALLER AGREEMENT, ENFORCED STRUCTURALLY RATHER THAN ASSERTED.
//
// `c_mangle.hpp` documents that `linkNameFor` exists precisely so its four
// callers — `program.cpp`'s cross-CU merge key, `compile_pipeline`'s definition
// rail, and the two `ffi::ingest` import sites — produce BYTE-IDENTICAL names.
// If one honors an override and another does not, `mir_merge`'s
// `definedNames.count(e.mangledName)` misses, the sibling-defined extern is not
// stripped, and an intra-image call is silently emitted as a dynamic import:
// green build, wrong binding, no diagnostic.
//
// The way a NEW override input breaks that is not by a caller passing the wrong
// value — it is by a caller not passing it AT ALL and inheriting a default. So
// the pin is that a three-argument call MUST NOT COMPILE. Adding `= {}` to the
// `linkBaseName` parameter turns this static_assert red, which is the
// red-on-disable for the whole invariant (MEASURED: with the default added, the
// build fails here naming this assertion).
template <typename... A>
concept CallableLinkNameFor = requires(A... a) { dss::ffi::linkNameFor(a...); };

static_assert(CallableLinkNameFor<std::string_view, std::string_view,
                                  CSymbolDecorationScheme, std::string_view>,
              "linkNameFor must accept (canonical, asmLabel, scheme, "
              "linkBaseName)");
static_assert(!CallableLinkNameFor<std::string_view, std::string_view,
                                   CSymbolDecorationScheme>,
              "linkNameFor's linkBaseName must be a REQUIRED parameter, never a "
              "defaulted one: a default lets a caller silently drop one rail's "
              "override, and the four callers' byte-for-byte agreement is what "
              "keeps mir_merge from emitting an intra-image call as a dynamic "
              "import (green build, wrong binding, no diagnostic)");

// ── D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN step C4: THE FLIP ITSELF ──────────
//
// ★★ THIS IS THE ONE TEST THAT WOULD GO RED IF C4 WERE REVERTED. Every other
// pin in this file takes a scheme as an argument and would pass just as happily
// if that scheme had been computed by a hardcoded C++ table keyed on the format
// identity — which is exactly what `kCManglingRules` was. This test closes the
// loop end to end: it LOADS A REAL SHIPPED `.format.json` FROM DISK, pulls the
// decoration rule out of the loaded schema, and drives the REAL mangler with
// it. Nothing between the JSON on disk and the emitted name is stubbed.
//
// RED LEVER (executed 2026-08-06): edit `cSymbolDecoration.scheme` in
// `macho64-arm64-darwin-exec.format.json` from `leading-underscore` to `none`
// and this test reds on the `_printf` expectation — with the C++ source
// unchanged. That is the whole point of the flip: the answer now lives in the
// descriptor, so changing the descriptor changes the compiler's behaviour.
TEST(FfiCMangle, SchemeIsReadFromTheShippedSchemaNotACppTable) {
    struct Row {
        char const* format;     // a SHIPPED format name, loaded by the real loader
        char const* expected;   // what the real mangler must then emit for `printf`
    };
    // Both families, so a rule that collapsed to one answer cannot pass.
    for (Row const row : {Row{"macho64-arm64-darwin-exec",  "_printf"},
                          Row{"macho64-x86_64-darwin-exec", "_printf"},
                          Row{"elf64-x86_64-linux-exec",    "printf"},
                          Row{"pe64-x86_64-windows-exec",   "printf"}}) {
        SCOPED_TRACE(row.format);
        auto schemaR = ObjectFormatSchema::loadShipped(row.format);
        ASSERT_TRUE(schemaR.has_value())
            << "shipped format must load: " << row.format;

        CSymbolDecorationScheme const scheme =
            (*schemaR)->cSymbolDecoration().scheme;
        ASSERT_NE(scheme, CSymbolDecorationScheme::Unspecified)
            << "a LOADED schema must never carry the sentinel — validate() "
               "requires a real scheme on every format";

        // The decoration the descriptor declares, applied by the real engine.
        EXPECT_EQ(applyCMangling("printf", scheme), row.expected);

        // And through `linkNameFor`, the single naming point all four callers
        // share, with both override channels empty (the ordinary path).
        EXPECT_EQ(linkNameFor("printf", {}, scheme, {}), row.expected);

        // The inverse recovers the canonical identifier under either scheme.
        EXPECT_EQ(unapplyCMangling(row.expected, scheme), "printf");
    }
}

// The composition C4 must not disturb: a per-target link BASE name is decorated
// by the FORMAT's declared rule, so the `_` stays the format's fact and never
// the symbol's. Driven from the shipped schemas for the same reason as above.
TEST(FfiCMangle, ShippedSchemeDecoratesADescriptorDeclaredLinkBaseName) {
    auto machoR = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    auto elfR   = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(machoR.has_value());
    ASSERT_TRUE(elfR.has_value());
    auto const macho = (*machoR)->cSymbolDecoration().scheme;
    auto const elf   = (*elfR)->cSymbolDecoration().scheme;

    // The measured Darwin case the channel exists for (TF-C121): libSystem's
    // x86_64 slice exports the 64-bit-inode `fstat` as `_fstat$INODE64`.
    EXPECT_EQ(linkNameFor("fstat", {}, macho, "fstat$INODE64"),
              "_fstat$INODE64");
    // Same declared base name under a non-decorating scheme: no `_`.
    EXPECT_EQ(linkNameFor("fstat", {}, elf, "fstat$INODE64"), "fstat$INODE64");

    // A user's source-level `__asm` still outranks both and is emitted verbatim
    // — it REPLACES the mangling rather than feeding it.
    EXPECT_EQ(linkNameFor("fstat", "myglobal", macho, "fstat$INODE64"),
              "myglobal");
}
