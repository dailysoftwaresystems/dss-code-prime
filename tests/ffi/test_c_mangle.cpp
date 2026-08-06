// Plan 11 FF4 (C name mangling) tests — `dss::ffi::applyCMangling` /
// `unapplyCMangling` / `cFormatAddsLeadingUnderscore`.
//
// Pins:
//   * Each ObjectFormatKind variant has a deterministic decoration rule.
//   * applyCMangling + unapplyCMangling round-trip cleanly for both
//     decorated and undecorated formats.
//   * Empty input is preserved (no synthesis).
//   * Undecorated input through unapplyCMangling on a "decorated"
//     format passes through unchanged (conservative).

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

// ── Per-format decoration rule pin ─────────────────────────

TEST(FfiCMangle, ElfNoUnderscore) {
    EXPECT_FALSE(cFormatAddsLeadingUnderscore(ObjectFormatKind::Elf));
}
TEST(FfiCMangle, PeNoUnderscoreV1) {
    // PE64 ships today; the 32-bit-cdecl `_func` lands at D-FF4-1
    // when a 32-bit PE target arrives.
    EXPECT_FALSE(cFormatAddsLeadingUnderscore(ObjectFormatKind::Pe));
}
TEST(FfiCMangle, MachOAddsLeadingUnderscore) {
    // Apple convention: every C symbol gets a leading `_` on both
    // 32-bit and 64-bit Mach-O, both x86 and ARM.
    EXPECT_TRUE(cFormatAddsLeadingUnderscore(ObjectFormatKind::MachO));
}
TEST(FfiCMangle, WasmNoUnderscore) {
    EXPECT_FALSE(cFormatAddsLeadingUnderscore(ObjectFormatKind::Wasm));
}
TEST(FfiCMangle, SpirvNoUnderscore) {
    EXPECT_FALSE(cFormatAddsLeadingUnderscore(ObjectFormatKind::Spirv));
}
TEST(FfiCMangle, UnknownNoUnderscoreDefensiveDefault) {
    EXPECT_FALSE(cFormatAddsLeadingUnderscore(ObjectFormatKind::Unknown));
}

// ── applyCMangling ─────────────────────────────────────────

TEST(FfiCMangle, ApplyMachOAddsUnderscore) {
    EXPECT_EQ(applyCMangling("printf", ObjectFormatKind::MachO), "_printf");
    EXPECT_EQ(applyCMangling("malloc", ObjectFormatKind::MachO), "_malloc");
    // already-underscored identifier just gets ANOTHER underscore
    // — the function applies the format rule mechanically, not
    // smart-deduplication (operator must hand a canonical name).
    EXPECT_EQ(applyCMangling("_existing", ObjectFormatKind::MachO),
              "__existing");
}

TEST(FfiCMangle, ApplyElfPassesThrough) {
    EXPECT_EQ(applyCMangling("printf", ObjectFormatKind::Elf), "printf");
    EXPECT_EQ(applyCMangling("_explicit", ObjectFormatKind::Elf), "_explicit");
}

TEST(FfiCMangle, ApplyPePassesThroughV1) {
    EXPECT_EQ(applyCMangling("printf", ObjectFormatKind::Pe), "printf");
}

TEST(FfiCMangle, ApplyWasmPassesThrough) {
    EXPECT_EQ(applyCMangling("env.printf", ObjectFormatKind::Wasm), "env.printf");
}

TEST(FfiCMangle, ApplyEmptyInputReturnsEmpty) {
    EXPECT_EQ(applyCMangling("", ObjectFormatKind::MachO), "");
    EXPECT_EQ(applyCMangling("", ObjectFormatKind::Elf), "");
}

// ── unapplyCMangling ───────────────────────────────────────

TEST(FfiCMangle, UnapplyMachOStripsLeadingUnderscore) {
    EXPECT_EQ(unapplyCMangling("_printf", ObjectFormatKind::MachO), "printf");
    EXPECT_EQ(unapplyCMangling("_malloc", ObjectFormatKind::MachO), "malloc");
    // Only ONE underscore is stripped — `__main` (Apple's
    // entrypoint canonicalization) demangles to `_main`.
    EXPECT_EQ(unapplyCMangling("__main", ObjectFormatKind::MachO), "_main");
}

TEST(FfiCMangle, UnapplyMachOPreservesMissingUnderscore) {
    // Conservative: a Mach-O symbol WITHOUT the expected leading
    // `_` (uncommon but possible — pre-mangled libraries, system
    // helpers) returns as-is rather than fabricating an error.
    EXPECT_EQ(unapplyCMangling("printf", ObjectFormatKind::MachO), "printf");
}

TEST(FfiCMangle, UnapplyElfPassesThrough) {
    EXPECT_EQ(unapplyCMangling("printf", ObjectFormatKind::Elf), "printf");
    // Even if the input HAS a leading `_` on ELF (uncommon — would
    // be a manually-decorated symbol), unapply does NOT strip it
    // because the format rule says ELF has no decoration.
    EXPECT_EQ(unapplyCMangling("_explicit", ObjectFormatKind::Elf), "_explicit");
}

TEST(FfiCMangle, UnapplyEmptyInputReturnsEmpty) {
    EXPECT_EQ(unapplyCMangling("", ObjectFormatKind::MachO), "");
    EXPECT_EQ(unapplyCMangling("", ObjectFormatKind::Elf), "");
}

// ── Round-trip on every variant ────────────────────────────

TEST(FfiCMangle, ApplyUnapplyRoundTripPreservesCanonicalForm) {
    // For every format-kind variant, apply→unapply must yield the
    // original canonical name back. Iteration is driven by
    // `kObjectFormatKindTable` so a future ObjectFormatKind variant
    // is exercised automatically (post-fold #2 test-analyzer P7).
    auto roundTrip = [](std::string_view canonical, ObjectFormatKind format) {
        std::string decorated = applyCMangling(canonical, format);
        return unapplyCMangling(decorated, format);
    };
    for (auto const& row : kObjectFormatKindTable.rows) {
        ObjectFormatKind const k = row.first;
        EXPECT_EQ(roundTrip("printf", k), "printf")
            << "format kind = " << static_cast<unsigned>(k);
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
    auto r = unapplyCManglingStrict("_printf", ObjectFormatKind::MachO, rep);
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
    auto r = unapplyCManglingStrict("printf", ObjectFormatKind::MachO, rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, MangleErrorKind::MissingExpectedPrefix);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_MangleMissingExpectedPrefix), 1u);
    // Detail must name the offending input so a log grep can locate it.
    EXPECT_NE(r.error().detail.find("printf"), std::string::npos);
}

TEST(FfiCMangleStrict, EmptyInputReturnsEmptySuccess) {
    DiagnosticReporter rep;
    auto r = unapplyCManglingStrict("", ObjectFormatKind::MachO, rep);
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
    for (auto const& row : kObjectFormatKindTable.rows) {
        ObjectFormatKind const k = row.first;
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

TEST(FfiCMangle, AllFormatKindsHaveExplicitRulePin) {
    // Enum-driven pin: a new ObjectFormatKind variant added without
    // a matching kCManglingRules row would be caught by the impl-
    // side static_assert at compile time, but the per-variant rule
    // semantics also need a test pin so a maintainer who updates
    // the table can't silently regress the decoration policy.
    // Iteration over kObjectFormatKindTable.rows means new variants
    // are automatically exercised (post-fold #2 test-analyzer P6).
    auto expectRule = [](ObjectFormatKind k) -> bool {
        switch (k) {
            case ObjectFormatKind::Unknown: return false;
            case ObjectFormatKind::Elf:     return false;
            case ObjectFormatKind::Pe:      return false;  // v1 PE64-only
            case ObjectFormatKind::MachO:   return true;
            case ObjectFormatKind::Wasm:    return false;
            case ObjectFormatKind::Spirv:   return false;
        }
        return false;  // closed-enum exhaustive switch — fallthrough is dead
    };
    for (auto const& row : kObjectFormatKindTable.rows) {
        ObjectFormatKind const k = row.first;
        EXPECT_EQ(cFormatAddsLeadingUnderscore(k), expectRule(k))
            << "rule mismatch for format kind " << static_cast<unsigned>(k)
            << " (" << row.second << ")";
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
    EXPECT_EQ(linkNameFor("fstat", /*asmLabel=*/"", ObjectFormatKind::MachO,
                          /*linkBaseName=*/"fstat$INODE64"),
              "_fstat$INODE64");
    // DEFAULT path — SAME format, SAME function, no override.
    EXPECT_EQ(linkNameFor("fstat", "", ObjectFormatKind::MachO, ""), "_fstat");
    // The elf arms of both, proving the `_` is the FORMAT's fact and not the
    // symbol's: an override on a format whose decoration is EMPTY composes to
    // the bare base name, with no underscore leaking across formats.
    EXPECT_EQ(linkNameFor("fstat", "", ObjectFormatKind::Elf, "fstat$INODE64"),
              "fstat$INODE64");
    EXPECT_EQ(linkNameFor("fstat", "", ObjectFormatKind::Elf, ""), "fstat");
    // …and pe, the third shipped decoration policy (also empty in v1).
    EXPECT_EQ(linkNameFor("f", "", ObjectFormatKind::Pe, "f2"), "f2");
}

// PRECEDENCE, pinned rather than left to reading order: a user's explicit source
// `__asm("x")` outranks the descriptor's per-target answer for the same name,
// and it is still VERBATIM (never decorated on top). The plausible wrong
// implementation — decorating the asm label once a linkBaseName exists — would
// emit `_dss_user_label` here.
TEST(FfiCMangleLinkName, AsmLabelOutranksLinkBaseNameAndStaysVerbatim) {
    EXPECT_EQ(linkNameFor("fstat", "dss_user_label", ObjectFormatKind::MachO,
                          "fstat$INODE64"),
              "dss_user_label");
    EXPECT_EQ(linkNameFor("fstat", "dss_user_label", ObjectFormatKind::Elf,
                          "fstat$INODE64"),
              "dss_user_label");
}

// Empty canonical + empty label + empty base ⇒ empty: the `nameOf`
// "module-private" signal, unchanged by this cycle. A base name supplied over an
// EMPTY canonical still composes (an odd but well-defined input; the rule has no
// special case for it).
TEST(FfiCMangleLinkName, EmptyInputsPreserveThePreExistingContract) {
    EXPECT_EQ(linkNameFor("", "", ObjectFormatKind::MachO, ""), "");
    EXPECT_EQ(linkNameFor("", "", ObjectFormatKind::Elf, ""), "");
    EXPECT_EQ(linkNameFor("", "", ObjectFormatKind::MachO, "base"), "_base");
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
                                  ObjectFormatKind, std::string_view>,
              "linkNameFor must accept (canonical, asmLabel, format, "
              "linkBaseName)");
static_assert(!CallableLinkNameFor<std::string_view, std::string_view,
                                   ObjectFormatKind>,
              "linkNameFor's linkBaseName must be a REQUIRED parameter, never a "
              "defaulted one: a default lets a caller silently drop one rail's "
              "override, and the four callers' byte-for-byte agreement is what "
              "keeps mir_merge from emitting an intra-image call as a dynamic "
              "import (green build, wrong binding, no diagnostic)");
