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

#include "ffi/mangling/c_mangle.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace dss;
using namespace dss::ffi;

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
    // original canonical name back. This is the systemic invariant
    // FF4's caller (FF5 ingestion) relies on.
    auto roundTrip = [](std::string_view canonical, ObjectFormatKind format) {
        std::string decorated = applyCMangling(canonical, format);
        return unapplyCMangling(decorated, format);
    };
    for (ObjectFormatKind k : { ObjectFormatKind::Unknown,
                                ObjectFormatKind::Elf,
                                ObjectFormatKind::Pe,
                                ObjectFormatKind::MachO,
                                ObjectFormatKind::Wasm,
                                ObjectFormatKind::Spirv }) {
        EXPECT_EQ(roundTrip("printf", k), "printf")
            << "format kind = " << static_cast<unsigned>(k);
        EXPECT_EQ(roundTrip("really_long_libc_function_name_123", k),
                  "really_long_libc_function_name_123");
    }
}
