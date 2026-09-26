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
//
// ★★ AND PAST ±4 GiB ([[D-LK-AARCH64-VENEER-CANNOT-REACH-PAST-FOUR-GIB]]) the same
// device reaches the second body: a stub reported 5 GiB away is out of the ADRP
// body's reach from every boundary, so the long literal body is elected — on
// ELF, whose image formats declare its 64-bit PC-relative relocation, and never
// on Mach-O, whose formats do not (no Mach-O reference links that shape). The
// ELF writer then lays the stub out where it really goes and the arm reads the
// literal back: PC-relative, so right at any distance.

#include "asm/asm.hpp"
#include "core/types/config_path_walk.hpp"
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
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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

// Every VALUE spelled `from` in a JSON subtree becomes `to` (keys untouched).
void renameRegister(nlohmann::json& j, std::string const& from, std::string const& to) {
    if (j.is_string()) {
        if (j.get<std::string>() == from) j = to;
        return;
    }
    if (j.is_array() || j.is_object())
        for (auto& e : j) renameRegister(e, from, to);
}

// Reading an emitted image back, just far enough to find a section.
std::uint64_t u64(std::vector<std::uint8_t> const& b, std::size_t o) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[o + static_cast<std::size_t>(i)];
    return v;
}
std::uint32_t u32(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16)
         | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}
std::uint16_t u16(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}

struct Section { std::uint64_t addr = 0, size = 0, offset = 0; };

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
        if (nm == name) return Section{u64(b, h + 16), u64(b, h + 32), u64(b, h + 24)};
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
                if (sn == sect && gn == seg)
                    return Section{u64(b, r + 32), u64(b, r + 40), u32(b, r + 48)};
            }
        }
        at += size;
    }
    return std::nullopt;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// (1) THE SHIPPED DECLARATIONS
// ─────────────────────────────────────────────────────────────────────────

namespace {

void expectRegister(LinkVeneerOperand const& op, std::string_view name) {
    EXPECT_EQ(op.kind, LinkVeneerOperandKind::Register) << name;
    EXPECT_EQ(op.registerName, name);
    EXPECT_EQ(op.reg, *arm64().registerByName(name)) << name;
}

}  // namespace

// arm64 declares exactly the ABI's grant, and the reference body first.
TEST(LinkVeneerVocabulary, Arm64DeclaresTheAbisGrantAndTheReferenceBody) {
    auto const* lv = arm64().linkVeneers();
    ASSERT_NE(lv, nullptr) << "arm64 must declare `linkVeneers`";

    ASSERT_EQ(lv->scratchRegisterNames, (std::vector<std::string>{"x16", "x17"}))
        << "AAPCS64 grants IP0 AND IP1 in one sentence; the long body writes both, "
           "and a register listed here is one the linker WILL clobber";
    ASSERT_EQ(lv->scratchRegisters.size(), 2u);
    EXPECT_EQ(lv->scratchRegisters[0], *arm64().registerByName("x16"));
    EXPECT_EQ(lv->scratchRegisters[1], *arm64().registerByName("x17"));

    ASSERT_EQ(lv->routableRelocationNames, std::vector<std::string>{"call26"})
        << "AAELF64 routes CALL26/JUMP26 only; DSS spells both with `call26`";
    EXPECT_EQ(lv->routableRelocations[0],
              arm64().relocationByName("call26")->kind);

    ASSERT_EQ(lv->bodies.size(), 2u);
    auto const& body = lv->bodies[0];
    EXPECT_EQ(body.name, "adrp-add-br") << "the cheap body is declared FIRST";
    ASSERT_EQ(body.sequence.size(), 2u);
    EXPECT_EQ(body.sequence[0].mnemonic, "lea");
    EXPECT_EQ(body.sequence[0].resultName, "x16");
    EXPECT_EQ(body.sequence[0].resultRegister, *arm64().registerByName("x16"));
    ASSERT_EQ(body.sequence[0].operands.size(), 1u);
    EXPECT_EQ(body.sequence[0].operands[0].kind, LinkVeneerOperandKind::Target);
    ASSERT_EQ(body.sequence[0].relocations.size(), 2u);
    EXPECT_EQ(body.sequence[0].relocations[0],
              arm64().relocationByName("adr_prel_pg_hi21")->kind);
    EXPECT_EQ(body.sequence[0].relocations[1],
              arm64().relocationByName("add_abs_lo12_nc")->kind);
    EXPECT_EQ(body.sequence[1].mnemonic, "jmp_indirect");
    EXPECT_FALSE(body.sequence[1].writesRegister());
    ASSERT_EQ(body.sequence[1].operands.size(), 1u);
    expectRegister(body.sequence[1].operands[0], "x16");
    EXPECT_TRUE(body.sequence[1].relocations.empty());
}

// ...and GNU ld 2.42's long-branch mechanism second
// ([[D-LK-AARCH64-VENEER-CANNOT-REACH-PAST-FOUR-GIB]]): 📄 its stub is
// `ldr ip0, 1f; adr ip1, #0; add ip0, ip0, ip1; br ip0; 1: .xword PREL64(X)+12`.
// The body spells the same mechanism with this target's own verbs — the
// two-word `lea` where GNU ld writes `adr ip1, #0` (the body predates the
// one-word `adr` of P68 round 9, and keeps its bytes) and a `load` where GNU ld
// writes a literal `ldr` — x17 holds the literal's address, so the literal is a
// plain PREL64 at itself.
TEST(LinkVeneerVocabulary, Arm64DeclaresGnuLdsLongBranchMechanismSecond) {
    auto const* lv = arm64().linkVeneers();
    ASSERT_NE(lv, nullptr);
    ASSERT_EQ(lv->bodies.size(), 2u);
    auto const& body = lv->bodies[1];
    EXPECT_EQ(body.name, "prel64-literal");
    ASSERT_EQ(body.sequence.size(), 5u);

    auto const& lea = body.sequence[0];
    EXPECT_EQ(lea.mnemonic, "lea");
    EXPECT_EQ(lea.resultName, "x17");
    ASSERT_EQ(lea.operands.size(), 1u);
    EXPECT_EQ(lea.operands[0].kind, LinkVeneerOperandKind::Literal);

    auto const& load = body.sequence[1];
    EXPECT_EQ(load.mnemonic, "load");
    EXPECT_EQ(load.resultName, "x16");
    ASSERT_EQ(load.operands.size(), 1u);
    EXPECT_EQ(load.operands[0].kind, LinkVeneerOperandKind::Memory);
    EXPECT_EQ(load.operands[0].registerName, "x17");
    EXPECT_EQ(load.operands[0].offset, 0);

    auto const& add = body.sequence[2];
    EXPECT_EQ(add.mnemonic, "add");
    EXPECT_EQ(add.resultName, "x16");
    ASSERT_EQ(add.operands.size(), 2u);
    expectRegister(add.operands[0], "x16");
    expectRegister(add.operands[1], "x17");

    EXPECT_EQ(body.sequence[3].mnemonic, "jmp_indirect");
    ASSERT_EQ(body.sequence[3].operands.size(), 1u);
    expectRegister(body.sequence[3].operands[0], "x16");

    auto const& data = body.sequence[4];
    EXPECT_TRUE(data.isData());
    EXPECT_EQ(data.dataBytes, 8u);
    ASSERT_EQ(data.operands.size(), 1u);
    EXPECT_EQ(data.operands[0].kind, LinkVeneerOperandKind::Target);
    ASSERT_EQ(data.relocations.size(), 1u);

    // The word's relocation: R_AARCH64_PREL64 semantics, S + A - P in 8 bytes.
    auto const* prel64 = arm64().relocationByName("prel64");
    ASSERT_NE(prel64, nullptr);
    EXPECT_EQ(data.relocations[0], prel64->kind);
    EXPECT_EQ(prel64->formulaKind, RelocFormulaKind::Linear);
    EXPECT_TRUE(prel64->pcRelative);
    EXPECT_EQ(prel64->widthBytes, 8u);
    EXPECT_EQ(prel64->addendBias, 0);
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

TEST(LinkVeneerVocabulary, AStepWritesARegisterIffItsOpcodeProducesAValue) {
    auto missing = mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
        firstBody(doc).at("sequence").at(0).erase("result");
    });
    expectRefusedAt(missing, "/linkVeneers/bodies/0/sequence/0/result",
                    "`lea` produces a value, and it must land in a granted "
                    "scratch register");
    auto spurious = mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
        firstBody(doc).at("sequence").at(1)["result"] = "x16";
    });
    expectRefusedAt(spurious, "/linkVeneers/bodies/0/sequence/1/result",
                    "`jmp_indirect` produces nothing to write");
}

// A step may name only the registers `scratchRegisters` grants — as its result,
// as an operand, or as a memory operand's base. x9 is a perfectly good register;
// the ABI just does not hand it to a linker.
TEST(LinkVeneerVocabulary, OnlyAGrantedScratchRegisterMayBeNamed) {
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            firstBody(doc).at("sequence").at(0)["result"] = "x9";
        }),
        "/linkVeneers/bodies/0/sequence/0/result",
        "writing a register the ABI does not grant is a clobber");
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            firstBody(doc).at("sequence").at(0)["result"] = "target";
        }),
        "/linkVeneers/bodies/0/sequence/0/result",
        "a keyword is not a register");
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            firstBody(doc).at("sequence").at(1)["operands"] = {"x9"};
        }),
        "/linkVeneers/bodies/0/sequence/1/operands/0",
        "reading an ungranted register is as foreign to a veneer as writing one");
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            doc.at("linkVeneers").at("bodies").at(1).at("sequence").at(1)
                .at("operands").at(0)["base"] = "x9";
        }),
        "/linkVeneers/bodies/1/sequence/1/operands/0/base",
        "a memory operand's base is a register the veneer names");
    // THE PART-1 KEYWORD IS RETIRED: with two granted registers, "scratch" no
    // longer says which one.
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            firstBody(doc).at("sequence").at(1)["operands"] = {"scratch"};
        }),
        "/linkVeneers/bodies/0/sequence/1/operands/0",
        "\"scratch\" names no register once two are granted");
}

TEST(LinkVeneerVocabulary, EveryNameResolvesOrTheLoadFailsAtItsPath) {
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            doc.at("linkVeneers").at("scratchRegisters") = {"x99"};
        }),
        "/linkVeneers/scratchRegisters/0", "an unknown scratch register");
    // A COHERENT mutant: `v0` granted AND used everywhere `x16` was, so the one
    // defect is the register's class. (Granting `v0` alone would fail sooner and
    // for another reason — every body names `x16`, which would then be
    // ungranted — and never reach the class rule this arm is about.)
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            renameRegister(doc.at("linkVeneers"), "x16", "v0");
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
// (1) THE DATA WORD — [[D-LK-AARCH64-VENEER-CANNOT-REACH-PAST-FOUR-GIB]]
// ─────────────────────────────────────────────────────────────────────────
namespace {

nlohmann::json& longSequence(nlohmann::json& doc) {
    return doc.at("linkVeneers").at("bodies").at(1).at("sequence");
}

}  // namespace

TEST(LinkVeneerVocabulary, ADataWordFollowsTheClosingBranch) {
    auto r = mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
        auto& seq = longSequence(doc);
        std::swap(seq.at(3), seq.at(4));  // the word before the `br`
    });
    expectRefusedAt(r, "/linkVeneers/bodies/1/sequence/3/data",
                    "a word before the branch would be EXECUTED");
}

TEST(LinkVeneerVocabulary, ADataWordIsWrittenPcRelatively) {
    auto r = mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
        longSequence(doc).at(4)["relocations"] = {"abs64"};
    });
    expectRefusedAt(r, "/linkVeneers/bodies/1/sequence/4/relocations/0",
                    "an absolute word would be a text relocation in every PIE and "
                    "shared object, and a veneer is built before anyone knows "
                    "which the image is");
}

TEST(LinkVeneerVocabulary, ADataWordIsExactlyAsWideAsItsRelocation) {
    auto r = mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
        longSequence(doc).at(4)["data"] = 4;
    });
    expectRefusedAt(r, "/linkVeneers/bodies/1/sequence/4/data",
                    "a 4-byte word written through an 8-byte relocation would "
                    "overrun the veneer");
}

TEST(LinkVeneerVocabulary, ADataWordHoldsTheTargetThroughOneRelocation) {
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            longSequence(doc).at(4)["operands"] = {"literal"};
        }),
        "/linkVeneers/bodies/1/sequence/4/operands",
        "the word holds the veneer's destination, nothing else");
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            longSequence(doc).at(4)["relocations"] = nlohmann::json::array();
        }),
        "/linkVeneers/bodies/1/sequence/4/relocations",
        "a word nothing writes would hold zero");
}

TEST(LinkVeneerVocabulary, ALiteralNeedsTheBodysOneDataWord) {
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            longSequence(doc).erase(4);
        }),
        "/linkVeneers/bodies/1/sequence",
        "\"literal\" addresses the body's data word, and there is none");
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            auto& seq = longSequence(doc);
            seq.push_back(seq.at(4));
        }),
        "/linkVeneers/bodies/1/sequence",
        "two words would leave \"literal\" ambiguous");
}

// The converse of the arm above: a word that no instruction addresses is never
// read. `lea x17, target` in place of `lea x17, literal` would pass every other
// rule and build a veneer that loads the TARGET's own code bytes as an address.
TEST(LinkVeneerVocabulary, ADataWordIsReadBySomeInstruction) {
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            longSequence(doc).at(0)["operands"] = {"target"};
        }),
        "/linkVeneers/bodies/1/sequence",
        "a word nothing reads, and a veneer that jumps through the target's own bytes");
}

TEST(LinkVeneerVocabulary, AStepIsAnInstructionOrADataWordNeverBoth) {
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            longSequence(doc).at(4)["mnemonic"] = "nop";
        }),
        "/linkVeneers/bodies/1/sequence/4",
        "a step naming both shapes");
    expectRefusedAt(
        mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
            longSequence(doc).at(4)["result"] = "x16";
        }),
        "/linkVeneers/bodies/1/sequence/4/result",
        "a word is never executed, so it writes no register");
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

constexpr std::uint64_t kMiBytes = 1024u * 1024u;
constexpr std::uint64_t kGiBytes = 1024u * kMiBytes;

// caller (`BL far@stub`, `RET`) │ callee (`calleeWords` × `RET`), with the
// import's stub REPORTED `pastTextEnd` bytes past the end of `.text` — an
// out-of-reach import call at the cost of a few words. The report is an UPPER
// BOUND by contract, so an over-estimate is legal: it can only add a veneer.
// The caller is the image's entry, so an image writer can encode the result.
struct FarImport {
    AssembledModule            module;
    link::ImportCallStubLayout stubs;
    SymbolId caller{1}, callee{2}, imp{40};
};

FarImport buildFarImport(TargetSchema const& target,
                         std::uint64_t pastTextEnd = 200 * kMiBytes,
                         std::size_t calleeWords = 1) {
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
    for (std::size_t i = 0; i < calleeWords; ++i) appendWord(e.bytes, kRet);
    f.module.functions.push_back(std::move(e));
    f.module.expectedFuncCount  = 2;
    f.module.imageEntryOverride = 0u;
    f.module.externImports.push_back(ExternImport{f.imp, "far", "libfar.so"});
    f.stubs.maxPastTextEnd.emplace(f.imp, pastTextEnd);
    return f;
}

ObjectFormatSchema const& shippedFormat(std::string_view name) {
    static std::vector<std::pair<std::string, std::shared_ptr<ObjectFormatSchema const>>> cache;
    for (auto const& [n, s] : cache)
        if (n == name) return *s;
    auto r = ObjectFormatSchema::loadShipped(name);
    EXPECT_TRUE(r.has_value()) << name;
    cache.emplace_back(std::string{name}, *r);
    return *cache.back().second;
}
ObjectFormatSchema const& elfExec()   { return shippedFormat("elf64-aarch64-linux-exec"); }
ObjectFormatSchema const& machoExec() { return shippedFormat("macho64-arm64-darwin-exec"); }

std::string allDiagnostics(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + d.actual;
    return s;
}

}  // namespace

TEST(LinkVeneerVocabulary, AFarImportCallIsCarriedAtKilobyteScale) {
    auto f = buildFarImport(arm64());
    ASSERT_TRUE(linker::branchVeneersNeeded(f.module, arm64(), f.stubs));
    DiagnosticReporter rep;
    linker::BranchVeneerWork work;
    ASSERT_TRUE(linker::injectBranchVeneers(f.module, arm64(), elfExec(), f.stubs,
                                            rep, &work))
        << allDiagnostics(rep);
    ASSERT_EQ(f.module.functions.size(), 3u);
    // The furthest boundary in reach of the call is the END of `.text`.
    auto const& veneer = f.module.functions[2];
    ASSERT_EQ(veneer.bytes.size(), 12u) << "200 MiB is the cheap ADRP body's to carry";
    ASSERT_EQ(veneer.relocations.size(), 2u);
    EXPECT_EQ(veneer.relocations[0].target.v, f.imp.v);
    EXPECT_EQ(f.module.functions[0].relocations[0].target.v, veneer.symbol.v);
    EXPECT_EQ(work.bodyProbes, 1u);
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
    EXPECT_FALSE(linker::injectBranchVeneers(f.module, **swapped, elfExec(), f.stubs, rep));
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
    EXPECT_TRUE(linker::injectBranchVeneers(g.module, arm64(), elfExec(), g.stubs, rep2));
}

// ─────────────────────────────────────────────────────────────────────────
// (1) PAST ±4 GiB, FULL PATH — [[D-LK-AARCH64-VENEER-CANNOT-REACH-PAST-FOUR-GIB]]
// ─────────────────────────────────────────────────────────────────────────
//
// A > 4 GiB image is not a unit test's to build, but the stub REPORT is an
// upper bound, so a stub reported 5 GiB past a KiB-sized `.text` puts the
// import past the ADRP body's reach from every boundary — and the writer, which
// lays the stub out where it really goes, then proves the long body's word
// right at the real distance: the literal is PC-relative, so it is correct at
// any distance the relocation's 64 bits can hold.
TEST(LinkVeneerVocabulary, AnImportPastFourGiBIsCarriedByTheLongBodyOnElf) {
    auto f = buildFarImport(arm64(), 5 * kGiBytes);
    DiagnosticReporter rep;
    linker::BranchVeneerWork work;
    ASSERT_TRUE(linker::injectBranchVeneers(f.module, arm64(), elfExec(), f.stubs,
                                            rep, &work))
        << allDiagnostics(rep);
    ASSERT_EQ(f.module.functions.size(), 3u);
    auto const& v = f.module.functions[2];  // at the end of `.text`: offset 12
    ASSERT_EQ(v.bytes.size(), 32u) << "20 bytes of code, 4 of slack, the 8-byte word";
    EXPECT_EQ(work.bodyProbes, 2u) << "the cheap body first, and it cannot reach";

    // The code, word by word: GNU ld's mechanism in this target's verbs.
    EXPECT_EQ(u32(v.bytes, 0) & 0x9F00001Fu, 0x90000011u) << "adrp x17, <literal>";
    EXPECT_EQ(u32(v.bytes, 4) & 0xFFC003FFu, 0x91000231u) << "add x17, x17, :lo12:<literal>";
    EXPECT_EQ(u32(v.bytes, 8), 0xF8400230u) << "ldur x16, [x17]";
    EXPECT_EQ(u32(v.bytes, 12), 0x8B110210u) << "add x16, x16, x17 — GNU ld's own word";
    EXPECT_EQ(u32(v.bytes, 16), 0xD61F0200u) << "br x16";

    // The veneer lands at text offset 12, so its word goes in the slot at 20:
    // 12 + 20 = 32, a multiple of 8.
    constexpr std::uint32_t kLiteral = 20;
    auto const lea0 = arm64().relocationByName("adr_prel_pg_hi21")->kind;
    auto const lea1 = arm64().relocationByName("add_abs_lo12_nc")->kind;
    auto const prel = arm64().relocationByName("prel64")->kind;
    ASSERT_EQ(v.relocations.size(), 3u);
    EXPECT_EQ(v.relocations[0].offset, 0u);
    EXPECT_EQ(v.relocations[0].kind, lea0);
    EXPECT_EQ(v.relocations[0].target.v, v.symbol.v) << "the lea addresses the veneer's OWN literal";
    EXPECT_EQ(v.relocations[0].addend, kLiteral);
    EXPECT_EQ(v.relocations[1].offset, 4u);
    EXPECT_EQ(v.relocations[1].kind, lea1);
    EXPECT_EQ(v.relocations[1].target.v, v.symbol.v);
    EXPECT_EQ(v.relocations[1].addend, kLiteral);
    EXPECT_EQ(v.relocations[2].offset, kLiteral);
    EXPECT_EQ(v.relocations[2].kind, prel);
    EXPECT_EQ(v.relocations[2].target.v, f.imp.v) << "the word holds the distance to the import";
    EXPECT_EQ(v.relocations[2].addend, 0);
    EXPECT_EQ(f.module.functions[0].relocations[0].target.v, v.symbol.v);

    // AND THE WRITER AGREES. Encode the image and read the word back: it is the
    // distance from itself to the real `.plt` stub, and the lea points at it.
    DiagnosticReporter encRep;
    auto const image = elf::encode(f.module, arm64(), elfExec(), encRep);
    ASSERT_FALSE(image.empty()) << allDiagnostics(encRep);
    auto const text = elfSection(image, ".text");
    auto const plt  = elfSection(image, ".plt");
    ASSERT_TRUE(text.has_value() && plt.has_value());
    std::uint64_t const veneerVa  = text->addr + 12;
    std::uint64_t const literalVa = veneerVa + kLiteral;
    EXPECT_EQ(literalVa % 8, 0u) << "the word lands 8-aligned, as GNU ld keeps it";
    std::uint64_t const fileOff = text->offset + 12 + kLiteral;
    std::int64_t word = 0;
    for (int i = 7; i >= 0; --i)
        word = static_cast<std::int64_t>((static_cast<std::uint64_t>(word) << 8)
                                         | image[fileOff + static_cast<std::size_t>(i)]);
    EXPECT_EQ(word, static_cast<std::int64_t>(plt->addr) - static_cast<std::int64_t>(literalVa))
        << "literal + &literal must be the import's stub";
    std::uint32_t const adrp = u32(image, text->offset + 12);
    std::uint32_t const add  = u32(image, text->offset + 16);
    std::int64_t pages = static_cast<std::int64_t>(((adrp >> 5) & 0x7FFFFu) << 2 | ((adrp >> 29) & 3u));
    if (pages & (std::int64_t{1} << 20)) pages -= std::int64_t{1} << 21;
    std::uint64_t const lea = ((veneerVa & ~std::uint64_t{0xFFF}) + static_cast<std::uint64_t>(pages * 4096))
                            + ((add >> 10) & 0xFFFu);
    EXPECT_EQ(lea, literalVa) << "x17 = the literal's own address";
}

// The long body's two declarations must agree too: swapping its `lea`'s two page
// relocations still LOADS (the names resolve), and only building the body sees
// it. The long body is assembled whenever a veneer is needed, so the refusal
// comes with a far import; nothing may be inserted. CONTROL: the shipped body.
TEST(LinkVeneerVocabulary, TheLongBodysDeclaredRelocationsMustMatchWhatItAssembles) {
    auto swapped = mutateShippedTargetSchemaDoc("arm64", [](nlohmann::json& doc) {
        longSequence(doc).at(0)["relocations"] =
            nlohmann::json::array({"add_abs_lo12_nc", "adr_prel_pg_hi21"});
    });
    ASSERT_TRUE(swapped.has_value());
    auto f = buildFarImport(**swapped, 5 * kGiBytes);
    DiagnosticReporter rep;
    EXPECT_FALSE(linker::injectBranchVeneers(f.module, **swapped, elfExec(), f.stubs, rep));
    bool named = false;
    for (auto const& d : rep.all())
        if (d.actual.find("prel64-literal") != std::string::npos
            && d.actual.find("declares the relocations") != std::string::npos
            && d.actual.find("assembled") != std::string::npos)
            named = true;
    EXPECT_TRUE(named) << "the refusal names the long body and both lists:"
                       << allDiagnostics(rep);
    EXPECT_EQ(f.module.functions.size(), 2u) << "and nothing may be inserted";

    auto g = buildFarImport(arm64(), 5 * kGiBytes);
    DiagnosticReporter rep2;
    EXPECT_TRUE(linker::injectBranchVeneers(g.module, arm64(), elfExec(), g.stubs, rep2))
        << allDiagnostics(rep2);
}

// The word lands aligned WHEREVER the veneer stands: a veneer at a text offset
// that is 4 mod 8 uses the slot at 20, one at 0 mod 8 the slot at 24.
TEST(LinkVeneerVocabulary, TheLongBodysWordLandsEightAlignedWhereverTheVeneerStands) {
    for (std::size_t const calleeWords : {1u, 2u}) {
        auto f = buildFarImport(arm64(), 5 * kGiBytes, calleeWords);
        std::uint64_t const veneerAt = 8 + 4 * calleeWords;  // the end of `.text`
        DiagnosticReporter rep;
        ASSERT_TRUE(linker::injectBranchVeneers(f.module, arm64(), elfExec(), f.stubs, rep))
            << allDiagnostics(rep);
        auto const& v = f.module.functions.back();
        ASSERT_EQ(v.relocations.size(), 3u);
        auto const slot = v.relocations[2].offset;
        EXPECT_EQ((veneerAt + slot) % 8, 0u)
            << "veneer at text offset " << veneerAt << ", word at +" << slot;
        EXPECT_EQ(slot, veneerAt % 8 == 4 ? 20u : 24u);
        EXPECT_EQ(v.relocations[0].addend, static_cast<std::int64_t>(slot))
            << "the lea follows the word to whichever slot it took";
        EXPECT_EQ(v.bytes.size(), 32u) << "the size does not depend on the slot";
    }
}

// MACH-O DECLARES NO 64-BIT PC-RELATIVE DATA RELOCATION, so the long body is
// never a candidate there and the same shape is REFUSED BY NAME — where the
// Mach-O references stop (✔MEASURED 2026-09-19: ld64.lld 18.1.3 refuses the
// shape, or links a thunk whose ADRP wrapped). CONTROL: 200 MiB on Mach-O is
// the ADRP body's, as on ELF.
TEST(LinkVeneerVocabulary, PastFourGiBIsRefusedByNameOnMachO) {
    auto f = buildFarImport(arm64(), 5 * kGiBytes);
    for (auto& e : f.module.externImports) e.libraryPath = "/usr/lib/libSystem.B.dylib";
    DiagnosticReporter rep;
    EXPECT_FALSE(linker::injectBranchVeneers(f.module, arm64(), machoExec(), f.stubs, rep));
    EXPECT_EQ(f.module.functions.size(), 2u) << "nothing may be inserted";
    bool named = false;
    for (auto const& d : rep.all())
        if (d.actual.find("prel64") != std::string::npos
            && d.actual.find("macho64-arm64-darwin-exec") != std::string::npos
            && d.actual.find("D-LK-AARCH64-VENEER-CANNOT-REACH-PAST-FOUR-GIB")
                   != std::string::npos)
            named = true;
    EXPECT_TRUE(named) << "the refusal names the format, the relocation it lacks "
                          "and the row:" << allDiagnostics(rep);

    auto g = buildFarImport(arm64(), 200 * kMiBytes);
    for (auto& e : g.module.externImports) e.libraryPath = "/usr/lib/libSystem.B.dylib";
    DiagnosticReporter rep2;
    ASSERT_TRUE(linker::injectBranchVeneers(g.module, arm64(), machoExec(), g.stubs, rep2))
        << allDiagnostics(rep2);
    ASSERT_EQ(g.module.functions.size(), 3u);
    EXPECT_EQ(g.module.functions[2].bytes.size(), 12u);
}

// THE WORD'S ALIGNMENT IS A PROMISE THE FORMAT MUST BE ABLE TO KEEP. The pass
// places the word by its TEXT OFFSET, which says nothing about its address
// unless `.text` itself starts at least word-aligned. A format declaring `.text`
// only 4-aligned cannot carry the long body, and the refusal says why.
// CONTROL: the shipped format (16-aligned) carries the same module.
TEST(LinkVeneerVocabulary, ALongBodyNeedsTextToStartAtLeastAsAlignedAsItsWord) {
    auto path = findShippedConfig(ShippedConfigLocator{
        "elf64-aarch64-linux-exec", "object-formats", ".format.json",
        "object format", DiagnosticCode::C_InvalidFormatName});
    ASSERT_TRUE(path.has_value());
    std::ifstream in{*path};
    std::ostringstream buf;
    buf << in.rdbuf();
    auto doc = nlohmann::json::parse(buf.str());
    bool edited = false;
    for (auto& s : doc.at("sections"))
        if (s.value("kind", "") == "text") {
            s["addrAlign"] = 4;
            edited = true;
        }
    ASSERT_TRUE(edited) << "the shipped format has no `.text` row to edit";
    auto loose = ObjectFormatSchema::loadFromText(doc.dump(), "four-aligned-text.format.json");
    ASSERT_TRUE(loose.has_value());

    auto f = buildFarImport(arm64(), 5 * kGiBytes);
    DiagnosticReporter rep;
    EXPECT_FALSE(linker::injectBranchVeneers(f.module, arm64(), **loose, f.stubs, rep));
    EXPECT_EQ(f.module.functions.size(), 2u);
    bool named = false;
    for (auto const& d : rep.all())
        if (d.actual.find("must land 8-aligned") != std::string::npos
            && d.actual.find("only 4-aligned") != std::string::npos)
            named = true;
    EXPECT_TRUE(named) << "the refusal says the word's alignment cannot be kept:"
                       << allDiagnostics(rep);

    auto g = buildFarImport(arm64(), 5 * kGiBytes);
    DiagnosticReporter rep2;
    EXPECT_TRUE(linker::injectBranchVeneers(g.module, arm64(), elfExec(), g.stubs, rep2))
        << allDiagnostics(rep2);
}

// AN IMAGE UNDER ~4 GiB PLANS EXACTLY AS IT DID WITH ONE BODY. The long body is
// usable here, but the ADRP body already covers the whole image, so the long one
// is not a candidate and the margin stays priced at 12 bytes. Two far calls:
// X to a stub 200 MiB on (it needs a veneer), Y to a stub it reaches with 30
// bytes to spare. Priced at 2 x 12, Y stays direct; priced at 2 x 32 it would
// take a veneer it does not need.
TEST(LinkVeneerVocabulary, AnImageUnderFourGiBPlansAsWithTheCheapBodyAlone) {
    auto f = buildFarImport(arm64(), 200 * kMiBytes);
    SymbolId const near{41};
    auto& caller = f.module.functions[0];
    caller.bytes.insert(caller.bytes.begin() + 4, {0, 0, 0x00, 0x94});  // a second BL at 4
    caller.relocations.push_back(
        Relocation{4u, near, arm64().relocationByName("call26")->kind, 0});
    f.module.externImports.push_back(ExternImport{near, "near", "libfar.so"});
    std::int64_t const reach = link::relocFieldReach(
        *arm64().relocationByName("call26"))->maxDelta;
    std::uint64_t const textSize = 12 + 4;  // caller 12, callee 4
    // Y at text offset 4 lands `reach - 30` bytes on.
    f.stubs.maxPastTextEnd.emplace(
        near, static_cast<std::uint64_t>(reach - 30 + 4) - textSize);
    DiagnosticReporter rep;
    linker::BranchVeneerWork work;
    ASSERT_TRUE(linker::injectBranchVeneers(f.module, arm64(), elfExec(), f.stubs, rep, &work))
        << allDiagnostics(rep);
    EXPECT_EQ(work.farSites, 2u);
    EXPECT_EQ(work.veneersPlaced, 1u)
        << "only X needs a veneer; a margin priced at the long body would give Y one";
    ASSERT_EQ(f.module.functions.size(), 3u);
    EXPECT_EQ(f.module.functions[2].bytes.size(), 12u);
    EXPECT_EQ(f.module.functions[0].relocations[1].target.v, near.v) << "Y stays direct";
}

// ─────────────────────────────────────────────────────────────────────────
// (1) ONE RULE SET, TWO CALLERS — the loader and the linker
// ─────────────────────────────────────────────────────────────────────────
//
// `validate()` refuses a malformed `linkVeneers` block at load, so a schema
// the JSON loader built never carries one. A schema built IN MEMORY never went
// through the loader, and the veneer pass must refuse it with the SAME rules —
// `detail::linkVeneerProblems`, the one function both call — instead of
// building a malformed body. These arms build such a schema the way
// `test_asm_roundtrip.cpp` does, straight from a `detail::TargetSchemaData`,
// with the shipped arm64 tables: the builder COULD build whatever it is handed,
// so only the rule set stands between a malformed body and the image.
namespace {

TargetSchema inMemoryArm64(LinkVeneerVocabulary vocab) {
    auto const& src = arm64();
    detail::TargetSchemaData d;
    // The builder refuses a schema with the invalid id. The loader mints one per
    // document from a process counter that starts at 1; a fixed id far above it
    // cannot collide with a schema this process loaded.
    d.id   = TargetSchemaId{0x7E570001u};
    d.name = "arm64-in-memory";
    d.opcodes.assign(src.opcodes().begin(), src.opcodes().end());
    for (std::size_t i = 0; i < d.opcodes.size(); ++i)
        d.mnemonicIndex.emplace(d.opcodes[i].mnemonic, static_cast<std::uint16_t>(i));
    d.registers.assign(src.registers().begin(), src.registers().end());
    for (std::size_t i = 0; i < d.registers.size(); ++i)
        d.registerIndex.emplace(d.registers[i].name, static_cast<std::uint16_t>(i));
    d.relocations.assign(src.relocations().begin(), src.relocations().end());
    for (std::size_t i = 0; i < d.relocations.size(); ++i) {
        d.relocationNameIndex.emplace(d.relocations[i].name, static_cast<std::uint16_t>(i));
        d.relocationKindIndex.emplace(d.relocations[i].kind, static_cast<std::uint16_t>(i));
    }
    d.linkVeneers = std::move(vocab);
    return TargetSchema{std::move(d)};
}

// The shipped vocabulary with its LONG body malformed by `breakIt`.
LinkVeneerVocabulary shippedVocabularyWithLongBody(
    std::function<void(std::vector<LinkVeneerStep>&)> const& breakIt) {
    LinkVeneerVocabulary v = *arm64().linkVeneers();
    breakIt(v.bodies.at(1).sequence);
    return v;
}

bool reported(DiagnosticReporter const& rep, std::string_view path, std::string_view text) {
    for (auto const& d : rep.all())
        if (d.actual.find(path) != std::string::npos
            && d.actual.find(text) != std::string::npos)
            return true;
    return false;
}

}  // namespace

// CONTROL: the in-memory schema with the SHIPPED vocabulary has no shape
// problem and links a far import exactly as the loaded schema does — so a
// refusal in the arms below is the malformation's, not the construction's.
TEST(LinkVeneerVocabulary, AnInMemorySchemaWithTheShippedVocabularyLinks) {
    auto const schema = inMemoryArm64(*arm64().linkVeneers());
    EXPECT_TRUE(schema.linkVeneerProblems().empty());
    auto f = buildFarImport(schema, 5 * kGiBytes);
    DiagnosticReporter rep;
    ASSERT_TRUE(linker::injectBranchVeneers(f.module, schema, elfExec(), f.stubs, rep))
        << allDiagnostics(rep);
    ASSERT_EQ(f.module.functions.size(), 3u);
    EXPECT_EQ(f.module.functions[2].bytes.size(), 32u) << "the long body, 5 GiB out";
}

// A data word BEFORE the closing branch. The builder would happily build it —
// it appends the word after the code whatever the declaration says — so the
// only thing that refuses it is the rule set, with `validate()`'s own words at
// the step's JSON path. And the pass refuses; it does not abort.
TEST(LinkVeneerVocabulary, TheLinkerRefusesAnInMemoryWordBeforeTheBranchInValidatesWords) {
    auto const schema = inMemoryArm64(shippedVocabularyWithLongBody(
        [](std::vector<LinkVeneerStep>& seq) { std::swap(seq.at(3), seq.at(4)); }));
    auto f = buildFarImport(schema, 5 * kGiBytes);
    DiagnosticReporter rep;
    EXPECT_FALSE(linker::injectBranchVeneers(f.module, schema, elfExec(), f.stubs, rep));
    EXPECT_TRUE(reported(rep, "/linkVeneers/bodies/1/sequence/3/data",
                         "a data word must follow the body's closing branch"))
        << "the refusal carries validate()'s rule, at its path:" << allDiagnostics(rep);
    EXPECT_EQ(f.module.functions.size(), 2u) << "and nothing is inserted";
}

// An UNGRANTED register — a rule the pass's old hand-copied subset never had.
// Built anyway, this veneer would clobber x9, which the ABI never hands a
// linker; 5 GiB out, the long body is the one that would be elected.
TEST(LinkVeneerVocabulary, TheLinkerRefusesAnInMemoryUngrantedRegister) {
    auto const x9 = *arm64().registerByName("x9");
    auto const schema = inMemoryArm64(shippedVocabularyWithLongBody(
        [&](std::vector<LinkVeneerStep>& seq) {
            auto& base = seq.at(1).operands.at(0);  // load x16, [x17] -> [x9]
            base.reg          = x9;
            base.registerName = "x9";
        }));
    auto f = buildFarImport(schema, 5 * kGiBytes);
    DiagnosticReporter rep;
    EXPECT_FALSE(linker::injectBranchVeneers(f.module, schema, elfExec(), f.stubs, rep));
    EXPECT_TRUE(reported(rep, "/linkVeneers/bodies/1/sequence/1/operands/0",
                         "veneer operand register 'x9' is not in scratchRegisters"))
        << allDiagnostics(rep);
    EXPECT_EQ(f.module.functions.size(), 2u);
}

// ONE rule set: the problems the linker is handed are exactly the linkVeneers
// problems `validate()` reports for the same data — path for path, word for word.
TEST(LinkVeneerVocabulary, TheLinkersRuleSetIsTheOneValidateApplies) {
    auto const vocab = shippedVocabularyWithLongBody(
        [](std::vector<LinkVeneerStep>& seq) { std::swap(seq.at(3), seq.at(4)); });
    auto const schema = inMemoryArm64(vocab);

    detail::TargetSchemaData data;  // the same tables, validated directly
    auto const& src = arm64();
    data.opcodes.assign(src.opcodes().begin(), src.opcodes().end());
    data.registers.assign(src.registers().begin(), src.registers().end());
    data.relocations.assign(src.relocations().begin(), src.relocations().end());
    for (std::size_t i = 0; i < data.relocations.size(); ++i)
        data.relocationKindIndex.emplace(data.relocations[i].kind, static_cast<std::uint16_t>(i));
    data.linkVeneers = vocab;
    std::vector<std::string> fromValidate;
    for (auto const& p : data.validate())
        if (p.path.starts_with("/linkVeneers")) fromValidate.push_back(p.path + ": " + p.message);
    std::vector<std::string> fromLinker;
    for (auto const& p : schema.linkVeneerProblems()) fromLinker.push_back(p.path + ": " + p.message);
    ASSERT_FALSE(fromLinker.empty());
    EXPECT_EQ(fromLinker, fromValidate);
}

// ─────────────────────────────────────────────────────────────────────────
// (2) THE WRITERS' STUB LAYOUTS, AGAINST THE IMAGES THEY EMIT
// ─────────────────────────────────────────────────────────────────────────
namespace {

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
