// P69 fold 3e (lane xa) -- the review of fold 3d's X9: "absolute" and an address's WIDTH each have ONE home. A writer
// writes ONE class -- ELF64 (elf.cpp), Mach-O 64 (macho.cpp), PE32+ (pe.cpp) -- whose address is its class word, read
// by every slot it sizes; its one public entry (`encode`) refuses, BY NAME, a document whose data model's pointer is
// another width (`link/format/address_class_word.hpp`, K_PointerWidthNotTheWritersClass), so no document is written
// with slots of the wrong width. And the one value the lowering reads from a document for a pointer-strided array --
// the pe-indexed TLS sequence's -- is the document's data model's pointer width, derived by the document's loader.
//
// THE UNIVERSE: every shipped document each writer encodes (every `elf64-*`, `macho64-*` and `pe64-*` document but the
// static-library containers, whose members a relocatable document writes), each with its data model made `ILP32` (a
// 4-byte pointer): refused at the writer's entry, naming the writer and the width -- and each as shipped: never so
// refused. The scratch documents live here, in memory; none ships.

#include "asm/asm.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf.hpp"
#include "link/format/macho.hpp"
#include "link/format/pe.hpp"
#include "link/object_format_schema.hpp"

#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

namespace fs = std::filesystem;

struct Writer {
    std::string_view prefix;   // the family of documents it encodes
    std::string_view entry;    // its one public entry, as the refusal names it
    std::vector<std::uint8_t> (*encode)(AssembledModule const&, TargetSchema const&, ObjectFormatSchema const&,
                                        DiagnosticReporter&, ImageRequest const&);
};

std::vector<std::uint8_t> encodeElf(AssembledModule const& m, TargetSchema const& t, ObjectFormatSchema const& f,
                                    DiagnosticReporter& r, ImageRequest const& q) {
    return elf::encode(m, t, f, r, q);
}
std::vector<std::uint8_t> encodeMachO(AssembledModule const& m, TargetSchema const& t, ObjectFormatSchema const& f,
                                      DiagnosticReporter& r, ImageRequest const& q) {
    return macho::encode(m, t, f, r, q);
}
std::vector<std::uint8_t> encodePe(AssembledModule const& m, TargetSchema const& t, ObjectFormatSchema const& f,
                                   DiagnosticReporter& r, ImageRequest const& q) {
    return pe::encode(m, t, f, r, q).value_or(std::vector<std::uint8_t>{});
}

Writer const kWriters[] = {
    {"elf64-", "elf::encode", &encodeElf},
    {"macho64-", "macho::encode", &encodeMachO},
    {"pe64-", "pe::encode", &encodePe},
};

// Every shipped document of `prefix`'s family the writer encodes: the static-library containers left out.
[[nodiscard]] std::vector<std::string> documentsOf(std::string_view prefix) {
    std::vector<std::string> out;
    for (auto const& e : fs::directory_iterator(dss::test::configRoot() / "object-formats")) {
        std::string const file = e.path().filename().string();
        std::string_view const suffix = ".format.json";
        if (!file.starts_with(prefix) || !file.ends_with(suffix)) continue;
        std::string const name = file.substr(0, file.size() - suffix.size());
        if (name.ends_with("-staticlib")) continue;
        out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The ISA a document's name says it is for (the shipped names' one spelling: `<family>64-<isa>-...`).
[[nodiscard]] std::string targetOf(std::string const& document) {
    return document.find("aarch64") != std::string::npos || document.find("arm64") != std::string::npos ? "arm64"
                                                                                                         : "x86_64";
}

[[nodiscard]] nlohmann::json shippedText(std::string const& document) {
    std::ifstream in{dss::test::configRoot() / "object-formats" / (document + ".format.json")};
    return nlohmann::json::parse(in);
}

[[nodiscard]] std::size_t refusalsNaming(DiagnosticReporter const& rep, std::string_view entry) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_PointerWidthNotTheWritersClass && d.actual.find(entry) != std::string::npos
            && d.actual.find("4 bytes") != std::string::npos) {
            ++n;
        }
    }
    return n;
}

[[nodiscard]] std::size_t refusalsOfAnyKind(DiagnosticReporter const& rep) {
    return static_cast<std::size_t>(std::count_if(rep.all().begin(), rep.all().end(), [](auto const& d) {
        return d.code == DiagnosticCode::K_PointerWidthNotTheWritersClass;
    }));
}

}  // namespace

// Every writer refuses each of its documents made 4-byte-pointered, at its entry and by name, writing nothing; each
// document as shipped is never so refused. RED-ON-DISABLE: drop a writer's entry check -> its 4-byte documents are
// written (or refused by something else, unnamed).
TEST(WriterAddressClass, EveryWriterRefusesADocumentWhosePointerIsNotItsClassWord) {
    for (Writer const& w : kWriters) {
        auto const documents = documentsOf(w.prefix);
        ASSERT_FALSE(documents.empty()) << "no shipped document of " << w.prefix;
        for (std::string const& document : documents) {
            SCOPED_TRACE(document);
            auto const target = TargetSchema::loadShipped(targetOf(document));
            ASSERT_TRUE(target.has_value());
            auto doc = shippedText(document);
            doc["dataModel"] = std::string{dataModelName(DataModel::Ilp32)};
            auto const narrow = ObjectFormatSchema::loadFromText(doc.dump(), document + ", ILP32");
            ASSERT_TRUE(narrow.has_value()) << "the 4-byte copy of " << document << " does not load";
            DiagnosticReporter rep;
            auto const bytes = w.encode(AssembledModule{}, **target, **narrow, rep, ImageRequest{});
            EXPECT_TRUE(bytes.empty()) << "a 4-byte document was written by " << w.entry;
            EXPECT_EQ(refusalsNaming(rep, w.entry), 1u)
                << "the writer did not refuse the 4-byte document once, by name, at its entry";
            auto const shipped = ObjectFormatSchema::loadShipped(document);
            ASSERT_TRUE(shipped.has_value());
            DiagnosticReporter srep;
            (void)w.encode(AssembledModule{}, **target, **shipped, srep, ImageRequest{});
            EXPECT_EQ(refusalsOfAnyKind(srep), 0u) << "the shipped document was refused as another class";
        }
    }
}

// The pe-indexed TLS sequence's array stride is the document's data model's pointer width, derived by its loader:
// 8 as shipped, 4 for the 4-byte copy -- never a literal the lowering spells.
TEST(WriterAddressClass, ThePeIndexedTlsStrideIsTheDataModelsPointerWidth) {
    for (std::string const& document : documentsOf("pe64-")) {
        SCOPED_TRACE(document);
        auto const shipped = ObjectFormatSchema::loadShipped(document);
        ASSERT_TRUE(shipped.has_value());
        if (!(*shipped)->tlsAccess().has_value()) continue;   // a document that lowers no thread-local
        EXPECT_EQ((*shipped)->tlsAccess()->blockPointerBytes, 8u);
        auto doc = shippedText(document);
        doc["dataModel"] = std::string{dataModelName(DataModel::Ilp32)};
        auto const narrow = ObjectFormatSchema::loadFromText(doc.dump(), document + ", ILP32");
        ASSERT_TRUE(narrow.has_value());
        ASSERT_TRUE((*narrow)->tlsAccess().has_value());
        EXPECT_EQ((*narrow)->tlsAccess()->blockPointerBytes, 4u) << "the stride is not the data model's";
    }
}
