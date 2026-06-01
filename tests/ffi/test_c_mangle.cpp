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

#include <string>

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

TEST(FfiCMangleStrict, ElfPassesThroughCleanly) {
    // No-decoration formats: strict mode is a no-op success path.
    DiagnosticReporter rep;
    auto r = unapplyCManglingStrict("printf", ObjectFormatKind::Elf, rep);
    ASSERT_TRUE(r.has_value()) << mangleErrorKindName(r.error().kind);
    EXPECT_EQ(*r, "printf");
    EXPECT_EQ(rep.errorCount(), 0u);
}

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
