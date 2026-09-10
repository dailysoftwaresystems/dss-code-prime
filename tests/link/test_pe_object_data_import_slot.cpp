// D-LK-PE-OBJECT-WEAK-DATA-EXTERN-REL32-TO-AN-ABSOLUTE-TARGET —
// THE FOREIGN-LINKER HALF.
//
// ⓘ IT NOW CARRIES THE FUNCTION ROW TOO —
// D-LK-PE-OBJECT-WEAK-FUNCTION-ADDR-REL32-TO-AN-ABSOLUTE-TARGET (P54), the
// same defect one symbol class over, whose arms live at the bottom of this
// file. The FILE NAME still says `data` and was deliberately not changed: the
// data row is CLOSED and names this path as its witness, and renaming the file
// would strand that citation to save a word.
//
// WHAT THE BYTE PINS CANNOT PROVE. `test_pe_writer.cpp`'s
// `PeObjDataImportSlot.*` assert the emitted shape: the `.text` REL32 names
// `.refptr.<import>`, a select-any COMDAT `.rdata` carries one ADDR64 against
// the import, a function extern keeps its direct reference. Every one of those
// is DSS agreeing with DSS's own reading of PE/COFF. The claim this row makes
// is about a consumer none of them contain — a FOREIGN linker — and the defect
// was invisible from inside the pipeline: the DSS-linked PE exec returned 42
// throughout, the ELF twin was green throughout, and only link.exe, mingw `ld`
// and lld-link could see anything wrong at all.
//
// THE DEFECT, ✔MEASURED 2026-09-02 at this tree before the fix, on a
// DSS-emitted `pe64-x86_64-windows` object for
// `extern int ea __attribute__((weak)); ... if (&ea) r += ea; else r += 7;`
// whose `.text` carried `IMAGE_REL_AMD64_REL32 ea`:
//   * link.exe 14.51 REFUSED it — `LNK2016: absolute symbol 'ea' used as
//     target of REL32 relocation in section 0x1`, twice, then LNK1165. No
//     image at all. This is the CORRECT verdict: a COFF weak external's
//     fallback is an ABSOLUTE value-0 symbol, and no 32-bit PC-relative
//     displacement reaches an absolute from a 0x140000000 image base.
//   * mingw GNU ld 13.2.0 linked it rc 0 with NO DIAGNOSTIC, having TRUNCATED
//     the displacement: the image computed `lea -0x4000102c(%rip)` =
//     0x100000000 for an address that must be 0, `if (&ea)` took the WRONG
//     arm, and the program died rc 139. ★ THE CRASH IS THIS FIXTURE'S LUCK,
//     NOT THE CLASS: the wrong arm happens to dereference. A program that only
//     TESTS the optional symbol gets a wrong answer and a clean exit, which is
//     the silent-wrong-answer failure this project's bar most abhors.
//   * lld-link did the same, silently, rc 139.
//
// AND WHY IT IS DSS'S DEFECT RATHER THAN `ld`'s. ✔MEASURED what the references
// emit, each probed SEPARATELY, with `weak` as the only variable on one triple:
// clang 18.1.3 `--target=x86_64-pc-windows-msvc` emits a direct `REL32 ea` for
// a STRONG data extern and `REL32 .refptr.ea` + a `.rdata$.refptr.ea` COMDAT
// holding `ADDR64 ea` for a WEAK one; clang `--target=x86_64-w64-windows-gnu`
// and mingw-w64 gcc 13.2.0 emit `.refptr.<name>` for EVERY PE data extern.
// Every reference avoids the shape; DSS emitted it. An object DSS produces that
// link.exe cannot link is below the union.
//
// TWO TIERS IN THIS FILE, NEITHER REPLACING THE OTHER, and both driven through
// `Program::compileFiles` — the REAL production driver — rather than a
// hand-built module, because the fix spans two tiers that a hand-built module
// would let drift apart: the format declares `dataImportBinding`, MIR->LIR
// emits the deref, and the linker mints the slot. A fixture that hand-wrote the
// deref would stay green if the config lost its key.
//
//   * TIER 1 (`PeObjectDataImportSlotDriver`) — HOST-INDEPENDENT. Compiles the
//     source to a `.obj` and asserts, against the emitted COFF, that the code's
//     relocation names the slot and the slot's names the import. Runs on every
//     leg, so a regression reddens on Linux and macOS too rather than only
//     where link.exe exists.
//   * TIER 2 (`PeObjectDataImportSlotNative`) — the RUNTIME witness. Hands the
//     SAME object to link.exe and to mingw `ld` and RUNS it. Exit 42 is a SUM
//     (7 from the null branch + 35 from a present global) so reversing either
//     branch changes it, and the pair-of-objects arm returns 52 for the same
//     reason. Windows-only, and it SKIPS rather than reddens when a toolchain
//     is absent — with the toolchain's absence named, on the
//     `test_coff_object_reader.cpp` discipline.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

// The subject. `&ea` must be NULL (the else arm, +7) while an ordinary global's
// address must not (+35): 7 + 35 = 42, so reversing EITHER branch changes the
// exit code and the two cannot mask each other. This is the same program
// `examples/c/weak_extern_import_null` runs through DSS's OWN linker; here it
// is compiled to a RELOCATABLE object and handed to someone else's.
constexpr char const* kWeakDataImportSource =
    "extern int ea __attribute__((weak));\n"
    "int present = 35;\n"
    "int main(void)\n"
    "{\n"
    "    int r = 0;\n"
    "    if (&ea) { r += ea; } else { r += 7; }\n"
    "    if (&present) { r += present; } else { r += 1; }\n"
    "    return r;\n"
    "}\n";

// D-LK-PE-OBJECT-WEAK-FUNCTION-ADDR-REL32-TO-AN-ABSOLUTE-TARGET (P54): the
// FUNCTION subject, deliberately the same arithmetic as the data one so the
// two rows' witnesses read against each other. `maybe` is TESTED and CALLED,
// which is the whole point — ✔MEASURED 2026-09-02 that link.exe 14.51 answers
// `LNK2016: absolute symbol 'maybe' used as target of REL32 relocation` TWICE
// on the pre-fix object, once for the ADDRESS lea and once for the CALL, so a
// fix that routed only the address through the slot would still not link. The
// local `present` is the other direction: a module-local function's address is
// a real code address and must NOT acquire a slot.
// D-LK-PE-OBJECT-STRONG-EXTERN-PAYS-THE-WEAK-IMPORTS-SLOT (P55): the STRONG
// twin, `weak` the ONLY variable, so the two objects are read against each
// other exactly as the reference probes were. ✔MEASURED 2026-09-02 that
// DSS emitted the IDENTICAL object for both before P55 — one `FF 15` call
// site and one 8-byte `.refptr.maybe` COMDAT — while clang 18.1.3 on BOTH
// windows triples and mingw-w64 gcc 13.2.0 all emit a plain direct
// `REL32 maybe` and NO `.refptr` section for this one.
constexpr char const* kStrongFunctionImportSource =
    "extern int maybe(void);\n"
    "int present(void) { return 35; }\n"
    "int main(void)\n"
    "{\n"
    "    int r = 0;\n"
    "    if (maybe) { r += maybe(); } else { r += 7; }\n"
    "    if (present) { r += present(); } else { r += 1; }\n"
    "    return r;\n"
    "}\n";

constexpr char const* kWeakFunctionImportSource =
    "extern int maybe(void) __attribute__((weak));\n"
    "int present(void) { return 35; }\n"
    "int main(void)\n"
    "{\n"
    "    int r = 0;\n"
    "    if (maybe) { r += maybe(); } else { r += 7; }\n"
    "    if (present) { r += present(); } else { r += 1; }\n"
    "    return r;\n"
    "}\n";

[[nodiscard]] fs::path writeSrc(fs::path const& dir, std::string_view name,
                                std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

// Compile one C source to a pe64 RELOCATABLE object through the production
// driver. Returns the `.obj` path, or an empty path on a compile failure (the
// caller asserts, so a failure is never silently a skip).
[[nodiscard]] fs::path buildObj(fs::path const& dir, std::string_view stem,
                                std::string_view source,
                                DiagnosticReporter& rep) {
    auto const src = writeSrc(dir, std::string{stem} + ".c", source);
    auto const out = dir / (std::string{stem} + ".out");
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    int const rc = p.compileFiles(std::vector<std::string>{src.string()}, "c",
                                  std::vector<std::string>{
                                      "x86_64:pe64-x86_64-windows"},
                                  rep);
    if (rc != 0) return {};
    auto const obj = out / (std::string{stem} + ".obj");
    return fs::exists(obj) ? obj : fs::path{};
}

[[nodiscard]] std::vector<std::uint8_t> readFile(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

// ── The smallest COFF reader tier 1 needs ─────────────────────────────
// Deliberately local and tiny: `test_pe_writer.cpp`'s richer set is in that
// file's anonymous namespace, and hoisting it to serve two files would move a
// large surface for two structures. What is duplicated here is a WIRE LAYOUT
// fixed by the PE/COFF specification, not a DSS decision that could drift.

[[nodiscard]] std::uint16_t rdU16(std::vector<std::uint8_t> const& b,
                                  std::size_t o) {
    return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}
[[nodiscard]] std::uint32_t rdU32(std::vector<std::uint8_t> const& b,
                                  std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16)
         | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

// A symbol's name in whichever of the two COFF spellings it uses.
[[nodiscard]] std::string symName(std::vector<std::uint8_t> const& b,
                                  std::uint32_t idx) {
    std::uint32_t const symPtr  = rdU32(b, 8);
    std::uint32_t const numSyms = rdU32(b, 12);
    std::size_t const rec = symPtr + static_cast<std::size_t>(idx) * 18u;
    if (b.size() < rec + 18u) return {};
    std::string s;
    if (rdU32(b, rec) == 0u) {
        std::size_t i = symPtr + static_cast<std::size_t>(numSyms) * 18u
                      + rdU32(b, rec + 4);
        for (; i < b.size() && b[i] != 0; ++i) s.push_back(static_cast<char>(b[i]));
        return s;
    }
    for (std::size_t k = 0; k < 8 && b[rec + k] != 0; ++k) {
        s.push_back(static_cast<char>(b[rec + k]));
    }
    return s;
}

struct Reloc {
    std::string section;
    std::uint16_t type = 0;
    std::string target;
};

// Every relocation in the object, each already resolved to its section name,
// wire type and TARGET SYMBOL NAME — which is the column the defect lived in.
[[nodiscard]] std::vector<Reloc> allRelocs(std::vector<std::uint8_t> const& b) {
    std::vector<Reloc> out;
    if (b.size() < 20u) return out;
    std::uint16_t const nSec = rdU16(b, 2);
    for (std::uint16_t i = 0; i < nSec; ++i) {
        std::size_t const h = 20u + static_cast<std::size_t>(i) * 40u;
        if (b.size() < h + 40u) break;
        std::string name;
        for (std::size_t k = 0; k < 8 && b[h + k] != 0; ++k) {
            name.push_back(static_cast<char>(b[h + k]));
        }
        std::uint32_t const relPtr = rdU32(b, h + 24);
        std::uint16_t const nRel   = rdU16(b, h + 32);
        for (std::uint16_t r = 0; r < nRel; ++r) {
            std::size_t const o = relPtr + static_cast<std::size_t>(r) * 10u;
            if (b.size() < o + 10u) break;
            out.push_back({name, rdU16(b, o + 8), symName(b, rdU32(b, o + 4))});
        }
    }
    return out;
}

[[nodiscard]] std::string dumpRelocs(std::vector<Reloc> const& rs) {
    std::string s;
    for (auto const& r : rs) {
        s += "\n  " + r.section + " type=" + std::to_string(r.type)
           + " -> " + r.target;
    }
    return s;
}

[[nodiscard]] bool hasReloc(std::vector<Reloc> const& rs,
                            std::string_view section, std::uint16_t type,
                            std::string_view target) {
    for (auto const& r : rs) {
        if (r.section == section && r.type == type && r.target == target) {
            return true;
        }
    }
    return false;
}

constexpr std::uint16_t kRel32  = 4;   // IMAGE_REL_AMD64_REL32
constexpr std::uint16_t kAddr64 = 1;   // IMAGE_REL_AMD64_ADDR64

}  // namespace

// ══ TIER 1 — host-independent: what the driver actually emitted ══

TEST(PeObjectDataImportSlotDriver, WeakDataImportReachesTheCodeThroughACarriedSlot) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo,
                                     "pe-import-slot"};
    DiagnosticReporter rep;
    auto const obj = buildObj(scratch.path(), "wkdata", kWeakDataImportSource, rep);
    ASSERT_FALSE(obj.empty())
        << "the production driver must compile this to a pe64 `.obj`; errs="
        << rep.errorCount();
    auto const bytes = readFile(obj);
    ASSERT_FALSE(bytes.empty());
    auto const rs = allRelocs(bytes);

    EXPECT_TRUE(hasReloc(rs, ".text", kRel32, ".refptr.ea"))
        << "the code must reach the weak import through the carried slot; a "
           "`.text` REL32 naming `ea` is the defect — link.exe answers LNK2016 "
           "and mingw ld truncates it silently" << dumpRelocs(rs);
    EXPECT_FALSE(hasReloc(rs, ".text", kRel32, "ea"))
        << "no direct PC-relative reference to the import may survive"
        << dumpRelocs(rs);
    EXPECT_TRUE(hasReloc(rs, ".rdata", kAddr64, "ea"))
        << "the slot must carry the ABSOLUTE fixup the final linker fills; "
           "without it every `&ea` reads 0 even when a definition IS linked"
        << dumpRelocs(rs);
}

TEST(PeObjectDataImportSlotDriver, WeakFunctionImportReachesTheCodeThroughACarriedSlot) {
    // D-LK-PE-OBJECT-WEAK-FUNCTION-ADDR-REL32-TO-AN-ABSOLUTE-TARGET (P54).
    // BOTH references — the ADDRESS test `if (maybe)` and the CALL `maybe()` —
    // must name the slot. Pinning only one would be the partial fix that reads
    // as a complete one: ✔MEASURED that link.exe answers LNK2016 once PER
    // remaining direct relocation, and that mingw-w64 gcc 13.2.0's own object
    // (address through `.refptr`, call direct) is refused for the call alone.
    test_support::ScratchDir scratch{test_support::Location::InsideRepo,
                                     "pe-import-slot"};
    DiagnosticReporter rep;
    auto const obj = buildObj(scratch.path(), "wkfn", kWeakFunctionImportSource,
                              rep);
    ASSERT_FALSE(obj.empty())
        << "the production driver must compile this to a pe64 `.obj`; errs="
        << rep.errorCount();
    auto const bytes = readFile(obj);
    ASSERT_FALSE(bytes.empty());
    auto const rs = allRelocs(bytes);

    EXPECT_TRUE(hasReloc(rs, ".text", kRel32, ".refptr.maybe"))
        << "the code must reach the weak FUNCTION import through the carried "
           "slot" << dumpRelocs(rs);
    EXPECT_FALSE(hasReloc(rs, ".text", kRel32, "maybe"))
        << "NO direct PC-relative reference to the import may survive — not "
           "the address lea and not the call. This is the assertion that "
           "separates a complete fix from the address-only one: mingw gcc "
           "emits exactly that mixture and link.exe refuses it"
        << dumpRelocs(rs);
    EXPECT_TRUE(hasReloc(rs, ".rdata", kAddr64, "maybe"))
        << "the slot must carry the ABSOLUTE fixup the final linker fills; "
           "without it `maybe` reads 0 even when a definition IS linked"
        << dumpRelocs(rs);
    // The other direction: a MODULE-LOCAL function is not an import and must
    // acquire no slot. A pass that minted one per referenced SYMBOL rather
    // than per referenced IMPORT would pass every assertion above.
    EXPECT_FALSE(hasReloc(rs, ".rdata", kAddr64, "present"))
        << "a module-local function's address is a real code address"
        << dumpRelocs(rs);
}

TEST(PeObjectDataImportSlotDriver, StrongFunctionImportKeepsItsDirectReference) {
    // D-LK-PE-OBJECT-STRONG-EXTERN-PAYS-THE-WEAK-IMPORTS-SLOT (P55) — THE
    // NARROWING, THROUGH THE PRODUCTION DRIVER, ON THE SHIPPED DOCUMENT.
    //
    // Same source as the test above with `weak` REMOVED and nothing else
    // changed. ✔MEASURED 2026-09-02 that P54's unconditional dispatch emitted
    // the IDENTICAL object for both — an `FF 15` call site plus an 8-byte
    // `.refptr.maybe` COMDAT for a symbol that can never resolve to an
    // absolute — and that all three references keep this one direct: clang
    // 18.1.3 on `--target=x86_64-pc-windows-msvc` AND
    // `--target=x86_64-w64-windows-gnu`, and mingw-w64 gcc 13.2.0, each
    // probed separately with `weak` as the only variable.
    //
    // ⚠ THIS IS A BYTE PIN AND IT CANNOT SEE THE HALF THAT MATTERS MOST.
    // `wf`'s sharpest P54 measurement was a mutant under which the relocation
    // pin stayed GREEN while the program exited 0xC0000005 — both relocations
    // still named `.refptr.maybe` and only the missing deref made it wrong.
    // The RUN witness for this direction is
    // `LinkExeLinksAndRunsAStrongFunctionImportDirectly` below.
    test_support::ScratchDir scratch{test_support::Location::InsideRepo,
                                     "pe-import-slot"};
    DiagnosticReporter rep;
    auto const obj = buildObj(scratch.path(), "stfn",
                              kStrongFunctionImportSource, rep);
    ASSERT_FALSE(obj.empty())
        << "the production driver must compile this to a pe64 `.obj`; errs="
        << rep.errorCount();
    auto const bytes = readFile(obj);
    ASSERT_FALSE(bytes.empty());
    auto const rs = allRelocs(bytes);

    EXPECT_TRUE(hasReloc(rs, ".text", kRel32, "maybe"))
        << "a STRONG undefined has no absolute resolution — the final link "
           "finds a definition or fails loud — so its reference is "
           "representable pc-relative and must stay direct"
        << dumpRelocs(rs);
    EXPECT_FALSE(hasReloc(rs, ".text", kRel32, ".refptr.maybe"))
        << "no reference to a strong import may go through a slot: that is a "
           "load per call site plus an 8-byte COMDAT bought for nothing, and "
           "it is what every reference declines to emit" << dumpRelocs(rs);
    EXPECT_FALSE(hasReloc(rs, ".rdata", kAddr64, "maybe"))
        << "and no slot may be minted for it" << dumpRelocs(rs);
}

// ══ TIER 2 — the foreign linkers, and the RUN ══

#if defined(_WIN32)
namespace {

// The vcvars64-entered link.exe environment. LOCATING it is
// `test_support::native_probe::locateMsvcToolchain`'s job — the ONE implementation, shared
// with the ABI conformance witnesses; this only USES what it returns.
struct MsvcEnv {
    fs::path vcvars;
    fs::path work;
    [[nodiscard]] bool run(std::string const& cmdline) const {
        auto const bat = work / "dss_p54_rp_link.bat";
        {
            std::ofstream b{bat};
            b << "@echo off\r\n"
              << "call \"" << vcvars.string() << "\" >nul 2>&1\r\n"
              << "cd /d \"" << work.string() << "\"\r\n"
              << cmdline << " >nul 2>&1\r\n";
        }
        std::string const sys = "\"\"" + bat.string() + "\"\"";
        return std::system(sys.c_str()) == 0;
    }
};

// mingw `ld` reached through the `gcc` on PATH. PRESENT is not USABLE: a shim
// with no toolchain behind it counts as ABSENT, so the probe proves it can
// link something trivial before this file promises a red on failure.
struct MingwLd {
    bool     usable = false;
    fs::path work;
    std::string detail;
    [[nodiscard]] bool link(std::string const& objs,
                            std::string const& exe) const {
        std::string const cmd = "cd /d \"" + work.string() + "\" && ld -e main -o "
                              + exe + " " + objs + " >nul 2>&1";
        return std::system(("\"" + cmd + "\"").c_str()) == 0;
    }
};

[[nodiscard]] MingwLd locateMingwLd(fs::path const& work) {
    MingwLd g;
    g.work = work;
    if (std::system("where ld >nul 2>&1") != 0) {
        g.detail = "no `ld` on PATH -- the mingw witness is inert on this host";
        return g;
    }
    g.usable = true;
    return g;
}

}  // namespace

TEST(PeObjectDataImportSlotNative, LinkExeLinksAndRunsAnUnresolvedWeakDataImport) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo,
                                     "pe-import-slot"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    MsvcEnv const env{msvc.vcvars, dir};

    DiagnosticReporter rep;
    auto const obj = buildObj(dir, "wkdata", kWeakDataImportSource, rep);
    ASSERT_FALSE(obj.empty()) << "errs=" << rep.errorCount();
    fs::copy_file(obj, dir / "wkdata.obj",
                  fs::copy_options::overwrite_existing);

    // `/NODEFAULTLIB` + `/ENTRY:main` keeps the CRT out of it: the subject is
    // ONE object and its relocations, and a CRT would add symbols whose failure
    // would be indistinguishable from this one's.
    ASSERT_TRUE(env.run("link /nologo /OUT:wkdata.exe /ENTRY:main "
                        "/SUBSYSTEM:CONSOLE /NODEFAULTLIB wkdata.obj"))
        << "link.exe must LINK a DSS object whose weak data import resolves to "
           "nothing. Before the carried slot it answered `LNK2016: absolute "
           "symbol 'ea' used as target of REL32 relocation` and produced no "
           "image at all.";
    auto const exe = dir / "wkdata.exe";
    ASSERT_TRUE(fs::exists(exe));
    auto const r = test_support::runBinary(exe);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "7 (the null branch, `&ea == 0`) + 35 (an ordinary global's address "
           "is NOT null) — reversing either branch changes this number";
}

TEST(PeObjectDataImportSlotNative, MingwLdLinksAndRunsTheSameObject) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo,
                                     "pe-import-slot"};
    auto const dir = scratch.path();
    auto const ld = locateMingwLd(dir);
    if (!ld.usable) GTEST_SKIP() << ld.detail;

    DiagnosticReporter rep;
    auto const obj = buildObj(dir, "wkdata", kWeakDataImportSource, rep);
    ASSERT_FALSE(obj.empty()) << "errs=" << rep.errorCount();
    fs::copy_file(obj, dir / "wkdata.obj",
                  fs::copy_options::overwrite_existing);

    // ⚠ THE `ld` ARM IS THE ONE THAT MATTERS MOST AND IT IS THE ONE A RETURN
    // CODE CANNOT SEE. `ld` LINKED THE BROKEN OBJECT TOO — rc 0, no diagnostic
    // — after truncating the out-of-range displacement. Only RUNNING it
    // separates the two: the truncated image took the wrong branch and died
    // rc 139 (and, on a program that merely TESTS the pointer, would have
    // exited cleanly with a wrong answer). So the link assertion below is
    // necessary and NOT sufficient, and the exit code is the real witness.
    ASSERT_TRUE(ld.link("wkdata.obj", "wkdata_ld.exe"))
        << "mingw ld must link the object";
    auto const exe = dir / "wkdata_ld.exe";
    ASSERT_TRUE(fs::exists(exe));
    auto const r = test_support::runBinary(exe);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "the pre-fix image linked rc 0 here and then died rc 139 on a "
           "truncated 0x100000000 address; 42 is the only value that says the "
           "null branch was taken through a real slot";
}

TEST(PeObjectDataImportSlotNative, TwoObjectsImportingOneNameFoldTheirSlots) {
    // THE PROPERTY ONE OBJECT CANNOT SHOW. Both TUs publish `.refptr.ea`, so
    // without IMAGE_SCN_LNK_COMDAT + IMAGE_COMDAT_SELECT_ANY the pair is a
    // duplicate definition and the link dies. 2 + 20 + 30 = 52 also says both
    // null branches were taken: if either object's slot had resolved to
    // something the sum would be 1 + 20 + 30 or 2 + 10 + 30.
    test_support::ScratchDir scratch{test_support::Location::InsideRepo,
                                     "pe-import-slot"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    MsvcEnv const env{msvc.vcvars, dir};

    DiagnosticReporter rep;
    auto const a1 = buildObj(dir, "slota",
        "extern int ea __attribute__((weak));\n"
        "int one(void) { return &ea ? 1 : 2; }\n", rep);
    auto const a2 = buildObj(dir, "slotb",
        "extern int ea __attribute__((weak));\n"
        "extern int one(void);\n"
        "int two(void) { return &ea ? 10 : 20; }\n"
        "int main(void) { return one() + two() + 30; }\n", rep);
    ASSERT_FALSE(a1.empty()) << "errs=" << rep.errorCount();
    ASSERT_FALSE(a2.empty()) << "errs=" << rep.errorCount();
    fs::copy_file(a1, dir / "slota.obj", fs::copy_options::overwrite_existing);
    fs::copy_file(a2, dir / "slotb.obj", fs::copy_options::overwrite_existing);

    ASSERT_TRUE(env.run("link /nologo /OUT:two.exe /ENTRY:main "
                        "/SUBSYSTEM:CONSOLE /NODEFAULTLIB slota.obj slotb.obj"))
        << "two objects each carrying a `.refptr.ea` slot must link: the slot "
           "is a select-any COMDAT precisely so the linker keeps one";
    auto const exe = dir / "two.exe";
    ASSERT_TRUE(fs::exists(exe));
    auto const r = test_support::runBinary(exe);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 52u) << "2 + 20 + 30 — both null branches taken";
}

TEST(PeObjectDataImportSlotNative, ADefinitionPresentResolvesThroughTheSlot) {
    // THE OTHER DIRECTION, and it is the one a slot gets WRONG most quietly. A
    // slot minted with no ADDR64 fixup, or one whose fixup names the wrong
    // symbol, still links and still runs — and reads 0 forever, so `&ea` stays
    // null even though `ea` IS defined and the program takes the branch it was
    // written to avoid. Only linking WITH a definition can see it.
    test_support::ScratchDir scratch{test_support::Location::InsideRepo,
                                     "pe-import-slot"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    MsvcEnv const env{msvc.vcvars, dir};

    DiagnosticReporter rep;
    auto const obj = buildObj(dir, "wkdata", kWeakDataImportSource, rep);
    ASSERT_FALSE(obj.empty()) << "errs=" << rep.errorCount();
    fs::copy_file(obj, dir / "wkdata.obj",
                  fs::copy_options::overwrite_existing);
    // The DEFINITION comes from the reference C compiler, not from DSS: a
    // second DSS object would leave both halves of the question answered by
    // one implementation.
    (void)writeSrc(dir, "eadef.c", "int ea = -28;\n");
    ASSERT_TRUE(env.run("cl /nologo /c eadef.c /Foeadef.obj"))
        << "the reference C compiler must build the defining TU";

    ASSERT_TRUE(env.run("link /nologo /OUT:withdef.exe /ENTRY:main "
                        "/SUBSYSTEM:CONSOLE /NODEFAULTLIB wkdata.obj eadef.obj"));
    auto const exe = dir / "withdef.exe";
    ASSERT_TRUE(fs::exists(exe));
    auto const r = test_support::runBinary(exe);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 7u)
        << "-28 (read THROUGH the resolved slot) + 35. A slot whose fixup "
           "never ran would give 42 here — the same number the unresolved case "
           "correctly produces, which is why this arm exists.";
}

// ══ THE FUNCTION HALF, through the same two foreign linkers ══
//    D-LK-PE-OBJECT-WEAK-FUNCTION-ADDR-REL32-TO-AN-ABSOLUTE-TARGET (P54)

TEST(PeObjectDataImportSlotNative, LinkExeLinksAndRunsAnUnresolvedWeakFunctionImport) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo,
                                     "pe-import-slot"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    MsvcEnv const env{msvc.vcvars, dir};

    DiagnosticReporter rep;
    auto const obj = buildObj(dir, "wkfn", kWeakFunctionImportSource, rep);
    ASSERT_FALSE(obj.empty()) << "errs=" << rep.errorCount();
    fs::copy_file(obj, dir / "wkfn.obj", fs::copy_options::overwrite_existing);

    ASSERT_TRUE(env.run("link /nologo /OUT:wkfn.exe /ENTRY:main "
                        "/SUBSYSTEM:CONSOLE /NODEFAULTLIB wkfn.obj"))
        << "link.exe must LINK a DSS object whose weak FUNCTION import "
           "resolves to nothing. Before the slot it answered `LNK2016: "
           "absolute symbol 'maybe' used as target of REL32 relocation` TWICE "
           "— once for the address test and once for the call — and produced "
           "no image at all.";
    auto const exe = dir / "wkfn.exe";
    ASSERT_TRUE(fs::exists(exe));
    auto const r = test_support::runBinary(exe);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u)
        << "7 (the null branch — `maybe` resolved to nothing) + 35 (a "
           "module-local function called normally)";
}

TEST(PeObjectDataImportSlotNative, MingwLdLinksAndRunsTheSameFunctionObject) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo,
                                     "pe-import-slot"};
    auto const dir = scratch.path();
    auto const ld = locateMingwLd(dir);
    if (!ld.usable) GTEST_SKIP() << ld.detail;

    DiagnosticReporter rep;
    auto const obj = buildObj(dir, "wkfn", kWeakFunctionImportSource, rep);
    ASSERT_FALSE(obj.empty()) << "errs=" << rep.errorCount();
    fs::copy_file(obj, dir / "wkfn.obj", fs::copy_options::overwrite_existing);

    // ⚠ `ld` LINKED THE BROKEN OBJECT TOO, rc 0 and silent, after TRUNCATING
    // the displacement — ✔MEASURED on mingw gcc's OWN object, whose linked
    // image carries `call 100000000` for a symbol whose address is 0. The
    // link assertion is necessary and NOT sufficient; the exit code is the
    // real witness, and it only differs because the guard is taken correctly.
    ASSERT_TRUE(ld.link("wkfn.obj", "wkfn_ld.exe"))
        << "mingw ld must link the object";
    auto const exe = dir / "wkfn_ld.exe";
    ASSERT_TRUE(fs::exists(exe));
    auto const r = test_support::runBinary(exe);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 42u);
}

TEST(PeObjectDataImportSlotNative, AFunctionDefinitionPresentResolvesThroughTheSlot) {
    // THE DIRECTION A SLOT GETS WRONG MOST QUIETLY, for the function half: a
    // slot with no ADDR64, or one naming the wrong symbol, still links and
    // still runs — and stays null forever, so `maybe` reads as absent even
    // though it IS linked and the program takes the branch it was written to
    // avoid. Only linking WITH a definition can see it, and the definition
    // comes from the reference C compiler so both halves are not answered by
    // one implementation.
    test_support::ScratchDir scratch{test_support::Location::InsideRepo,
                                     "pe-import-slot"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    MsvcEnv const env{msvc.vcvars, dir};

    DiagnosticReporter rep;
    auto const obj = buildObj(dir, "wkfn", kWeakFunctionImportSource, rep);
    ASSERT_FALSE(obj.empty()) << "errs=" << rep.errorCount();
    fs::copy_file(obj, dir / "wkfn.obj", fs::copy_options::overwrite_existing);
    (void)writeSrc(dir, "maybedef.c", "int maybe(void) { return -28; }\n");
    ASSERT_TRUE(env.run("cl /nologo /c maybedef.c /Fomaybedef.obj"))
        << "the reference C compiler must build the defining TU";

    ASSERT_TRUE(env.run("link /nologo /OUT:wkfndef.exe /ENTRY:main "
                        "/SUBSYSTEM:CONSOLE /NODEFAULTLIB wkfn.obj "
                        "maybedef.obj"));
    auto const exe = dir / "wkfndef.exe";
    ASSERT_TRUE(fs::exists(exe));
    auto const r = test_support::runBinary(exe);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 7u)
        << "-28 (returned by the definition, reached THROUGH the resolved "
           "slot) + 35. A slot whose fixup never ran would give 42 — the same "
           "number the unresolved case correctly produces.";
}

// ══ D-LK-PE-OBJECT-STRONG-EXTERN-PAYS-THE-WEAK-IMPORTS-SLOT (P55):
//    THE STRONG DIRECTION, AND IT NEEDS BOTH WITNESSES ══
//
// A relocation pin can show that the `E8` names `maybe` directly. It CANNOT
// show that the resulting program runs — ✔MEASURED in P54 that a byte pin
// stayed green over a mutant whose image exited 0xC0000005, because the
// relocations were right and only the deref was missing. So the direct shape
// gets a RUN too, and the number is a SUM (the definition's value + a
// module-local call) so reversing either branch changes it.

TEST(PeObjectDataImportSlotNative, LinkExeLinksAndRunsAStrongFunctionImportDirectly) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo,
                                     "pe-import-slot"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    MsvcEnv const env{msvc.vcvars, dir};

    DiagnosticReporter rep;
    auto const obj = buildObj(dir, "stfn", kStrongFunctionImportSource, rep);
    ASSERT_FALSE(obj.empty()) << "errs=" << rep.errorCount();
    fs::copy_file(obj, dir / "stfn.obj", fs::copy_options::overwrite_existing);
    (void)writeSrc(dir, "maybedef.c", "int maybe(void) { return -28; }\n");
    ASSERT_TRUE(env.run("cl /nologo /c maybedef.c /Fomaybedef.obj"))
        << "the reference C compiler must build the defining TU";

    ASSERT_TRUE(env.run("link /nologo /OUT:stfndef.exe /ENTRY:main "
                        "/SUBSYSTEM:CONSOLE /NODEFAULTLIB stfn.obj "
                        "maybedef.obj"))
        << "link.exe must link a DIRECT `E8` reference to a strong import "
           "against its definition — this is the shape the narrowing restores, "
           "and the shape clang and mingw gcc both emit";
    auto const exe = dir / "stfndef.exe";
    ASSERT_TRUE(fs::exists(exe));
    auto const r = test_support::runBinary(exe);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    EXPECT_FALSE(r.timedOut);
    EXPECT_EQ(r.exitCode, 7u)
        << "-28 (the definition, reached by a DIRECT call) + 35 (the local). "
           "A direct reference retargeted at a slot would branch into pointer "
           "bytes; a slot that stayed null would give 42.";
}

TEST(PeObjectDataImportSlotNative, AStrongFunctionImportWithNoDefinitionIsRefusedLoudly) {
    // ★ THE OTHER SIDE OF THE NARROWING, AND THE REASON IT IS SAFE: the slot
    // exists because a WEAK reference may legally resolve to NOTHING. A STRONG
    // one may not — the link must find a definition or FAIL — so removing its
    // slot cannot produce a quietly-null program, only a loud unresolved
    // symbol. ✔MEASURED at this tree: link.exe answers LNK2019 + LNK1120,
    // mingw ld answers `undefined reference to 'maybe'` at BOTH call sites,
    // and lld-link 18.1.3 answers `undefined symbol: maybe`.
    // ⓘ AND THE DIAGNOSTIC GOT BETTER, which is worth pinning because it is
    // the opposite of what a cost-narrowing usually does: with the reference
    // direct, link.exe names the REFERENCING FUNCTION (LNK2019 `referenced in
    // function main`) where before it named an anonymous COMDAT (LNK2001),
    // and mingw ld names the two `.text` sites where before it named
    // `.rdata[.refptr.maybe]`. The unnarrowed slot was hiding the reference
    // site from the reader.
    test_support::ScratchDir scratch{test_support::Location::InsideRepo,
                                     "pe-import-slot"};
    auto const dir = scratch.path();
    auto const msvc = test_support::native_probe::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    MsvcEnv const env{msvc.vcvars, dir};

    DiagnosticReporter rep;
    auto const obj = buildObj(dir, "stfn", kStrongFunctionImportSource, rep);
    ASSERT_FALSE(obj.empty()) << "errs=" << rep.errorCount();
    fs::copy_file(obj, dir / "stfn.obj", fs::copy_options::overwrite_existing);

    EXPECT_FALSE(env.run("link /nologo /OUT:stfnbare.exe /ENTRY:main "
                         "/SUBSYSTEM:CONSOLE /NODEFAULTLIB stfn.obj"))
        << "a STRONG import with no definition must be REFUSED — if this "
           "linked, the narrowing would have turned a required symbol into an "
           "optional one, which is the only way it could be unsafe";
    EXPECT_FALSE(fs::exists(dir / "stfnbare.exe"))
        << "and no image may be produced";
}
#endif  // _WIN32
