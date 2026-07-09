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
    std::string const run = "\"\"" + exe.string() + "\" > \"" + out.string() + "\"\"";
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
