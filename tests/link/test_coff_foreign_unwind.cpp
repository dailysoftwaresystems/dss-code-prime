// P69 fold 3c, lane p69/xa — D-LK-COFF-FOREIGN-UNWIND-DROP: a COFF object's `.pdata`/`.xdata` read into its
// functions and written back.
//
// The COFF reader decodes each RUNTIME_FUNCTION's UNWIND_INFO into the function's `cfi`
// (`link/format/win64_unwind_decode.hpp`) and carries a handler — its routine, its phases, its data and the addresses
// the data holds — as the function's `languageHandler`; the PE writers encode both back
// (`pe.cpp`'s `buildFunctionUnwindInfo`). The fixtures are the reference compilers' own objects
// (`tests/link/data/win64_unwind_{forms,try,huge}.source.c` state how each was built):
//
//   win64_unwind_forms_x86_64_pe_cl_O2.obj       md5 00fc7fb0ebc3d54c79abd29d84c361f3   cl 19.44.35229 /O2
//   win64_unwind_forms_x86_64_pe_cl_Od.obj       md5 8315ff5df0389809c69df2b9e52d4577   cl 19.44.35229 /Od
//   win64_unwind_forms_x86_64_pe_clangcl_O2.obj  md5 6ad49c56ca35b08b82479ce9807b737b   clang-cl 19.1.5 /O2
//   win64_unwind_forms_x86_64_pe_gcc_O2.obj      md5 e8bebdd0fa47aafd41ec01a9e44070b0   mingw-w64 gcc 13.2.0 -O2
//   win64_unwind_try_x86_64_pe_cl_O2.obj         md5 456673ea5a0db1a9a0bd7853219a0740   cl 19.44.35229 /O2
//   win64_unwind_try_x86_64_pe_cl_Od.obj         md5 c845ec0ca0cba582ae0def850743440c   cl 19.44.35229 /Od
//   win64_unwind_try_x86_64_pe_clangcl_O2.obj    md5 4bbee256080fc8ca3b4e445d7be1f009   clang-cl 19.1.5 /O2
//   win64_unwind_huge_x86_64_pe_cl_O2.obj        md5 0e891c54cbedc3620b22d361982bc6ca   cl 19.44.35229 /O2
//   win64_unwind_huge_x86_64_pe_cl_Od.obj        md5 7a8f3302014a8d2b54d3474a0d7d2411   cl 19.44.35229 /Od
//   win64_unwind_huge_x86_64_pe_clangcl_O2.obj   md5 28d27e5709409eb1cb511c7e1902b4d0   clang-cl 19.1.5 /O2
//   win64_unwind_huge_x86_64_pe_gcc_O2.obj       md5 8c2d658251dfa25615fade918c238f55   mingw-w64 gcc 13.2.0 -O2
//
// ✔MEASURED at the build (P69 folds 3c and 3d): each object comes out of a second build byte-identical and records no
// path of the host that built it; each `try` object linked by link.exe beside a cl `main` runs to 42, and each `huge`
// object linked by its own toolchain beside a `reach` that unwinds two frames with RtlVirtualUnwind lands in `main`
// (exit 42). No guard enforces an md5: it records WHICH BYTES were measured.
//
// D-WIN64-HUGE-FRAME-ALLOC (P69 fold 3d): the `huge` objects carry the long spellings of the codes — UWOP_ALLOC_LARGE
// op-info 1 (every object), UWOP_SAVE_NONVOL_FAR (cl /O2) and UWOP_SAVE_XMM128_FAR (every /O2 and -O2 object) — and
// DSS compiles the same source and runs it, its own frames written in those spellings.
//
// Every TEST but the native suite runs in-process on every leg: the fixtures are read, never compiled.

#include "asm/asm.hpp"
#include "core/types/cfi.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"

#include "../core/native_c_probe.hpp"
#include "host_native_target.hpp"
#include "image_dependency_table.hpp"
#include "repo_root.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::vector<std::uint8_t> bytesOf(fs::path const& path) {
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        ADD_FAILURE() << "cannot open " << path.string();
        return {};
    }
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::vector<std::uint8_t> fixture(std::string_view name) {
    return bytesOf(dss::test::repoRoot() / "tests" / "link" / "data" / std::string{name});
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s.empty() ? std::string{"\n  <no diagnostic>"} : s;
}

[[nodiscard]] bool said(DiagnosticReporter const& rep, std::string_view text) {
    return std::any_of(rep.all().begin(), rep.all().end(),
                       [&](auto const& d) { return d.actual.find(text) != std::string::npos; });
}

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> object;   // pe64-x86_64-windows
};

[[nodiscard]] Loaded load() {
    Loaded l;
    if (auto t = TargetSchema::loadShipped("x86_64"); t.has_value()) l.target = *t;
    if (auto f = ObjectFormatSchema::loadShipped("pe64-x86_64-windows"); f.has_value()) l.object = *f;
    EXPECT_TRUE(l.target && l.object) << "the shipped x86_64 target and pe64 object documents";
    return l;
}

[[nodiscard]] std::optional<AssembledModule> read(Loaded const& l, std::span<std::uint8_t const> bytes,
                                                  DiagnosticReporter& rep) {
    return pe::readRelocatableObject(bytes, *l.target, *l.object, rep, CompilationUnitId{1});
}

// ── A small COFF reader of the test's own, so a writer is checked against what an OBJECT says, not against the
//    reader under test. ───────────────────────────────────────────────────────────────────────────────────────────
struct Coff {
    struct Sec {
        std::string   name;
        std::uint32_t size = 0, ptr = 0, relPtr = 0;
        std::uint16_t nrel = 0;
    };
    struct Sym {
        std::string   name;
        std::uint32_t value = 0;
        std::int16_t  section = 0;
        std::uint8_t  cls = 0;
        bool          aux = false;
    };
    struct Rel {
        std::uint32_t va = 0, sym = 0;
        std::uint16_t type = 0;
    };
    std::vector<std::uint8_t> b;
    std::vector<Sec>          secs;
    std::vector<Sym>          syms;

    [[nodiscard]] std::uint16_t u16(std::size_t o) const { return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8)); }
    [[nodiscard]] std::uint32_t u32(std::size_t o) const {
        return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8)
             | (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
    }
    [[nodiscard]] static Coff of(std::vector<std::uint8_t> bytes) {
        Coff c;
        c.b                          = std::move(bytes);
        std::uint16_t const nsec     = c.u16(2);
        std::uint32_t const symptr   = c.u32(8);
        std::uint32_t const nsym     = c.u32(12);
        std::uint16_t const optsz    = c.u16(16);
        std::size_t const   strtab   = symptr + 18u * nsym;
        auto const          strAt    = [&](std::size_t o) {
            std::string s;
            while (o < c.b.size() && c.b[o] != 0) s.push_back(static_cast<char>(c.b[o++]));
            return s;
        };
        for (std::uint16_t i = 0; i < nsec; ++i) {
            std::size_t const o = 20u + optsz + 40u * i;
            Sec               s;
            for (std::size_t k = 0; k < 8 && c.b[o + k] != 0; ++k) s.name.push_back(static_cast<char>(c.b[o + k]));
            if (!s.name.empty() && s.name[0] == '/') s.name = strAt(strtab + std::stoul(s.name.substr(1)));
            s.size   = c.u32(o + 16);
            s.ptr    = c.u32(o + 20);
            s.relPtr = c.u32(o + 24);
            s.nrel   = c.u16(o + 32);
            c.secs.push_back(std::move(s));
        }
        for (std::uint32_t i = 0; i < nsym;) {
            std::size_t const o = symptr + 18u * i;
            Sym               s;
            if (c.u32(o) == 0u) s.name = strAt(strtab + c.u32(o + 4));
            else for (std::size_t k = 0; k < 8 && c.b[o + k] != 0; ++k) s.name.push_back(static_cast<char>(c.b[o + k]));
            s.value   = c.u32(o + 8);
            s.section = static_cast<std::int16_t>(c.u16(o + 12));
            s.cls     = c.b[o + 16];
            std::uint8_t const naux = c.b[o + 17];
            c.syms.push_back(s);
            for (std::uint8_t a = 0; a < naux; ++a) c.syms.push_back(Sym{{}, 0, 0, 0, true});
            i += 1u + naux;
        }
        return c;
    }
    [[nodiscard]] std::vector<Rel> rels(Sec const& s) const {
        std::vector<Rel> out;
        for (std::uint16_t r = 0; r < s.nrel; ++r) {
            std::size_t const o = s.relPtr + 10u * r;
            out.push_back(Rel{u32(o), u32(o + 4), u16(o + 8)});
        }
        return out;
    }
    // The function a (section, offset) lies in: the defined, named, non-section symbol of that section with the
    // greatest value at or below the offset — "<name>+<residual>".
    [[nodiscard]] std::string functionAt(std::int16_t section, std::uint32_t offset) const {
        Sym const* best = nullptr;
        for (auto const& s : syms) {
            if (s.aux || s.section != section || s.value > offset || s.name.empty()) continue;
            if (s.name[0] == '.' || s.name[0] == '$' || (s.cls != 2 && s.cls != 3)) continue;
            if (best == nullptr || s.value > best->value) best = &s;
        }
        if (best == nullptr) return "<none>+" + std::to_string(offset);
        return best->name + "+" + std::to_string(offset - best->value);
    }
};

// One function's unwind description as an object states it.
struct FunctionUnwind {
    std::vector<std::uint8_t> codes;       // the UNWIND_INFO's header and code slots, as written
    std::string               handler;     // the routine its handler field names; empty without one
    std::vector<std::uint8_t> data;        // the handler's data, every relocated field zeroed
    std::vector<std::string>  addresses;   // "<offset in data>:<function>+<residual>" per relocation of the data
};

// Every RUNTIME_FUNCTION of an object, keyed by the function it begins (the data running, as the reader reads it, to
// the next UNWIND_INFO of its section or the section's end).
[[nodiscard]] std::map<std::string, FunctionUnwind> unwindOf(Coff const& c) {
    std::map<std::string, FunctionUnwind> out;
    auto const isOf = [](std::string const& n, std::string_view fam) {
        return n == fam || (n.size() > fam.size() && n.compare(0, fam.size(), fam) == 0
                            && (n[fam.size()] == '$' || n[fam.size()] == '.'));
    };
    // (section index, offset) of every UNWIND_INFO a RUNTIME_FUNCTION names.
    std::map<std::size_t, std::vector<std::uint32_t>> starts;
    struct Rf {
        std::string   fn;
        std::size_t   xsec;
        std::uint32_t at;
    };
    std::vector<Rf> rfs;
    for (auto const& s : c.secs) {
        if (!isOf(s.name, ".pdata")) continue;
        std::map<std::uint32_t, Coff::Rel> byVa;
        for (auto const& r : c.rels(s)) byVa[r.va] = r;
        for (std::uint32_t e = 0; e + 12u <= s.size; e += 12u) {
            auto const& begin = c.syms[byVa.at(e).sym];
            auto const& info  = c.syms[byVa.at(e + 8u).sym];
            std::uint32_t const fnOff = begin.value + c.u32(s.ptr + e);
            std::string         fn    = c.functionAt(begin.section, fnOff);
            std::uint32_t const at    = info.value + c.u32(s.ptr + e + 8u);
            rfs.push_back(Rf{fn, static_cast<std::size_t>(info.section - 1), at});
            starts[static_cast<std::size_t>(info.section - 1)].push_back(at);
        }
    }
    for (auto& [_, v] : starts) std::sort(v.begin(), v.end());
    for (auto const& rf : rfs) {
        auto const&         xs     = c.secs[rf.xsec];
        std::size_t const   u      = xs.ptr + rf.at;
        std::uint8_t const  ncodes = c.b[u + 2];
        std::size_t const   after  = 4u + 2u * (ncodes + (ncodes & 1u));
        FunctionUnwind      f;
        f.codes.assign(c.b.begin() + static_cast<std::ptrdiff_t>(u),
                       c.b.begin() + static_cast<std::ptrdiff_t>(u + 4u + 2u * ncodes));
        if ((c.b[u] >> 3) & 3u) {
            std::map<std::uint32_t, Coff::Rel> byVa;
            for (auto const& r : c.rels(xs)) byVa[r.va] = r;
            std::uint32_t const hField = rf.at + static_cast<std::uint32_t>(after);
            auto const          hRel   = byVa.find(hField);
            f.handler = hRel == byVa.end() ? std::string{"<a handler field with no relocation>"}
                                           : c.syms[hRel->second.sym].name;
            auto const&         v      = starts.at(rf.xsec);
            auto const          next   = std::upper_bound(v.begin(), v.end(), rf.at);
            std::uint32_t const end    = next == v.end() ? xs.size : *next;
            std::uint32_t const d0     = hField + 4u;
            f.data.assign(c.b.begin() + xs.ptr + d0, c.b.begin() + xs.ptr + end);
            for (auto const& [va, r] : byVa) {
                if (va < d0 || va >= end) continue;
                auto const&         t    = c.syms[r.sym];
                std::uint32_t const off  = t.value + c.u32(xs.ptr + va);
                f.addresses.push_back(std::to_string(va - d0) + ":" + c.functionAt(t.section, off));
                for (std::size_t k = 0; k < 4; ++k) f.data[va - d0 + k] = 0;
            }
        }
        out[rf.fn] = std::move(f);
    }
    return out;
}

// An UNWIND_INFO's header and codes with the one field the references disagree on, and no unwinder reads, zeroed:
// UWOP_SET_FPREG's operation info, "reserved and shouldn't be used" (Microsoft's x64 exception-handling
// documentation). ✔MEASURED on the fixtures: cl 19.44 writes the frame register's scaled offset there (`0e23`, `0b23`:
// offset 2), clang-cl 19.1.5 and mingw-w64 gcc 13.2.0 write 0 (`0a03`, `0b03`), and so does DSS's encoder. Every other
// bit is compared as written.
[[nodiscard]] std::vector<std::uint8_t> withoutReservedBits(std::vector<std::uint8_t> codes) {
    if (codes.size() < 4) return codes;
    std::size_t const count = codes[2];
    for (std::size_t slot = 0; slot < count && 4 + 2 * slot + 1 < codes.size();) {
        std::uint8_t& opByte = codes[4 + 2 * slot + 1];
        std::uint8_t const op = opByte & 0x0Fu, info = opByte >> 4;
        if (op == 3u) opByte = 3u;   // UWOP_SET_FPREG: the reserved operation info
        std::size_t const nodes = op == 1u ? (info == 0u ? 2u : 3u)        // UWOP_ALLOC_LARGE
                                : op == 4u || op == 8u ? 2u               // UWOP_SAVE_NONVOL, UWOP_SAVE_XMM128
                                : op == 5u || op == 9u ? 3u               // the _FAR forms
                                : 1u;
        slot += nodes;
    }
    return codes;
}

[[nodiscard]] std::string hex(std::vector<std::uint8_t> const& v) {
    static char const* const d = "0123456789abcdef";
    std::string              s;
    for (auto x : v) {
        s.push_back(d[x >> 4]);
        s.push_back(d[x & 15]);
    }
    return s;
}

constexpr char const* kForms[] = {"win64_unwind_forms_x86_64_pe_cl_O2.obj", "win64_unwind_forms_x86_64_pe_cl_Od.obj",
                                  "win64_unwind_forms_x86_64_pe_clangcl_O2.obj",
                                  "win64_unwind_forms_x86_64_pe_gcc_O2.obj"};
constexpr char const* kTries[] = {"win64_unwind_try_x86_64_pe_cl_O2.obj", "win64_unwind_try_x86_64_pe_cl_Od.obj",
                                  "win64_unwind_try_x86_64_pe_clangcl_O2.obj"};
constexpr char const* kHuge[]  = {"win64_unwind_huge_x86_64_pe_cl_O2.obj", "win64_unwind_huge_x86_64_pe_cl_Od.obj",
                                  "win64_unwind_huge_x86_64_pe_clangcl_O2.obj",
                                  "win64_unwind_huge_x86_64_pe_gcc_O2.obj"};

[[nodiscard]] std::vector<std::string> everyFixture() {
    std::vector<std::string> all(std::begin(kForms), std::end(kForms));
    all.insert(all.end(), std::begin(kTries), std::end(kTries));
    all.insert(all.end(), std::begin(kHuge), std::end(kHuge));
    return all;
}

// The operations an UNWIND_INFO's codes state, each with its operation info: "<op>/<info>" in array order, read by
// the test's own walk of the slots.
[[nodiscard]] std::vector<std::string> opsOf(std::vector<std::uint8_t> const& codes) {
    std::vector<std::string> out;
    std::size_t const        count = codes.size() >= 4 ? codes[2] : 0u;
    for (std::size_t slot = 0; slot < count && 4 + 2 * slot + 1 < codes.size();) {
        std::uint8_t const op = codes[4 + 2 * slot + 1] & 0x0Fu, info = codes[4 + 2 * slot + 1] >> 4;
        out.push_back(std::to_string(op) + "/" + std::to_string(info));
        slot += op == 1u ? (info == 0u ? 2u : 3u) : op == 4u || op == 8u ? 2u : op == 5u || op == 9u ? 3u : 1u;
    }
    return out;
}

[[nodiscard]] std::string nameOf(AssembledModule const& m, SymbolId id) {
    for (auto const& s : m.symbols) {
        if (s.symbol == id && !s.name.empty()) return s.name;
    }
    for (auto const& e : m.externImports) {
        if (e.symbol == id) return e.mangledName;
    }
    return "#" + std::to_string(id.v);
}

}  // namespace

// ══ THE CARRY ════════════════════════════════════════════════════════════════════════════════════════════════════════

// Every function a RUNTIME_FUNCTION describes arrives with its frame, and every handler with its routine. Before the
// carry each of these was `nullopt`, with no diagnostic.
TEST(CoffForeignUnwind, EveryDescribedFunctionOfAReferenceObjectArrivesWithItsFrameAndHandler) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    for (auto const& name : everyFixture()) {
        SCOPED_TRACE(name);
        auto const bytes = fixture(name);
        ASSERT_FALSE(bytes.empty());
        auto const expected = unwindOf(Coff::of(bytes));
        ASSERT_FALSE(expected.empty()) << "the fixture describes its functions";
        DiagnosticReporter rep;
        auto const         mod = read(l, bytes, rep);
        ASSERT_TRUE(mod.has_value()) << diagnosticsOf(rep);
        std::size_t framed = 0, handled = 0;
        for (auto const& fn : mod->functions) {
            if (!fn.cfi.has_value()) continue;
            ++framed;
            EXPECT_EQ(fn.cfi->codeLength, fn.bytes.size()) << nameOf(*mod, fn.symbol)
                << ": the description covers the function's bytes";
            if (!fn.languageHandler.has_value()) continue;
            ++handled;
            std::string const routine = nameOf(*mod, fn.languageHandler->personality);
            auto const        it      = expected.find(nameOf(*mod, fn.symbol) + "+0");
            ASSERT_NE(it, expected.end()) << nameOf(*mod, fn.symbol);
            EXPECT_EQ(routine, it->second.handler) << nameOf(*mod, fn.symbol) << ": the routine its object named";
            EXPECT_EQ(fn.languageHandler->data.size(), it->second.data.size()) << nameOf(*mod, fn.symbol);
            EXPECT_EQ(fn.languageHandler->relocations.size(), it->second.addresses.size()) << nameOf(*mod, fn.symbol);
        }
        std::size_t expectedHandled = 0;
        for (auto const& [fn, u] : expected) expectedHandled += u.handler.empty() ? 0u : 1u;
        EXPECT_EQ(framed, expected.size()) << "every RUNTIME_FUNCTION describes one function";
        EXPECT_EQ(handled, expectedHandled) << "every handler arrives with its function";
    }
}

// THE ROUND TRIP: what the object says, read and written back by the shipped writer, is what it said — every code
// byte for byte, every handler naming the same routine, its data the same bytes and its addresses the same places.
// The encoder's forms are all here: UWOP_PUSH_NONVOL (every fixture), UWOP_SET_FPREG with a frame offset (cl's and
// gcc's `dyn`), saves measured from the frame register's base (cl /O2's `dyn`), and the long spellings of the
// allocation and of both saves (the `huge` fixtures; `TheHugeFixturesStateEveryLongSpelling` holds them to it).
TEST(CoffForeignUnwind, ReferenceUnwindInfoSurvivesTheShippedWriterByteForByte) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    for (auto const& name : everyFixture()) {
        SCOPED_TRACE(name);
        auto const bytes = fixture(name);
        ASSERT_FALSE(bytes.empty());
        DiagnosticReporter rep;
        auto               mod = read(l, bytes, rep);
        ASSERT_TRUE(mod.has_value()) << diagnosticsOf(rep);
        // The relocatable writer names an INTERNAL-LINKAGE function `sym_<id>`, by design
        // (D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION; `ObjectSymbolNames::definedName`): cl's filter
        // funclets (`trygs$filt$0`, class STATIC) come back under that name, so the written object is read by it.
        std::map<std::string, std::string> writtenAs;
        for (auto const& s : mod->symbols) {
            if (s.binding != SymbolBinding::Local || s.name.empty()) continue;
            for (auto const& f : mod->functions) {
                if (f.symbol == s.symbol) writtenAs[s.name] = "sym_" + std::to_string(s.symbol.v);
            }
        }
        std::vector<AssembledModule> one;
        one.push_back(std::move(*mod));
        LinkedImage const written = linker::link(std::span<AssembledModule const>{one}, *l.target, *l.object, rep);
        ASSERT_TRUE(written.ok()) << "the shipped writer takes back what the reader read:" << diagnosticsOf(rep);
        auto const before = unwindOf(Coff::of(bytes));
        auto const after  = unwindOf(Coff::of(written.bytes));
        EXPECT_EQ(after.size(), before.size()) << "one description per described function, before and after";
        for (auto const& [fn, was] : before) {
            std::string key  = fn;
            auto const  plus = fn.rfind('+');
            if (auto const w = writtenAs.find(fn.substr(0, plus)); w != writtenAs.end()) {
                key = w->second + fn.substr(plus);
            }
            auto const it = after.find(key);
            ASSERT_NE(it, after.end()) << fn << " (written as " << key << ") lost its description";
            EXPECT_EQ(hex(withoutReservedBits(it->second.codes)), hex(withoutReservedBits(was.codes)))
                << fn << ": the UNWIND_INFO's header and codes";
            EXPECT_EQ(it->second.handler, was.handler) << fn << ": the handler's routine";
            EXPECT_EQ(hex(it->second.data), hex(was.data)) << fn << ": the handler's data";
            std::vector<std::string> wasWritten;   // "<offset>:<function>+<residual>", the function by its written name
            for (auto const& a : was.addresses) {
                auto const colon = a.find(':');
                auto const plus  = a.rfind('+');
                auto const w     = writtenAs.find(a.substr(colon + 1, plus - colon - 1));
                wasWritten.push_back(w == writtenAs.end() ? a : a.substr(0, colon + 1) + w->second + a.substr(plus));
            }
            EXPECT_EQ(it->second.addresses, wasWritten) << fn << ": the addresses the handler's data holds";
        }
    }
}

// The round trip above is only as strong as the spellings its fixtures hold: each long spelling is in the `huge`
// objects, where the references wrote it (✔MEASURED P69 fold 3d at the build, llvm-readobj 19.1.5 `--unwind`).
TEST(CoffForeignUnwind, TheHugeFixturesStateEveryLongSpelling) {
    std::map<std::string, std::vector<std::string>> holders;   // a spelling -> the fixtures stating it
    for (auto const* name : kHuge) {
        SCOPED_TRACE(name);
        auto const bytes = fixture(name);
        ASSERT_FALSE(bytes.empty());
        auto const u = unwindOf(Coff::of(bytes));
        EXPECT_EQ(u.size(), 3u) << "huge, hugeregs and hugexmm, each described";
        std::set<std::string> here;
        for (auto const& [fn, f] : u) {
            for (auto const& op : opsOf(f.codes)) {
                if (op == "1/1") here.insert("UWOP_ALLOC_LARGE op-info 1");
                if (op.rfind("5/", 0) == 0) here.insert("UWOP_SAVE_NONVOL_FAR");
                if (op.rfind("9/", 0) == 0) here.insert("UWOP_SAVE_XMM128_FAR");
            }
        }
        for (auto const& s : here) holders[s].push_back(name);
    }
    using V = std::vector<std::string>;
    EXPECT_EQ(holders["UWOP_ALLOC_LARGE op-info 1"], V(std::begin(kHuge), std::end(kHuge)))
        << "every reference allocates a frame past 512 KiB - 8 in the long spelling";
    EXPECT_EQ(holders["UWOP_SAVE_NONVOL_FAR"], V{"win64_unwind_huge_x86_64_pe_cl_O2.obj"})
        << "cl /O2 saves rbx in its caller's home slot, 700128 bytes above the establisher frame";
    EXPECT_EQ(holders["UWOP_SAVE_XMM128_FAR"],
              (V{"win64_unwind_huge_x86_64_pe_cl_O2.obj", "win64_unwind_huge_x86_64_pe_clangcl_O2.obj",
                 "win64_unwind_huge_x86_64_pe_gcc_O2.obj"}))
        << "every optimising reference saves a vector register past 1 MiB";
}

// A long spelling means what its short one means, in bytes: the rules read from the codes are the frame the machine
// code builds. ✔MEASURED P69 fold 3d (llvm-objdump of the cl /O2 object): `hugeregs` stores rbx at entry with
// `movq %rbx, 0x18(%rsp)` — 16 above the CFA — then pushes seven registers and subtracts 0xAAE90 (700048);
// `hugexmm` pushes rbx, subtracts 0x125010 (1200144) and stores xmm6 to xmm11 at 0x125000 down to 0x124FB0 above the
// new stack pointer, 32 to 112 below the CFA. gcc's `huge` subtracts 700040 and pushes nothing.
TEST(CoffForeignUnwind, ALongSpellingDecodesToTheFrameItsCodeBuilds) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    struct Rule {
        std::uint32_t pc;
        char const*   reg;   // null: the CFA's offset
        std::int64_t  offset;
    };
    struct Case {
        char const*       object;
        char const*       function;
        std::vector<Rule> rules;
    };
    std::vector<Case> const cases{
        {"win64_unwind_huge_x86_64_pe_cl_O2.obj", "hugeregs",
         {{6, nullptr, 16}, {6, "rbp", -16}, {7, nullptr, 24}, {7, "rsi", -24}, {8, nullptr, 32}, {8, "rdi", -32},
          {10, nullptr, 40}, {10, "r12", -40}, {12, nullptr, 48}, {12, "r13", -48}, {14, nullptr, 56},
          {14, "r14", -56}, {16, nullptr, 64}, {16, "r15", -64}, {29, nullptr, 700112}, {29, "rbx", 16}}},
        {"win64_unwind_huge_x86_64_pe_cl_O2.obj", "hugexmm",
         {{2, nullptr, 16}, {2, "rbx", -16}, {15, nullptr, 1200160}, {23, "xmm6", -32}, {31, "xmm7", -48},
          {40, "xmm8", -64}, {49, "xmm9", -80}, {58, "xmm10", -96}, {67, "xmm11", -112}}},
        {"win64_unwind_huge_x86_64_pe_gcc_O2.obj", "huge", {{13, nullptr, 700048}}},
    };
    for (auto const& c : cases) {
        SCOPED_TRACE(std::string{c.object} + " " + c.function);
        auto const         bytes = fixture(c.object);
        DiagnosticReporter rep;
        auto const         mod = read(l, bytes, rep);
        ASSERT_TRUE(mod.has_value()) << diagnosticsOf(rep);
        AssembledFunction const* fn = nullptr;
        for (auto const& f : mod->functions) {
            if (nameOf(*mod, f.symbol) == c.function) fn = &f;
        }
        ASSERT_NE(fn, nullptr);
        ASSERT_TRUE(fn->cfi.has_value());
        ASSERT_EQ(fn->cfi->ops.size(), c.rules.size()) << "one rule per code's statement";
        for (std::size_t k = 0; k < c.rules.size(); ++k) {
            auto const& got = fn->cfi->ops[k];
            auto const& want = c.rules[k];
            EXPECT_EQ(got.pcOffset, want.pc) << "rule " << k;
            if (want.reg == nullptr) {
                EXPECT_EQ(static_cast<int>(got.kind), static_cast<int>(CfiOpKind::DefCfaOffset)) << "rule " << k;
            } else {
                EXPECT_EQ(static_cast<int>(got.kind), static_cast<int>(CfiOpKind::RegAtCfaOffset)) << "rule " << k;
                auto const r = l.target->registerByName(want.reg);
                ASSERT_TRUE(r.has_value()) << want.reg;
                EXPECT_TRUE(got.reg == CfiRegRef::physical(*r)) << "rule " << k << ": " << want.reg;
            }
            EXPECT_EQ(got.offset, want.offset) << "rule " << k;
        }
    }
}

// The decode and the encode are inverses on every frame form, read back through the reader: each frame below is
// written by the shipped object writer and read again, and its rules come back as they went in.
TEST(CoffForeignUnwind, EveryFrameFormDecodesBackToTheRulesItWasWrittenFrom) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    auto const reg = [&](char const* n) {
        auto const o = l.target->registerByName(n);
        EXPECT_TRUE(o.has_value()) << n;
        return CfiRegRef::physical(o.value_or(0));
    };
    auto const sp = l.target->registerByName("rsp");
    ASSERT_TRUE(sp.has_value());
    CfiInitialState const entry{*sp, 8, -8, std::nullopt};
    auto const def    = [](std::uint32_t pc, std::int64_t c) { return CfiOp{pc, CfiOpKind::DefCfaOffset, {}, {}, c, 0u}; };
    auto const at     = [](std::uint32_t pc, CfiRegRef r, std::int64_t o) {
        return CfiOp{pc, CfiOpKind::RegAtCfaOffset, r, {}, o, 0u};
    };
    struct Frame {
        char const*        name;
        std::uint32_t      prologueEnd;
        std::vector<CfiOp> ops;
    };
    std::vector<Frame> const frames{
        // Two pushes and an allocation: UWOP_PUSH_NONVOL twice, then UWOP_ALLOC_SMALL.
        {"pushes", 6, {def(1, 16), at(1, reg("rbp"), -16), def(2, 24), at(2, reg("rbx"), -24), def(6, 64)}},
        // A frame register set 32 bytes above the stack pointer, and a save after it, measured from its base.
        {"framed", 14, {def(1, 16), at(1, reg("rbp"), -16), def(5, 64),
                        CfiOp{10, CfiOpKind::DefCfa, reg("rbp"), {}, 32, 0u}, at(14, reg("rbx"), 0)}},
        // A save stated before the allocation it is measured after (the caller's home area).
        {"homesave", 10, {at(5, reg("rbx"), 0), def(6, 16), at(6, reg("rdi"), -16), def(10, 48)}},
        // A large allocation and a vector save.
        {"large", 17, {def(1, 16), at(1, reg("rsi"), -16), def(8, 224), at(17, reg("xmm15"), -32)}},
        // An allocation of one slot that saves nothing (clang's `push rax`): no push.
        {"oneslot", 3, {def(1, 16), at(1, reg("rbp"), -16), def(3, 24)}},
        // Past every short spelling's reach: an allocation of 700000 (UWOP_ALLOC_LARGE op-info 1), a general
        // register 700032 above the establisher frame (UWOP_SAVE_NONVOL_FAR) and a vector register 1200000 above it
        // (UWOP_SAVE_XMM128_FAR).
        {"hugealloc", 8, {def(1, 16), at(1, reg("rbp"), -16), def(8, 700016)}},
        {"farsave", 12, {def(1, 16), at(1, reg("rbp"), -16), def(8, 700016), at(12, reg("rbx"), 16)}},
        {"farvector", 17, {def(8, 1200016), at(17, reg("xmm6"), -16)}},
        // A general register at a slot no multiple of 8 can name: the short spelling cannot state it, the long one
        // states it exactly.
        {"oddslot", 9, {def(4, 48), at(9, reg("rbx"), -44)}},
    };
    AssembledModule mod;
    mod.cuId = CompilationUnitId{1};
    for (std::size_t i = 0; i < frames.size(); ++i) {
        AssembledFunction fn;
        fn.symbol = SymbolId{static_cast<std::uint32_t>(i + 1)};
        fn.bytes.assign(64, 0x90);
        fn.bytes.back() = 0xC3;
        CfiFunction cfi;
        cfi.codeLength    = 64;
        cfi.initial       = entry;
        cfi.prologueEndPc = frames[i].prologueEnd;
        cfi.ops           = frames[i].ops;
        fn.cfi            = std::move(cfi);
        mod.functions.push_back(std::move(fn));
        mod.symbols.push_back(ModuleSymbol{SymbolId{static_cast<std::uint32_t>(i + 1)}, frames[i].name,
                                           SymbolBinding::Global, SymbolVisibility::Default});
    }
    mod.expectedFuncCount = mod.functions.size();
    DiagnosticReporter           rep;
    std::vector<AssembledModule> one{mod};
    LinkedImage const written = linker::link(std::span<AssembledModule const>{one}, *l.target, *l.object, rep);
    ASSERT_TRUE(written.ok()) << diagnosticsOf(rep);
    auto const back = read(l, written.bytes, rep);
    ASSERT_TRUE(back.has_value()) << diagnosticsOf(rep);
    for (auto const& f : frames) {
        SCOPED_TRACE(f.name);
        AssembledFunction const* got = nullptr;
        for (auto const& fn : back->functions) {
            if (nameOf(*back, fn.symbol) == f.name) got = &fn;
        }
        ASSERT_NE(got, nullptr);
        ASSERT_TRUE(got->cfi.has_value());
        EXPECT_EQ(got->cfi->prologueEndPc, std::optional<std::uint32_t>{f.prologueEnd});
        EXPECT_TRUE(got->cfi->initial == entry);
        ASSERT_EQ(got->cfi->ops.size(), f.ops.size()) << "the rules came back as many as went in";
        for (std::size_t k = 0; k < f.ops.size(); ++k) {
            EXPECT_EQ(got->cfi->ops[k].pcOffset, f.ops[k].pcOffset) << "rule " << k;
            EXPECT_EQ(static_cast<int>(got->cfi->ops[k].kind), static_cast<int>(f.ops[k].kind)) << "rule " << k;
            EXPECT_TRUE(got->cfi->ops[k].reg == f.ops[k].reg) << "rule " << k;
            EXPECT_EQ(got->cfi->ops[k].offset, f.ops[k].offset) << "rule " << k;
        }
    }
}

// The format's declared personality is imported from the image its role names, as a DSS `__try`'s is; another
// routine stays a name for the link to resolve.
TEST(CoffForeignUnwind, TheDeclaredPersonalityIsImportedFromItsRolesImage) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    auto const& declared = l.object->sehPersonality();
    ASSERT_TRUE(declared.has_value());
    ASSERT_FALSE(declared->libraryPath.empty());
    {
        DiagnosticReporter rep;
        auto const         bytes = fixture("win64_unwind_try_x86_64_pe_cl_O2.obj");
        auto const         mod   = read(l, bytes, rep);
        ASSERT_TRUE(mod.has_value()) << diagnosticsOf(rep);
        bool found = false;
        for (auto const& e : mod->externImports) {
            if (e.mangledName != declared->mangledName) continue;
            found = true;
            EXPECT_EQ(e.libraryPath, declared->libraryPath) << "the routine is imported from its role's image";
        }
        EXPECT_TRUE(found);
    }
    {
        DiagnosticReporter rep;
        auto const         bytes = fixture("win64_unwind_forms_x86_64_pe_cl_O2.obj");
        auto const         mod   = read(l, bytes, rep);
        ASSERT_TRUE(mod.has_value()) << diagnosticsOf(rep);
        bool found = false;
        for (auto const& e : mod->externImports) {
            if (e.mangledName != "__GSHandlerCheck") continue;
            found = true;
            EXPECT_TRUE(e.libraryPath.empty()) << "a routine the document does not declare stays a name";
        }
        EXPECT_TRUE(found);
    }
}

// ══ THE REFUSALS — each on the REAL object's bytes, edited at one place, and each asserting its sentence ══════════════

namespace {

// The byte offset in the file of the UNWIND_INFO the RUNTIME_FUNCTION of `function` names.
[[nodiscard]] std::optional<std::size_t> unwindInfoOf(Coff const& c, std::string_view function) {
    for (auto const& s : c.secs) {
        if (s.name != ".pdata") continue;
        std::map<std::uint32_t, Coff::Rel> byVa;
        for (auto const& r : c.rels(s)) byVa[r.va] = r;
        for (std::uint32_t e = 0; e + 12u <= s.size; e += 12u) {
            auto const& begin = c.syms[byVa.at(e).sym];
            if (c.functionAt(begin.section, begin.value + c.u32(s.ptr + e)) != std::string{function} + "+0") continue;
            auto const& info = c.syms[byVa.at(e + 8u).sym];
            return c.secs[static_cast<std::size_t>(info.section - 1)].ptr + info.value + c.u32(s.ptr + e + 8u);
        }
    }
    return std::nullopt;
}

}  // namespace

TEST(CoffForeignUnwind, AFormNoReferenceWroteIsRefusedByName) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    auto const original = fixture("win64_unwind_forms_x86_64_pe_cl_O2.obj");
    ASSERT_FALSE(original.empty());
    Coff const c = Coff::of(original);
    auto const big = unwindInfoOf(c, "big");      // `@15 ALLOC_LARGE`, `@2 PUSH_NONVOL rbx`
    ASSERT_TRUE(big.has_value());
    ASSERT_EQ(original[*big + 5] & 0x0F, 1) << "the fixture's first code of `big` is UWOP_ALLOC_LARGE";
    struct Edit {
        char const* what;
        std::size_t at;
        std::uint8_t keep, set;   // byte = (byte & keep) | set
        char const* sentence;
    };
    // The long spellings of UWOP_ALLOC_LARGE and of both saves are READ (every reference writes them past the short
    // ones' reach: `TheHugeFixturesStateEveryLongSpelling`); an operation info UWOP_ALLOC_LARGE does not define is a
    // corrupt record, refused as one.
    std::vector<Edit> const edits{
        {"version 2", *big, 0xF8, 0x02, "is version 2"},
        {"chained", *big, 0xFF, 0x04 << 3, "is chained to another function's"},
        {"UWOP_PUSH_MACHFRAME", *big + 5, 0xF0, 10, "states UWOP_PUSH_MACHFRAME"},
        {"UWOP_ALLOC_LARGE op-info 2", *big + 5, 0x0F, 0x20,
         "states UWOP_ALLOC_LARGE with op-info 2; the format defines 0"},
    };
    for (auto const& e : edits) {
        SCOPED_TRACE(e.what);
        auto bytes = original;
        bytes[e.at] = static_cast<std::uint8_t>((bytes[e.at] & e.keep) | e.set);
        DiagnosticReporter rep;
        EXPECT_FALSE(read(l, bytes, rep).has_value()) << "a form no reference wrote is refused, never guessed at";
        EXPECT_TRUE(said(rep, e.sentence)) << diagnosticsOf(rep);
    }
}

// A relocation of the two sections that no record this reader reads accounts for is refused by name: here `guarded`'s
// handler flags are cleared, so its handler field and its scope table's addresses belong to nothing.
TEST(CoffForeignUnwind, ARelocationOfTheUnwindSectionsNoRecordReadsIsRefused) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    auto bytes = fixture("win64_unwind_try_x86_64_pe_cl_O2.obj");
    ASSERT_FALSE(bytes.empty());
    auto const at = unwindInfoOf(Coff::of(bytes), "guarded");
    ASSERT_TRUE(at.has_value());
    ASSERT_NE(bytes[*at] >> 3, 0) << "the fixture's `guarded` names a handler";
    bytes[*at] = static_cast<std::uint8_t>(bytes[*at] & 0x07u);
    DiagnosticReporter rep;
    EXPECT_FALSE(read(l, bytes, rep).has_value());
    EXPECT_TRUE(said(rep, "is part of no RUNTIME_FUNCTION and of no handler this reader reads")) << diagnosticsOf(rep);
}

// A RUNTIME_FUNCTION that runs past its function would describe another function's bytes, and is refused. (One that
// stops short is read: the references pad past it — gcc to the section's alignment, cl /Od further, with `int3` — and
// the carried description covers the function whole; the gcc and cl /Od fixtures hold such functions.)
TEST(CoffForeignUnwind, ARecordThatRunsPastItsFunctionIsRefused) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    auto bytes = fixture("win64_unwind_try_x86_64_pe_cl_O2.obj");
    ASSERT_FALSE(bytes.empty());
    Coff const c     = Coff::of(bytes);
    auto const pdata = std::find_if(c.secs.begin(), c.secs.end(), [](Coff::Sec const& s) { return s.name == ".pdata"; });
    ASSERT_NE(pdata, c.secs.end());
    std::size_t const   endField = pdata->ptr + 4u;   // the first record's EndAddress: its addend, in place
    std::uint32_t const past     = c.u32(endField) + 64u;
    for (std::size_t k = 0; k < 4; ++k) bytes[endField + k] = static_cast<std::uint8_t>(past >> (8 * k));
    DiagnosticReporter rep;
    EXPECT_FALSE(read(l, bytes, rep).has_value()) << "a record past its function's end is refused";
    EXPECT_TRUE(said(rep, "a record past its function's end describes another function's bytes")) << diagnosticsOf(rep);
}

// A handler comes in two shapes, one per unwind format (`eh_frame_handler_refusal.hpp`), and each writer refuses, by
// name and before any byte, the shape it cannot state: the `.eh_frame` writers a handler whose data its object wrote
// after the UNWIND_INFO (an `.eh_frame` names a handler's data only by a pointer to a data area), the PE writer a
// handler that names its data by such a pointer (UNWIND_INFO carries it inline) -- and the PE writer a function that
// carries both a foreign handler and scopes of its own.
TEST(CoffForeignUnwind, AWriterThatCannotStateAHandlerRefusesItByName) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    auto const sp = l.target->registerByName("rsp");
    ASSERT_TRUE(sp.has_value());
    auto const handled = [&](bool withScope) {
        AssembledModule m;
        m.cuId = CompilationUnitId{1};
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        fn.bytes  = {0x48, 0x83, 0xEC, 0x28, 0x90, 0x48, 0x83, 0xC4, 0x28, 0xC3};
        CfiFunction cfi;
        cfi.codeLength    = 10;
        cfi.initial       = CfiInitialState{*sp, 8, -8, std::nullopt};
        cfi.prologueEndPc = 4;
        cfi.ops           = {CfiOp{4, CfiOpKind::DefCfaOffset, {}, {}, 48, 0u}};
        fn.cfi            = std::move(cfi);
        LanguageHandler h;
        h.personality    = SymbolId{2};
        h.exceptionPhase = true;
        h.data           = {0x00, 0x00, 0x00, 0x00};
        fn.languageHandler = std::move(h);
        if (withScope) {
            SehScopeEntry sc;
            sc.beginByteOffset      = 4;
            sc.endByteOffset        = 5;
            sc.jumpTargetByteOffset = 5;
            sc.filterFuncletSymbol  = SymbolId{1};
            sc.personalitySymbol    = SymbolId{2};
            fn.sehScopes.push_back(sc);
        }
        m.functions.push_back(std::move(fn));
        m.expectedFuncCount = 1;
        m.symbols.push_back(ModuleSymbol{SymbolId{1}, "handled", SymbolBinding::Global, SymbolVisibility::Default});
        ExternImport routine;
        routine.symbol      = SymbolId{2};
        routine.mangledName = "routine";
        m.externImports.push_back(routine);
        return m;
    };
    for (char const* format : {"elf64-x86_64-linux", "macho64-x86_64-darwin"}) {
        SCOPED_TRACE(format);
        auto const f = ObjectFormatSchema::loadShipped(format);
        ASSERT_TRUE(f.has_value());
        DiagnosticReporter           rep;
        std::vector<AssembledModule> one{handled(false)};
        LinkedImage const            img = linker::link(std::span<AssembledModule const>{one}, *l.target, **f, rep);
        EXPECT_FALSE(img.ok());
        EXPECT_TRUE(said(rep, "carries a handler whose data its object wrote after the function's Win64 unwind "
                              "information (4 byte(s), 0 relocation(s))"))
            << diagnosticsOf(rep);
    }
    {
        DiagnosticReporter           rep;
        std::vector<AssembledModule> one{handled(true)};
        LinkedImage const img = linker::link(std::span<AssembledModule const>{one}, *l.target, *l.object, rep);
        EXPECT_FALSE(img.ok());
        EXPECT_TRUE(said(rep, "carries a foreign object's handler too")) << diagnosticsOf(rep);
    }
    // The DWARF shape: no inline data, a pointer to an area in a data item of the link.
    AssembledModule dwarf = handled(false);
    dwarf.functions[0].languageHandler->data.clear();
    dwarf.functions[0].languageHandler->dataArea = DataAreaRef{SymbolId{3}, 2};
    AssembledData area;
    area.symbol  = SymbolId{3};
    area.section = DataSectionKind::Rodata;
    area.bytes   = {0x00, 0x00, 0xff, 0x9b, 0x01, 0x00};
    dwarf.dataItems.push_back(std::move(area));
    DiagnosticReporter           rep;
    std::vector<AssembledModule> one{std::move(dwarf)};
    LinkedImage const            img = linker::link(std::span<AssembledModule const>{one}, *l.target, *l.object, rep);
    EXPECT_FALSE(img.ok()) << "a handler naming its data by a pointer was written into UNWIND_INFO";
    EXPECT_TRUE(said(rep, "names its data by a pointer to a language-specific data area, and UNWIND_INFO carries a "
                          "handler's data only inline"))
        << diagnosticsOf(rep);
}

// ══ THE LINK: a foreign `__try` object and a C `main`, through the production driver ═══════════════════════════════

namespace {

constexpr char const* kMainBoth =
    "int guarded(int volatile *); int guarded_call(int volatile *);\n"
    "int deref(int volatile *p) { return *p; }\n"
    "int main(void) { int volatile v = 20;\n"
    "  if (guarded(&v) != 20 || guarded_call(&v) != 20) return 90;\n"
    "  if (guarded((int volatile *)0) != -1) return 11;\n"
    "  return guarded_call((int volatile *)0) == -1 ? 42 : 12; }\n";
// clang-cl's `guarded` carries no handler (the fixture's source says why): its object is run through `guarded_call`.
constexpr char const* kMainCallOnly =
    "int guarded_call(int volatile *);\n"
    "int deref(int volatile *p) { return *p; }\n"
    "int main(void) { int volatile v = 20;\n"
    "  if (guarded_call(&v) != 20) return 90;\n"
    "  return guarded_call((int volatile *)0) == -1 ? 42 : 12; }\n";
constexpr char const* kExit =
    "42 is each fault dispatched to the object's `__except`; 90 an unguarded read miscomputed; 11 or 12 a handler ran "
    "and returned the wrong value; 0xC0000005 a fault escaped: the image carries no scope table the OS found";

// DSS links `name` beside `main`; the image's path, or empty after a failed assertion.
[[nodiscard]] fs::path dssImage(fs::path const& dir, std::string_view name, char const* main) {
    auto const bytes = fixture(name);
    if (bytes.empty()) return {};
    auto const obj = dir / std::string{name};
    std::ofstream(obj, std::ios::binary).write(reinterpret_cast<char const*>(bytes.data()),
                                               static_cast<std::streamsize>(bytes.size()));
    auto const src = dir / "main.c";
    std::ofstream(src, std::ios::binary) << main;
    auto const out = dir / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    DiagnosticReporter rep;
    int const          rc = p.compileUnits(std::vector<std::string>{obj.string(), src.string()}, "c",
                                           std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
    EXPECT_EQ(rc, 0) << "DSS links the foreign `__try` object:" << diagnosticsOf(rep);
    if (rc != 0) return {};
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file() && e.path().extension() == ".exe") return e.path();
    }
    ADD_FAILURE() << "no image in " << out.string();
    return {};
}

}  // namespace

// Every leg: the link is made (the routine imported, the scope tables' addresses bound across the merge).
TEST(CoffForeignUnwind, DssLinksAReferenceTryObjectBesideACMain) {
    for (auto const* name : kTries) {
        SCOPED_TRACE(name);
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-foreign-unwind"};
        bool const               callOnly = std::string_view{name}.find("clangcl") != std::string_view::npos;
        EXPECT_FALSE(dssImage(scratch.path(), name, callOnly ? kMainCallOnly : kMainBoth).empty());
    }
}

// Windows x86_64: the image runs, and every fault reaches the object's `__except` (link.exe's image of the same
// object runs to 42, MEASURED at the fixtures' build).
TEST(CoffForeignUnwindNative, AReferenceTryObjectLinkedByDssCatchesItsFault) {
    if (test_support::hostNativeTarget().execTarget != "x86_64:pe64-x86_64-windows-exec") {
        GTEST_SKIP() << "the image runs only where the host is Windows x86_64";
    }
    for (auto const* name : kTries) {
        SCOPED_TRACE(name);
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-foreign-unwind-run"};
        bool const               callOnly = std::string_view{name}.find("clangcl") != std::string_view::npos;
        auto const               image    = dssImage(scratch.path(), name, callOnly ? kMainCallOnly : kMainBoth);
        ASSERT_FALSE(image.empty());
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << kExit;
    }
}

// ══ A FILTER THAT IS AN EXPRESSION: cl's funclet in a section that goes with its function ═══════════════════════════
//
// `tests/link/data/win64_unwind_filter.source.c` (it states how each object was built): `cs_trygs`'s `__except`
// filter is an EXPRESSION, so cl writes it as a function of its own, the funclet `cs_trygs$filt$0` (a STATIC
// DTYPE_FUNCTION), and the scope table names it by ADDR32NB. Under /O2 and under /Gy the funclet's `.text$x` is a
// COMDAT of selection ASSOCIATIVE to `cs_trygs`'s NODUPLICATES `.text$mn` -- a C object reaching ASSOCIATIVE with no
// C++ in sight. ✔MEASURED P69 fold 3d (lane `xa`'s `seh_filter_probe.py`, cl 19.51, 2026-10-29): the objects of
// /O2 /GS-, /O2 /GS, /Od /GS- and /Od /Gy /GS-, each linked by link.exe beside a cl `main` and by DSS from objects
// alone and beside a C `main`, run to 42 -- the filter taking 0xE0000002 and declining 0xE0000003 to an outer
// `__except`. Until P69 fold 3d the reader refused the /O2 and /Gy objects (checkpoint 35's Gate 3, "has COMDAT
// Selection 5"); lane `hm`'s census object of the same shape (cl /O2 /GS /MD) was refused so.
//
//   win64_unwind_filter_x86_64_pe_cl_O2.obj    2190 B  md5 ddad35b486f36f585d4dbd4dcb07c2f9   cl 19.51 /O2 /GS /MD
//   win64_unwind_filter_x86_64_pe_cl_OdGy.obj  2752 B  md5 4b548680aed546dbe42364c65a0f9716   cl 19.51 /Od /Gy /GS- /MD

namespace {

constexpr char const* kFilters[] = {"win64_unwind_filter_x86_64_pe_cl_O2.obj",
                                    "win64_unwind_filter_x86_64_pe_cl_OdGy.obj"};
constexpr char const* kFilterMain =
    "int cs_trygs(unsigned code);\nint cs_trygs_outer(void);\n"
    "int main(void) { return cs_trygs(0xE0000002u) + cs_trygs_outer(); }\n";
constexpr char const* kFilterExit =
    "42 is the filter RUN, taking 0xE0000002 to its handler (40) and declining 0xE0000003 to the outer `__except` (2); "
    "80 every exception taken by the inner handler (the filter's answer never read); an exception code 0xE000000x no "
    "handler was reached (the scope table named no filter the dispatcher could call)";

// The symbol id the unit gives the name, or none.
[[nodiscard]] std::optional<SymbolId> symbolNamed(AssembledModule const& m, std::string_view name) {
    for (auto const& s : m.symbols) {
        if (s.name == name) return s.symbol;
    }
    return std::nullopt;
}

}  // namespace

// Every leg: the funclet is a LOCAL function of its own section, a section of its unit in no group (the section it goes
// with is defined once); `cs_trygs`'s scope table names it; and DSS links each object beside a C `main`.
// RED-ON-DISABLE: refuse an associative section rooted in NODUPLICATES (checkpoint 35's reader) -> both objects are
// refused at read.
TEST(CoffForeignUnwind, AFilterFuncletInASectionThatGoesWithItsFunctionIsReadAndLinked) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    for (auto const* name : kFilters) {
        SCOPED_TRACE(name);
        auto const bytes = fixture(name);
        ASSERT_FALSE(bytes.empty());
        DiagnosticReporter rep;
        auto const unit = read(l, bytes, rep);
        ASSERT_TRUE(unit.has_value()) << diagnosticsOf(rep);
        auto const funclet = symbolNamed(*unit, "cs_trygs$filt$0");
        auto const parent  = symbolNamed(*unit, "cs_trygs");
        ASSERT_TRUE(funclet.has_value() && parent.has_value()) << "the reader states the funclet and its function";
        for (auto const& s : unit->symbols) {
            if (s.symbol == *funclet) EXPECT_EQ(s.binding, SymbolBinding::Local) << "the funclet is file-local";
        }
        AssembledFunction const* filter = nullptr;
        AssembledFunction const* owner  = nullptr;
        for (auto const& f : unit->functions) {
            if (f.symbol == *funclet) filter = &f;
            if (f.symbol == *parent) owner = &f;
        }
        ASSERT_NE(filter, nullptr) << "the funclet is a function of the unit";
        ASSERT_NE(owner, nullptr);
        ASSERT_TRUE(filter->inputSection.has_value() && owner->inputSection.has_value());
        EXPECT_NE(filter->inputSection->section, owner->inputSection->section)
            << "the funclet is cut from its own section, the one that goes with its function's";
        EXPECT_TRUE(unit->sectionGroupOfInputSection.empty())
            << "the section it goes with is defined once: no section of the unit is a copy";
        ASSERT_TRUE(owner->languageHandler.has_value()) << "cs_trygs carries its handler and scope table";
        bool const namesTheFilter =
            std::any_of(owner->languageHandler->relocations.begin(), owner->languageHandler->relocations.end(),
                        [&](Relocation const& r) { return r.target == *funclet; });
        EXPECT_TRUE(namesTheFilter) << "the scope table's filter field names the funclet";
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-foreign-unwind-filter"};
        EXPECT_FALSE(dssImage(scratch.path(), name, kFilterMain).empty());
    }
}

// Windows x86_64: the filter RUNS -- each object's image, DSS's beside the C `main` and link.exe's beside a cl
// `main`, raises both exceptions and runs to 42.
TEST(CoffForeignUnwindNative, AFilterFuncletDecidesEachExceptionUnderLinkExeAndDss) {
    if (test_support::hostNativeTarget().execTarget != "x86_64:pe64-x86_64-windows-exec") {
        GTEST_SKIP() << "the image runs only where the host is Windows x86_64";
    }
    test_support::ScratchDir refs{test_support::Location::InsideRepo, "coff-foreign-unwind-filter-refs"};
    auto const msvc = test_support::native_probe::locateMsvcToolchain(refs.path());
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = test_support::native_probe::msvcToolsIn(msvc, refs.path());
    ASSERT_TRUE(tools.ready()) << tools.describe();
    std::ofstream(refs.path() / "main.c", std::ios::binary) << kFilterMain;
    ASSERT_TRUE(tools.run("cl /nologo /c /O2 /GS- main.c /Fomain.obj")) << "cl compiles the C main";
    std::size_t k = 0;
    for (auto const* name : kFilters) {
        SCOPED_TRACE(name);
        {
            test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-foreign-unwind-filter-run"};
            auto const image = dssImage(scratch.path(), name, kFilterMain);
            ASSERT_FALSE(image.empty());
            auto const r = test_support::runBinary(image);
            ASSERT_TRUE(r.spawned) << r.diagnostic;
            EXPECT_FALSE(r.timedOut);
            EXPECT_EQ(r.exitCode, 42u) << "DSS's image: " << kFilterExit;
        }
        auto const bytes = fixture(name);
        ASSERT_FALSE(bytes.empty());
        std::ofstream(refs.path() / name, std::ios::binary)
            .write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        std::string const exe = "filter_" + std::to_string(k++) + ".exe";
        ASSERT_TRUE(tools.run("link /nologo /OUT:" + exe + " main.obj " + name)) << "MEASURED: link.exe links it";
        auto const r = test_support::runBinary(refs.path() / exe);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "link.exe's image: " << kFilterExit;
    }
}

// ══ DSS's OWN FRAMES PAST THE SHORT SPELLINGS — D-WIN64-HUGE-FRAME-ALLOC ═════════════════════════════════════════════
//
// DSS compiles the `huge` fixtures' own source. Its frames pass 512 KiB - 8 as the references' do, and its writer
// states them in the long spelling of UWOP_ALLOC_LARGE; before P69 fold 3d it refused every such frame by name.

namespace {

// `reach` reads through `p`: a null `p` faults inside it, and the fault is dispatched through the frame past 512 KiB
// to the `__except` of the `main` that called it.
constexpr char const* kHugeMain =
    "int huge(int volatile *);\n"
    "int hugeregs(int volatile *, int const *);\n"
    "int hugexmm(int volatile *, double, double, double);\n"
    "int reach(int volatile *p, char *b) { return *p + b[0]; }\n"
    "int use(int x) { return x & 1; }\n"
    "double dsink(double x) { return x * 0.5; }\n"
    "int main(void) {\n"
    "  int volatile v = 20;\n"
    "  int w[8] = {0, 0, 0, 0, 0, 0, 0, 0};\n"
    "  int r = 0;\n"
    "  if (huge(&v) != 20 || hugeregs(&v, w) != 20 || hugexmm(&v, 0.0, 0.0, 0.0) != 20) return 90;\n"
    "  __try { r = huge((int volatile *)0); } __except (1) { r = -1; }\n"
    "  if (r != -1) return 11;\n"
    "  __try { r = hugeregs((int volatile *)0, w); } __except (1) { r = -2; }\n"
    "  if (r != -2) return 12;\n"
    "  __try { r = hugexmm((int volatile *)0, 0.0, 0.0, 0.0); } __except (1) { r = -3; }\n"
    "  return r == -3 ? 42 : 13;\n"
    "}\n";
constexpr char const* kHugeExit =
    "42 is each fault dispatched through its frame past 512 KiB to `main`'s `__except`; 90 a frame miscomputed; 11 to "
    "13 a handler ran and returned the wrong value; 0xC0000005 a fault escaped: the dispatcher could not unwind the "
    "frame past 512 KiB";

// The `huge` fixtures' source, copied into `dir` as `huge.c`.
[[nodiscard]] fs::path hugeSource(fs::path const& dir) {
    auto const bytes = fixture("win64_unwind_huge.source.c");
    auto const src   = dir / "huge.c";
    std::ofstream(src, std::ios::binary).write(reinterpret_cast<char const*>(bytes.data()),
                                               static_cast<std::streamsize>(bytes.size()));
    return src;
}

}  // namespace

// Every leg: the relocatable object DSS writes for the source describes each of its three functions, and each with
// UWOP_ALLOC_LARGE op-info 1 for the whole of its frame past 512 KiB - 8; the reader reads the object back.
TEST(Win64HugeFrame, DssWritesItsOwnFramePastFiveHundredTwelveKiBInTheLongSpelling) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "win64-huge-frame-object"};
    auto const               src = hugeSource(scratch.path());
    auto const               out = scratch.path() / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    DiagnosticReporter rep;
    ASSERT_EQ(p.compileFiles(std::vector<std::string>{src.string()}, "c",
                             std::vector<std::string>{"x86_64:pe64-x86_64-windows"}, rep),
              0)
        << "DSS describes a frame past 512 KiB:" << diagnosticsOf(rep);
    auto const bytes = bytesOf(out / "huge.obj");
    ASSERT_FALSE(bytes.empty());
    auto const u = unwindOf(Coff::of(bytes));
    for (char const* fn : {"huge", "hugeregs", "hugexmm"}) {
        SCOPED_TRACE(fn);
        auto const it = u.find(std::string{fn} + "+0");
        ASSERT_NE(it, u.end()) << "the function is described";
        // The allocation's operand: the two nodes after its code's own, low word first, unscaled.
        auto const&                  c = it->second.codes;
        auto const                   node = [&](std::size_t k) { return static_cast<std::uint32_t>(c[4 + 2 * k] | (c[5 + 2 * k] << 8)); };
        std::optional<std::uint32_t> size;
        for (std::size_t slot = 0; slot < c[2];) {
            std::uint8_t const op = c[5 + 2 * slot] & 0x0Fu, info = c[5 + 2 * slot] >> 4;
            if (op == 1u && info == 1u) size = node(slot + 1) | (node(slot + 2) << 16);
            slot += op == 1u ? (info == 0u ? 2u : 3u) : op == 4u || op == 8u ? 2u : op == 5u || op == 9u ? 3u : 1u;
        }
        ASSERT_TRUE(size.has_value()) << "UWOP_ALLOC_LARGE op-info 1, codes " << hex(c);
        EXPECT_GT(*size, 524280u) << "an allocation the short spelling cannot state";
        EXPECT_EQ(*size % 8u, 0u);
    }
    DiagnosticReporter rrep;
    auto const         l = load();
    ASSERT_TRUE(l.target && l.object);
    EXPECT_TRUE(read(l, bytes, rrep).has_value()) << "the reader reads DSS's own long spellings:" << diagnosticsOf(rrep);
}

// The reference `huge` objects, each built by its own toolchain from the source above (each states how), and the
// routines of the platform runtime each names. ✔MEASURED P69 fold 3d (the first run that linked them,
// 20261022-021409-34892492, when DSS refused each link by exactly these names): cl /O2 names the probe, the /GS
// cookie, its check and the handler its UNWIND_INFO carries (`__GSHandlerCheck`); cl /Od those and
// `__report_rangecheckfailure`; clang-cl /O2 the probe, the cookie and its check; gcc -O2 its probe alone. Since lane
// `hm`'s H5 every one is DEFINED in a pe64 image by the format's compilerSupport runtime
// (D-LK-PE-FOREIGN-OBJECT-STACK-PROBE-ROUTINE-UNDEFINED, D-LK-PE-FOREIGN-OBJECT-GS-COOKIE-ROUTINES-UNDEFINED).
namespace {

constexpr char const* kHugeObjects[] = {
    "win64_unwind_huge_x86_64_pe_cl_O2.obj",
    "win64_unwind_huge_x86_64_pe_cl_Od.obj",
    "win64_unwind_huge_x86_64_pe_clangcl_O2.obj",
    "win64_unwind_huge_x86_64_pe_gcc_O2.obj",
};
std::set<std::string> const kHugePlatformRoutines{"__chkstk",           "___chkstk_ms",     "__security_cookie",
                                                  "__security_check_cookie", "__GSHandlerCheck",
                                                  "__report_rangecheckfailure"};

// DSS's image of the reference object beside `kHugeMain` (a 4 MiB stack: `hugexmm`'s frame passes the 1 MiB
// default), or empty with `rep` holding why.
[[nodiscard]] fs::path hugeObjectImage(fs::path const& dir, char const* object, DiagnosticReporter& rep) {
    auto const bytes = fixture(object);
    if (bytes.empty()) return {};
    auto const obj = dir / object;
    std::ofstream(obj, std::ios::binary).write(reinterpret_cast<char const*>(bytes.data()),
                                               static_cast<std::streamsize>(bytes.size()));
    auto const main = dir / "main.c";
    std::ofstream(main, std::ios::binary) << kHugeMain;
    auto const out = dir / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    p.setStackReserveBytes(std::uint64_t{4} << 20);
    if (p.compileUnits(std::vector<std::string>{obj.string(), main.string()}, "c",
                       std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep)
        != 0) {
        return {};
    }
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file() && e.path().extension() == ".exe") return e.path();
    }
    return {};
}

}  // namespace

// Every leg: each reference `huge` object, linked by DSS beside the same `main`, is READ whole, its long spellings
// included, and LINKS -- every routine of the platform runtime it calls defined in the image, none imported. Until
// H5 the link was refused by name for exactly those routines.
TEST(CoffForeignUnwind, AReferenceHugeObjectLinksWithEveryPlatformRoutineItCallsDefined) {
    for (auto const* object : kHugeObjects) {
        SCOPED_TRACE(object);
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-foreign-huge-link"};
        DiagnosticReporter rep;
        auto const image = hugeObjectImage(scratch.path(), object, rep);
        ASSERT_FALSE(image.empty()) << "DSS links the reference object beside the C `main`:" << diagnosticsOf(rep);
        auto const imported = test_support::peImportedSymbols(bytesOf(image));
        for (auto const& routine : kHugePlatformRoutines) {
            EXPECT_EQ(std::find(imported.begin(), imported.end(), routine), imported.end())
                << routine << " is imported; the compilerSupport runtime defines it in the image";
        }
    }
}

// Windows x86_64: each reference object's program runs, every fault dispatched through the REFERENCE's own frame past
// 512 KiB to `main`'s `__except`, as the DSS-compiled source's program does below.
TEST(CoffForeignUnwindNative, EachReferenceHugeObjectsFaultReachesMainsExcept) {
    if (test_support::hostNativeTarget().execTarget != "x86_64:pe64-x86_64-windows-exec") {
        GTEST_SKIP() << "the image runs only where the host is Windows x86_64";
    }
    for (auto const* object : kHugeObjects) {
        SCOPED_TRACE(object);
        test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-foreign-huge-run"};
        DiagnosticReporter rep;
        auto const image = hugeObjectImage(scratch.path(), object, rep);
        ASSERT_FALSE(image.empty()) << diagnosticsOf(rep);
        auto const r = test_support::runBinary(image);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << kHugeExit;
    }
}

// Windows x86_64: DSS compiles the source beside a `main` that faults through each frame inside a `__try`, and every
// fault reaches its `__except` (the references' own links of the same functions unwind into `main`, MEASURED at the
// fixtures' build). The image asks for a 4 MiB stack: `hugexmm`'s frame passes the 1 MiB default.
TEST(Win64HugeFrameNative, ADssFramePastFiveHundredTwelveKiBUnwindsToItsCallersExcept) {
    if (test_support::hostNativeTarget().execTarget != "x86_64:pe64-x86_64-windows-exec") {
        GTEST_SKIP() << "the image runs only where the host is Windows x86_64";
    }
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "win64-huge-frame-run"};
    auto const               src  = hugeSource(scratch.path());
    auto const               main = scratch.path() / "main.c";
    std::ofstream(main, std::ios::binary) << kHugeMain;
    auto const out = scratch.path() / "dss.out";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    p.setStackReserveBytes(std::uint64_t{4} << 20);
    DiagnosticReporter rep;
    ASSERT_EQ(p.compileUnits(std::vector<std::string>{src.string(), main.string()}, "c",
                             std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep),
              0)
        << diagnosticsOf(rep);
    fs::path image;
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.is_regular_file() && e.path().extension() == ".exe") image = e.path();
    }
    ASSERT_FALSE(image.empty()) << "no image in " << out.string();
    auto const r = test_support::runBinary(image);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u) << kHugeExit;
}
