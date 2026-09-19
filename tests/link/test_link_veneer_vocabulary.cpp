// [[D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH]]
// THE TWO NEW DECLARATIONS THE VENEER PASS READS, EACH PINNED AGAINST ITS OWNER:
//
//   (1) the target's `linkVeneers` vocabulary — what a linker may build and
//       clobber — against the loader that resolves it and the rules that shape
//       it, and against the assembler that builds the body out of it;
//   (2) each writer's `importCallStubLayout` — where it will put an import's
//       call stub — against the image that writer actually emits.
//
// ★ BOTH ARE TWO-OWNER FACTS MADE TO AGREE LOUDLY, and these arms exist to
// prove the agreement is enforced, not assumed: a vocabulary whose declared
// relocations disagree with the opcodes it names is refused at link time; a
// writer whose real stub lands outside its reported bound refuses its own
// image. The arms below are cheap: an out-of-reach import call is manufactured
// by the STUB LAYOUT (a stub reported 200 MiB past a KiB-sized `.text`) rather
// than by allocating the bytes that would put it there.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/target_schema.hpp"
#include "link/branch_veneers.hpp"
#include "link/format/elf.hpp"
#include "link/format/macho.hpp"
#include "link/image_request.hpp"
#include "link/import_call_stub_layout.hpp"
#include "link/object_format_schema.hpp"
#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using test_support::mutateShippedTargetSchemaDoc;

namespace {

TargetSchema const& arm64() {
    static auto const loaded = TargetSchema::loadShipped("arm64");
    EXPECT_TRUE(loaded.has_value()) << "arm64 target did not load";
    return **loaded;
}

// A load that must FAIL, naming the JSON path it failed at.
void expectRefusedAt(LoadResult<std::shared_ptr<TargetSchema>> const& r,
                     std::string_view path, std::string_view why) {
    ASSERT_FALSE(r.has_value()) << why << " — the mutant LOADED";
    bool atPath = false;
    std::string seen;
    for (auto const& d : r.error()) {
        seen += "\n  " + d.path + ": " + d.message;
        if (d.path == path) atPath = true;
    }
    EXPECT_TRUE(atPath) << why << " — expected a diagnostic at " << path
                        << ", got:" << seen;
}

nlohmann::json& firstBody(nlohmann::json& doc) {
    return doc.at("linkVeneers").at("bodies").at(0);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// (1) THE SHIPPED DECLARATIONS
// ─────────────────────────────────────────────────────────────────────────

// arm64 declares exactly the ABI's grant and the reference body.
TEST(LinkVeneerVocabulary, Arm64DeclaresTheAbisGrantAndTheReferenceBody) {
    auto const* lv = arm64().linkVeneers();
    ASSERT_NE(lv, nullptr) << "arm64 must declare `linkVeneers`";

    ASSERT_EQ(lv->scratchRegisterNames, std::vector<std::string>{"x16"})
        << "AAPCS64 grants IP0/IP1; the declared body writes IP0 only, and a "
           "register listed here is one the linker WILL clobber";
    ASSERT_EQ(lv->scratchRegisters.size(), 1u);
    EXPECT_EQ(lv->scratchRegisters[0], *arm64().registerByName("x16"));

    ASSERT_EQ(lv->routableRelocationNames, std::vector<std::string>{"call26"})
        << "AAELF64 routes CALL26/JUMP26 only; DSS spells both with `call26`";
    EXPECT_EQ(lv->routableRelocations[0],
              arm64().relocationByName("call26")->kind);

    ASSERT_EQ(lv->bodies.size(), 1u);
    auto const& body = lv->bodies[0];
    EXPECT_EQ(body.name, "adrp-add-br");
    ASSERT_EQ(body.sequence.size(), 2u);
    EXPECT_EQ(body.sequence[0].mnemonic, "lea");
    EXPECT_TRUE(body.sequence[0].resultIsScratch);
    ASSERT_EQ(body.sequence[0].operands.size(), 1u);
    EXPECT_EQ(body.sequence[0].operands[0], LinkVeneerOperandRole::Target);
    ASSERT_EQ(body.sequence[0].relocations.size(), 2u);
    EXPECT_EQ(body.sequence[0].relocations[0],
              arm64().relocationByName("adr_prel_pg_hi21")->kind);
    EXPECT_EQ(body.sequence[0].relocations[1],
              arm64().relocationByName("add_abs_lo12_nc")->kind);
    EXPECT_EQ(body.sequence[1].mnemonic, "jmp_indirect");
    EXPECT_FALSE(body.sequence[1].resultIsScratch);
    ASSERT_EQ(body.sequence[1].operands.size(), 1u);
    EXPECT_EQ(body.sequence[1].operands[0], LinkVeneerOperandRole::Scratch);
    EXPECT_TRUE(body.sequence[1].relocations.empty());
}

// x86-64 declares none, on measured grounds: neither GNU ld 2.42 nor ld.lld
// 18.1.3 extends an out-of-range `rel32` (both refuse a call 3 GiB away).
TEST(LinkVeneerVocabulary, X86_64DeclaresNoneBecauseNoReferenceExtendsRel32) {
    auto const x86 = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(x86.has_value());
    EXPECT_EQ((*x86)->linkVeneers(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────
// (1) THE LOADER AND THE SHAPE RULES — every name resolves, every rule holds
// ─────────────────────────────────────────────────────────────────────────

TEST(LinkVeneerVocabulary, ABodyMustEndInAnIndirectBranch) {
    auto r = mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
        firstBody(doc).at("sequence").erase(1);  // drop the `br`
    });
    expectRefusedAt(r, "/linkVeneers/bodies/0/sequence/0/mnemonic",
                    "a body that never branches would fall through into the "
                    "next function");
}

TEST(LinkVeneerVocabulary, ABodyStepMayNotBeACall) {
    auto r = mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
        auto& step = firstBody(doc).at("sequence").at(0);
        step = nlohmann::json{{"mnemonic", "call"},
                              {"operands", {"target"}},
                              {"relocations", {"call26"}}};
    });
    expectRefusedAt(r, "/linkVeneers/bodies/0/sequence/0/mnemonic",
                    "a call writes the link register, which a veneer must "
                    "preserve");
}

TEST(LinkVeneerVocabulary, AStepWritesTheScratchIffItsOpcodeProducesAValue) {
    auto missing = mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
        firstBody(doc).at("sequence").at(0).erase("result");
    });
    expectRefusedAt(missing, "/linkVeneers/bodies/0/sequence/0/result",
                    "`lea` produces a value, and it must land in the scratch "
                    "register");
    auto spurious = mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
        firstBody(doc).at("sequence").at(1)["result"] = "scratch";
    });
    expectRefusedAt(spurious, "/linkVeneers/bodies/0/sequence/1/result",
                    "`jmp_indirect` produces nothing to write");
}

TEST(LinkVeneerVocabulary, OnlyTheScratchRoleMayBeWritten) {
    auto r = mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
        firstBody(doc).at("sequence").at(0)["result"] = "target";
    });
    expectRefusedAt(r, "/linkVeneers/bodies/0/sequence/0/result",
                    "writing anything but the granted scratch register is a "
                    "clobber the ABI does not grant");
}

TEST(LinkVeneerVocabulary, EveryNameResolvesOrTheLoadFailsAtItsPath) {
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            doc.at("linkVeneers").at("scratchRegisters") = {"x99"};
        }),
        "/linkVeneers/scratchRegisters/0", "an unknown scratch register");
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            doc.at("linkVeneers").at("scratchRegisters") = {"v0"};
        }),
        "/linkVeneers/scratchRegisters/0",
        "a vector register cannot hold a branch destination");
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            doc.at("linkVeneers").at("routableRelocations") = {"jump26"};
        }),
        "/linkVeneers/routableRelocations/0", "an undeclared relocation row");
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            firstBody(doc).at("sequence").at(0)["mnemonic"] = "adrp_nonexistent";
        }),
        "/linkVeneers/bodies/0/sequence/0/mnemonic", "an unknown opcode");
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            firstBody(doc).at("sequence").at(1)["operands"] = {"scratchh"};
        }),
        "/linkVeneers/bodies/0/sequence/1/operands/0", "a misspelled role");
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            doc.at("linkVeneers")["scratchRegister"] = "x16";
        }),
        "/linkVeneers/scratchRegister",
        "an unknown key in the block — a typo must not silently no-op");
}

TEST(LinkVeneerVocabulary, APresentBlockMustGrantSomething) {
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            doc.at("linkVeneers").at("routableRelocations") = nlohmann::json::array();
        }),
        "/linkVeneers/routableRelocations",
        "a vocabulary that routes nothing reads as a capability and grants none");
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            doc.at("linkVeneers").at("bodies") = nlohmann::json::array();
        }),
        "/linkVeneers/bodies", "a vocabulary with nothing to build");
}

// ─────────────────────────────────────────────────────────────────────────
// (1) THE BODY, BUILT — and the two declarations of its relocations, agreeing
// ─────────────────────────────────────────────────────────────────────────
namespace {

constexpr std::uint32_t kBL  = 0x94000000u;
constexpr std::uint32_t kRet = 0xD65F03C0u;

void appendWord(std::vector<std::uint8_t>& b, std::uint32_t w) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((w >> (8 * i)) & 0xFFu));
}

// caller (`BL far@stub`, `RET`) │ callee (`RET`), with the import's stub
// REPORTED 200 MiB past the end of `.text` — an out-of-reach import call at the
// cost of twelve bytes.
struct FarImport {
    AssembledModule            module;
    link::ImportCallStubLayout stubs;
    SymbolId caller{1}, callee{2}, imp{40};
};

FarImport buildFarImport(TargetSchema const& target) {
    FarImport f;
    AssembledFunction c;
    c.symbol = f.caller;
    appendWord(c.bytes, kBL);
    appendWord(c.bytes, kRet);
    c.relocations.push_back(
        Relocation{0u, f.imp, target.relocationByName("call26")->kind, 0});
    f.module.functions.push_back(std::move(c));
    AssembledFunction e;
    e.symbol = f.callee;
    appendWord(e.bytes, kRet);
    f.module.functions.push_back(std::move(e));
    f.module.expectedFuncCount = 2;
    f.module.externImports.push_back(ExternImport{f.imp, "far", "libfar.so"});
    f.stubs.maxPastTextEnd.emplace(f.imp, std::uint64_t{200} * 1024 * 1024);
    return f;
}

}  // namespace

TEST(LinkVeneerVocabulary, AFarImportCallIsCarriedAtKilobyteScale) {
    auto f = buildFarImport(arm64());
    ASSERT_TRUE(linker::branchVeneersNeeded(f.module, arm64(), f.stubs));
    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectBranchVeneers(f.module, arm64(), f.stubs, rep));
    ASSERT_EQ(f.module.functions.size(), 3u);
    // The furthest boundary in reach of the call is the END of `.text`.
    auto const& veneer = f.module.functions[2];
    ASSERT_EQ(veneer.bytes.size(), 12u);
    ASSERT_EQ(veneer.relocations.size(), 2u);
    EXPECT_EQ(veneer.relocations[0].target.v, f.imp.v);
    EXPECT_EQ(f.module.functions[0].relocations[0].target.v, veneer.symbol.v);
}

TEST(LinkVeneerVocabulary, DeclaredRelocationsThatDisagreeWithTheOpcodesAreRefused) {
    // Swap the two page relocations the `lea` step declares. The names still
    // resolve, so the LOADER accepts it; only building the body can notice.
    auto swapped = mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
        firstBody(doc).at("sequence").at(0)["relocations"] =
            nlohmann::json::array({"add_abs_lo12_nc", "adr_prel_pg_hi21"});
    });
    ASSERT_TRUE(swapped.has_value());
    auto f = buildFarImport(**swapped);
    DiagnosticReporter rep;
    EXPECT_FALSE(linker::injectBranchVeneers(f.module, **swapped, f.stubs, rep));
    bool named = false;
    for (auto const& d : rep.all())
        if (d.actual.find("declares the relocations") != std::string::npos
            && d.actual.find("assembled") != std::string::npos)
            named = true;
    EXPECT_TRUE(named)
        << "a body whose declared relocations disagree with what its opcodes "
           "assemble must be refused, naming both lists";
    EXPECT_EQ(f.module.functions.size(), 2u) << "and nothing may be inserted";

    // CONTROL: the SHIPPED declaration, same module — accepted.
    auto g = buildFarImport(arm64());
    DiagnosticReporter rep2;
    EXPECT_TRUE(linker::injectBranchVeneers(g.module, arm64(), g.stubs, rep2));
}

// ─────────────────────────────────────────────────────────────────────────
// (2) THE WRITERS' STUB LAYOUTS, AGAINST THE IMAGES THEY EMIT
// ─────────────────────────────────────────────────────────────────────────
namespace {

std::uint64_t u64(std::vector<std::uint8_t> const& b, std::size_t o) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[o + static_cast<std::size_t>(i)];
    return v;
}
std::uint32_t u32(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint32_t>(u64(b, o) & 0xFFFFFFFFu);
}
std::uint16_t u16(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}

struct Section { std::uint64_t addr = 0, size = 0; };

std::optional<Section> elfSection(std::vector<std::uint8_t> const& b,
                                  std::string_view name) {
    std::uint64_t const shoff = u64(b, 0x28);
    std::uint16_t const shentsize = u16(b, 0x3A), shnum = u16(b, 0x3C),
                        shstrndx = u16(b, 0x3E);
    std::uint64_t const strOff = u64(b, shoff + shstrndx * shentsize + 24);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::size_t const h = shoff + static_cast<std::size_t>(i) * shentsize;
        std::size_t const n = strOff + u32(b, h);
        std::string_view const nm{reinterpret_cast<char const*>(&b[n])};
        if (nm == name) return Section{u64(b, h + 16), u64(b, h + 32)};
    }
    return std::nullopt;
}

std::optional<Section> machoSection(std::vector<std::uint8_t> const& b,
                                    std::string_view seg, std::string_view sect) {
    std::uint32_t const ncmds = u32(b, 16);
    std::size_t at = 32;
    for (std::uint32_t c = 0; c < ncmds; ++c) {
        std::uint32_t const cmd = u32(b, at), size = u32(b, at + 4);
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            std::uint32_t const nsects = u32(b, at + 64);
            for (std::uint32_t s = 0; s < nsects; ++s) {
                std::size_t const r = at + 72 + static_cast<std::size_t>(s) * 80;
                std::string const sn(reinterpret_cast<char const*>(&b[r]),
                                     ::strnlen(reinterpret_cast<char const*>(&b[r]), 16));
                std::string const gn(reinterpret_cast<char const*>(&b[r + 16]),
                                     ::strnlen(reinterpret_cast<char const*>(&b[r + 16]), 16));
                if (sn == sect && gn == seg) return Section{u64(b, r + 32), u64(b, r + 40)};
            }
        }
        at += size;
    }
    return std::nullopt;
}

// Two function imports around a data import, plus `.rodata` items of three
// alignments — every term of the ELF bound gets something to measure.
AssembledModule importingModule() {
    AssembledModule mod;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    appendWord(fn.bytes, kBL);
    appendWord(fn.bytes, kBL);
    appendWord(fn.bytes, kRet);
    auto const call26 = arm64().relocationByName("call26")->kind;
    fn.relocations.push_back(Relocation{0u, SymbolId{90}, call26, 0});
    fn.relocations.push_back(Relocation{4u, SymbolId{92}, call26, 0});
    mod.functions.push_back(std::move(fn));
    mod.expectedFuncCount  = 1;
    mod.imageEntryOverride = 0u;
    ExternImport f1{SymbolId{90}, "f1", "libone.so"};
    ExternImport d1{SymbolId{91}, "d1", "libone.so"};
    d1.isData = true;
    ExternImport f2{SymbolId{92}, "f2", "libone.so"};
    mod.externImports = {f1, d1, f2};
    auto rodata = [&](std::uint32_t sym, std::size_t n, std::uint32_t align) {
        AssembledData d;
        d.symbol    = SymbolId{sym};
        d.section   = DataSectionKind::Rodata;
        d.bytes.assign(n, 0x5Au);
        d.alignment = *Alignment::fromBytes(align);
        return d;
    };
    mod.dataItems = {rodata(70, 5, 1), rodata(71, 9, 8), rodata(72, 3, 16)};
    return mod;
}

// A shared object / dylib has no entry: the writers refuse one.
AssembledModule entrylessImportingModule() {
    auto mod = importingModule();
    mod.imageEntryOverride.reset();
    return mod;
}

AssembledModule machoImportingModule(AssembledModule mod) {
    mod.dataItems.clear();
    for (auto& e : mod.externImports) e.libraryPath = "/usr/lib/libSystem.B.dylib";
    return mod;
}

// The ELF `.plt` lies within the reported bound, and the bound is TIGHT: it
// over-states the distance by at most the alignment slack it has to allow.
void expectElfPltWithinATightBound(std::string_view formatName,
                                   AssembledModule const& mod) {
    auto fmtR = ObjectFormatSchema::loadShipped(formatName);
    ASSERT_TRUE(fmtR.has_value()) << formatName;
    auto const& fmt = **fmtR;
    auto const layout = elf::importCallStubLayout(mod, fmt);
    ASSERT_EQ(layout.maxPastTextEnd.size(), 2u)
        << formatName << ": two FUNCTION imports get a stub; the data import "
                         "gets none";
    EXPECT_FALSE(layout.maxPastTextEnd.contains(SymbolId{91}));

    DiagnosticReporter rep;
    auto const image = elf::encode(mod, arm64(), fmt, rep);
    ASSERT_FALSE(image.empty()) << formatName << ": the writer refused — its own "
                                   "assertion of this bound fires before it emits";
    auto const text = elfSection(image, ".text");
    auto const plt  = elfSection(image, ".plt");
    ASSERT_TRUE(text.has_value() && plt.has_value()) << formatName;
    std::uint64_t const textEnd = text->addr + text->size;
    // Slack: `.rodata`'s alignment pad (< 16) + its items' pads (0 + 7 + 15)
    // + the `.plt` pad (< 16).
    constexpr std::uint64_t kSlack = 15 + 22 + 15;
    std::uint64_t slot = 0;
    for (auto const sym : {SymbolId{90}, SymbolId{92}}) {
        std::uint64_t const stub = plt->addr + slot * 16;
        auto const bound = layout.maxPastTextEnd.at(sym);
        EXPECT_LE(stub - textEnd, bound)
            << formatName << ": stub " << slot << " past its bound";
        EXPECT_LE(bound - (stub - textEnd), kSlack)
            << formatName << ": the bound for stub " << slot << " is looser than "
               "its slack — a loose bound veneers calls that reach";
        ++slot;
    }
}

// Mach-O puts `__stubs` right after `__text`: the reported layout is EXACT.
void expectMachoStubsExactlyWhereReported(std::string_view formatName,
                                          AssembledModule const& mod) {
    auto fmtR = ObjectFormatSchema::loadShipped(formatName);
    ASSERT_TRUE(fmtR.has_value()) << formatName;
    auto const& fmt = **fmtR;
    auto const layout = macho::importCallStubLayout(mod, fmt);
    ASSERT_EQ(layout.maxPastTextEnd.size(), 2u) << formatName;
    DiagnosticReporter rep;
    auto const image = macho::encode(
        mod, arm64(), fmt, rep,
        ImageRequest{.artifactFileName = "dss_veneer_stub_layout_pin"});
    ASSERT_FALSE(image.empty()) << formatName << ": the writer refused";
    auto const text  = machoSection(image, "__TEXT", "__text");
    auto const stubs = machoSection(image, "__TEXT", "__stubs");
    ASSERT_TRUE(text.has_value() && stubs.has_value()) << formatName;
    std::uint64_t const textEnd = text->addr + text->size;
    EXPECT_EQ(stubs->addr, textEnd) << formatName;
    std::uint64_t j = 0;
    for (auto const sym : {SymbolId{90}, SymbolId{92}}) {
        EXPECT_EQ(layout.maxPastTextEnd.at(sym), j * 12)
            << formatName << ": stub " << j << ": Mach-O's layout is exact, so "
               "the answer is too";
        ++j;
    }
}

}  // namespace

// ONE ARM PER FLAVOR THE DYNAMIC WRITERS EMIT — an executable with imports, a
// PIE, a shared object; a Mach-O executable and dylib — because each reaches
// the writer by its own route, and a flavor the report forgot would leave every
// far call into its stubs unmeasured.
TEST(ImportCallStubLayout, TheElfPltLiesWithinTheReportedBoundAndTheBoundIsTight) {
    expectElfPltWithinATightBound("elf64-aarch64-linux-exec", importingModule());
}

TEST(ImportCallStubLayout, APiesPltLiesWithinTheReportedBound) {
    expectElfPltWithinATightBound("elf64-aarch64-linux-pie", importingModule());
}

TEST(ImportCallStubLayout, ASharedObjectsPltLiesWithinTheReportedBound) {
    expectElfPltWithinATightBound("elf64-aarch64-linux-dyn",
                                  entrylessImportingModule());
}

TEST(ImportCallStubLayout, TheMachoStubsLieExactlyWhereReported) {
    expectMachoStubsExactlyWhereReported("macho64-arm64-darwin-exec",
                                         machoImportingModule(importingModule()));
}

TEST(ImportCallStubLayout, ADylibsStubsLieExactlyWhereReported) {
    expectMachoStubsExactlyWhereReported(
        "macho64-arm64-darwin-dylib",
        machoImportingModule(entrylessImportingModule()));
}

// The flavors that emit no stub report none: a relocatable object, and an
// executable with no import (the static writers) — in both families.
TEST(ImportCallStubLayout, FlavorsWithoutStubsReportNone) {
    auto noImports = importingModule();
    noImports.externImports.clear();
    for (auto const name : {"elf64-aarch64-linux", "elf64-aarch64-linux-exec"}) {
        auto fmt = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(fmt.has_value()) << name;
        auto const& mod = std::string_view{name} == "elf64-aarch64-linux"
                              ? importingModule() : noImports;
        EXPECT_TRUE(elf::importCallStubLayout(mod, **fmt).maxPastTextEnd.empty())
            << name;
    }
    for (auto const name : {"macho64-arm64-darwin", "macho64-arm64-darwin-exec"}) {
        auto fmt = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(fmt.has_value()) << name;
        auto const mod = std::string_view{name} == "macho64-arm64-darwin"
                             ? machoImportingModule(importingModule())
                             : machoImportingModule(noImports);
        EXPECT_TRUE(macho::importCallStubLayout(mod, **fmt).maxPastTextEnd.empty())
            << name;
    }
}
