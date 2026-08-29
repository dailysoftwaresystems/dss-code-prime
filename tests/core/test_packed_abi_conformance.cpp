// D-CSUBSET-PACKED — the CI cross-compile-COMPARE conformance witness for packed
// struct layout (the twin of test_bitfield_abi_conformance.cpp).
//
// The hermetic half of the witness lives in `test_type_layout.cpp` (it pins
// `computeLayout(packed)` to hand-reasoned goldens). THIS file is the load-bearing
// other half: it compiles the SAME packed-struct battery with the HOST's NATIVE C
// compiler (cl.exe via `#pragma pack(1)` on Windows; gcc/clang/cc via
// `__attribute__((packed))` on Linux/macOS), reads back the compiler's real
// `sizeof` + per-field `offsetof`, and asserts dss's `computeLayout(packed)` is
// BYTE-IDENTICAL. So the goldens can never silently drift from the real ABI — a
// packed struct that DSS lays out wrong makes this test go red on every CI leg.
//
// The battery is restricted to structs where GNU `__attribute__((packed))` and MSVC
// `#pragma pack(1)` AGREE (no bit-fields — packed + bit-field is fail-loud, and no
// over-aligned members): both mean "remove ALL inter-field padding, alignment 1",
// which is exactly what DSS models.
//
// SKIPS ONLY WHEN THE TOOL IS ABSENT. TF-C97 fixed this file's run-step shell quoting
// but left the SECOND half of the defect in place: a compiler that was present and
// then failed to build, launch or answer still collapsed into the same `nullopt` as
// "no compiler installed", and the test skipped green either way. The quoting fix is
// what made that branch unreachable HERE — not anything that made it safe. It is now
// a fail-loud assert, and the probe machinery is shared (`native_c_probe.hpp`) so the
// bit-field twin cannot drift away from it again.
//
// D-TEST-NATIVE-ORACLE-INERT-ON-POSIX — a native oracle that skips on error is a broken oracle that reports success.

#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include "native_c_probe.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace native_probe = dss::test_support::native_probe;
using namespace dss;

namespace {

// One packed probe struct: a name + its field types. The C source and the interner
// build both derive from `fieldTypes` (so they cannot silently disagree).
struct PackedProbe {
    std::string           name;
    std::vector<TypeKind> fieldTypes;
};

// The battery — a spread of alignment mixes where padding would otherwise appear.
std::vector<PackedProbe> battery() {
    using K = TypeKind;
    return {
        {"A", {K::Char, K::U32}},                       // char + int → size 5
        {"B", {K::Char, K::F64, K::Char}},              // char + double + char → size 10
        {"C", {K::U16, K::U32, K::Char}},               // short + int + char → size 7
        {"D", {K::Char, K::U16, K::Char, K::U32}},      // mixed → size 8
        {"E", {K::U32, K::Char}},                       // int + char → size 5
        {"F", {K::Char, K::Char, K::U64}},              // 2 char + long long → size 10
    };
}

// The C type spelling of a probe field TypeKind (kept in lockstep with the interner
// primitive built below).
[[nodiscard]] char const* cTypeName(TypeKind k) {
    switch (k) {
        case TypeKind::Char: return "char";
        case TypeKind::U8:   return "unsigned char";
        case TypeKind::U16:  return "unsigned short";
        case TypeKind::U32:  return "unsigned";
        case TypeKind::U64:  return "unsigned long long";
        case TypeKind::I16:  return "short";
        case TypeKind::I32:  return "int";
        case TypeKind::I64:  return "long long";
        case TypeKind::F32:  return "float";
        case TypeKind::F64:  return "double";
        default:             return "int";
    }
}

// The native compiler's measured layout of one packed struct: total size + the
// byte offset of each field, keyed by field index.
struct NativeLayout {
    std::uint64_t              size = 0;
    std::vector<std::uint64_t> offsets;
};

// Locating the host compiler, spelling the run-step redirect for the host shell, and
// building/launching/reading back the probe now live in `native_c_probe.hpp` — shared
// verbatim with `test_bitfield_abi_conformance.cpp`. They used to be two copies, and
// that duplication is exactly what let the TF-C97 quoting fix land on this file only.

// The number of field offsets this battery expects the oracle to compare. DERIVED
// from the battery so the inert-oracle floor cannot drift away from the data it
// guards; a hand-typed count is a floor that silently stops being one.
[[nodiscard]] std::size_t probedFieldCount(std::vector<PackedProbe> const& bs) {
    std::size_t n = 0;
    for (auto const& b : bs) n += b.fieldTypes.size();
    return n;
}

// Emit the probe C program: declares each packed struct (host-specific packed
// syntax) and prints "<NAME> size=<n>" then "<NAME>.<i> off=<o>" per field.
void writeProbeSource(fs::path const& src, std::vector<PackedProbe> const& bs,
                      bool msvc) {
    std::ofstream o{src};
    o << "#include <stdio.h>\n#include <stddef.h>\n";
    for (auto const& b : bs) {
        if (msvc) o << "#pragma pack(push,1)\n";
        o << "struct " << b.name << " {";
        for (std::size_t i = 0; i < b.fieldTypes.size(); ++i)
            o << " " << cTypeName(b.fieldTypes[i]) << " f" << i << ";";
        o << " }";
        if (!msvc) o << " __attribute__((packed))";
        o << ";\n";
        if (msvc) o << "#pragma pack(pop)\n";
    }
    o << "int main(void){\n";
    for (auto const& b : bs) {
        o << "  printf(\"" << b.name << " size=%lu\\n\", (unsigned long)sizeof(struct "
          << b.name << "));\n";
        for (std::size_t i = 0; i < b.fieldTypes.size(); ++i)
            o << "  printf(\"" << b.name << "." << i << " off=%lu\\n\", "
              << "(unsigned long)offsetof(struct " << b.name << ", f" << i << "));\n";
    }
    o << "  return 0;\n}\n";
}

// Parse the probe's captured stdout into per-struct layouts. PURE PARSING — whether
// the probe ran at all is decided upstream by `runNativeCProbe`'s status, never by
// this returning an empty map.
std::map<std::string, NativeLayout>
parseNative(std::vector<PackedProbe> const& bs,
            std::vector<std::string> const& lines) {
    std::map<std::string, NativeLayout> result;
    for (auto const& b : bs) result[b.name].offsets.assign(b.fieldTypes.size(), 0);

    for (auto const& line : lines) {
        std::istringstream ls{line};
        std::string tok;
        ls >> tok;
        auto const dot = tok.find('.');
        if (dot == std::string::npos) {
            std::string sz; ls >> sz;
            auto const eq = sz.find('=');
            if (eq == std::string::npos) continue;
            auto it = result.find(tok);
            if (it == result.end()) continue;
            it->second.size = std::stoull(sz.substr(eq + 1));
        } else {
            std::string const nm = tok.substr(0, dot);
            std::size_t const idx = std::stoul(tok.substr(dot + 1));
            std::string offT; ls >> offT;
            auto const oe = offT.find('=');
            if (oe == std::string::npos) continue;
            auto it = result.find(nm);
            if (it == result.end() || idx >= it->second.offsets.size()) continue;
            it->second.offsets[idx] = std::stoull(offT.substr(oe + 1));
        }
    }
    return result;
}

[[nodiscard]] TypeId internPacked(TypeInterner& ti, PackedProbe const& b,
                                  std::uint64_t key) {
    std::vector<TypeId> fields;
    fields.reserve(b.fieldTypes.size());
    for (auto k : b.fieldTypes) fields.push_back(ti.primitive(k));
    TypeId const s = ti.forwardComposite(TypeKind::Struct, b.name, key);
    ti.completeComposite(s, fields, /*packed=*/true);
    return s;
}

// ── TF-C97 (D-CSUBSET-PACKED-BITFIELD-INTERACTION): the `#pragma pack(N)` +
// BIT-FIELD half of the witness ──
//
// The battery above deliberately excludes bit-fields, and that exclusion is exactly
// where the engine had a hole: `#pragma pack(N)` was read ONLY on the non-bit-field
// path, so a struct that CONTAINED a bit-field silently ignored the cap. It was a
// pure-silence defect — no diagnostic, no crash, just a struct laid out to the wrong
// ABI (MEASURED before the fix: `pack(4) { u64 a; unsigned b:1; }` came out 16/8
// where clang says 12/4).
//
// This probe pins `_Alignof` ALONGSIDE `sizeof`, which the battery above does not.
// Alignment is the half a `_Static_assert(sizeof …)` in a header never checks, and it
// is ABI-visible the moment the struct lands in an array or another struct.
//
// The four bit-field rows below (Q1/Q3/Q4/Q5) were wrong in BOTH size and alignment
// before the fix, so `sizeof` alone would have caught them. Pinning alignment anyway
// is not belt-and-braces: the real-world instances are alignment-ONLY. The macOS
// SDK's `mach/message.h` `pack(4)` descriptors are 16 bytes with the cap and 16
// without — identical `sizeof`, `_Alignof` 4 vs 8 — so a size-only witness passes
// them while the ABI is still wrong. `struct C` in
// examples/c/packed_bitfield_align makes the same point end to end.
struct PackBitProbe {
    std::string               name;
    std::string               cDecl;       // declared inside a #pragma pack(N) region
    std::uint32_t             packN = 0;   // the cap; 0 = no `#pragma pack` at all
    std::vector<TypeKind>     fieldTypes;
    std::vector<std::int64_t> widths;      // kNotBitfield = ordinary field
};

// The battery. Every expectation is DERIVED FROM THE HOST COMPILER at run time —
// nothing here is hand-reasoned; the comments record what `/usr/bin/clang -arch
// arm64` measured when the case was added, purely so a reader can see the intent.
//
// The two `_plain` entries are the CONTROLS: same pack region, same field types, NO
// bit-field. They exercise the cap's non-bit-field path, which was already correct,
// so they must stay green before AND after the fix. That is what distinguishes "the
// cap broke generally" from "the cap broke only where a bit-field is present" — a
// regression that reds the whole battery is a different bug from one that reds only
// the bit-field rows.
std::vector<PackBitProbe> packBitBattery() {
    using K = TypeKind;
    std::int64_t const O = kNotBitfield;
    return {
        // cap 4 vs a natural-8 member + a bit-field: 12/4 (was 16/8)
        {"Q1", "struct Q1 { unsigned long long a; unsigned b:1; };", 4,
         {K::U64, K::U32}, {O, 1}},
        // CONTROL — same shape, no bit-field: 12/4 before and after
        {"Q2", "struct Q2 { unsigned long long a; unsigned b; };", 4,
         {K::U64, K::U32}, {O, O}},
        // a capped FP member + two bit-fields sharing one unit: 12/4 (was 16/8)
        {"Q3", "struct Q3 { double d; unsigned b:3; unsigned c:3; };", 4,
         {K::F64, K::U32, K::U32}, {O, 3, 3}},
        // bit-field FIRST, then a capped natural-8 ordinary field: 12/4 (was 16/8).
        // This is the row that pins the ORDINARY field's capped offset (clang: 4).
        {"Q4", "struct Q4 { unsigned b:3; unsigned long long a; };", 4,
         {K::U32, K::U64}, {3, O}},
        // cap 1 — the strongest cap: 3/1 (was 4/4)
        {"Q5", "struct Q5 { char c; unsigned b:9; };", 1, {K::Char, K::U32}, {O, 9}},
        // CONTROL — same shape, no bit-field: 5/1 before and after
        {"Q6", "struct Q6 { char c; unsigned b; };", 1, {K::Char, K::U32}, {O, O}},
        // a cap WEAKER than every natural align here (16 >= 4) must be inert: same
        // 8/4 as with no pragma at all. Guards the "a cap only ever LOWERS" half of
        // `clampedBaselineAlign` — a clamp written as an unconditional assignment
        // rather than a min would red this row.
        {"Q7", "struct Q7 { unsigned a; unsigned b:4; };", 16, {K::U32, K::U32}, {O, 4}},
    };
}

// Emit the pack+bit-field probe: each struct inside its own `#pragma pack(N)` region
// (or none when packN == 0), then print `<NAME> size=<n> align=<a>`.
void writePackBitProbeSource(fs::path const& src,
                             std::vector<PackBitProbe> const& bs) {
    std::ofstream o{src};
    o << "#include <stdio.h>\n#include <stddef.h>\n";
    for (auto const& b : bs) {
        if (b.packN != 0) o << "#pragma pack(push," << b.packN << ")\n";
        o << b.cDecl << "\n";
        if (b.packN != 0) o << "#pragma pack(pop)\n";
    }
    o << "int main(void){\n";
    for (auto const& b : bs)
        o << "  printf(\"" << b.name << " size=%lu align=%lu\\n\", "
          << "(unsigned long)sizeof(struct " << b.name << "), "
          << "(unsigned long)_Alignof(struct " << b.name << "));\n";
    o << "  return 0;\n}\n";
}

// The native compiler's measured size + alignment for one probe struct.
struct NativeSizeAlign {
    std::uint64_t size  = 0;
    std::uint64_t align = 0;
};

// Parse the pack+bit-field probe's captured stdout. PURE PARSING — see `parseNative`.
std::map<std::string, NativeSizeAlign>
parsePackBitNative(std::vector<std::string> const& lines) {
    std::map<std::string, NativeSizeAlign> result;
    for (auto const& line : lines) {
        std::istringstream ls{line};
        std::string name, szT, alT;
        if (!(ls >> name >> szT >> alT)) continue;
        auto const se = szT.find('=');
        auto const ae = alT.find('=');
        if (se == std::string::npos || ae == std::string::npos) continue;
        result[name] = NativeSizeAlign{std::stoull(szT.substr(se + 1)),
                                       std::stoull(alT.substr(ae + 1))};
    }
    return result;
}

// Intern a probe as a struct carrying the `#pragma pack(N)` cap (`maxFieldAlign`) and
// its per-field bit widths — the two channels whose INTERACTION this file witnesses.
[[nodiscard]] TypeId internPackBit(TypeInterner& ti, PackBitProbe const& b,
                                   std::uint64_t key) {
    std::vector<TypeId> fields;
    fields.reserve(b.fieldTypes.size());
    for (auto k : b.fieldTypes) fields.push_back(ti.primitive(k));
    TypeId const s = ti.forwardComposite(TypeKind::Struct, b.name, key);
    ti.completeComposite(s, fields, /*packed=*/false, b.widths,
                         /*fieldOffsets=*/{}, /*fieldAligns=*/{},
                         /*explicitAlign=*/0, /*maxFieldAlign=*/b.packN);
    return s;
}

} // namespace

// THE cross-compile-compare witness. For every packed struct in the battery, dss's
// computeLayout(packed) MUST equal the native compiler's sizeof + per-field offset.
// Skips cleanly when no toolchain (the hermetic goldens in test_type_layout still
// pin the layout; this CI leg re-derives them where a compiler is present).
TEST(PackedAbiConformance, DssPackedLayoutMatchesNativeCompiler) {
    auto const bs = battery();
    auto const probe = native_probe::runNativeCProbe(
        "packed-abi", [&bs](fs::path const& src, native_probe::Toolchain tc) {
            writeProbeSource(src, bs, tc == native_probe::Toolchain::Msvc);
        });

    // The ONE legitimate skip: no toolchain on this machine. `probe.detail` says WHICH
    // shape of absent (nothing on PATH, or a shim that cannot build a trivial program).
    if (probe.toolAbsent()) {
        GTEST_SKIP() << "no usable native C compiler on this host (" << probe.detail
                     << ") — the hermetic packed goldens in test_type_layout still pin "
                        "the layout; this CI leg re-derives them where a toolchain is "
                        "present.";
    }
    // Every other outcome is a harness defect, not a missing tool. Fail loud.
    ASSERT_TRUE(probe.ok()) << probe.describe();

    auto const native = parseNative(bs, probe.lines);

    // Packed layout is data-model-independent (all scalar widths are fixed by their
    // TypeKind; no pointers in the battery), so Lp64 params suffice on every host.
    AggregateLayoutParams const params{ScalarAlignmentRule::Natural, 16};

    // The inert-oracle guard — below every early exit so a legitimate skip is not
    // turned into a red. Floors derived from the battery, never typed in.
    native_probe::ExecutedRows sizeRows{"packed sizeof vs native", bs.size()};
    native_probe::ExecutedRows offsetRows{"packed field offset vs native",
                                          probedFieldCount(bs)};

    TypeInterner ti{CompilationUnitId{1}};
    std::uint64_t key = 100;
    for (auto const& b : bs) {
        auto const itN = native.find(b.name);
        ASSERT_NE(itN, native.end()) << b.name;
        NativeLayout const& nl = itN->second;

        TypeId const s = internPacked(ti, b, key++);
        auto const l = computeLayout(s, ti, params, DataModel::Lp64);
        ASSERT_TRUE(l.has_value())
            << "dss failed to lay out packed struct " << b.name;

        EXPECT_EQ(l->size, nl.size)
            << "packed struct " << b.name << ": dss sizeof " << l->size
            << " != native " << nl.size;
        sizeRows.record();

        ASSERT_EQ(l->fieldOffsets.size(), nl.offsets.size()) << b.name;
        for (std::size_t i = 0; i < nl.offsets.size(); ++i) {
            EXPECT_EQ(l->fieldOffsets[i], nl.offsets[i])
                << "packed struct " << b.name << " field " << i
                << ": dss offset " << l->fieldOffsets[i]
                << " != native " << nl.offsets[i];
            offsetRows.record();
        }
    }
}

// TF-C97 (D-CSUBSET-PACKED-BITFIELD-INTERACTION). The cross-compile-compare witness
// for `#pragma pack(N)` on a struct that CONTAINS a bit-field — size AND `_Alignof`.
// Every row was mis-laid before the fix; the two `_plain` controls were already
// correct and must stay correct (see `packBitBattery`).
//
// GNU-ABI hosts only. `#pragma pack` + bit-fields is precisely where GnuPacked and
// MsvcStraddle DIVERGE, and this repo has no cl.exe-measured ground truth for the
// MSVC side (the MsvcStraddle cap is implemented from MSVC's documented `pack(n)`
// semantics, not from a measurement) — so rather than pin an unverified expectation,
// the MSVC leg skips loudly. Pinning a guess is how a wrong layout becomes a golden.
TEST(PackedAbiConformance, DssPackBitfieldLayoutMatchesNativeCompiler) {
#if defined(_WIN32)
    GTEST_SKIP() << "MSVC host: `#pragma pack` + bit-fields is the point where "
                    "GnuPacked and MsvcStraddle diverge, and the MsvcStraddle cap "
                    "has no cl.exe-measured ground truth in this repo yet. Skipped "
                    "rather than pinned to a guess.";
#else
    auto const bs = packBitBattery();
    auto const probe = native_probe::runNativeCProbe(
        "pack-bitfield-abi", [&bs](fs::path const& src, native_probe::Toolchain) {
            writePackBitProbeSource(src, bs);
        });

    // The ONE legitimate skip: no toolchain on this machine (see the twin above for
    // why the detail is echoed).
    if (probe.toolAbsent()) {
        GTEST_SKIP() << "no usable native C compiler on this host (" << probe.detail
                     << ") — this leg re-derives the pack+bit-field layout wherever a "
                        "toolchain is present.";
    }
    // Every other outcome is a harness defect, not a missing tool. Fail loud.
    ASSERT_TRUE(probe.ok()) << probe.describe();

    auto const native = parsePackBitNative(probe.lines);

    // GnuPacked: the strategy every GNU-ABI host (Linux/macOS, gcc/clang) uses, which
    // is the ABI the probe was just compiled under.
    AggregateLayoutParams const params{ScalarAlignmentRule::Natural, 16,
                                       BitFieldStrategy::GnuPacked};

    // The inert-oracle guard. Each row contributes a sizeof AND an _Alignof
    // comparison, so the floor is the battery size for each ledger.
    native_probe::ExecutedRows sizeRows{"pack+bit-field sizeof vs native", bs.size()};
    native_probe::ExecutedRows alignRows{"pack+bit-field _Alignof vs native", bs.size()};

    TypeInterner ti{CompilationUnitId{1}};
    std::uint64_t key = 900;
    for (auto const& b : bs) {
        ASSERT_EQ(b.fieldTypes.size(), b.widths.size())
            << b.name << ": probe field/width arity disagreement";
        auto const itN = native.find(b.name);
        ASSERT_NE(itN, native.end()) << b.name;
        NativeSizeAlign const& nl = itN->second;
        ASSERT_GT(nl.size, 0u) << b.name << ": native probe produced no size";

        TypeId const s = internPackBit(ti, b, key++);
        auto const l = computeLayout(s, ti, params, DataModel::Lp64);
        ASSERT_TRUE(l.has_value())
            << "dss failed to lay out `#pragma pack(" << b.packN << ")` struct "
            << b.name << " — " << b.cDecl;

        EXPECT_EQ(l->size, nl.size)
            << "pack(" << b.packN << ") " << b.cDecl << ": dss sizeof " << l->size
            << " != native " << nl.size;
        sizeRows.record();

        EXPECT_EQ(l->align.bytes(), nl.align)
            << "pack(" << b.packN << ") " << b.cDecl << ": dss _Alignof "
            << l->align.bytes() << " != native " << nl.align;
        alignRows.record();
    }
#endif
}

// D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT. The zero-width battery, and the mirror
// image of the twin above: that one runs on GNU-ABI hosts and skips on Windows,
// this one runs on Windows and skips on GNU-ABI hosts. The asymmetry is not
// tidiness — it is WHICH HALF OF THIS ROW HAS A SETTLED ANSWER.
//
// `msvc_straddle` has one reference (cl.exe) for every PE format, and it answers the
// same way on x64 and arm64 (MEASURED via clang's `-target {x86_64,aarch64}-pc-
// windows-msvc` record dumps, then CONFIRMED exactly by cl.exe 19.51). So the MSVC
// half is decidable and is what this battery pins.
//
// `gnu_packed` does NOT have one answer. The formats that select it DISAGREE, and
// gcc 13.3.0 and clang 18.1.3 agree with each other per target rather than with each
// other across targets (MEASURED, `{char c; unsigned :0; char d;}`):
//     elf64-x86_64-linux ....... 5 / 1      macho64-{arm64,x86_64}-darwin .. 5 / 1
//     elf64-aarch64-linux ...... 8 / 4      (arm-linux-gnueabihf also 8 / 4)
// dss currently computes 8/4 for ALL of them — right on the aarch64 ELF formats,
// a silent miscompile on the x86_64 ELF and every Mach-O one. Closing that half needs
// a per-ABI layout key on the format documents (the same place `bitFieldStrategy`
// lives), NOT a change to this packer, so running this battery on a GNU host today
// would red on a gap that is filed, measured and out of this file's reach.
// ⚠ THE SKIP BELOW IS THEREFORE A STATED GAP, NOT A MISSING TOOLCHAIN, and its
// message must keep saying so — a skip that reads like "no compiler here" is exactly
// how a live miscompile goes quiet.
std::vector<PackBitProbe> zeroWidthBattery() {
    using K = TypeKind;
    std::int64_t const O = kNotBitfield;
    return {
        // No unit open ⇒ the zero-width field is INERT. cl.exe 2/1 (dss was 8/4) —
        // THE row the defect got wrong, and it needs no `#pragma pack` to reproduce.
        {"W1", "struct W1 { char c; unsigned :0; char d; };", 0,
         {K::Char, K::U32, K::Char}, {O, 0, O}},
        // Trailing, no unit open: cl.exe 1/1 (dss was 4/4).
        {"W2", "struct W2 { char c; unsigned :0; };", 0, {K::Char, K::U32}, {O, 0}},
        // A WIDER inert zero-width is still inert: cl.exe 2/1 (dss was 16/8).
        {"W3", "struct W3 { char c; unsigned long long :0; char d; };", 0,
         {K::Char, K::U64, K::Char}, {O, 0, O}},
        // Unit OPEN ⇒ the zero-width field terminates it: cl.exe 8/4 (already right).
        {"W4", "struct W4 { unsigned a:1; unsigned :0; unsigned b:1; };", 0,
         {K::U32, K::U32, K::U32}, {1, 0, 1}},
        // THE DISCRIMINATOR. Only a rule that BOTH folds the terminating type's
        // alignment AND bumps the high-water to it reaches 16/8; "close the unit and
        // do nothing" gives 8/4. cl.exe 16/8.
        {"W5", "struct W5 { unsigned a:1; unsigned long long :0; unsigned b:1; };", 0,
         {K::U32, K::U64, K::U32}, {1, 0, 1}},
        // Leading zero-width, nothing open yet: cl.exe 1/1 (dss was 4/4).
        {"W6", "struct W6 { unsigned :0; char c; };", 0, {K::U32, K::Char}, {0, O}},
        {"W7", "struct W7 { char c; unsigned short :0; char d; };", 0,
         {K::Char, K::U16, K::Char}, {O, 0, O}},
        // Trailing zero-width AFTER an open unit still sizes that unit: cl.exe 4/4.
        {"W8", "struct W8 { unsigned a:3; unsigned :0; };", 0,
         {K::U32, K::U32}, {3, 0}},
        // CONTROL — `pack(1)` was already correct BY ACCIDENT (the cap clamped the
        // folded alignment to 1), which is how the TF-C97 pack battery missed this
        // row entirely. It must not move. cl.exe 8/1.
        {"W9", "struct W9 { unsigned a:1; unsigned :0; unsigned b:1; };", 1,
         {K::U32, K::U32, K::U32}, {1, 0, 1}},
        // …and the caps that were NOT correct by accident. cl.exe 2/1 for both
        // (dss was 4/2 and 8/4).
        {"W10", "struct W10 { char c; unsigned :0; char d; };", 2,
         {K::Char, K::U32, K::Char}, {O, 0, O}},
        {"W11", "struct W11 { char c; unsigned long long :0; char d; };", 4,
         {K::Char, K::U64, K::Char}, {O, 0, O}},
        // CONTROLS — no zero-width anywhere. Green before AND after; a regression
        // that reds these is a different bug from one that reds only the W rows.
        {"W12", "struct W12 { char c; char d; };", 0, {K::Char, K::Char}, {O, O}},
        {"W13", "struct W13 { unsigned a:1; unsigned b:1; };", 0,
         {K::U32, K::U32}, {1, 1}},
    };
}

// The ONE row that is not portable, and the reason is a MEASURED engine limitation
// rather than an ABI fact. `{char c; unsigned :3; char d;}` is an UNNAMED bit-field
// WITH storage, so the same per-ABI axis governs it: cl.exe 12/4, aarch64-linux 4/4,
// but x86_64-linux and Apple arm64 say 3/1 — where dss says 4/4, because
// `TypeInterner` stores a per-field WIDTH and no per-field NAME and this packer
// therefore cannot tell `unsigned :3` from `unsigned x:3`. Zero width needs no such
// channel (a NAMED zero-width bit-field is a hard error, so width 0 is unnamed by
// construction), which is why every other row above IS portable.
// ⇒ Kept as a Windows-only CONTROL: on MSVC named and unnamed agree, so it pins that
// the zero-width inertness did NOT leak into fields with storage. It is deliberately
// absent from the `ignored` hosts rather than skipped there, because a row that
// silently self-disables on the platform where it would fail is the shape this file
// exists to avoid. See D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT.
std::vector<PackBitProbe> zeroWidthMsvcOnlyBattery() {
    using K = TypeKind;
    std::int64_t const O = kNotBitfield;
    return {
        {"W14", "struct W14 { char c; unsigned :3; char d; };", 0,
         {K::Char, K::U32, K::Char}, {O, 3, O}},
    };
}

// ★ THIS BATTERY RUNS ON EVERY HOST. The earlier revision of this cycle skipped it on
// GNU-ABI hosts because the `gnu_packed` half of the row was still open; that half is
// now closed by the `unnamedBitFieldAlignment` format key, so the skip is RETIRED
// rather than left standing. A skip whose stated reason has been fixed is a test that
// has quietly stopped testing.
//
// The host's own ABI selects BOTH axes, the way `test_bitfield_abi_conformance`
// selects the strategy — the probe below is compiled by THIS machine's compiler, so
// the expectations must be read under THIS machine's rules. ✔MEASURED per host family
// (gcc 13.3.0, clang 18.1.3, Apple clang 21.0.0, cl.exe 19.51):
//   Windows/PE ......... msvc_straddle; the axis is not consulted by that packer
//   Apple (any arch) ... gnu_packed + ignored   (an APPLE divergence from AAPCS64)
//   aarch64 / arm ELF .. gnu_packed + contributes
//   everything else .... gnu_packed + ignored   (x86_64 SysV, riscv64, ppc64le)
TEST(PackedAbiConformance, DssZeroWidthBitfieldLayoutMatchesNativeCompiler) {
    auto bs = zeroWidthBattery();
#if defined(_WIN32)
    for (auto const& r : zeroWidthMsvcOnlyBattery()) bs.push_back(r);
#endif
    auto const probe = native_probe::runNativeCProbe(
        "zero-width-bitfield-abi", [&bs](fs::path const& src, native_probe::Toolchain) {
            writePackBitProbeSource(src, bs);
        });

    // The ONE legitimate skip: no toolchain on this machine (see the twins above).
    if (probe.toolAbsent()) {
        GTEST_SKIP() << "no usable native C compiler on this host (" << probe.detail
                     << ") — the hermetic MsvcStraddle goldens in test_type_layout "
                        "still pin this layout; this leg re-derives them from cl.exe "
                        "where a toolchain is present.";
    }
    ASSERT_TRUE(probe.ok()) << probe.describe();
    // The dss-side rules and the reference must describe the SAME ABI. A host whose
    // toolchain does not match the arm selected below would be a wrong GREEN as
    // easily as a wrong red, so assert the pairing rather than assume it.
#if defined(_WIN32)
    ASSERT_EQ(probe.toolchain, native_probe::Toolchain::Msvc)
        << "the Windows arm pins the MSVC bit-field ABI and must be measured with "
           "cl.exe, not with whatever else answered on PATH";
#else
    ASSERT_EQ(probe.toolchain, native_probe::Toolchain::Unix)
        << "the GNU arm pins the SysV/AAPCS bit-field ABI and must be measured with "
           "cc/clang/gcc";
#endif

    auto const native = parsePackBitNative(probe.lines);

#if defined(_WIN32)
    AggregateLayoutParams const params{ScalarAlignmentRule::Natural, 16,
                                       BitFieldStrategy::MsvcStraddle};
#elif defined(__APPLE__)
    // Apple DIVERGES from generic AAPCS64 on this axis — the claim this repo's own
    // format `$comment`s asserted the opposite of until P45. ✔MEASURED on the physical
    // macOS host, Apple clang 21.0.0: `{char c; unsigned :0; char d;}` is 5/1.
    AggregateLayoutParams const params{ScalarAlignmentRule::Natural, 16,
                                       BitFieldStrategy::GnuPacked,
                                       UnnamedBitFieldAlignment::Ignored};
#elif defined(__aarch64__) || defined(__arm__)
    AggregateLayoutParams const params{ScalarAlignmentRule::Natural, 16,
                                       BitFieldStrategy::GnuPacked,
                                       UnnamedBitFieldAlignment::Contributes};
#else
    AggregateLayoutParams const params{ScalarAlignmentRule::Natural, 16,
                                       BitFieldStrategy::GnuPacked,
                                       UnnamedBitFieldAlignment::Ignored};
#endif

    native_probe::ExecutedRows sizeRows{"zero-width sizeof vs native", bs.size()};
    native_probe::ExecutedRows alignRows{"zero-width _Alignof vs native", bs.size()};

    TypeInterner ti{CompilationUnitId{1}};
    std::uint64_t key = 1900;
    for (auto const& b : bs) {
        ASSERT_EQ(b.fieldTypes.size(), b.widths.size())
            << b.name << ": probe field/width arity disagreement";
        auto const itN = native.find(b.name);
        ASSERT_NE(itN, native.end()) << b.name;
        NativeSizeAlign const& nl = itN->second;
        ASSERT_GT(nl.size, 0u) << b.name << ": native probe produced no size";

        TypeId const s = internPackBit(ti, b, key++);
        auto const l = computeLayout(s, ti, params, DataModel::Lp64);
        ASSERT_TRUE(l.has_value())
            << "dss failed to lay out " << b.cDecl << " (pack " << b.packN << ")";

        EXPECT_EQ(l->size, nl.size)
            << b.cDecl << " (pack " << b.packN << "): dss sizeof " << l->size
            << " != native " << nl.size;
        sizeRows.record();

        EXPECT_EQ(l->align.bytes(), nl.align)
            << b.cDecl << " (pack " << b.packN << "): dss _Alignof "
            << l->align.bytes() << " != native " << nl.align;
        alignRows.record();
    }
}

// The KNOWN GAP, pinned so it cannot regress into silence.
//
// Under a member-alignment cap the GNU ABI stops bumping a bit-field past its unit
// boundary, so a field can end up STRADDLING that boundary. `BitFieldPlacement`
// cannot express that — it is one `unitBytes`-wide load/store at one offset — so
// `computeLayout` REFUSES (nullopt → positioned diagnostic) instead of emitting a
// placement whose accesses would silently drop the overhanging bits.
//
// This test pins the REFUSAL, not a layout. It is a limitation, not the desired end
// state: clang -arch arm64 MEASURES `#pragma pack(2) struct { unsigned a; unsigned
// long long b:40; }` at sizeof 10 / _Alignof 2, and that is the answer to produce
// once `BitFieldPlacement` grows a multi-unit access. Until then the guarantee is
// that DSS says so out loud — before the fix it silently answered 16/8.
TEST(PackedAbiConformance, PackBitfieldStraddleIsRefusedNotMislaid) {
    AggregateLayoutParams const params{ScalarAlignmentRule::Natural, 16,
                                       BitFieldStrategy::GnuPacked};
    TypeInterner ti{CompilationUnitId{1}};

    // `#pragma pack(2) struct { unsigned a; unsigned long long b:40; }` — the u64
    // unit starts at bit 32 and the 40-bit field runs to bit 72, crossing it.
    std::array<TypeId, 2> const fields{ti.primitive(TypeKind::U32),
                                       ti.primitive(TypeKind::U64)};
    std::array<std::int64_t, 2> const widths{kNotBitfield, 40};
    TypeId const s = ti.forwardComposite(TypeKind::Struct, "Straddle", 950);
    ti.completeComposite(s, fields, /*packed=*/false, widths,
                         /*fieldOffsets=*/{}, /*fieldAligns=*/{},
                         /*explicitAlign=*/0, /*maxFieldAlign=*/2);
    EXPECT_FALSE(computeLayout(s, ti, params, DataModel::Lp64).has_value())
        << "a capped bit-field that straddles its allocation unit must FAIL LOUD; "
           "silently laying it out is the miscompile this guard exists to prevent";

    // The same shape with NO cap keeps the ordinary GNU bump and stays computable —
    // the red-on-disable partner: if the cap plumbing were dropped, this case and the
    // one above would become indistinguishable.
    TypeId const u = ti.forwardComposite(TypeKind::Struct, "NoCap", 951);
    ti.completeComposite(u, fields, /*packed=*/false, widths);
    auto const l = computeLayout(u, ti, params, DataModel::Lp64);
    ASSERT_TRUE(l.has_value()) << "uncapped bit-field straddle must still lay out";
    EXPECT_EQ(l->align.bytes(), 8u);

    // PRESENCE, not VALUE: `#pragma pack(16)` clamps nothing here (every natural
    // align is <= 16) yet it still turns the straddle bump off, so `{u8 a; unsigned
    // b:31; u8 c;}` becomes a straddler and is refused — while the SAME struct with
    // no pragma keeps the bump and lays out fine. MEASURED with clang -arch arm64:
    // sizeof 12 bare, sizeof 8 under pack(16) — the compiler agrees the two differ.
    //
    // This pair is the ONLY observable pin on that rule (every case where suppression
    // changes the layout is, by construction, a case that then straddles), and it is
    // what would red if `allowStraddlePadding` were "simplified" to a value test like
    // `packCap >= natural`.
    std::array<TypeId, 3> const wide{ti.primitive(TypeKind::U8),
                                     ti.primitive(TypeKind::U32),
                                     ti.primitive(TypeKind::U8)};
    std::array<std::int64_t, 3> const wideW{kNotBitfield, 31, kNotBitfield};
    TypeId const inert = ti.forwardComposite(TypeKind::Struct, "InertCap", 952);
    ti.completeComposite(inert, wide, /*packed=*/false, wideW,
                         /*fieldOffsets=*/{}, /*fieldAligns=*/{},
                         /*explicitAlign=*/0, /*maxFieldAlign=*/16);
    EXPECT_FALSE(computeLayout(inert, ti, params, DataModel::Lp64).has_value())
        << "a cap that clamps no alignment STILL suppresses the straddle bump; this "
           "struct straddles under it and must be refused, not laid out as if bare";

    TypeId const bare = ti.forwardComposite(TypeKind::Struct, "NoCapWide", 953);
    ti.completeComposite(bare, wide, /*packed=*/false, wideW);
    auto const bl = computeLayout(bare, ti, params, DataModel::Lp64);
    ASSERT_TRUE(bl.has_value()) << "with no cap the bump applies and this lays out";
    EXPECT_EQ(bl->size, 12u);
    EXPECT_EQ(bl->align.bytes(), 4u);
}
