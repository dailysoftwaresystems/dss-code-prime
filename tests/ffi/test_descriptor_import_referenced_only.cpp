// D-FFI-DESCRIPTOR-EAGER-IMPORT — REFERENCED-ONLY import of shipped-descriptor
// symbols, asserted on the EMITTED IMAGE of every format the descriptor
// mechanism ships for.
//
// ── THE DEFECT, MEASURED ─────────────────────────────────────────────────────
//
// A `#include`d descriptor's symbols were EAGER: bound into the image whether or
// not the translation unit referenced them, by an explicit law
// (`ShippedExternSymbol::eagerImport` defaulting true) that the linker's
// reference gate honours by keeping an eager row unconditionally. ✔MEASURED at
// fcb3a9d7 with the probe source below — three headers, a handful of referenced
// names — by walking each format's real import pointer chain:
//
//     x86_64:elf64-x86_64-linux-exec      libc.so.6                     86 symbols
//     x86_64:pe64-x86_64-windows-exec     ucrtbase.dll                  85 symbols
//     x86_64:macho64-x86_64-darwin-exec   /usr/lib/libSystem.B.dylib    97 symbols
//
// ── WHY REFERENCED-ONLY IS REQUIRED RATHER THAN NICE ─────────────────────────
//
// ✔MEASURED 2026-09-03, each reference probed SEPARATELY and each arm carrying a
// control that FIRED, at BOTH tiers (the object file's undefined symbols and the
// linked image's import table):
//
//     gcc 13.3.0 (WSL, elf)        unreferenced: NOT emitted / NOT imported
//     clang 18.1.3 (WSL, elf)      unreferenced: NOT emitted / NOT imported
//     MSVC 19.51.36252 (pe)        unreferenced: NOT emitted / NOT imported
//     mingw-w64 gcc 13.2.0 (pe)    unreferenced: NOT emitted / NOT imported
//
// ⚠ The FIRST MSVC arm was UNINSTRUMENTED and is recorded as such: `cl r2.c` links
// the STATIC CRT, so its import table was empty and every "absent" answer was
// vacuous. Re-run with `/MD`, the control (`puts` from
// api-ms-win-crt-stdio-l1-1-0.dll) fired and the absences became evidence.
//
// Under `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` the unanimity is POSITIVE behaviour
// — not four references obeying a shared constraint — so referenced-only is
// reference behaviour, and the eager law was a divergence from all four.
//
// ★★ AND THE DIVERGENCE WAS INTERNAL TOO, WHICH IS THE SHARPEST STATEMENT OF IT.
// C23 7.1.4p2 entitles a program to DECLARE a library function itself instead of
// including its header, and says the two are equivalent. ✔MEASURED in ONE eager
// tree, same three referenced names, one program hand-declaring them and one
// `#include`ing the headers:
//
//     hand-declared (`extern int puts(char const *);` …)   elf **3**   pe **3**
//     `#include <stdio.h> <string.h> <stdlib.h>`           elf **86**  pe **85**
//
// A hand-declared extern is producer A and was ALREADY non-eager, so the two
// spellings C23 calls equivalent produced two different programs at the loader —
// differing by a factor of 28 in what they ask ld.so / the PE loader to resolve.
// Referenced-only import is what makes them agree.
//
// ── WHAT THIS FILE ASSERTS THAT NO EXAMPLE CAN ───────────────────────────────
//
// `examples/c/descriptor_import_referenced_only` is the RUN witness, and it can
// only witness the OVER-pruning direction: it exits 42 iff every reference shape
// still binds. Nothing portable lets a running process read its own import table,
// so the DROP half — the ~80 names that must now be absent — is only assertable
// against artifact CONTENT, which is what this file reads. Same assertion split,
// and the same reason, as `project_module_standalone_build`'s archive-magic pin.
//
// ── THE INSTRUMENT, AND WHY IT IS NOT A BYTE SCAN ────────────────────────────
//
// Each extractor follows its format's real pointer chain. A `.dynstr`/`.idata`
// substring scan cannot tell a genuine import row from an incidental string in a
// neighbouring blob — and this file's load-bearing assertions are ABSENCES, which
// is exactly the answer a scan gets wrong. PE reuses the shared extractor in
// `tests/test_support/image_dependency_table.hpp`; ELF and Mach-O undefined-symbol
// walks live here because that header recovers LIBRARY names for those two and the
// granularity this defect lives at is the SYMBOL.
//
// ⚠ EVERY ABSENCE ARM IS GUARDED BY A PRESENCE ARM IN THE SAME IMAGE. An extractor
// that silently stopped parsing returns EMPTY, and an empty list satisfies every
// absence assertion ever written. So each arm first asserts the names the program
// genuinely references ARE there; only then does it read the absences. The total
// count is asserted too, so a regression that keeps a name outside the hand-listed
// absent set still reds.
//
// ── ⚠⚠ IF THIS PIN IS RED, ONE TOKEN MOVED — READ THIS BEFORE EDITING IT ─────
//
// The behaviour is a single DEFAULT: `ShippedExternSymbol::eagerImport` in
// `src/analysis/semantic/semantic_model.hpp`, which is FALSE. Everything else the
// closure needed was ALREADY in the tree — `rejectOrDropUnreferencedExterns` keeps a
// NON-eager import only when a relocation in a function or a data item references
// it, and two descriptor producers (the `shippedSourcePath` rows and the UCRT
// shim-core companions) already depended on exactly that.
//
// ✔MEASURED at the commit that carries this file: the import counts are 5 (elf
// x86_64) / 5 (elf arm64) / 8 (pe) / 5 (macho x86_64) / 5 (macho arm64), against a
// pre-fix 86 / 86 / 85 / 97 / 97, and `examples/c/descriptor_import_referenced_only`
// exits 42 with byte-exact stdout on pe64, elf64-x86_64 and elf64-aarch64.
//
// ⇒ A failure here reading "the eager law is back" means that default went back to
// `true`. Fix the default; do NOT relax this file. The FIELD itself must stay — its
// one true remaining producer is the SEH personality (`synth_seh_funclets.cpp`),
// whose reference lives in the pe UNWIND_INFO handler RVA and is therefore invisible
// to a reloc scan.

#include "core/types/diagnostic_reporter.hpp"
#include "image_dependency_table.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

using dss::DiagnosticReporter;
using dss::Program;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// ── THE PROBE PROGRAM ────────────────────────────────────────────────────────
//
// Three descriptor headers (~120 declared symbols between them) and exactly four
// referenced names, in three DIFFERENT reference shapes so the gate's reference
// scan is exercised where it is weakest:
//   * `g_table`  — a DATA-ITEM relocation (an imported function's address in a
//                  static initializer; the sqlite `aSyscall[]` shape). The gate
//                  scans data-item relocations as well as function ones.
//   * `strlen`   — an ordinary direct CALL relocation.
//   * `stdout`   — an imported DATA OBJECT, lowered GOT-INDIRECT on elf/macho and
//                  reached through `__acrt_iob_func` on pe: two lowerings of one
//                  construct, and the reference a reloc-based scan is likeliest
//                  to miss.
// Byte-for-byte the same program as the corpus example, deliberately — the two
// halves of one claim must be measured on one subject.
constexpr char const* kProbeSource =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "typedef int (*putfn)(char const *);\n"
    "static putfn const g_table[] = { puts };\n"
    "int main(void) {\n"
    "    if (strlen(\"referenced-only\") != 15u) { return 91; }\n"
    "    if (fputs(\"descriptor_import_referenced_only: ok\\n\", stdout) < 0) { return 92; }\n"
    "    {\n"
    "        volatile putfn f = g_table[0];\n"
    "        if (((putfn)f)(\"descriptor_import_referenced_only: indirect ok\") < 0) { return 93; }\n"
    "    }\n"
    "    return 42;\n"
    "}\n";

// ── EXTRACTORS ───────────────────────────────────────────────────────────────

// ELF64: every `.dynsym` row whose `st_shndx` is SHN_UNDEF and whose `st_name` is
// non-zero — the names this image asks ld.so to resolve. Elf64_Sym is 24 bytes
// {u32 st_name, u8 st_info, u8 st_other, u16 st_shndx, u64 st_value, u64 st_size}.
// Returns EMPTY on any buffer it cannot parse; every caller guards that with a
// presence assertion first.
[[nodiscard]] std::vector<std::string>
elfUndefinedDynamicSymbols(std::vector<std::uint8_t> const& b) {
    using namespace dss::test_support::image_deps_detail;
    std::vector<std::string> out;
    if (b.size() < 0x40) return out;
    if (!(b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F')) return out;
    std::uint64_t const shoff     = rdU64(b, 0x28);
    std::uint16_t const shentsize = rdU16(b, 0x3A);
    std::uint16_t const shnum     = rdU16(b, 0x3C);
    std::uint16_t const shstrndx  = rdU16(b, 0x3E);
    if (shoff == 0 || shentsize < 64 || shnum == 0 || shstrndx >= shnum) return out;

    auto secOff = [&](std::uint16_t i) -> std::size_t {
        return static_cast<std::size_t>(shoff)
             + static_cast<std::size_t>(i) * shentsize;
    };
    if (secOff(shnum) > b.size()) return out;
    std::size_t const shstrBase =
        static_cast<std::size_t>(rdU64(b, secOff(shstrndx) + 0x18));

    std::size_t dynsymOff = 0, dynsymSize = 0, dynstrOff = 0;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::size_t const o  = secOff(i);
        std::string const nm = rdCStr(b, shstrBase + rdU32(b, o + 0));
        if (nm == ".dynsym") {
            dynsymOff  = static_cast<std::size_t>(rdU64(b, o + 0x18));
            dynsymSize = static_cast<std::size_t>(rdU64(b, o + 0x20));
        } else if (nm == ".dynstr") {
            dynstrOff = static_cast<std::size_t>(rdU64(b, o + 0x18));
        }
    }
    if (dynsymOff == 0 || dynstrOff == 0 || dynsymSize < 24) return out;
    if (dynsymOff + dynsymSize > b.size()) return out;

    for (std::size_t o = dynsymOff; o + 24 <= dynsymOff + dynsymSize; o += 24) {
        std::uint32_t const stName  = rdU32(b, o + 0);
        std::uint16_t const stShndx = rdU16(b, o + 6);
        if (stShndx != 0 || stName == 0) continue;   // SHN_UNDEF and named only
        out.push_back(rdCStr(b, dynstrOff + stName));
    }
    return out;
}

// Mach-O 64: every LC_SYMTAB nlist_64 row that is UNDEFINED and EXTERNAL — the
// names dyld must bind. nlist_64 is 16 bytes {u32 n_strx, u8 n_type, u8 n_sect,
// u16 n_desc, u64 n_value}; N_TYPE is the 0x0e mask (N_UNDF == 0) and N_EXT the
// 0x01 bit. Returns EMPTY on a buffer it cannot parse.
[[nodiscard]] std::vector<std::string>
machoUndefinedSymbols(std::vector<std::uint8_t> const& b) {
    using namespace dss::test_support::image_deps_detail;
    std::vector<std::string> out;
    if (b.size() < 32) return out;
    if (rdU32(b, 0) != 0xFEEDFACFu) return out;   // MH_MAGIC_64, little-endian
    constexpr std::uint32_t kLcSymtab = 0x02u;
    std::uint32_t const ncmds = rdU32(b, 16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds && off + 8 <= b.size(); ++i) {
        std::uint32_t const cmd     = rdU32(b, off);
        std::uint32_t const cmdsize = rdU32(b, off + 4);
        if (cmdsize == 0) break;
        if (cmd == kLcSymtab && off + 24 <= b.size()) {
            std::size_t const symoff = rdU32(b, off + 8);
            std::size_t const nsyms  = rdU32(b, off + 12);
            std::size_t const stroff = rdU32(b, off + 16);
            for (std::size_t k = 0; k < nsyms; ++k) {
                std::size_t const o = symoff + k * 16;
                if (o + 16 > b.size()) break;
                std::uint8_t const nType = b[o + 4];
                if ((nType & 0x0Eu) != 0x00u) continue;   // not N_UNDF
                if ((nType & 0x01u) == 0u) continue;      // not N_EXT
                out.push_back(rdCStr(b, stroff + rdU32(b, o + 0)));
            }
        }
        off += cmdsize;
    }
    return out;
}

[[nodiscard]] bool has(std::vector<std::string> const& v, std::string_view n) {
    return std::find(v.begin(), v.end(), n) != v.end();
}

[[nodiscard]] std::string join(std::vector<std::string> const& v) {
    std::string s;
    for (auto const& e : v) {
        if (!s.empty()) s += ", ";
        s += e;
    }
    return s;
}

[[nodiscard]] std::vector<std::uint8_t> readWholeBinary(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

[[nodiscard]] fs::path writeSrc(fs::path const& dir, std::string_view name,
                                std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream(p, std::ios::binary) << text;
    return p;
}

// One source, one target, through the real production driver.
[[nodiscard]] int buildOne(fs::path const& outDir, fs::path const& src,
                           std::string const& target, DiagnosticReporter& rep) {
    Program p;
    p.setOutputDir(outDir);
    return p.compileFiles(std::vector<std::string>{src.string()}, "c",
                          std::vector<std::string>{target}, rep);
}

// What one target is expected to look like. `referenced` and `absent` are spelled
// in the FORMAT's own decorated vocabulary (Mach-O's leading underscore is a
// property of the format, not a detail to paper over), which is also why the two
// lists are per-format data rather than one list run through a mangler here: a
// mangler in the test would reproduce the compiler's own rule and could agree with
// it while both were wrong.
struct FormatExpectation {
    char const*                    spec;
    char const*                    artifact;
    std::vector<std::string> const referenced;   // MUST be imported
    std::vector<std::string> const absent;       // MUST NOT be imported
    std::size_t                    ceiling;      // total imported names, upper bound
};

// The imported-symbol names of an emitted image, per format. One entry point so a
// caller never picks the wrong extractor for a spec.
[[nodiscard]] std::vector<std::string>
importedSymbolsOf(std::vector<std::uint8_t> const& bytes) {
    if (bytes.size() >= 4 && bytes[0] == 0x7F && bytes[1] == 'E') {
        return elfUndefinedDynamicSymbols(bytes);
    }
    if (bytes.size() >= 2 && bytes[0] == 'M' && bytes[1] == 'Z') {
        return dss::test_support::peImportedSymbols(bytes);
    }
    return machoUndefinedSymbols(bytes);
}

// ── THE EXPECTATIONS ─────────────────────────────────────────────────────────
//
// The `absent` names are all MEASURED members of the pre-fix import table for
// this exact source — not plausible-looking picks. Each is declared by one of the
// three included headers and referenced by nothing in the program.
//
// ⚠ `memcpy` / `memset` are deliberately NOT in any `absent` list: codegen may
// legitimately materialize a call to either for an aggregate copy, so their
// absence is not a property of this change and asserting it would make the pin
// fail for a reason it does not describe.
//
// The `ceiling` is the second half of the drop assertion and the half that cannot
// rot: a regression that re-imports a name nobody listed still pushes the total
// past it. ✔MEASURED with the fix in place — elf x86_64 **5**, elf arm64 **5**,
// pe **8**, macho x86_64 **5**, macho arm64 **5** — against a pre-fix 86 / 85 / 97.
// The ceilings are set THREE above each measurement, which is headroom for the
// entry trampoline's own imports to grow (`exit` is the only one today) without
// being anywhere near a return of the eager law. A ceiling is an UPPER bound only:
// the count may FALL freely, which is what lets the pe surplus documented below be
// fixed without touching this file.
[[nodiscard]] std::vector<FormatExpectation> expectations() {
    return {
        {"x86_64:elf64-x86_64-linux-exec", "probe",
         {"puts", "strlen", "fputs", "stdout"},
         {"qsort", "bsearch", "system", "popen", "strstr", "atoi", "malloc",
          "free", "fopen", "rand", "printf", "vfprintf", "setlocale"},
         8},
        {"arm64:elf64-aarch64-linux-exec", "probe",
         {"puts", "strlen", "fputs", "stdout"},
         {"qsort", "bsearch", "system", "popen", "strstr", "atoi", "malloc",
          "free", "fopen", "rand", "printf", "vfprintf", "setlocale"},
         8},
        {"x86_64:pe64-x86_64-windows-exec", "probe.exe",
         {"puts", "strlen", "fputs", "__acrt_iob_func"},
         {"qsort", "bsearch", "system", "strstr", "atoi", "malloc", "free",
          "fopen", "rand", "setlocale", "_set_abort_behavior", "__p__fmode",
          "__p__environ"},
         // pe's ceiling is its siblings' now, and the history is why it was
         // not. ✔MEASURED before P69: pe imported 8 where elf and macho import
         // 5, the three extra `__stdio_common_vfprintf` / `_vsprintf` /
         // `_vsscanf` — held alive by the UCRT SHIM SYNTHESIS, which emitted a
         // body for every `synthesize` row `#include <stdio.h>` injected,
         // called or not, so the ceiling sat two higher. P69 retired the
         // synthesis for DSS's runtime source (runtime/platform/src/stdio.c),
         // and the same surplus came back ONE LAYER DOWN: the static-archive
         // pull followed every extern the module DECLARED, so the unused rows
         // linked the runtime member and its UCRT imports (✔MEASURED, 9). With
         // the pull behind the same reference gate as the import table
         // (D-LK-ARCHIVE-PULL-TAKES-UNREFERENCED-EXTERNS-AS-REFERENCES) the
         // count is the five the probe uses; `__acrt_iob_func` is one of them,
         // referenced through the `stdout` macro. The surplus itself is pinned
         // below, by `AnIncludedHeaderNothingUsesLinksNothingIn`.
         8},
        {"x86_64:macho64-x86_64-darwin-exec", "probe",
         {"_puts", "_strlen", "_fputs", "___stdoutp"},
         {"_qsort", "_bsearch", "_system", "_popen", "_strstr", "_atoi",
          "_malloc", "_free", "_fopen", "_rand", "_printf", "_setlocale"},
         8},
        {"arm64:macho64-arm64-darwin-exec", "probe",
         {"_puts", "_strlen", "_fputs", "___stdoutp"},
         {"_qsort", "_bsearch", "_system", "_popen", "_strstr", "_atoi",
          "_malloc", "_free", "_fopen", "_rand", "_printf", "_setlocale"},
         8},
    };
}

}  // namespace

// ── THE PIN ──────────────────────────────────────────────────────────────────
//
// HOST-INDEPENDENT: nothing is RUN. "What does this image ask its loader to
// resolve" is a judgment about bytes DSS just wrote, so the Windows, Linux and
// macOS legs all answer it, for all five targets.
TEST(DescriptorImportReferencedOnly, EmittedImageImportsOnlyReferencedDescriptorSymbols) {
    for (auto const& e : expectations()) {
        SCOPED_TRACE(e.spec);
        ScratchDir scratch{Location::InsideRepo, "descriptor-referenced-only"};
        auto const dir = scratch.path();
        auto const src = writeSrc(dir, "probe.c", kProbeSource);

        DiagnosticReporter rep;
        ASSERT_EQ(buildOne(dir, src, e.spec, rep), 0)
            << "the build must succeed before its import table means anything. "
            << (rep.all().empty() ? std::string{} : rep.all().front().actual);
        auto const artifact = dir / e.artifact;
        ASSERT_TRUE(fs::exists(artifact)) << artifact.generic_string();

        auto const bytes = readWholeBinary(artifact);
        ASSERT_FALSE(bytes.empty()) << artifact.generic_string();
        auto const names = importedSymbolsOf(bytes);

        // (1) THE INSTRUMENT'S OWN PRECONDITION. An extractor that stopped
        //     parsing returns empty, and empty satisfies every absence below.
        ASSERT_FALSE(names.empty())
            << "recovered NO imported symbol names from " << e.spec
            << " — the extractor is broken, so every absence assertion here "
               "would be vacuous";

        // ⓘ Both loops below COLLECT and then assert ONCE, rather than asserting
        //    per name. The per-name form named every offender — and reprinted the
        //    whole 86-entry import table beside each of them, thirteen times per
        //    target, which buries the one line a reader needs. One assertion per
        //    CLASS keeps every name and prints the table once.

        // (2) NOTHING REFERENCED WAS OVER-PRUNED. This is the dangerous
        //     direction: an import too FEW is a program that calls through a
        //     slot nothing filled, and the four names are three DIFFERENT
        //     reference shapes (data-item reloc, direct call, imported data
        //     object) reaching one gate.
        std::vector<std::string> dropped;
        for (auto const& want : e.referenced) {
            if (!has(names, want)) dropped.push_back(want);
        }
        EXPECT_TRUE(dropped.empty())
            << dropped.size() << " name(s) the probe REFERENCES are missing from "
            << e.spec << "'s import table — the reference gate dropped a live "
               "import, which is the direction that produces a call through a slot "
               "nothing filled: [" << join(dropped) << "]. Imported: ["
            << join(names) << ']';

        // (3) THE DROP. Every one of these is declared by an included header,
        //     referenced by nothing, and was MEASURED present before the change.
        std::vector<std::string> stillThere;
        for (auto const& gone : e.absent) {
            if (has(names, gone)) stillThere.push_back(gone);
        }
        EXPECT_TRUE(stillThere.empty())
            << stillThere.size() << " name(s) declared by an included descriptor "
               "and referenced by NOTHING are still imported by " << e.spec
            << " — the eager law is back: [" << join(stillThere)
            << "]. Fix `ShippedExternSymbol::eagerImport`'s default in "
               "src/analysis/semantic/semantic_model.hpp (`true` → `false`); do "
               "not relax this pin. Imported: [" << join(names) << ']';

        // (4) THE HALF THAT CANNOT ROT. A regression that re-imports names
        //     nobody hand-listed still fails here.
        EXPECT_LE(names.size(), e.ceiling)
            << e.spec << " imports " << names.size()
            << " symbols for a program that references four. The pre-fix "
               "measurement was 86 (elf) / 85 (pe) / 97 (macho); gcc, clang, "
               "MSVC and mingw-w64 all import only what is referenced. Fix "
               "`ShippedExternSymbol::eagerImport`'s default in "
               "src/analysis/semantic/semantic_model.hpp (`true` → `false`). "
               "Imported: [" << join(names) << ']';
    }
}

// ── THE LIBRARY-LEVEL HALF, WHERE ONE FORMAT CAN ANSWER IT ───────────────────
//
// An unreferenced header must cost no DEPENDENCY either, not merely no symbol.
// ELF is the only format whose shipped corpus splits the C library across two
// images (`math.json` names `libm.so.6` where `stdio/string/stdlib.json` name
// `libc.so.6`); pe and macho route every one of them to a single image
// (`ucrtbase.dll`, `/usr/lib/libSystem.B.dylib`), so on those two the question
// "did the unused header add a dependency" has no observable answer at all. That
// is a property of the corpus, stated rather than papered over with a
// same-image assertion that could never fail.
//
// ✔MEASURED before the change: `#include <math.h>` with NO math reference emitted
// DT_NEEDED `libm.so.6` and 42 math imports.
//
// ✔THE REFERENCES, MEASURED SEPARATELY ON THIS EXACT QUESTION (`readelf -d` NEEDED):
//   clang 18.1.3 — `#include <math.h>` + nothing: NEEDED libc.so.6 ONLY. Its CONTROL
//                  FIRED: the same TU calling `sqrt`, linked `-lm`, gets
//                  `libm.so.6 libc.so.6` and imports `sqrt`.
//   gcc 13.3.0   — `#include <math.h>` + nothing: NEEDED libc.so.6 ONLY. ⚠ Its
//                  CONTROL DID NOT FIRE and that is recorded as an ABSTENTION on the
//                  control, not as agreement: gcc folds `sqrt(1764.0)` through
//                  `__builtin_sqrt` even at -O0, so its `usemath` arm imports no
//                  `sqrt` and needs no libm either. The verdict half still stands
//                  (no dependency for the unreferenced header); the discrimination
//                  half rests on clang.
//   MSVC         — ABSTAINS BY CONSTRUCTION: the UCRT ships math in the same image as
//                  everything else, so "did the unused header add a dependency" has
//                  no observable answer there, exactly as it has none on DSS's own
//                  pe and macho arms.
TEST(DescriptorImportReferencedOnly, UnreferencedHeaderAddsNoElfLibraryDependency) {
    ScratchDir scratch{Location::InsideRepo, "descriptor-referenced-only-lib"};
    auto const dir  = scratch.path();
    char const* kSpec = "x86_64:elf64-x86_64-linux-exec";

    // THE CONTROL, FIRST. A TU that DOES call into <math.h> must carry the
    // dependency — otherwise "absent" below is a statement about a broken
    // extractor rather than about the compiler.
    {
        auto const src = writeSrc(dir, "usemath.c",
            "#include <math.h>\n"
            "int main(void) { return (int)sqrt(1764.0); }\n");
        DiagnosticReporter rep;
        ASSERT_EQ(buildOne(dir, src, kSpec, rep), 0)
            << (rep.all().empty() ? std::string{} : rep.all().front().actual);
        auto const bytes = readWholeBinary(dir / "usemath");
        ASSERT_FALSE(bytes.empty());
        auto const libs = dss::test_support::elfNeededLibraries(bytes);
        ASSERT_EQ(dss::test_support::dependencyOccurrences(libs, "libm.so.6"), 1u)
            << "a TU that calls sqrt must DT_NEEDED libm.so.6 exactly once; got ["
            << dss::test_support::joinDependencies(libs) << ']';
        EXPECT_TRUE(has(elfUndefinedDynamicSymbols(bytes), "sqrt"))
            << "sqrt is called and must be imported";
    }

    // THE PIN. The same header, referenced by nothing.
    {
        auto const src = writeSrc(dir, "nomath.c",
            "#include <math.h>\n"
            "int main(void) { return 42; }\n");
        DiagnosticReporter rep;
        ASSERT_EQ(buildOne(dir, src, kSpec, rep), 0)
            << (rep.all().empty() ? std::string{} : rep.all().front().actual);
        auto const bytes = readWholeBinary(dir / "nomath");
        ASSERT_FALSE(bytes.empty());
        auto const libs  = dss::test_support::elfNeededLibraries(bytes);
        auto const names = elfUndefinedDynamicSymbols(bytes);
        EXPECT_EQ(dss::test_support::dependencyOccurrences(libs, "libm.so.6"), 0u)
            << "a TU that includes <math.h> and references nothing from it must "
               "carry NO libm.so.6 dependency — MEASURED, gcc 13.3.0 and clang "
               "18.1.3 both emit NEEDED libc.so.6 alone for this program, and "
               "clang's control fires (it adds libm.so.6 when sqrt is really "
               "referenced). Got dependencies ["
            << dss::test_support::joinDependencies(libs)
            << "] and imports [" << join(names) << ']';
        EXPECT_FALSE(has(names, "sqrt"))
            << "sqrt is declared by the included header and referenced by nothing";
        EXPECT_FALSE(has(names, "pow"))
            << "pow is declared by the included header and referenced by nothing";
    }
}

// ── D-LK-ARCHIVE-PULL-TAKES-UNREFERENCED-EXTERNS-AS-REFERENCES ──────────────────
//
// The same law one tier down: a header's declarations must not link anything
// in when nothing uses them — no import (above), and no BODY. DSS realizes some
// rows from its own runtime source (the pe and Mach-O printf/scanf/strto
// families, <stdlib.h>'s C23 names), compiled into single-member archives that
// the link pulls by reference; the pull used to follow every extern a module
// DECLARED, so `#include <stdio.h>` with an empty `main` linked the whole stdio
// runtime and its UCRT imports into the image (✔MEASURED P69, pe64: `.text`
// 0x23 -> 0x7c3, imports 1 -> 6). gcc, clang and MSVC link nothing for an
// unused declaration (their objects carry no undefined symbol for one).
//
// And the same law one tier EARLIER, for the bodies DSS SYNTHESIZES rather than
// pulls (D-MIR-THREADS-SHIM-SYNTHESIZES-UNREFERENCED-RECIPES): the <threads.h>
// pass synthesized a body for every recipe a header injected, so `#include
// <threads.h>` alone put all of them and their helpers into the image (✔MEASURED
// P69: pe64 `.text` 0x23 -> 0x81c, imports 1 -> 26; Mach-O arm64 0x18 -> 0x508,
// 1 -> 29) — and on Mach-O x86_64, whose format declares no threads vehicle, the
// program did not compile at all. <stdlib.h> carries C23's once trio too
// (call_once, DSS's runtime source on pe).
//
// THE PIN compares two programs that differ ONLY by the headers they include:
// the code section and the import list must be IDENTICAL, on every pair. An
// equality, not a magic number — any member a declaration drags in grows one
// side alone. RED-ON-DISABLE: the pull's worklist taking every declared extern
// again turns pe and both Mach-O pairs red (their runtime members are pulled);
// the threads pass synthesizing every recipe again turns pe and Mach-O arm64 red
// on the comparison and Mach-O x86_64 red on the compile.
namespace {

// The size of an image's main code section: PE `.text` VirtualSize, ELF `.text`
// sh_size, Mach-O `__TEXT,__text` size. 0 when the image is not one of the three.
[[nodiscard]] std::uint64_t codeSectionSize(std::vector<std::uint8_t> const& b) {
    using namespace dss::test_support::image_deps_detail;
    // A fixed-width, NUL-padded name field (PE's 8 bytes, Mach-O's 16).
    auto fixedName = [&](std::size_t at, std::size_t width) {
        std::string s;
        for (std::size_t i = 0; i < width && at + i < b.size() && b[at + i] != 0; ++i) {
            s.push_back(static_cast<char>(b[at + i]));
        }
        return s;
    };
    if (b.size() >= 0x40 && b[0] == 'M' && b[1] == 'Z') {
        std::size_t const peOff = rdU32(b, 0x3C);
        if (peOff + 24 > b.size()) return 0;
        std::size_t   const coffOff   = peOff + 4;
        std::uint16_t const numSecs   = rdU16(b, coffOff + 2);
        std::uint16_t const optSize   = rdU16(b, coffOff + 16);
        std::size_t   const secTabOff = coffOff + 20 + optSize;
        for (std::size_t i = 0; i < numSecs && secTabOff + (i + 1) * 40 <= b.size(); ++i) {
            std::size_t const s = secTabOff + i * 40;
            if (fixedName(s, 8) == ".text") return rdU32(b, s + 8);   // VirtualSize
        }
        return 0;
    }
    if (b.size() >= 0x40 && b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') {
        std::uint64_t const shoff     = rdU64(b, 0x28);
        std::uint16_t const shentsize = rdU16(b, 0x3A);
        std::uint16_t const shnum     = rdU16(b, 0x3C);
        std::uint16_t const shstrndx  = rdU16(b, 0x3E);
        if (shoff == 0 || shentsize < 64 || shstrndx >= shnum) return 0;
        auto secOff = [&](std::size_t i) { return static_cast<std::size_t>(shoff) + i * shentsize; };
        if (secOff(shnum) > b.size()) return 0;
        std::size_t const strBase = static_cast<std::size_t>(rdU64(b, secOff(shstrndx) + 0x18));
        for (std::size_t i = 0; i < shnum; ++i) {
            if (rdCStr(b, strBase + rdU32(b, secOff(i))) == ".text") return rdU64(b, secOff(i) + 0x20);
        }
        return 0;
    }
    if (b.size() >= 32 && rdU32(b, 0) == 0xFEEDFACFu) {
        std::uint32_t const ncmds = rdU32(b, 16);
        std::size_t off = 32;
        for (std::uint32_t c = 0; c < ncmds && off + 8 <= b.size(); ++c) {
            std::uint32_t const cmd     = rdU32(b, off);
            std::uint32_t const cmdsize = rdU32(b, off + 4);
            if (cmdsize == 0) break;
            if (cmd == 0x19u && off + 72 <= b.size()) {   // LC_SEGMENT_64
                std::uint32_t const nsects = rdU32(b, off + 64);
                for (std::uint32_t k = 0; k < nsects; ++k) {
                    std::size_t const s = off + 72 + static_cast<std::size_t>(k) * 80;
                    if (s + 80 > b.size()) break;
                    if (fixedName(s, 16) == "__text" && fixedName(s + 16, 16) == "__TEXT") {
                        return rdU64(b, s + 40);
                    }
                }
            }
            off += cmdsize;
        }
    }
    return 0;
}

}  // namespace

TEST(DescriptorImportReferencedOnly, AnIncludedHeaderNothingUsesLinksNothingIn) {
    struct Pair {
        char const* spec;
        char const* artifact;
    };
    for (Pair const p : {Pair{"x86_64:pe64-x86_64-windows-exec", "probe.exe"},
                         Pair{"x86_64:elf64-x86_64-linux-exec", "probe"},
                         Pair{"arm64:elf64-aarch64-linux-exec", "probe"},
                         Pair{"arm64:macho64-arm64-darwin-exec", "probe"},
                         Pair{"x86_64:macho64-x86_64-darwin-exec", "probe"}}) {
        SCOPED_TRACE(p.spec);
        auto measure = [&](char const* source, std::uint64_t& code, std::vector<std::string>& imports) {
            ScratchDir scratch{Location::InsideRepo, "header-links-nothing"};
            auto const dir = scratch.path();
            auto const src = writeSrc(dir, "probe.c", source);
            DiagnosticReporter rep;
            ASSERT_EQ(buildOne(dir, src, p.spec, rep), 0)
                << (rep.all().empty() ? std::string{} : rep.all().front().actual);
            auto const bytes = readWholeBinary(dir / p.artifact);
            ASSERT_FALSE(bytes.empty());
            code    = codeSectionSize(bytes);
            imports = importedSymbolsOf(bytes);
            ASSERT_NE(code, 0u) << "the code-section reader found no code section -- the pin would be vacuous";
            ASSERT_FALSE(imports.empty()) << "the import reader found nothing -- the pin would be vacuous";
        };
        std::uint64_t            bareCode = 0, includedCode = 0;
        std::vector<std::string> bareImports, includedImports;
        measure("int main(void) { return 42; }\n", bareCode, bareImports);
        measure("#include <inttypes.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <threads.h>\n"
                "int main(void) { return 42; }\n",
                includedCode, includedImports);
        EXPECT_EQ(includedCode, bareCode)
            << "including <stdio.h>, <stdlib.h>, <inttypes.h> and <threads.h> and using none of them "
               "must link (or synthesize) no body in";
        std::sort(bareImports.begin(), bareImports.end());
        std::sort(includedImports.begin(), includedImports.end());
        EXPECT_EQ(includedImports, bareImports)
            << "...and import nothing: [" << join(includedImports) << "] vs [" << join(bareImports) << "]";
    }
}
