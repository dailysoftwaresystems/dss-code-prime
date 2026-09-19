// [[D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH]] — the
// REAL-SCALE end-to-end, OPT-IN (`-DDSS_REAL_SCALE_TESTS=ON`; see
// tests/link/CMakeLists.txt). It is the subject row's own shape, linked by the
// real linker: the synthetic entry trampoline, the writer's stub layout, the
// veneer pass and the format writer, on an image whose `.text` is 144 MiB —
// past `call26`'s reach — so the entry's call to its process-exit import cannot
// reach the stub the writer puts past the end of `.text`.
//
// ⓘ WHY OPT-IN. Each arm holds the 144 MiB module, the linker's copy-on-write
// clone and the emitted image at once (~600 MB peak) and hashes the image for
// its build-id. The default gate pins the same decisions through the planner's
// size-only model (`test_branch_veneers.cpp`) at no such cost; this file exists
// to prove the whole path end to end and, where an arm64 runner exists, to RUN
// the result.
//
// ✔ Before this change the same shape was refused: a 212 992-statement C
// program on `arm64:elf64-aarch64-linux-exec` failed with `call26` shifted value
// 34 930 704 (139 722 816 bytes), the entry's `bl exit@plt` (lane `il`,
// 2026-09-17), and identically on `macho64-arm64-darwin-exec`.

#include "arm_verdict_ledger.hpp"
#include "run_binary.hpp"
#include "scratch_dir.hpp"

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/image_request.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

constexpr std::uint64_t kFiller   = 144ull * 1024 * 1024;
constexpr std::uint32_t kMovW0_42 = 0x52800540u;  // mov w0, #42
constexpr std::uint32_t kRet      = 0xD65F03C0u;

// `main` returns 42; a 144 MiB never-executed filler follows it. The linker
// prepends the entry trampoline, whose last act is the process-exit import call.
AssembledModule subjectModule() {
    AssembledModule m;
    AssembledFunction main;
    main.symbol = SymbolId{1};
    for (auto const w : {kMovW0_42, kRet})
        for (int i = 0; i < 4; ++i)
            main.bytes.push_back(static_cast<std::uint8_t>((w >> (8 * i)) & 0xFFu));
    m.functions.push_back(std::move(main));
    AssembledFunction filler;
    filler.symbol = SymbolId{2};
    filler.bytes.resize(static_cast<std::size_t>(kFiller), 0u);
    m.functions.push_back(std::move(filler));
    m.expectedFuncCount = 2;
    m.userEntrySymbol   = SymbolId{1};
    return m;
}

std::uint64_t rd(std::vector<std::uint8_t> const& b, std::size_t o, int n) {
    std::uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | b[o + static_cast<std::size_t>(i)];
    return v;
}

struct Section { std::uint64_t addr = 0, offset = 0, size = 0; };

std::optional<Section> elfSection(std::vector<std::uint8_t> const& b, std::string_view name) {
    std::uint64_t const shoff = rd(b, 0x28, 8);
    auto const shentsize = rd(b, 0x3A, 2), shnum = rd(b, 0x3C, 2), shstrndx = rd(b, 0x3E, 2);
    std::uint64_t const strOff = rd(b, shoff + shstrndx * shentsize + 24, 8);
    for (std::uint64_t i = 0; i < shnum; ++i) {
        std::size_t const h = shoff + i * shentsize;
        std::size_t const n = strOff + rd(b, h, 4);
        if (std::string_view{reinterpret_cast<char const*>(&b[n])} == name)
            return Section{rd(b, h + 16, 8), rd(b, h + 24, 8), rd(b, h + 32, 8)};
    }
    return std::nullopt;
}

// The branch target of a BL at `va` whose word is `w`.
std::uint64_t blTarget(std::uint64_t va, std::uint32_t w) {
    std::int64_t imm = static_cast<std::int64_t>(w & 0x03FFFFFFu);
    if (imm & 0x02000000) imm -= 0x04000000;
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(va) + imm * 4);
}

// Decode the reference veneer body `adrp x16, P; add x16, x16, #L; br x16` at
// `va` and return the address it branches to — or nullopt if it is not one.
std::optional<std::uint64_t> veneerDestination(std::vector<std::uint8_t> const& b,
                                               std::size_t off, std::uint64_t va) {
    auto const adrp = static_cast<std::uint32_t>(rd(b, off, 4));
    auto const add  = static_cast<std::uint32_t>(rd(b, off + 4, 4));
    auto const br   = static_cast<std::uint32_t>(rd(b, off + 8, 4));
    if ((adrp & 0x9F00001Fu) != 0x90000010u) return std::nullopt;
    if ((add & 0xFFC003FFu) != 0x91000210u) return std::nullopt;
    if (br != 0xD61F0200u) return std::nullopt;
    std::int64_t page = static_cast<std::int64_t>(((adrp >> 29) & 0x3u) | (((adrp >> 5) & 0x7FFFFu) << 2));
    if (page & (std::int64_t{1} << 20)) page -= (std::int64_t{1} << 21);
    std::uint64_t const base = (va & ~std::uint64_t{0xFFF}) + static_cast<std::uint64_t>(page * 4096);
    return base + ((add >> 10) & 0xFFFu);
}

struct Linked {
    std::vector<std::uint8_t> bytes;
    bool                      ok = false;
};

Linked linkFor(std::string_view formatName, ImageRequest const& request = {}) {
    Linked out;
    auto const target = TargetSchema::loadShipped("arm64");
    auto const format = ObjectFormatSchema::loadShipped(formatName);
    EXPECT_TRUE(target.has_value() && format.has_value());
    if (!target.has_value() || !format.has_value()) return out;
    DiagnosticReporter rep;
    auto image = linker::link(subjectModule(), **target, **format, rep, request);
    for (auto const& d : rep.all()) ADD_FAILURE() << formatName << ": " << d.actual;
    out.ok    = image.ok() && rep.errorCount() == 0;
    out.bytes = std::move(image.bytes);
    return out;
}

// The ELF arms: e_entry is the trampoline; its LAST `bl` is the exit call, and
// it must land on a veneer whose ADRP+ADD address the `.plt` entry of `exit`
// (the one function import, slot 0).
void expectElfEntryReachesExitThroughAVeneer(std::vector<std::uint8_t> const& b) {
    auto const text = elfSection(b, ".text");
    auto const plt  = elfSection(b, ".plt");
    ASSERT_TRUE(text.has_value() && plt.has_value());
    EXPECT_GT(text->size, kFiller) << "the image must really span past the reach";
    std::uint64_t const entry = rd(b, 0x18, 8);
    auto const fileOf = [&](std::uint64_t va) { return text->offset + (va - text->addr); };
    std::optional<std::uint64_t> exitCall;
    for (std::uint64_t va = entry; va < entry + 64; va += 4) {
        auto const w = static_cast<std::uint32_t>(rd(b, fileOf(va), 4));
        if ((w & 0xFC000000u) == 0x94000000u) exitCall = va;          // BL
        if (w == 0xD4200000u) break;                                   // BRK #0: the end
    }
    ASSERT_TRUE(exitCall.has_value()) << "no `bl` in the entry trampoline";
    auto const w = static_cast<std::uint32_t>(rd(b, fileOf(*exitCall), 4));
    std::uint64_t const landing = blTarget(*exitCall, w);
    EXPECT_NE(landing, plt->addr)
        << "the entry's exit call cannot reach `.plt` 144 MiB away — it must land "
           "on a veneer";
    auto const dest = veneerDestination(b, fileOf(landing), landing);
    ASSERT_TRUE(dest.has_value())
        << "the exit call lands on something that is not the reference veneer "
           "body (adrp x16 / add x16 / br x16)";
    EXPECT_EQ(*dest, plt->addr) << "the veneer must finish the trip to exit's `.plt` stub";
}

// Run the ELF where this host can: natively on arm64 Linux, else under
// `qemu-aarch64` — the examples runner's own policy, including the strict-mode
// rule that turns "no runner" from a skip into a red.
void runExpecting42(std::vector<std::uint8_t> const& bytes, std::string_view name) {
    std::vector<std::string> launcher;
    if (test_support::currentHostOs() != "linux") {
        GTEST_SKIP() << "an ELF executable cannot run on host OS '"
                     << test_support::currentHostOs() << "'";
    }
    if (test_support::currentHostArch() != "arm64") {
        auto const qemu = test_support::findOnPath("qemu-aarch64");
        if (qemu.empty()) {
            if (test_support::readStrictArmVerdicts().on)
                FAIL() << "no arm64 runner: not arm64 and qemu-aarch64 is not on PATH";
            GTEST_SKIP() << "no arm64 runner (qemu-aarch64 not on PATH)";
        }
        launcher.push_back(qemu);
    }
    test_support::ScratchDir dir{test_support::Location::Temp, "link-veneer-real-scale"};
    auto const path = dir.path() / std::string{name};
    {
        std::ofstream f{path, std::ios::binary};
        f.write(reinterpret_cast<char const*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
    auto const r = test_support::runBinary(path, test_support::kRunBudget, false, launcher);
    ASSERT_TRUE(r.spawned) << r.diagnostic;
    ASSERT_FALSE(r.timedOut) << r.diagnostic;
    EXPECT_EQ(r.exitCode, 42u) << "the image must RUN: main returns 42 and the entry "
                                  "hands it to exit through the veneer";
}

}  // namespace

TEST(BranchVeneerRealScale, TheEntrysExitCallCrossesA144MiBElfExecutable) {
    auto const linked = linkFor("elf64-aarch64-linux-exec");
    ASSERT_TRUE(linked.ok) << "the image must LINK — before this change it was refused";
    expectElfEntryReachesExitThroughAVeneer(linked.bytes);
    runExpecting42(linked.bytes, "entry_veneer_exec");
}

TEST(BranchVeneerRealScale, TheEntrysExitCallCrossesA144MiBPie) {
    auto const linked = linkFor("elf64-aarch64-linux-pie");
    ASSERT_TRUE(linked.ok) << "the PIE must LINK";
    expectElfEntryReachesExitThroughAVeneer(linked.bytes);
    runExpecting42(linked.bytes, "entry_veneer_pie");
}

// Mach-O: LC_MAIN names the trampoline; its last `bl` must land on a veneer that
// addresses `exit`'s `__stubs` entry (the one function import, slot 0).
TEST(BranchVeneerRealScale, TheEntrysExitCallCrossesA144MiBMachOExecutable) {
    auto const linked = linkFor("macho64-arm64-darwin-exec",
                                ImageRequest{.artifactFileName = "entry_veneer_macho"});
    ASSERT_TRUE(linked.ok) << "the Mach-O image must LINK";
    auto const& b = linked.bytes;
    std::uint32_t const ncmds = static_cast<std::uint32_t>(rd(b, 16, 4));
    std::size_t at = 32;
    std::uint64_t textSegVa = 0, textSegOff = 0, entryoff = 0, stubsAddr = 0;
    bool haveMain = false, haveStubs = false;
    for (std::uint32_t c = 0; c < ncmds; ++c) {
        auto const cmd = static_cast<std::uint32_t>(rd(b, at, 4));
        auto const size = static_cast<std::uint32_t>(rd(b, at + 4, 4));
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            std::string const seg(reinterpret_cast<char const*>(&b[at + 8]),
                                  ::strnlen(reinterpret_cast<char const*>(&b[at + 8]), 16));
            if (seg == "__TEXT") {
                textSegVa  = rd(b, at + 24, 8);
                textSegOff = rd(b, at + 40, 8);
            }
            auto const nsects = static_cast<std::uint32_t>(rd(b, at + 64, 4));
            for (std::uint32_t s = 0; s < nsects; ++s) {
                std::size_t const r = at + 72 + static_cast<std::size_t>(s) * 80;
                std::string const sn(reinterpret_cast<char const*>(&b[r]),
                                     ::strnlen(reinterpret_cast<char const*>(&b[r]), 16));
                if (sn == "__stubs") { stubsAddr = rd(b, r + 32, 8); haveStubs = true; }
            }
        }
        if (cmd == 0x80000028u) { entryoff = rd(b, at + 8, 8); haveMain = true; }  // LC_MAIN
        at += size;
    }
    ASSERT_TRUE(haveMain && haveStubs);
    std::uint64_t const entry = textSegVa + entryoff;
    auto const fileOf = [&](std::uint64_t va) { return textSegOff + (va - textSegVa); };
    std::optional<std::uint64_t> exitCall;
    for (std::uint64_t va = entry; va < entry + 64; va += 4) {
        auto const w = static_cast<std::uint32_t>(rd(b, fileOf(va), 4));
        if ((w & 0xFC000000u) == 0x94000000u) exitCall = va;
        if (w == 0xD4200000u) break;
    }
    ASSERT_TRUE(exitCall.has_value());
    auto const w = static_cast<std::uint32_t>(rd(b, fileOf(*exitCall), 4));
    std::uint64_t const landing = blTarget(*exitCall, w);
    EXPECT_NE(landing, stubsAddr);
    auto const dest = veneerDestination(b, fileOf(landing), landing);
    ASSERT_TRUE(dest.has_value()) << "the exit call must land on the reference veneer body";
    EXPECT_EQ(*dest, stubsAddr) << "the veneer must finish the trip to exit's `__stubs` entry";
}
