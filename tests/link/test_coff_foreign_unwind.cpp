// P69 fold 3c, lane p69/xa — D-LK-COFF-FOREIGN-UNWIND-DROP: a COFF object's `.pdata`/`.xdata` read into its
// functions and written back.
//
// The COFF reader decodes each RUNTIME_FUNCTION's UNWIND_INFO into the function's `cfi`
// (`link/format/win64_unwind_decode.hpp`) and carries a handler — its routine, its phases, its data and the addresses
// the data holds — as the function's `languageHandler`; the PE writers encode both back
// (`pe.cpp`'s `buildFunctionUnwindInfo`). The fixtures are the reference compilers' own objects
// (`tests/link/data/win64_unwind_{forms,try}.source.c` state how each was built):
//
//   win64_unwind_forms_x86_64_pe_cl_O2.obj       md5 00fc7fb0ebc3d54c79abd29d84c361f3   cl 19.44.35229 /O2
//   win64_unwind_forms_x86_64_pe_cl_Od.obj       md5 8315ff5df0389809c69df2b9e52d4577   cl 19.44.35229 /Od
//   win64_unwind_forms_x86_64_pe_clangcl_O2.obj  md5 6ad49c56ca35b08b82479ce9807b737b   clang-cl 19.1.5 /O2
//   win64_unwind_forms_x86_64_pe_gcc_O2.obj      md5 e8bebdd0fa47aafd41ec01a9e44070b0   mingw-w64 gcc 13.2.0 -O2
//   win64_unwind_try_x86_64_pe_cl_O2.obj         md5 456673ea5a0db1a9a0bd7853219a0740   cl 19.44.35229 /O2
//   win64_unwind_try_x86_64_pe_cl_Od.obj         md5 c845ec0ca0cba582ae0def850743440c   cl 19.44.35229 /Od
//   win64_unwind_try_x86_64_pe_clangcl_O2.obj    md5 4bbee256080fc8ca3b4e445d7be1f009   clang-cl 19.1.5 /O2
//
// ✔MEASURED at the build (P69 fold 3c): each object comes out of a second build byte-identical and records no path of
// the host that built it; each `try` object linked by link.exe beside a cl `main` runs to 42. No guard enforces an
// md5: it records WHICH BYTES were measured.
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

#include "host_native_target.hpp"
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
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::vector<std::uint8_t> fixture(std::string_view name) {
    auto const    path = dss::test::repoRoot() / "tests" / "link" / "data" / std::string{name};
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) {
        ADD_FAILURE() << "cannot open " << path.string();
        return {};
    }
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
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
    std::vector<std::string> all(std::begin(kForms), std::end(kForms));
    all.insert(all.end(), std::begin(kTries), std::end(kTries));
    for (auto const& name : all) {
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
// The encoder's three forms are all here: UWOP_PUSH_NONVOL (every fixture), UWOP_SET_FPREG with a frame offset (cl's
// and gcc's `dyn`), and saves measured from the frame register's base (cl /O2's `dyn`).
TEST(CoffForeignUnwind, ReferenceUnwindInfoSurvivesTheShippedWriterByteForByte) {
    auto const l = load();
    ASSERT_TRUE(l.target && l.object);
    std::vector<std::string> all(std::begin(kForms), std::end(kForms));
    all.insert(all.end(), std::begin(kTries), std::end(kTries));
    for (auto const& name : all) {
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
    std::vector<Edit> const edits{
        {"version 2", *big, 0xF8, 0x02, "is version 2"},
        {"chained", *big, 0xFF, 0x04 << 3, "is chained to another function's"},
        {"UWOP_ALLOC_LARGE op-info 1", *big + 5, 0x0F, 0x10, "states UWOP_ALLOC_LARGE with op-info 1"},
        {"UWOP_PUSH_MACHFRAME", *big + 5, 0xF0, 10, "states UWOP_PUSH_MACHFRAME"},
        {"UWOP_SAVE_NONVOL_FAR", *big + 5, 0xF0, 5, "states UWOP_SAVE_NONVOL_FAR"},
        {"UWOP_SAVE_XMM128_FAR", *big + 5, 0xF0, 9, "states UWOP_SAVE_XMM128_FAR"},
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

// The `.eh_frame` writers state no personality and no data area yet: a function carrying a handler is refused by
// each, by name, before any byte — and the PE writer refuses a function that carries both a foreign handler and
// scopes of its own.
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
        EXPECT_TRUE(said(rep, "carries the handler routine and data its object named for it")) << diagnosticsOf(rep);
    }
    DiagnosticReporter           rep;
    std::vector<AssembledModule> one{handled(true)};
    LinkedImage const            img = linker::link(std::span<AssembledModule const>{one}, *l.target, *l.object, rep);
    EXPECT_FALSE(img.ok());
    EXPECT_TRUE(said(rep, "carries a foreign object's handler too")) << diagnosticsOf(rep);
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
