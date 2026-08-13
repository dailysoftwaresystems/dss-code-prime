// ★★★ A HAND-WRITTEN `.s` BINDS ITS EXTERNS TO A REAL IMAGE
// (D-ASM-EXTERN-CALL-CANNOT-BIND-A-LIBRARY +
//  D-ASM-RESOLVE-LIBRARY-SILENTLY-IGNORED-ON-ENCODE-TIER, closed 2026-08-13).
//
// A `.s` that calls libc used to be REFUSED on every EXEC format:
//   error[K_SymbolUndefined] undefined symbol 'putchar'
// The `encode` pipeline tier never asked the platform who owns a name. The C
// path has asked since UCRT-P4 — `ffi::realizeShippedExternSymbols` answers it
// for a HAND-DECLARED prototype, precisely so `extern int putchar(int);` and
// `#include <stdio.h>` realize identically — and `call putchar` from assembly
// is the same claim about the same platform. Worse, `--resolve-library` was
// ACCEPTED on this tier and dropped without a word, so the operator who passed
// the exact flag that should have bound the symbol got the same
// "no library import binds it" message.
//
// ── WHY THIS TIER AND NOT `examples/**` ─────────────────────────────────────
// Two of the four claims here have no example-manifest surface at all:
//   * `--resolve-library` is a DRIVER flag; a corpus example declares sources
//     and targets, not link inputs.
//   * the per-format DECORATION gate is about a source that must be REFUSED on
//     Mach-O and ACCEPTED on ELF/PE — one text, three verdicts, in one process.
// The positive end-to-end path (build it, run it, read its stdout) belongs in
// `examples/asm/**` and is requested there; this file pins the machinery the
// example cannot reach.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// `putchar` is the fixture symbol on purpose: stdio.json declares it for EVERY
// object format with an ordinary library row (no `synthesize` recipe, no
// per-target `linkName`), so a single source text is realizable on all five
// shipped legs and the only thing that varies is the platform's answer for
// which image owns it. `printf` is deliberately NOT used — on pe it is a
// `synthesize` row, which this tier must refuse (its own test below).
constexpr std::string_view kCallsPutchar =
    "\t.text\n"
    "\t.globl\tmain\n"
    "\t.type\tmain, @function\n"
    "main:\n"
    "\tsubq\t$40, %rsp\n"
    "\tmovl\t$42, %ecx\n"      // Win64 first integer arg
    "\tmovl\t$42, %edi\n"      // SysV first integer arg
    "\tcall\tputchar\n"
    "\tmovl\t$0, %eax\n"
    "\taddq\t$40, %rsp\n"
    "\tret\n";

// The SAME file with the Mach-O on-binary spelling of the symbol. gas requires
// the leading underscore there and `asm_text_to_lir` takes a written name
// VERBATIM, so this — not the undecorated form — is what a real Darwin `.s`
// contains.
constexpr std::string_view kCallsUnderscorePutchar =
    "\t.text\n"
    "\t.globl\t_main\n"
    "\t.type\t_main, @function\n"
    "_main:\n"
    "\tsubq\t$40, %rsp\n"
    "\tmovl\t$42, %edi\n"
    "\tcall\t_putchar\n"
    "\tmovl\t$0, %eax\n"
    "\taddq\t$40, %rsp\n"
    "\tret\n";

// pe realizes `printf` as a COMPILER-SYNTHESIZED shim over
// `__stdio_common_vfprintf` (ucrtbase exports no bare `printf`), and the
// `encode` tier runs no MIR synthesis. Binding it would link clean and die at
// LOAD with 0xC0000139.
constexpr std::string_view kCallsPrintf =
    "\t.text\n"
    "\t.globl\tmain\n"
    "\t.type\tmain, @function\n"
    "main:\n"
    "\tsubq\t$40, %rsp\n"
    "\tcall\tprintf\n"
    "\tmovl\t$0, %eax\n"
    "\taddq\t$40, %rsp\n"
    "\tret\n";

constexpr std::string_view kAttLanguage = "asm-x86_64-att";

fs::path writeFile(fs::path const& dir, std::string_view name,
                   std::string_view text) {
    auto const p = dir / std::string{name};
    std::ofstream f(p, std::ios::binary);
    f << text;
    return p;
}

[[nodiscard]] std::string allDiagnosticText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        out += d.contextPrefix;
        out += ' ';
        out += d.actual;
        out += '\n';
    }
    return out;
}

[[nodiscard]] bool sawCode(DiagnosticReporter const& rep, DiagnosticCode c) {
    for (auto const& d : rep.all()) {
        if (d.code == c) return true;
    }
    return false;
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// THE HEADLINE: `call putchar` LINKS AN EXEC, ON EVERY NON-DECORATING FORMAT.
// ════════════════════════════════════════════════════════════════════════════
//
// One source text, three (target × format) legs, one platform oracle. The claim
// is not "it compiles" — a relocatable `.o` already did, which is exactly why
// the gap survived — it is that the EXEC LINK, where nothing binds later,
// resolves the reference.
//
// ── RED-ON-DISABLE (measured) ───────────────────────────────────────────────
// Delete the `realizeShippedExternSymbols` call from `bindAsmExternImports`
// (src/program/compile_pipeline.cpp) — or revert `assembleAsmUnit` to taking
// `entryVerbs()` instead of the whole `ObjectFormatSchema`, which removes the
// data model and format kind the oracle needs — and every leg below comes back
// `K_SymbolUndefined`, which the test asserts is ABSENT.
TEST(AsmExternBinding, CallToLibcBindsAndLinksAnExecOnEveryFormat) {
    for (std::string_view spec : {"x86_64:elf64-x86_64-linux-exec",
                                  "x86_64:pe64-x86_64-windows-exec"}) {
        ScratchDir scratch{Location::InsideRepo, "asm-extern"};
        scratch.useAsCwd();
        auto const src = writeFile(scratch.path(), "callslibc.s", kCallsPutchar);

        DiagnosticReporter rep;
        Program            prog;
        int const rc = prog.compileFiles({src.generic_string()},
                                         std::string{kAttLanguage},
                                         {std::string{spec}}, rep);
        std::string const text = allDiagnosticText(rep);

        EXPECT_EQ(rc, 0) << "target " << spec
                         << ": a `.s` calling libc must build an EXEC\n"
                         << text;
        EXPECT_FALSE(sawCode(rep, DiagnosticCode::K_SymbolUndefined))
            << "target " << spec
            << ": the platform's descriptor corpus owns 'which image exports "
               "putchar here'; the encode tier must consult it exactly as the "
               "C path does for a hand-declared prototype\n"
            << text;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// `--resolve-library` IS HONOURED ON THIS TIER — not accepted and dropped.
// ════════════════════════════════════════════════════════════════════════════
//
// The observable is chosen so it cannot be satisfied by the descriptor oracle
// having bound the symbol anyway: the flag names a path that does NOT EXIST. A
// tier that honours the flag must open it and fail LOUD; a tier that drops it
// silently would sail past and produce an artifact.
//
// ⚠ THIS IS THE POINT OF THE PIN. Asserting the happy path ("point it at a real
// library and the symbol binds") would stay green under a driver that ignored
// the flag entirely, because the corpus binds `putchar` on its own.
//
// RED-ON-DISABLE: remove the `opts` parameter from `assembleAsmUnit` (or stop
// passing `perCuOpts` at src/program/program.cpp) and the bogus path is never
// opened — rc becomes 0 and `F_FileOpenFailed` never appears.
TEST(AsmExternBinding, ResolveLibraryIsHonouredNotSilentlyDroppedOnEncodeTier) {
    ScratchDir scratch{Location::InsideRepo, "asm-extern"};
    scratch.useAsCwd();
    auto const src = writeFile(scratch.path(), "flag.s", kCallsPutchar);
    fs::path const bogus = scratch.path() / "no-such-library.dll";
    ASSERT_FALSE(fs::exists(bogus));

    DiagnosticReporter rep;
    Program            prog;
    prog.setResolveLibraries(std::vector<fs::path>{bogus});
    int const rc = prog.compileFiles({src.generic_string()},
                                     std::string{kAttLanguage},
                                     {std::string{"x86_64:pe64-x86_64-windows-exec"}},
                                     rep);
    std::string const text = allDiagnosticText(rep);

    EXPECT_NE(rc, 0)
        << "a `--resolve-library` path that cannot be opened must FAIL the "
           "build on the encode tier, exactly as it does on the C path — "
           "accepting the flag and quietly ignoring it is the fail-loud rule "
           "inverted\n"
        << text;
    EXPECT_TRUE(sawCode(rep, DiagnosticCode::F_FileOpenFailed))
        << "the refusal must name the unreadable library\n" << text;
}

// ════════════════════════════════════════════════════════════════════════════
// A `synthesize` ROW IS REFUSED LOUD, AND ONLY WHERE IT IS A RECIPE.
// ════════════════════════════════════════════════════════════════════════════
//
// `printf` is a shim on pe and an ordinary import on elf. ONE source, TWO
// verdicts, from ONE config-driven policy — which is also what proves the
// refusal is not a format branch: the same code path answers both ways because
// the descriptor says different things.
//
// RED-ON-DISABLE: drop the `recipeId.empty()` guard in `bindAsmExternImports`
// and the pe leg binds `printf` to ucrtbase.dll, links green, and produces a
// binary that fails at LOAD — rc becomes 0 and the assertion below reds.
TEST(AsmExternBinding, SynthesizeRecipeRowIsRefusedOnPeAndBoundOnElf) {
    {
        ScratchDir scratch{Location::InsideRepo, "asm-extern"};
        scratch.useAsCwd();
        auto const src = writeFile(scratch.path(), "shim.s", kCallsPrintf);
        DiagnosticReporter rep;
        Program            prog;
        int const rc = prog.compileFiles(
            {src.generic_string()}, std::string{kAttLanguage},
            {std::string{"x86_64:pe64-x86_64-windows-exec"}}, rep);
        std::string const text = allDiagnosticText(rep);
        EXPECT_NE(rc, 0)
            << "pe realizes printf as a COMPILER-SYNTHESIZED body; the encode "
               "tier emits none, and importing the name directly would link "
               "clean and fail at LOAD\n" << text;
        EXPECT_TRUE(sawCode(rep, DiagnosticCode::A_AsmTextUnsupported)) << text;
        EXPECT_NE(text.find("SYNTHESIZED"), std::string::npos)
            << "the refusal must say WHY, not merely that the symbol is "
               "unavailable\n" << text;
    }
    {
        ScratchDir scratch{Location::InsideRepo, "asm-extern"};
        scratch.useAsCwd();
        auto const src = writeFile(scratch.path(), "shim.s", kCallsPrintf);
        DiagnosticReporter rep;
        Program            prog;
        int const rc = prog.compileFiles(
            {src.generic_string()}, std::string{kAttLanguage},
            {std::string{"x86_64:elf64-x86_64-linux-exec"}}, rep);
        EXPECT_EQ(rc, 0)
            << "on elf the SAME name is an ordinary libc export and must bind — "
               "the refusal above is the platform's answer, not a rule about "
               "the spelling `printf`\n"
            << allDiagnosticText(rep);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// THE DECORATION ROUND TRIP: a name that is not this format's C symbol
// STAYS UNBOUND, rather than binding to a spelling the image does not export.
// ════════════════════════════════════════════════════════════════════════════
//
// ★ THIS PIN EXISTS BECAUSE THE FIRST IMPLEMENTATION GOT IT WRONG, AND ONLY A
// RUN FOUND IT. Keying the corpus lookup on the name as written missed on every
// decorating format (`_putchar` is not a key in a corpus indexed by C
// identifiers), so Mach-O silently kept failing while elf and pe worked. The
// obvious repair — un-decorate with `unapplyCMangling` — then over-corrected:
// that inverse passes an UNDECORATED name through unchanged by design, so a
// Mach-O source writing `call putchar` bound to libSystem under a spelling
// libSystem does not export. That trades a BUILD error for a dyld failure at
// process start, which is a regression however green the build looks.
//
// The rule is therefore a ROUND TRIP: re-decorating the recovered canonical
// name must reproduce byte-for-byte what the file wrote.
//
// RED-ON-DISABLE: delete the `applyCMangling(canonical, scheme) != mangledName`
// guard and the second half of this test goes green-then-wrong (rc == 0);
// delete the `unapplyCMangling` call and the first half reds.
TEST(AsmExternBinding, MachOBindsTheDecoratedNameAndRefusesTheBareOne) {
    constexpr std::string_view kMachOSpec = "x86_64:macho64-x86_64-darwin-exec";
    {
        ScratchDir scratch{Location::InsideRepo, "asm-extern"};
        scratch.useAsCwd();
        auto const src =
            writeFile(scratch.path(), "dec.s", kCallsUnderscorePutchar);
        DiagnosticReporter rep;
        Program            prog;
        int const rc = prog.compileFiles({src.generic_string()},
                                         std::string{kAttLanguage},
                                         {std::string{kMachOSpec}}, rep);
        EXPECT_EQ(rc, 0)
            << "`_putchar` IS the Mach-O C symbol for putchar; the lookup must "
               "un-decorate through the FORMAT's declared scheme to reach the "
               "corpus row\n"
            << allDiagnosticText(rep);
    }
    {
        ScratchDir scratch{Location::InsideRepo, "asm-extern"};
        scratch.useAsCwd();
        auto const src = writeFile(scratch.path(), "bare.s", kCallsPutchar);
        DiagnosticReporter rep;
        Program            prog;
        int const rc = prog.compileFiles({src.generic_string()},
                                         std::string{kAttLanguage},
                                         {std::string{kMachOSpec}}, rep);
        std::string const text = allDiagnosticText(rep);
        EXPECT_NE(rc, 0)
            << "an undecorated `putchar` is not a C symbol under Mach-O's "
               "declared decoration; binding it would produce an image whose "
               "import dyld cannot resolve. Refuse at BUILD time\n"
            << text;
        EXPECT_TRUE(sawCode(rep, DiagnosticCode::K_SymbolUndefined))
            << "the reference must reach the link tier unbound, exactly as it "
               "did before this binder existed\n"
            << text;
    }
}
