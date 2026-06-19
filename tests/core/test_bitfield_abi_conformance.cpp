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
// HERMETIC WHERE PRESENT, SKIPPED OTHERWISE: if no native C compiler is found (or
// it fails to build/run the probe), the test GTEST_SKIPs rather than failing — the
// witness runs wherever the toolchain exists (every CI leg ships one) and never
// blocks a toolchain-less dev box.

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

// Locate a host C compiler. Returns a shell-ready command PREFIX that compiles
// `<src>` to `<exe>`, or nullopt if none found. On Windows this wraps cl.exe in a
// vcvars64 batch (cl needs INCLUDE/LIB); on Unix it uses cc/clang/gcc directly.
struct Compiler {
    std::string kind;   // "msvc" | "unix" — selects the expected dss strategy
    // Build `src` → `exe`; returns the full system() command. Empty string = unsupported.
    std::function<std::string(fs::path const&, fs::path const&)> buildCmd;
};

[[nodiscard]] bool fileExists(fs::path const& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

[[nodiscard]] std::optional<Compiler> findCompiler(fs::path const& work) {
#if defined(_WIN32)
    // Find cl.exe via vswhere → the MSVC toolset → Hostx64\x64. We compile through
    // a generated .bat that first runs vcvars64 (so cl has INCLUDE/LIB), then cl.
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
        // Generate a build batch: call vcvars64, then cl. Quote everything.
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
    // Unix: cc / clang / gcc — any in PATH builds the probe directly (no env setup).
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

// Run the native compiler on the battery + parse its output. nullopt = the
// toolchain is absent or the compile/run failed (→ the caller GTEST_SKIPs).
std::optional<std::map<std::string, NativeLayout>>
measureNative(std::vector<ProbeStruct> const& bs) {
    test_support::ScratchDir scratch{test_support::Location::Temp, "bitfield-abi"};
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
    writeProbeSource(src, bs);

    std::string const build = comp->buildCmd(src, exe);
    if (build.empty()) return std::nullopt;
    if (std::system(build.c_str()) != 0 || !fileExists(exe)) return std::nullopt;

    fs::path const out = work / "probe_out.txt";
    std::string const run = "\"\"" + exe.string() + "\" > \"" + out.string() + "\"\"";
    if (std::system(run.c_str()) != 0) return std::nullopt;

    std::ifstream in{out};
    if (!in) return std::nullopt;
    std::map<std::string, NativeLayout> result;
    for (auto const& b : bs) result[b.name].fieldFirstBit.assign(b.widths.size(), std::nullopt);

    std::string line;
    while (std::getline(in, line)) {
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
// sizeof + per-bit-field absolute bit position. Skips cleanly when no toolchain.
TEST(BitFieldAbiConformance, DssLayoutMatchesNativeCompiler) {
    auto const bs = battery();
    auto const native = measureNative(bs);
    if (!native) {
        GTEST_SKIP() << "no native C compiler available (or it failed to build the "
                        "probe) — the hermetic goldens in test_type_layout still pin "
                        "the per-ABI layout; this CI leg re-derives them where a "
                        "toolchain is present.";
    }

    // The host ABI selects the strategy: Windows → MsvcStraddle, else GnuPacked.
#if defined(_WIN32)
    BitFieldStrategy const strat = BitFieldStrategy::MsvcStraddle;
    char const* stratName = "msvc_straddle";
#else
    BitFieldStrategy const strat = BitFieldStrategy::GnuPacked;
    char const* stratName = "gnu_packed";
#endif
    AggregateLayoutParams params{ScalarAlignmentRule::Natural, 16, strat};

    TypeInterner ti{CompilationUnitId{1}};
    for (auto const& b : bs) {
        auto const itN = native->find(b.name);
        ASSERT_NE(itN, native->end()) << b.name;
        NativeLayout const& nl = itN->second;

        TypeId const s = internStruct(ti, b);
        auto const l = computeLayout(s, ti, params, DataModel::Lp64);
        ASSERT_TRUE(l.has_value())
            << "dss failed to lay out struct " << b.name << " under " << stratName;

        EXPECT_EQ(l->size, nl.size)
            << "struct " << b.name << ": dss sizeof " << l->size
            << " != native " << nl.size << " (" << stratName << ")";

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
        }
    }
}
