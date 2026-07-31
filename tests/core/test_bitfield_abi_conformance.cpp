// D-CSUBSET-BITFIELD-ABI-EXACT — the CI cross-compile-COMPARE conformance witness.
//
// The hermetic half of the witness lives in `test_type_layout.cpp` (it pins
// `computeLayout` under each strategy to the native-compiler-derived goldens). THIS
// file is the load-bearing other half: it compiles the SAME struct battery with the
// HOST's NATIVE C compiler (cl.exe on Windows → MsvcStraddle; gcc/clang/cc on
// Linux/macOS → GnuPacked), reads back the compiler's real `sizeof` + per-field bit
// position, and asserts dss's `computeLayout` (under the strategy that matches the
// host ABI) is BYTE-IDENTICAL to it. So the hardcoded goldens can never silently
// drift from the real ABI — a compiler/ABI change makes this test go red.
//
// **For Apple-arm64 this CI leg is the ONLY runtime witness** (no local Mac /
// emulator): on macos-latest, clang compiles the battery and this test confirms
// dss's gnu_packed layout == clang's — the empirical proof that Apple does NOT
// diverge from generic AAPCS64 on bit-field packing (Apple's documented arm64
// divergences are char/wchar_t signedness, long double=double, va_list, the red
// zone, the stack argument area, and empty structs — NOT bit-field allocation).
//
// HERMETIC WHERE PRESENT, SKIPPED ONLY WHEN THE TOOL IS ABSENT: with no native C
// compiler that can build a trivial program the test GTEST_SKIPs, so a toolchain-less
// dev box is never blocked — and a bare `cc` SHIM with no toolchain behind it counts
// as absent, not as broken. Any OTHER failure — a compiler that builds a trivial
// program but cannot build, launch or be read back from THIS probe — is a HARNESS
// DEFECT and goes RED.
//
// That sentence used to end "or it fails to build/run the probe, the test GTEST_SKIPs
// rather than failing", and the difference was not academic. The run step was spelled
// with cmd.exe quoting unconditionally, POSIX `sh` rejected it, and the skip branch
// swallowed the failure under the message "no native C compiler available" — so on
// every Unix host this witness executed ZERO comparisons and reported success. It is
// **the ONLY runtime witness for Apple-arm64**, which means the platform this file
// claims to cover is exactly the platform it was silent on. See `native_c_probe.hpp`
// for the seam that now separates a missing tool from a broken invocation.
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
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace native_probe = dss::test_support::native_probe;
using namespace dss;

namespace {

// One probe struct: a name, its interned field types + bit-widths, and the C
// source declaring it. The C source and the interner build MUST describe the same
// struct (the test cross-checks they agree); keeping both here makes that visible.
struct ProbeStruct {
    std::string                name;
    std::string                cDecl;      // e.g. "struct A { int a:1; char b:1; };"
    std::vector<TypeKind>      fieldTypes; // declared type per field
    std::vector<std::int64_t>  widths;     // bit width per field (kNotBitfield = ordinary)
};

// The divergence battery + same-type controls (the empirically-measured cases).
std::vector<ProbeStruct> battery() {
    using K = TypeKind;
    std::int64_t const O = kNotBitfield;
    return {
        {"A", "struct A { int a:1; char b:1; };",       {K::I32, K::Char},          {1, 1}},
        {"B", "struct B { char a:7; int b:25; };",      {K::Char, K::I32},          {7, 25}},
        {"F", "struct F { char a:1; int b:1; };",       {K::Char, K::I32},          {1, 1}},
        {"C", "struct C { int a:3; int b:5; };",        {K::I32, K::I32},           {3, 5}},
        {"D", "struct D { int a:30; int b:5; };",       {K::I32, K::I32},           {30, 5}},
        {"G", "struct G { int a:8; int b:8; short c:8; };", {K::I32, K::I32, K::I16}, {8, 8, 8}},
        {"J", "struct J { char x; int a:3; };",         {K::Char, K::I32},          {O, 3}},
        {"K", "struct K { int a:3; char y; };",         {K::I32, K::Char},          {3, O}},
        {"M", "struct M { int a:1; };",                 {K::I32},                   {1}},
        {"N", "struct N { char a:1; char b:1; };",      {K::Char, K::Char},         {1, 1}},
        {"P", "struct P { char a:1; int b:1; char c:1; };", {K::Char, K::I32, K::Char}, {1, 1, 1}},
    };
}

// The native compiler's measured layout of one struct: total size + the absolute
// FIRST-SET-BIT position of each BIT-FIELD (byte*8 + bit-in-byte), keyed by field
// index. Ordinary fields are not probed (their offset is not a bit-field concern).
struct NativeLayout {
    std::uint64_t size = 0;
    std::vector<std::optional<std::uint64_t>> fieldFirstBit;  // per field; nullopt = ordinary
};

// Locating the host compiler, spelling the run-step redirect for the host shell, and
// building/launching/reading back the probe all live in `native_c_probe.hpp` now —
// shared verbatim with `test_packed_abi_conformance.cpp`. They were duplicated here,
// and the duplicate is what let the TF-C97 quoting fix reach only one of the two.

// The number of bit-field positions this battery expects the oracle to compare.
// DERIVED from the battery so the inert-oracle floor below cannot drift away from the
// data it guards; a hand-typed count is a floor that silently stops being one.
[[nodiscard]] std::size_t probedBitFieldCount(std::vector<ProbeStruct> const& bs) {
    std::size_t n = 0;
    for (auto const& b : bs)
        for (auto w : b.widths)
            if (w != kNotBitfield && w != 0) ++n;
    return n;
}

// The C identifier of field `i` in a probe struct. The battery uses a,b,c for the
// bit-fields and x/y for the ordinary fields, in declaration order. Kept in
// lockstep with `battery()` — explicit per-struct so a battery edit cannot
// silently mis-name a probe.
std::string fieldNameFor(ProbeStruct const& b, std::size_t i) {
    static std::array<char const*, 3> const abc{"a", "b", "c"};
    if (b.name == "J") return i == 0 ? "x" : "a";
    if (b.name == "K") return i == 0 ? "a" : "y";
    return abc[i];
}

// Emit the probe C program for the battery: prints "<NAME> size=<n>" then, for each
// bit-field, "<NAME>.<i> byte=<b> bit=<k>" (the lowest set byte + lowest set bit in
// it when ONLY that field is set to all-1s). Ordinary fields are skipped.
void writeProbeSource(fs::path const& src, std::vector<ProbeStruct> const& bs) {
    std::ofstream o{src};
    o << "#include <stdio.h>\n#include <string.h>\n";
    for (auto const& b : bs) o << b.cDecl << "\n";
    o << "static void probe(const char* nm, int idx, const unsigned char* p, "
         "unsigned long n){\n"
      << "  unsigned long byte=0; int bit=-1;\n"
      << "  for (unsigned long i=0;i<n;++i){ if(p[i]){ byte=i; "
         "for(int k=0;k<8;++k) if(p[i]&(1u<<k)){bit=k;break;} break; } }\n"
      << "  printf(\"%s.%d byte=%lu bit=%d\\n\", nm, idx, byte, bit);\n}\n";
    o << "int main(void){\n";
    for (auto const& b : bs) {
        o << "  printf(\"" << b.name << " size=%lu\\n\", (unsigned long)sizeof(struct "
          << b.name << "));\n";
        for (std::size_t i = 0; i < b.widths.size(); ++i) {
            if (b.widths[i] == kNotBitfield) continue;  // ordinary field: not probed
            o << "  { struct " << b.name << " s; memset(&s,0,sizeof s); s."
              << fieldNameFor(b, i) << " = -1; probe(\"" << b.name << "\", " << i
              << ", (const unsigned char*)&s, sizeof s); }\n";
        }
    }
    o << "  return 0;\n}\n";
}

// Parse the probe's captured stdout into per-struct layouts.
//
// PURE PARSING, deliberately. Whether the probe ran at all is decided upstream by
// `runNativeCProbe`'s status; this function never reports "no toolchain". Folding the
// two together is what produced the original defect — a parse over zero lines yields
// a map of zeroes that is indistinguishable from a real measurement of a zero-sized
// struct, so the emptiness has to be caught before it gets here.
std::map<std::string, NativeLayout>
parseNative(std::vector<ProbeStruct> const& bs,
            std::vector<std::string> const& lines) {
    std::map<std::string, NativeLayout> result;
    for (auto const& b : bs) result[b.name].fieldFirstBit.assign(b.widths.size(), std::nullopt);

    for (auto const& line : lines) {
        std::istringstream ls{line};
        std::string tok;
        ls >> tok;
        auto const dot = tok.find('.');
        if (dot == std::string::npos) {
            // "<NAME> size=<n>"
            std::string sz; ls >> sz;
            auto const eq = sz.find('=');
            if (eq == std::string::npos) continue;
            auto it = result.find(tok);
            if (it == result.end()) continue;
            it->second.size = std::stoull(sz.substr(eq + 1));
        } else {
            // "<NAME>.<idx> byte=<b> bit=<k>"
            std::string const nm = tok.substr(0, dot);
            std::size_t const idx = std::stoul(tok.substr(dot + 1));
            std::string byteT, bitT; ls >> byteT >> bitT;
            auto const be = byteT.find('=');
            auto const bk = bitT.find('=');
            if (be == std::string::npos || bk == std::string::npos) continue;
            std::uint64_t const byte = std::stoull(byteT.substr(be + 1));
            long const bit = std::stol(bitT.substr(bk + 1));
            if (bit < 0) continue;  // field had no set bit (shouldn't happen)
            auto it = result.find(nm);
            if (it == result.end() || idx >= it->second.fieldFirstBit.size()) continue;
            it->second.fieldFirstBit[idx] = byte * 8 + static_cast<std::uint64_t>(bit);
        }
    }
    return result;
}

[[nodiscard]] TypeId internStruct(TypeInterner& ti, ProbeStruct const& b) {
    std::vector<TypeId> fields;
    fields.reserve(b.fieldTypes.size());
    for (auto k : b.fieldTypes) fields.push_back(ti.primitive(k));
    return ti.structType(b.name, fields, b.widths);
}

} // namespace

// THE cross-compile-compare witness. For every struct in the battery, dss's
// computeLayout (under the host-ABI strategy) MUST equal the native compiler's
// sizeof + per-bit-field absolute bit position.
//
// SKIPS only when the machine has no C compiler at all. A compiler that IS present
// and then fails to build, launch, or answer is a defect in this harness and goes
// RED — see the `native_c_probe.hpp` docblock for the three cycles that policy cost.
TEST(BitFieldAbiConformance, DssLayoutMatchesNativeCompiler) {
    auto const bs = battery();
    auto const probe = native_probe::runNativeCProbe(
        "bitfield-abi",
        [&bs](fs::path const& src, native_probe::Toolchain) { writeProbeSource(src, bs); });

    // The ONE legitimate skip: no toolchain on this machine. `probe.detail` is echoed
    // because "absent" now has two shapes — nothing on PATH, and a shim that cannot
    // build a trivial program — and a reader of a skipped CI leg needs to know which.
    if (probe.toolAbsent()) {
        GTEST_SKIP() << "no usable native C compiler on this host (" << probe.detail
                     << ") — the hermetic goldens in test_type_layout still pin the "
                        "per-ABI layout; this CI leg re-derives them where a toolchain "
                        "is present.";
    }
    // Every other outcome is FAIL-LOUD. This assert is the whole fix: it is the
    // branch that used to be folded into the skip above.
    ASSERT_TRUE(probe.ok()) << probe.describe();

    auto const native = parseNative(bs, probe.lines);

    // The host ABI selects the strategy: Windows → MsvcStraddle, else GnuPacked.
#if defined(_WIN32)
    BitFieldStrategy const strat = BitFieldStrategy::MsvcStraddle;
    char const* stratName = "msvc_straddle";
#else
    BitFieldStrategy const strat = BitFieldStrategy::GnuPacked;
    char const* stratName = "gnu_packed";
#endif
    AggregateLayoutParams params{ScalarAlignmentRule::Natural, 16, strat};

    // The inert-oracle guard. Constructed HERE, below every early exit, so a
    // legitimate tool-absent skip never trips it. Both floors are derived from the
    // battery, so adding a probe struct raises the bar automatically.
    native_probe::ExecutedRows sizeRows{"bit-field sizeof vs native", bs.size()};
    native_probe::ExecutedRows bitRows{"bit-field bit-position vs native",
                                       probedBitFieldCount(bs)};

    TypeInterner ti{CompilationUnitId{1}};
    for (auto const& b : bs) {
        auto const itN = native.find(b.name);
        ASSERT_NE(itN, native.end()) << b.name;
        NativeLayout const& nl = itN->second;

        TypeId const s = internStruct(ti, b);
        auto const l = computeLayout(s, ti, params, DataModel::Lp64);
        ASSERT_TRUE(l.has_value())
            << "dss failed to lay out struct " << b.name << " under " << stratName;

        EXPECT_EQ(l->size, nl.size)
            << "struct " << b.name << ": dss sizeof " << l->size
            << " != native " << nl.size << " (" << stratName << ")";
        sizeRows.record();

        ASSERT_EQ(l->bitFields.size(), b.widths.size()) << b.name;
        for (std::size_t i = 0; i < b.widths.size(); ++i) {
            if (b.widths[i] == kNotBitfield) continue;        // ordinary: not probed
            if (b.widths[i] == 0) continue;                   // zero-width: no storage
            ASSERT_TRUE(nl.fieldFirstBit[i].has_value())
                << b.name << " field " << i << " (native gave no set bit)";
            std::uint64_t const dssAbsBit =
                l->fieldOffsets[i] * 8 + l->bitFields[i].bitOffset;
            EXPECT_EQ(dssAbsBit, *nl.fieldFirstBit[i])
                << "struct " << b.name << " field " << i
                << ": dss bit position " << dssAbsBit << " != native "
                << *nl.fieldFirstBit[i] << " (" << stratName << ")";
            bitRows.record();
        }
    }
}
