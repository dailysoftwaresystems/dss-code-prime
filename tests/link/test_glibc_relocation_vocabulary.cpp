// D-LK-ELF-READER-REFUSES-GOTPCREL-BLOCKS-REAL-GLIBC-MEMBERS —
// the relocation VOCABULARY of the ELF relocatable-object reader, measured on
// REAL members of a REAL distro glibc `libc.a` rather than on a synthesised
// stand-in. The row's whole point is FOREIGN-TOOLCHAIN INPUT: a DSS-emitted
// object is written by the same table the reader reads back, so it can never
// carry a wire type DSS has not declared, and therefore can never exercise this.
//
// ── WHAT EACH TEST HERE IS FOR ────────────────────────────────────────────
//
//  1. `ShippedVocabularyRefusesAnUndeclaredTypeInARealGlibcMember`
//     The GUARD, pinned on real bytes. `uselocale.o` reaches
//     R_X86_64_GOTTPOFF (22, TLS initial-exec) at `.rela.text` 0x07 before any
//     other undeclared type, and the read must fail LOUD naming the type and
//     the format. Paired with a CONTROL member that reads clean through the
//     SAME schemas, so "it refused" cannot be confused with "this reader
//     refuses foreign objects".
//
//  2. `DeclaringTheWireTypeIsWhatMakesTheRealMemberRead`
//     The DIAGNOSIS. The same reader, the same real `exit.o` bytes, and a
//     format document that differs from the shipped one by ONE relocation row
//     — the member reads, and its GOTPCREL relocations come back with the
//     addend the wire actually carries. This is the measurement that says the
//     defect is a vocabulary gap and NOT a broken guard.
//
//  3. `WideningTheVocabularyByOneRowDoesNotWeakenTheGuard`
//     The CONTROL for (2), and the property the row's closing work is required
//     to preserve: with type 9 declared, type 22 is STILL refused. A fix that
//     made the reader permissive would go green on (2) and red here.
//
// ⚠ THE DOCUMENTS IN (2) AND (3) ARE DERIVED FROM THE SHIPPED ONES AT RUN TIME,
// never transcribed. A transcribed copy is a second owner of the vocabulary and
// goes stale silently; deriving means these tests keep saying the same thing on
// both sides of the day the shipped documents gain the row.
//
// ⚠ THIS PARAGRAPH USED TO END *"(they then find it already present and inject
// nothing)"*, AND THAT WAS FALSE FOR THE NEGATIVE ARM — measured, on the very day
// the shipped documents gained the row. `injectGotPcRelKind` returned the existing
// row's kind **without re-shaping its `formula`**, so the `linear` arm asserted
// against the SHIPPED non-linear declaration and went red: three assertions,
// ctest rc 8. The helper now re-shapes the row it finds.
// ★ AND THE SHAPE OF THE MISTAKE IS THE POINT. A lane forbidden the config
// documents necessarily tests the tree as it stands BEFORE its own handover patch
// lands, so its green is a statement about a tree that stops existing the moment
// the patch is applied ([[feedback-a-handover-is-pinned-against-by-its-own-lane]]).
// Deriving from the shipped documents is still right; what was missing was
// exercising the AFTER state.
//
// ⚠ WHAT THIS FILE DOES *NOT* CLAIM. Declaring the wire type makes the member
// READ. It does not make the link COMPLETE: applying a GOT-slot-relative
// reference needs a GOT slot, which DSS's static ET_EXEC path does not
// synthesize — `applyExecRelocations` refuses it explicitly, pinned in
// `test_aarch64_reloc_formulas.cpp` (`X86_64GotPcRel.ApplyFailsLoud...`).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf_object_reader.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include "gcc_lib_c164_object.inc"
#include "glibc_relocation_vocabulary_members.inc"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace dss;

namespace {

// The wire ids this file names. Spelled once, beside the psABI names, because a
// number repeated across assertions is a number that can drift in one of them.
constexpr std::uint32_t kRX8664GotPcRel = 9;   // R_X86_64_GOTPCREL
constexpr std::uint32_t kRX8664GotTpOff = 22;  // R_X86_64_GOTTPOFF

// The shipped documents these tests read and derive from.
constexpr char const* kTargetName = "x86_64";
constexpr char const* kFormatName = "elf64-x86_64-linux-staticlib";

[[nodiscard]] std::string readShippedDocument(std::string const& relativePath) {
    auto const path = dss::test::configRoot() / relativePath;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "cannot read shipped document " << path.string();
        return {};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// Every ERROR message the reporter carries, concatenated — so an assertion can
// name the substring it needs without caring which diagnostic carried it. The
// reader emits through the shared `report(...)` shim, which puts the whole
// sentence in `actual`.
[[nodiscard]] std::string errorText(DiagnosticReporter const& reporter) {
    std::string all;
    for (auto const& d : reporter.all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        all += d.actual;
        all += "\n";
    }
    return all;
}

// ── The DERIVED pair: the shipped documents plus the one relocation row ────
//
// `x86_64.target.json` gains a `gotpcrel32` kind whose formula is the closed-enum
// discriminator `x86_64_gotpcrel`, and the staticlib format document maps wire id
// 9 onto it.
//
// ★ THE FORMULA MUST BE THE DEDICATED DISCRIMINATOR, AND THAT IS MEASURED, NOT
// PREFERRED. Declaring the row `linear` + `pcRelative` + `addendBias: 0` +
// `widthBytes: 4` — which is what its wire arithmetic looks like — makes the
// READ fail with K_RelocationKindMismatch: `pcrel32` already occupies the
// "32-bit PC-relative, no implicit addend bias" description, and
// D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS refuses a target where two rows
// match it, because it can no longer say which one a DWARF FDE
// `initial_location` means. The negative is pinned below so this reasoning stops
// being a comment.
struct DerivedSchemas {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
    std::uint32_t                       kind = 0;
};

[[nodiscard]] std::uint32_t
injectGotPcRelKind(nlohmann::json& targetDoc, std::string const& formula) {
    auto& rows = targetDoc.at("relocations");
    for (auto& r : rows) {
        if (r.contains("name") && r.at("name") == "gotpcrel32") {
            // The row may ALREADY be in the shipped document. Returning its
            // kind unchanged would silently hand the caller the SHIPPED
            // formula, which makes the `linear` negative arm assert against a
            // non-linear declaration and go red. Re-shape it instead.
            r["formula"] = formula;
            r.erase("pcRelative");
            r.erase("addendBias");
            r.erase("widthBytes");
            if (formula == "linear") {
                r["pcRelative"] = true;
                r["addendBias"] = 0;
                r["widthBytes"] = 4;
            }
            return r.at("kind").get<std::uint32_t>();
        }
    }
    std::uint32_t highest = 0;
    for (auto const& r : rows) {
        highest = std::max(highest, r.at("kind").get<std::uint32_t>());
    }
    std::uint32_t const kind = highest + 1;
    nlohmann::json row;
    row["name"]    = "gotpcrel32";
    row["kind"]    = kind;
    row["formula"] = formula;
    if (formula == "linear") {
        row["pcRelative"] = true;
        row["addendBias"] = 0;
        row["widthBytes"] = 4;
    }
    rows.push_back(std::move(row));
    return kind;
}

[[nodiscard]] DerivedSchemas
deriveWithGotPcRelDeclared(std::string const& formula = "x86_64_gotpcrel") {
    DerivedSchemas out;

    auto targetDoc = nlohmann::json::parse(
        readShippedDocument("targets/x86_64.target.json"), nullptr, false);
    auto formatDoc = nlohmann::json::parse(
        readShippedDocument("object-formats/elf64-x86_64-linux-staticlib.format.json"),
        nullptr, false);
    if (targetDoc.is_discarded() || formatDoc.is_discarded()) {
        ADD_FAILURE() << "a shipped document did not parse as JSON";
        return out;
    }

    out.kind = injectGotPcRelKind(targetDoc, formula);

    bool declared = false;
    for (auto const& r : formatDoc.at("relocations")) {
        if (r.contains("nativeId")
            && r.at("nativeId").get<std::uint32_t>() == kRX8664GotPcRel) {
            declared = true;
            out.kind = r.at("kind").get<std::uint32_t>();
        }
    }
    if (!declared) {
        nlohmann::json row;
        row["name"]     = "R_X86_64_GOTPCREL";
        row["kind"]     = out.kind;
        row["nativeId"] = kRX8664GotPcRel;
        formatDoc.at("relocations").push_back(std::move(row));
    }

    auto t = TargetSchema::loadFromText(targetDoc.dump(), "<derived x86_64>");
    if (!t.has_value()) {
        std::string why;
        for (auto const& d : t.error()) why += d.message + "\n";
        ADD_FAILURE() << "derived target document did not load: " << why;
        return out;
    }
    auto f = ObjectFormatSchema::loadFromText(formatDoc.dump(),
                                              "<derived staticlib>");
    if (!f.has_value()) {
        std::string why;
        for (auto const& d : f.error()) why += d.message + "\n";
        ADD_FAILURE() << "derived format document did not load: " << why;
        return out;
    }
    out.target = std::move(t).value();
    out.format = std::move(f).value();
    return out;
}

struct Shipped {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Shipped loadShippedPair() {
    Shipped out;
    auto t = TargetSchema::loadShipped(kTargetName);
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(" << kTargetName << ") failed";
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped(kFormatName);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(" << kFormatName << ") failed";
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

// Does a schema declare this wire id at all? Used to state the PREMISE of a
// test out loud, so the day the shipped vocabulary changes the test says which
// half moved instead of quietly asserting the wrong thing.
[[nodiscard]] bool declaresNativeId(ObjectFormatSchema const& format,
                                    std::uint32_t nativeId) {
    for (auto const& r : format.relocations()) {
        if (r.nativeId == nativeId) return true;
        if (r.pltNativeId == nativeId) return true;
    }
    return false;
}

}  // namespace

// ── 1. The guard, on real foreign bytes ───────────────────────────────────
TEST(GlibcRelocationVocabulary,
     ShippedVocabularyRefusesAnUndeclaredTypeInARealGlibcMember) {
    auto shipped = loadShippedPair();
    ASSERT_TRUE(shipped.target && shipped.format);
    // The premise, stated rather than assumed: 22 is not in the vocabulary.
    ASSERT_FALSE(declaresNativeId(*shipped.format, kRX8664GotTpOff))
        << "this test pins the refusal of R_X86_64_GOTTPOFF; the shipped "
           "format now declares it, so the pin needs re-aiming rather than "
           "silently asserting something else";

    DiagnosticReporter reporter;
    auto const module = elf::readRelocatableObject(
        dss::test::glibcUselocaleObject(), *shipped.target, *shipped.format,
        reporter);
    EXPECT_FALSE(module.has_value())
        << "an undeclared relocation type in a real glibc member must never "
           "be guessed at or silently dropped";
    auto const text = errorText(reporter);
    EXPECT_NE(text.find("relocation type "
                        + std::to_string(kRX8664GotTpOff)),
              std::string::npos)
        << "the refusal must NAME the wire type it could not map. Got:\n"
        << text;
    EXPECT_NE(text.find(kFormatName), std::string::npos)
        << "the refusal must NAME the format whose vocabulary is short. Got:\n"
        << text;

    // CONTROL — the same reader, the same two schemas, a foreign gcc-produced
    // object whose every relocation IS declared. Without this arm, "the read
    // failed" is equally consistent with "this reader cannot read foreign
    // objects at all", which would make the assertion above say nothing.
    DiagnosticReporter controlReporter;
    auto const control = elf::readRelocatableObject(
        dss::test::gccLibC164Object(), *shipped.target, *shipped.format,
        controlReporter);
    EXPECT_TRUE(control.has_value())
        << "control: a foreign gcc `.o` using only declared relocation types "
           "must read clean through the SAME schemas. Got:\n"
        << errorText(controlReporter);
}

// ── 2. The diagnosis: one relocation row is the whole difference ──────────
TEST(GlibcRelocationVocabulary,
     DeclaringTheWireTypeIsWhatMakesTheRealMemberRead) {
    auto derived = deriveWithGotPcRelDeclared();
    ASSERT_TRUE(derived.target && derived.format);
    ASSERT_TRUE(declaresNativeId(*derived.format, kRX8664GotPcRel));

    DiagnosticReporter reporter;
    auto const module = elf::readRelocatableObject(
        dss::test::glibcExitObject(), *derived.target, *derived.format,
        reporter);
    ASSERT_TRUE(module.has_value())
        << "a real glibc `exit.o` must READ once its GOT-relative wire type is "
           "declared — the refusal was a vocabulary gap, not a structural one. "
           "Got:\n"
        << errorText(reporter);

    // The two GOTPCREL sites the member actually carries. ✔MEASURED with
    // `readelf -Wr` on the fixture's source member: `.rela.text` offsets 0x1b
    // and 0x123, both `cmpq $0x0,sym@GOTPCREL(%rip)` against a NOTYPE WEAK
    // HIDDEN UND symbol, r_addend −5 (a 4-byte displacement followed by a
    // 1-byte immediate, so the site is 5 bytes from the next instruction).
    //
    // The addend is asserted because it is the half a wrong declaration gets
    // wrong SILENTLY: the reader un-bakes `addendBias` from the wire addend, so
    // a row copied from `rel32` (bias −4) would hand the merge −1 and every
    // patched site would land one byte off.
    std::size_t gotPcRelSites = 0;
    for (auto const& fn : module->functions) {
        for (auto const& rel : fn.relocations) {
            if (rel.kind.v != derived.kind) continue;
            ++gotPcRelSites;
            EXPECT_EQ(rel.addend, -5)
                << "a GOTPCREL site's DSS-native addend must be the wire "
                   "addend un-baked against a ZERO bias";
        }
    }
    EXPECT_EQ(gotPcRelSites, 2u)
        << "the fixture member carries exactly two plain-GOTPCREL sites";

    // Both name an EXTERN — the weak-undefined symbols the idiom checks for.
    bool sawCallTlsDtors = false;
    bool sawIoCleanup    = false;
    for (auto const& e : module->externImports) {
        if (e.mangledName == "__call_tls_dtors") sawCallTlsDtors = true;
        if (e.mangledName == "_IO_cleanup")      sawIoCleanup    = true;
    }
    EXPECT_TRUE(sawCallTlsDtors)
        << "`__call_tls_dtors` is one of the two GOTPCREL targets";
    EXPECT_TRUE(sawIoCleanup)
        << "`_IO_cleanup` is the other";
}

// ── 2b. Why the row needs its OWN formula discriminator ───────────────────
//
// The negative of the design decision above, so the reasoning is a measurement
// rather than a comment. Declaring the wire type with the Linear shape its
// arithmetic suggests makes the read fail for an entirely different reason: the
// target then holds TWO rows matching the DWARF FDE `initial_location`
// description and D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS cannot say which.
TEST(GlibcRelocationVocabulary,
     ALinearDeclarationCollidesWithTheFdeDisambiguator) {
    auto derived = deriveWithGotPcRelDeclared("linear");
    ASSERT_TRUE(derived.target && derived.format);

    DiagnosticReporter reporter;
    auto const module = elf::readRelocatableObject(
        dss::test::glibcExitObject(), *derived.target, *derived.format,
        reporter);
    EXPECT_FALSE(module.has_value())
        << "a Linear/pcRelative/bias-0/width-4 declaration of GOTPCREL is "
           "indistinguishable from `pcrel32` and must be refused";
    auto const text = errorText(reporter);
    EXPECT_NE(text.find("gotpcrel32"), std::string::npos)
        << "the refusal must name the colliding row. Got:\n" << text;
    EXPECT_NE(text.find("pcrel32"), std::string::npos)
        << "...and the row it collides with. Got:\n" << text;
}

// ── 3. Widening the vocabulary must not weaken the guard ──────────────────
TEST(GlibcRelocationVocabulary,
     WideningTheVocabularyByOneRowDoesNotWeakenTheGuard) {
    auto derived = deriveWithGotPcRelDeclared();
    ASSERT_TRUE(derived.target && derived.format);
    ASSERT_TRUE(declaresNativeId(*derived.format, kRX8664GotPcRel));
    ASSERT_FALSE(declaresNativeId(*derived.format, kRX8664GotTpOff));

    DiagnosticReporter reporter;
    auto const module = elf::readRelocatableObject(
        dss::test::glibcUselocaleObject(), *derived.target, *derived.format,
        reporter);
    EXPECT_FALSE(module.has_value())
        << "declaring ONE more wire type must not turn the reader permissive: "
           "a still-undeclared type stays refused";
    EXPECT_NE(errorText(reporter).find("relocation type "
                                       + std::to_string(kRX8664GotTpOff)),
              std::string::npos)
        << "and the refusal must still name the type it could not map";
}
