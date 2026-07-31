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
// which is exactly what DSS models. Skips cleanly when no toolchain is present.

#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
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

struct Compiler {
    std::string kind;   // "msvc" | "unix"
    std::function<std::string(fs::path const&, fs::path const&)> buildCmd;
};

[[nodiscard]] bool fileExists(fs::path const& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// Run `exe` with stdout redirected into `out`, spelled for the host's shell.
//
// These two spellings are NOT interchangeable, and getting it wrong is SILENT: the
// cmd.exe form wraps the whole command in an extra pair of quotes (`""prog" > "file""`)
// so a spaced path survives, but a POSIX shell reads that same string as ONE command
// name and reports `prog > file: No such file or directory`. `std::system` then
// returns non-zero, `measure*Native` returns nullopt, and the test GTEST_SKIPs with
// "no native C compiler available" — a conformance witness that looks fine in the log
// while never actually comparing anything.
//
// MEASURED: that is exactly what this file (and its bitfield twin) did on macOS —
// the cmd.exe form was used unconditionally, so the cross-compile-compare leg had
// been inert on every Unix host. Both call sites below go through here now.
[[nodiscard]] std::string redirectCmd(fs::path const& exe, fs::path const& out) {
#if defined(_WIN32)
    return "\"\"" + exe.string() + "\" > \"" + out.string() + "\"\"";
#else
    return "\"" + exe.string() + "\" > \"" + out.string() + "\"";
#endif
}

[[nodiscard]] std::optional<Compiler> findCompiler(fs::path const& work) {
#if defined(_WIN32)
    fs::path const vswhere =
        fs::path{"C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"};
    if (!fileExists(vswhere)) return std::nullopt;
    fs::path const vswhereOut = work / "vsinstall.txt";
    std::string const q =
        "\"\"" + vswhere.string() + "\" -latest -products * "
        "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
        "-property installationPath > \"" + vswhereOut.string() + "\"\"";
    if (std::system(q.c_str()) != 0) return std::nullopt;
    std::ifstream vin{vswhereOut};
    std::string vsPath;
    std::getline(vin, vsPath);
    while (!vsPath.empty() && (vsPath.back() == '\r' || vsPath.back() == '\n'))
        vsPath.pop_back();
    if (vsPath.empty()) return std::nullopt;
    fs::path const vcvars =
        fs::path{vsPath} / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
    if (!fileExists(vcvars)) return std::nullopt;
    Compiler c;
    c.kind = "msvc";
    c.buildCmd = [vcvars, work](fs::path const& src, fs::path const& exe) -> std::string {
        fs::path const bat = work / "build_probe.bat";
        std::ofstream b{bat};
        b << "@echo off\r\n"
          << "call \"" << vcvars.string() << "\" >nul 2>&1\r\n"
          << "cl /nologo /W3 /Fe:\"" << exe.string() << "\" \""
          << src.string() << "\" >nul 2>&1\r\n";
        b.close();
        return "\"\"" + bat.string() + "\"\"";
    };
    return c;
#else
    for (char const* cc : {"cc", "clang", "gcc"}) {
        std::string const probe = std::string{"command -v "} + cc + " >/dev/null 2>&1";
        if (std::system(probe.c_str()) == 0) {
            Compiler c;
            c.kind = "unix";
            std::string const ccName = cc;
            c.buildCmd = [ccName](fs::path const& src, fs::path const& exe) -> std::string {
                return ccName + " -std=c11 -O0 -o \"" + exe.string() + "\" \""
                     + src.string() + "\"";
            };
            return c;
        }
    }
    return std::nullopt;
#endif
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

std::optional<std::map<std::string, NativeLayout>>
measureNative(std::vector<PackedProbe> const& bs) {
    test_support::ScratchDir scratch{test_support::Location::Temp, "packed-abi"};
    fs::path const work = scratch.path();
    auto comp = findCompiler(work);
    if (!comp) return std::nullopt;

    fs::path const src = work / "probe.c";
    fs::path const exe = work / (
#if defined(_WIN32)
        "probe.exe"
#else
        "probe"
#endif
    );
    writeProbeSource(src, bs, comp->kind == "msvc");

    std::string const build = comp->buildCmd(src, exe);
    if (build.empty()) return std::nullopt;
    if (std::system(build.c_str()) != 0 || !fileExists(exe)) return std::nullopt;

    fs::path const out = work / "probe_out.txt";
    std::string const run = redirectCmd(exe, out);
    if (std::system(run.c_str()) != 0) return std::nullopt;

    std::ifstream in{out};
    if (!in) return std::nullopt;
    std::map<std::string, NativeLayout> result;
    for (auto const& b : bs) result[b.name].offsets.assign(b.fieldTypes.size(), 0);

    std::string line;
    while (std::getline(in, line)) {
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
// examples/c-subset/packed_bitfield_align makes the same point end to end.
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

std::optional<std::map<std::string, NativeSizeAlign>>
measurePackBitNative(std::vector<PackBitProbe> const& bs) {
    test_support::ScratchDir scratch{test_support::Location::Temp, "pack-bitfield-abi"};
    fs::path const work = scratch.path();
    auto comp = findCompiler(work);
    if (!comp) return std::nullopt;

    fs::path const src = work / "probe.c";
    fs::path const exe = work / (
#if defined(_WIN32)
        "probe.exe"
#else
        "probe"
#endif
    );
    writePackBitProbeSource(src, bs);

    std::string const build = comp->buildCmd(src, exe);
    if (build.empty()) return std::nullopt;
    if (std::system(build.c_str()) != 0 || !fileExists(exe)) return std::nullopt;

    fs::path const out = work / "probe_out.txt";
    std::string const run = redirectCmd(exe, out);
    if (std::system(run.c_str()) != 0) return std::nullopt;

    std::ifstream in{out};
    if (!in) return std::nullopt;
    std::map<std::string, NativeSizeAlign> result;
    std::string line;
    while (std::getline(in, line)) {
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
    auto const native = measureNative(bs);
    if (!native) {
        GTEST_SKIP() << "no native C compiler available (or it failed to build the "
                        "probe) — the hermetic packed goldens in test_type_layout "
                        "still pin the layout; this CI leg re-derives them where a "
                        "toolchain is present.";
    }

    // Packed layout is data-model-independent (all scalar widths are fixed by their
    // TypeKind; no pointers in the battery), so Lp64 params suffice on every host.
    AggregateLayoutParams const params{ScalarAlignmentRule::Natural, 16};
    TypeInterner ti{CompilationUnitId{1}};
    std::uint64_t key = 100;
    for (auto const& b : bs) {
        auto const itN = native->find(b.name);
        ASSERT_NE(itN, native->end()) << b.name;
        NativeLayout const& nl = itN->second;

        TypeId const s = internPacked(ti, b, key++);
        auto const l = computeLayout(s, ti, params, DataModel::Lp64);
        ASSERT_TRUE(l.has_value())
            << "dss failed to lay out packed struct " << b.name;

        EXPECT_EQ(l->size, nl.size)
            << "packed struct " << b.name << ": dss sizeof " << l->size
            << " != native " << nl.size;
        ASSERT_EQ(l->fieldOffsets.size(), nl.offsets.size()) << b.name;
        for (std::size_t i = 0; i < nl.offsets.size(); ++i) {
            EXPECT_EQ(l->fieldOffsets[i], nl.offsets[i])
                << "packed struct " << b.name << " field " << i
                << ": dss offset " << l->fieldOffsets[i]
                << " != native " << nl.offsets[i];
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
    auto const native = measurePackBitNative(bs);
    if (!native) {
        GTEST_SKIP() << "no native C compiler available (or it failed to build the "
                        "probe) — this leg re-derives the pack+bit-field layout "
                        "wherever a toolchain is present.";
    }

    // GnuPacked: the strategy every GNU-ABI host (Linux/macOS, gcc/clang) uses, which
    // is the ABI the probe was just compiled under.
    AggregateLayoutParams const params{ScalarAlignmentRule::Natural, 16,
                                       BitFieldStrategy::GnuPacked};
    TypeInterner ti{CompilationUnitId{1}};
    std::uint64_t key = 900;
    for (auto const& b : bs) {
        ASSERT_EQ(b.fieldTypes.size(), b.widths.size())
            << b.name << ": probe field/width arity disagreement";
        auto const itN = native->find(b.name);
        ASSERT_NE(itN, native->end()) << b.name;
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
        EXPECT_EQ(l->align.bytes(), nl.align)
            << "pack(" << b.packN << ") " << b.cDecl << ": dss _Alignof "
            << l->align.bytes() << " != native " << nl.align;
    }
#endif
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
