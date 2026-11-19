// P69 fold 3f, lane `xa` (D-LK-PE-IMAGE-CANNOT-HONOUR-EVERY-LINKER-DIRECTIVE item (2)): THE MANIFEST AN IMAGE EMBEDS.
// A unit's `/MANIFESTDEPENDENCY:<identity>` asks that the image depend on a side-by-side assembly -- the common
// controls version 6 every GUI program names -- which the Windows loader activates from the image's manifest. Until
// this fold a DSS image carried no manifest and the image link refused the directive by name.
//
// ✔MEASURED 2026-11-17 (probe-reference-cc runs 20261117-140648-927055da, 20261117-140952-a037b4db, 20261117-142310-025e5c2e, 20261117-142455-10b66d4b and 20261117-143614-c0ec388c; cl 19.52 + link.exe 14.52
// /MANIFEST:EMBED, clang 18.1.3 + lld-link 18.1.3 /manifest:embed):
//   * ONE resource, RT_MANIFEST (24) / id 1 in an EXE, 2 in a DLL / language 0x409, code page 0. The tree is three
//     one-entry directories at section offsets 0x0, 0x18 and 0x30, the data entry at 0x48, the text at 0x60; the
//     section's size is the text's end rounded up to 8, and the resource data directory states the whole section
//     (0x2E0 for a 734-byte span). `.rsrc` (0x40000040) follows the data and import sections and precedes `.reloc`.
//     `expectedSection` below reproduces link.exe's section bytes for one and two dependencies byte for byte (736 and
//     984 bytes: MEASURED equal, run 20261117-142310-025e5c2e).
//   * the text is link.exe's lines (kHead, kDependency, kTail), each ended by CR LF: one dependency 638 bytes, two
//     887, none 381. lld-link spells its own (LF), with the same meaning.
//   * the dependencies are a link-wide set: in the order the units first state them (two units either way), a repeat
//     once (across units and within one), and one more space or another case is another identity (link.exe and
//     lld-link alike).
//   * by default neither reference embeds a manifest (each writes one beside the image: DOCUMENTED), and an image
//     whose units state none carries none -- a DSS image embeds only when a unit states a dependency.
//   * the loader ACTIVATES the dependency: link.exe's image of a program beside an object depending on an assembly no
//     Windows installs is not started (WinError 14001), where beside the common-controls one it runs to 42.
// The fixtures (tests/link/data/manifest_dependency.source.c states how each was built: each built twice,
// byte-identical, recording no host path):
//   manifest_dependency_cc6_x86_64_pe_cl_MD_O2.obj      761 bytes, md5 d3d4c53c38808f6087c38e3e40f0a2b3
//   manifest_dependency_gdip_x86_64_pe_cl_MD_O2.obj     754 bytes, md5 bd460bfb1f8e75c223ff6e61b9a84201
//   manifest_dependency_both_x86_64_pe_cl_MD_O2.obj     921 bytes, md5 2407b3d5c79c2b6637e671b06f682a7b
//   manifest_dependency_missing_x86_64_pe_cl_MD_O2.obj  757 bytes, md5 75526bef819793a1b0d5d2dbd46ef523
// Every TEST but the `...Native` ones runs in-process on every leg; the native ones run each image where the host is
// Windows x86_64.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/unit_linker_requests.hpp"
#include "link/format/coff_linker_directives.hpp"
#include "link/format/pe_resources.hpp"
#include "link/image_request.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link/unit_linker_decisions.hpp"
#include "format_reject_support.hpp"
#include "object_entry_support.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace dss;
using namespace dss::test_support::object_entry;
using dss::link_format::test::countAtPath;
using dss::link_format::test::countWithMessage;
using dss::link_format::test::rejectSummary;

namespace {

constexpr char const* kPe    = "x86_64:pe64-x86_64-windows-exec";
constexpr char const* kPeDll = "x86_64:pe64-x86_64-windows-dll";

// ── What the references write (✔MEASURED, the header) ──────────────────────

constexpr char const* kCc6 = "type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "
                             "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'";
constexpr char const* kGdip = "type='win32' name='Microsoft.Windows.GdiPlus' version='1.0.0.0' "
                              "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'";
constexpr char const* kMissing = "type='win32' name='Dss.Probe.MissingAssembly' version='1.0.0.0' "
                                 "processorArchitecture='*' publicKeyToken='0000000000000000' language='*'";
// The common controls' identity with one more space, and with its name's case changed: two OTHER identities to both
// references (runs 20261117-142310-025e5c2e, 20261117-142455-10b66d4b).
constexpr char const* kCc6Spaced = "type='win32'  name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "
                                   "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'";
constexpr char const* kCc6Cased = "type='win32' name='microsoft.windows.common-controls' version='6.0.0.0' "
                                  "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'";

// A well-formed block, for the document that must not declare one.
constexpr char const* kManifestBlock =
    R"({"section": ".rsrc", "characteristics": 1073741888, "resourceType": 24, "resourceId": 1, "language": 1033,
        "codePage": 0, "lineEnd": "\r\n", "head": [], "dependency": ["<assemblyIdentity {identity} />"],
        "tail": []})";

// link.exe's manifest, line by line (runs 20261117-140648-927055da, 20261117-142310-025e5c2e).
constexpr char const* kHead[] = {
    "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>",
    "<assembly xmlns='urn:schemas-microsoft-com:asm.v1' manifestVersion='1.0'>",
    "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\">",
    "    <security>",
    "      <requestedPrivileges>",
    "        <requestedExecutionLevel level='asInvoker' uiAccess='false' />",
    "      </requestedPrivileges>",
    "    </security>",
    "  </trustInfo>",
};
constexpr char const* kDependencyOpen[]  = {"  <dependency>", "    <dependentAssembly>"};
constexpr char const* kDependencyClose[] = {"    </dependentAssembly>", "  </dependency>"};
constexpr char const* kTail              = "</assembly>";

// link.exe's text for `identities`, written here from the measurement and never from the document.
[[nodiscard]] std::string referenceText(std::vector<std::string> const& identities) {
    std::string t;
    for (auto const* l : kHead) t += std::string{l} + "\r\n";
    for (auto const& id : identities) {
        for (auto const* l : kDependencyOpen) t += std::string{l} + "\r\n";
        t += "      <assemblyIdentity " + id + " />\r\n";
        for (auto const* l : kDependencyClose) t += std::string{l} + "\r\n";
    }
    return t + kTail + "\r\n";
}

[[nodiscard]] std::uint32_t u32At(std::vector<std::uint8_t> const& b, std::size_t off) {
    return static_cast<std::uint32_t>(b.at(off)) | static_cast<std::uint32_t>(b.at(off + 1)) << 8
         | static_cast<std::uint32_t>(b.at(off + 2)) << 16 | static_cast<std::uint32_t>(b.at(off + 3)) << 24;
}

// link.exe's `.rsrc` section at `rva` for `identities`, laid out as the header states it -- independent of the
// writer's encoder.
[[nodiscard]] std::vector<std::uint8_t> expectedSection(std::vector<std::string> const& identities,
                                                        std::uint32_t resourceId, std::uint32_t rva) {
    std::string const text = referenceText(identities);
    std::vector<std::uint8_t> s(0x60, 0);
    auto const put = [&](std::size_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) s[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    for (std::size_t dir : {0x0u, 0x18u, 0x30u}) s[dir + 14] = 1;   // one id entry each
    put(0x10, 24);                            // RT_MANIFEST
    put(0x14, 0x80000000u | 0x18u);
    put(0x28, resourceId);
    put(0x2C, 0x80000000u | 0x30u);
    put(0x40, 0x409);
    put(0x44, 0x48);                          // the data entry
    put(0x48, rva + 0x60);
    put(0x4C, static_cast<std::uint32_t>(text.size()));
    s.insert(s.end(), text.begin(), text.end());
    s.resize((s.size() + 7u) & ~std::size_t{7}, 0);
    return s;
}

// ── A PE image read back ───────────────────────────────────────────────────

struct Section {
    std::string   name;
    std::uint32_t va = 0, vsize = 0, rawSize = 0, raw = 0, chars = 0;
};

struct Image {
    std::vector<std::uint8_t> b;
    std::vector<Section>      sections;
    std::uint32_t             resourceRva = 0, resourceSize = 0;
};

[[nodiscard]] std::optional<Image> readImage(std::vector<std::uint8_t> bytes) {
    if (bytes.size() < 0x40) return std::nullopt;
    Image img;
    img.b = std::move(bytes);
    std::size_t const pe = u32At(img.b, 0x3C);
    std::size_t const nSecs =
        static_cast<std::size_t>(img.b.at(pe + 6)) | static_cast<std::size_t>(img.b.at(pe + 7)) << 8;
    std::size_t const optSize =
        static_cast<std::size_t>(img.b.at(pe + 20)) | static_cast<std::size_t>(img.b.at(pe + 21)) << 8;
    std::size_t const opt = pe + 24;
    img.resourceRva  = u32At(img.b, opt + 112 + 2 * 8);   // data directory 2 (PE32+: the table at 112)
    img.resourceSize = u32At(img.b, opt + 112 + 2 * 8 + 4);
    for (std::size_t i = 0; i < nSecs; ++i) {
        std::size_t const o = opt + optSize + i * 40u;
        Section s;
        for (std::size_t k = 0; k < 8 && img.b.at(o + k) != 0; ++k) s.name.push_back(static_cast<char>(img.b[o + k]));
        s.vsize   = u32At(img.b, o + 8);
        s.va      = u32At(img.b, o + 12);
        s.rawSize = u32At(img.b, o + 16);
        s.raw     = u32At(img.b, o + 20);
        s.chars   = u32At(img.b, o + 36);
        img.sections.push_back(s);
    }
    return img;
}

[[nodiscard]] std::optional<std::size_t> indexOf(Image const& img, std::string_view name) {
    for (std::size_t i = 0; i < img.sections.size(); ++i) {
        if (img.sections[i].name == name) return i;
    }
    return std::nullopt;
}

// The image holds exactly link.exe's `.rsrc` for `identities` under `resourceId`, after `.idata` and before `.reloc`,
// and its resource data directory states that section.
void expectTheManifest(std::vector<std::uint8_t> bytes, std::vector<std::string> const& identities,
                       std::uint32_t resourceId) {
    auto const img = readImage(std::move(bytes));
    ASSERT_TRUE(img.has_value());
    auto const at = indexOf(*img, ".rsrc");
    ASSERT_TRUE(at.has_value()) << "the image embeds a manifest";
    Section const& s = img->sections[*at];
    EXPECT_EQ(s.chars, 0x40000040u) << "initialized, read-only data, as link.exe writes it";
    EXPECT_EQ(img->resourceRva, s.va) << "the resource directory is the section";
    EXPECT_EQ(img->resourceSize, s.vsize) << "the resource directory states the whole section";
    auto const want = expectedSection(identities, resourceId, s.va);
    EXPECT_EQ(s.vsize, want.size());
    ASSERT_LE(static_cast<std::size_t>(s.raw) + want.size(), img->b.size());
    std::vector<std::uint8_t> const got(img->b.begin() + s.raw, img->b.begin() + s.raw + want.size());
    EXPECT_TRUE(got == want) << "link.exe's tree and text, byte for byte";
    if (auto const idata = indexOf(*img, ".idata")) EXPECT_LT(*idata, *at) << ".rsrc follows the import section";
    if (auto const reloc = indexOf(*img, ".reloc")) EXPECT_EQ(*reloc, *at + 1) << ".reloc follows .rsrc";
}

void expectNoManifest(std::vector<std::uint8_t> bytes) {
    auto const img = readImage(std::move(bytes));
    ASSERT_TRUE(img.has_value());
    EXPECT_FALSE(indexOf(*img, ".rsrc").has_value()) << "no unit states a dependency: no manifest";
    EXPECT_EQ(img->resourceRva, 0u);
    EXPECT_EQ(img->resourceSize, 0u);
}

[[nodiscard]] std::string fixtureName(char const* which) {
    return std::string{"manifest_dependency_"} + which + "_x86_64_pe_cl_MD_O2.obj";
}

constexpr char const* kProgram = "int main(void) { return 42; }\n";

// One program beside fixture objects, in that order of units: the program's unit first, as the references were
// linked (runs 20261117-142310-025e5c2e, 20261117-143614-c0ec388c).
struct Cell {
    char const*              label;
    std::vector<char const*> objects;
    std::vector<std::string> identities;   // what link.exe's manifest names, in order
};

[[nodiscard]] std::vector<Cell> cells() {
    return {
        {"one object, one dependency", {"cc6"}, {kCc6}},
        {"one object stating two: their order", {"both"}, {kCc6, kGdip}},
        {"two objects: the units' order", {"cc6", "gdip"}, {kCc6, kGdip}},
        {"two objects, the other way", {"gdip", "cc6"}, {kGdip, kCc6}},
        {"a repeat across units is one, at its first place", {"cc6", "both"}, {kCc6, kGdip}},
        {"a repeat across units is one, the other way", {"gdip", "both"}, {kGdip, kCc6}},
    };
}

[[nodiscard]] std::vector<Input> inputsOf(Cell const& c) {
    std::vector<Input> in{written("program.c", kProgram)};
    for (auto const* o : c.objects) in.push_back(fixture(fixtureName(o)));
    return in;
}

// DSS's driver builds `inputs` into a DLL: the one `.dll` it writes, or an empty path.
[[nodiscard]] fs::path buildDll(fs::path const& dir, std::vector<Input> const& inputs, Built& b) {
    build(dir, inputs, kPeDll, b);
    for (auto const& e : fs::directory_iterator(dir / "dss.out")) {
        if (e.path().extension() == ".dll") return e.path();
    }
    return {};
}

}  // namespace

// ── THE SHIPPED DECLARATION ─────────────────────────────────────────────────

// Both relocatable documents read `/MANIFESTDEPENDENCY:` as a dependency; both image documents embed link.exe's
// manifest, a program's as resource 1 and a DLL's as 2; no relocatable document declares one.
TEST(ManifestDependency, TheShippedDocumentsDeclareTheReferencesManifest) {
    for (auto const* stem : {"pe64-x86_64-windows", "pe64-x86_64-windows-staticlib"}) {
        SCOPED_TRACE(stem);
        auto f = ObjectFormatSchema::loadShipped(stem);
        ASSERT_TRUE(f.has_value());
        ASSERT_TRUE((*f)->pe().linkerDirectives.has_value());
        bool found = false;
        for (auto const& row : (*f)->pe().linkerDirectives->directives) {
            if (row.option != "manifestdependency") continue;
            found = true;
            EXPECT_EQ(row.meaning, LinkerDirectiveMeaning::ManifestDependency);
            EXPECT_TRUE(row.reason.empty()) << "a meaning the link applies needs no excuse";
        }
        EXPECT_TRUE(found);
        EXPECT_FALSE((*f)->pe().embeddedManifest.has_value()) << "an object embeds no resource";
    }
    for (auto const& [stem, id] :
         {std::pair{"pe64-x86_64-windows-exec", 1u}, std::pair{"pe64-x86_64-windows-dll", 2u}}) {
        SCOPED_TRACE(stem);
        auto f = ObjectFormatSchema::loadShipped(stem);
        ASSERT_TRUE(f.has_value());
        auto const& m = (*f)->pe().embeddedManifest;
        ASSERT_TRUE(m.has_value());
        EXPECT_EQ(m->section, ".rsrc");
        EXPECT_EQ(m->characteristics, 0x40000040u);
        EXPECT_EQ(m->resourceType, 24u);
        EXPECT_EQ(m->resourceId, id);
        EXPECT_EQ(m->language, 0x409u);
        EXPECT_EQ(m->codePage, 0u);
        for (auto const& ids : {std::vector<std::string>{}, std::vector<std::string>{kCc6},
                                std::vector<std::string>{kCc6, kGdip}}) {
            EXPECT_EQ(pe::embeddedManifestText(*m, ids), referenceText(ids)) << "link.exe's text, byte for byte";
        }
        EXPECT_EQ(pe::embeddedManifestText(*m, std::vector<std::string>{kCc6}).size(), 638u);
        EXPECT_EQ(pe::embeddedManifestText(*m, std::vector<std::string>{kCc6, kGdip}).size(), 887u);
    }
}

// The loader refuses a block a writer cannot read one way: on a relocatable document, without a key, with the
// identity's place missing, twice, or outside the dependency, with an id of 0 or past 0xFFFF, a section name past 8
// bytes, or a key it does not know.
TEST(ManifestDependency, TheLoaderRefusesEveryMalformedOrMisplacedManifest) {
    auto const root = dss::test::findConfigRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::configRootDiagnostic();
    auto const documentText = [&](char const* stem) {
        std::ifstream in{*root / "object-formats" / (std::string{stem} + ".format.json"), std::ios::binary};
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    struct Case {
        char const* label;
        char const* stem;
        void (*mutate)(nlohmann::json&);
        char const* path;
        char const* words;
    };
    Case const kCases[] = {
        {"declared on a relocatable document", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["embeddedManifest"] = nlohmann::json::parse(kManifestBlock); },
         "/pe/embeddedManifest", "declared on a relocatable PE document"},
        {"a missing key", "pe64-x86_64-windows-exec",
         [](nlohmann::json& d) { d["pe"]["embeddedManifest"].erase("lineEnd"); },
         "/pe/embeddedManifest/lineEnd", "must be a non-empty string"},
        {"no place for the identity", "pe64-x86_64-windows-exec",
         [](nlohmann::json& d) { d["pe"]["embeddedManifest"]["dependency"][2] = "      <assemblyIdentity />"; },
         "/pe/embeddedManifest/dependency", "must appear exactly once"},
        {"the identity's place twice", "pe64-x86_64-windows-exec",
         [](nlohmann::json& d) { d["pe"]["embeddedManifest"]["dependency"][0] = "  <dependency {identity}>"; },
         "/pe/embeddedManifest/dependency", "must appear exactly once"},
        {"the identity's place in the head", "pe64-x86_64-windows-exec",
         [](nlohmann::json& d) { d["pe"]["embeddedManifest"]["head"][0] = "<!-- {identity} -->"; },
         "/pe/embeddedManifest/dependency", "must appear exactly once"},
        {"a resource id of 0", "pe64-x86_64-windows-exec",
         [](nlohmann::json& d) { d["pe"]["embeddedManifest"]["resourceId"] = 0; },
         "/pe/embeddedManifest/resourceId", "must be an integer in [1, 65535]"},
        {"a language past 0xFFFF", "pe64-x86_64-windows-dll",
         [](nlohmann::json& d) { d["pe"]["embeddedManifest"]["language"] = 65536; },
         "/pe/embeddedManifest/language", "must be an integer in [0, 65535]"},
        {"a section name past 8 bytes", "pe64-x86_64-windows-exec",
         [](nlohmann::json& d) { d["pe"]["embeddedManifest"]["section"] = ".resources"; },
         "/pe/embeddedManifest/section", "exceeds the 8 bytes"},
        {"an empty dependency", "pe64-x86_64-windows-exec",
         [](nlohmann::json& d) { d["pe"]["embeddedManifest"]["dependency"] = nlohmann::json::array(); },
         "/pe/embeddedManifest/dependency", "must be a non-empty array of strings"},
        {"an unknown key", "pe64-x86_64-windows-exec",
         [](nlohmann::json& d) { d["pe"]["embeddedManifest"]["resourceName"] = "MANIFEST"; },
         "/pe/embeddedManifest/resourceName", ""},
    };
    for (auto const& c : kCases) {
        SCOPED_TRACE(c.label);
        auto const text = documentText(c.stem);
        ASSERT_FALSE(text.empty());
        nlohmann::json doc = nlohmann::json::parse(text);
        c.mutate(doc);
        auto const r = ObjectFormatSchema::loadFromText(doc.dump(), c.label);
        ASSERT_FALSE(r.has_value()) << "the document loads";
        EXPECT_GE(countAtPath(r, c.path), 1u) << rejectSummary(r);
        if (*c.words != '\0') EXPECT_GE(countWithMessage(r, c.words), 1u) << rejectSummary(r);
    }
}

// ── THE DIRECTIVE, READ AND DECIDED ─────────────────────────────────────────

// The parse reads the identity verbatim, quoted whole or in its value; one that names nothing is refused by name.
// The decision keeps each distinct identity once, at its first statement's place in unit order, and compares as text.
TEST(ManifestDependency, TheDependenciesAreALinkWideSetOfTexts) {
    auto vocabulary = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(vocabulary.has_value());
    auto const& v = *(*vocabulary)->pe().linkerDirectives;
    auto const read = [&](std::string const& text) {
        auto const r = pe::parseCoffLinkerDirectives(text, v);
        EXPECT_TRUE(r.has_value()) << (r.has_value() ? std::string{} : r.error());
        UnitLinkerRequests out;
        if (r.has_value()) {
            out.image  = r->image;
            out.handOn = r->handOn;
        }
        return out;
    };
    auto const stated = [](char const* identity) { return " /manifestdependency:\"" + std::string{identity} + "\""; };
    std::string const cc6 = stated(kCc6);
    std::string const gdip = " \"/MANIFESTDEPENDENCY:" + std::string{kGdip} + "\"";   // the whole directive quoted
    auto const one = read(cc6);
    ASSERT_EQ(one.image.size(), 1u);
    auto const* dep = std::get_if<UnitManifestDependencyRequest>(&one.image[0].value);
    ASSERT_NE(dep, nullptr);
    EXPECT_EQ(dep->identity, kCc6) << "the identity, verbatim, its quotes unwrapped";
    EXPECT_EQ(one.handOn.size(), 1u) << "a relocatable artifact hands it on";
    auto const empty = pe::parseCoffLinkerDirectives(" /manifestdependency:", v);
    ASSERT_FALSE(empty.has_value());
    EXPECT_NE(empty.error().find("must read '/manifestdependency:<identity>'"), std::string::npos) << empty.error();

    auto exec = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    auto dll = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-dll");
    ASSERT_TRUE(exec.has_value() && dll.has_value());
    struct Decided {
        char const*              label;
        std::vector<std::string> units;   // each unit's directive text, in unit order
        std::vector<std::string> want;
    };
    for (Decided const& d : {Decided{"unit order", {cc6, gdip}, {kCc6, kGdip}},
                             Decided{"the other order", {gdip, cc6}, {kGdip, kCc6}},
                             Decided{"a repeat across units", {cc6, cc6 + gdip, cc6}, {kCc6, kGdip}},
                             Decided{"a repeat within one", {cc6 + cc6}, {kCc6}},
                             Decided{"one more space is another", {cc6, stated(kCc6Spaced)}, {kCc6, kCc6Spaced}},
                             Decided{"another case is another", {cc6, stated(kCc6Cased)}, {kCc6, kCc6Cased}}}) {
        SCOPED_TRACE(d.label);
        UnitLinkerRequests all;
        for (auto const& u : d.units) appendUnitLinkerRequests(all, read(u));
        for (auto const* format : {exec->get(), dll->get()}) {
            DiagnosticReporter rep;
            auto const decided = linker::decideUnitLinkerRequests(all, *format, ImageRequest{}, rep);
            ASSERT_TRUE(decided.has_value()) << format->name();
            EXPECT_EQ(decided->image.manifestDependencies, d.want) << format->name();
            EXPECT_TRUE(rep.all().empty()) << "nothing is dropped, so nothing is said";
        }
    }
}

// ── THE IMAGE ───────────────────────────────────────────────────────────────

// A program beside cl's objects embeds link.exe's manifest of their dependencies, in their order, a repeat once; a
// program beside none embeds none.
TEST(ManifestDependency, AnImageEmbedsTheManifestItsUnitsDependOn) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "manifest_dependency"};
    int k = 0;
    for (auto const& c : cells()) {
        SCOPED_TRACE(c.label);
        Built b;
        build(scratch.path() / std::to_string(k++), inputsOf(c), kPe, b);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        ASSERT_FALSE(b.image.empty());
        expectTheManifest(bytesOf(b.image), c.identities, 1u);
    }
    {
        SCOPED_TRACE("no unit states a dependency");
        Built b;
        build(scratch.path() / "none", {written("program.c", kProgram)}, kPe, b);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        expectNoManifest(bytesOf(b.image));
    }
    {
        SCOPED_TRACE("an assembly no Windows installs is embedded all the same: the loader judges it");
        Built b;
        build(scratch.path() / "missing", {written("program.c", kProgram), fixture(fixtureName("missing"))}, kPe, b);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        expectTheManifest(bytesOf(b.image), {kMissing}, 1u);
    }
}

// A DLL's manifest is resource 2, as link.exe and lld-link write a DLL's.
TEST(ManifestDependency, ADllEmbedsItsManifestAsResourceTwo) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "manifest_dependency_dll"};
    Built b;
    auto const dll = buildDll(scratch.path(),
                              {written("library.c", "int dss_library_value(void) { return 42; }\n"),
                               fixture(fixtureName("cc6"))},
                              b);
    ASSERT_EQ(b.rc, 0) << b.diagnostics;
    ASSERT_FALSE(dll.empty()) << "the build writes a DLL";
    expectTheManifest(bytesOf(dll), {kCc6}, 2u);
}

// A DSS unit states the dependency as a cl object does: its `#pragma comment(linker, "/manifestdependency:...")`
// reaches the image link through the COFF directives' one home that reads a cl object's `.drectve`
// (D-C-PRAGMA-COMMENT-REFUSED-WHERE-CL-AND-CLANG-HONOUR-IT), so the image embeds link.exe's manifest of it -- alone,
// beside a cl object stating another (the units' order), and beside one stating it again (once, at its first place).
TEST(ManifestDependency, ADssUnitsPragmaStatesTheDependencyAsAClObjectDoes) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "manifest_dependency_pragma"};
    std::string const program =
        "#pragma comment(linker, \"/manifestdependency:\\\"" + std::string{kCc6} + "\\\"\")\n" + kProgram;
    struct PragmaCell {
        char const*              label;
        std::vector<char const*> objects;
        std::vector<std::string> identities;   // what link.exe's manifest names, in order
    };
    std::vector<PragmaCell> const pragmaCells{
        {"the DSS unit alone", {}, {kCc6}},
        {"beside a cl object stating another: the units' order", {"gdip"}, {kCc6, kGdip}},
        {"beside a cl object stating it again: once, at its first place", {"both"}, {kCc6, kGdip}},
    };
    int k = 0;
    for (PragmaCell const& c : pragmaCells) {
        SCOPED_TRACE(c.label);
        std::vector<Input> in{written("program.c", program)};
        for (auto const* o : c.objects) in.push_back(fixture(fixtureName(o)));
        Built b;
        build(scratch.path() / std::to_string(k++), in, kPe, b);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        ASSERT_FALSE(b.image.empty());
        expectTheManifest(bytesOf(b.image), c.identities, 1u);
    }
}

// An image format that declares no manifest refuses the dependency by name, naming the assembly and the key, rather
// than writing an image that would not activate it.
TEST(ManifestDependency, AFormatDeclaringNoManifestRefusesTheDependency) {
    auto const root = dss::test::findConfigRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::configRootDiagnostic();
    std::ifstream in{*root / "object-formats" / "pe64-x86_64-windows-exec.format.json", std::ios::binary};
    nlohmann::json doc =
        nlohmann::json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
    doc["pe"].erase("embeddedManifest");
    auto const format = ObjectFormatSchema::loadFromText(doc.dump(), "pe64 exec declaring no manifest");
    ASSERT_TRUE(format.has_value()) << rejectSummary(format);
    auto const target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto vocabulary = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(vocabulary.has_value());
    auto const parsed = pe::parseCoffLinkerDirectives(" /manifestdependency:\"" + std::string{kCc6} + "\"",
                                                      *(*vocabulary)->pe().linkerDirectives);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    AssembledModule m;
    m.cuId              = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};   // mov eax, 42 ; ret
    m.functions.push_back(fn);
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global, SymbolVisibility::Default});
    m.userEntrySymbol         = SymbolId{1};
    m.linkerRequests.image    = parsed->image;
    m.linkerRequests.handOn   = parsed->handOn;
    std::vector<AssembledModule> const units{m};
    ImageRequest request;
    request.artifactFileName = "manifest_dependency.exe";
    DiagnosticReporter rep;
    auto const image = linker::link(std::span<AssembledModule const>{units}, **target, **format, rep, request);
    EXPECT_TRUE(image.bytes.empty()) << "no image is written";
    EXPECT_NE(textOf(rep, DiagnosticCode::K_LinkerDirectiveUnhonourable).find(kCc6), std::string::npos)
        << diagnosticsOf(rep);
    EXPECT_NE(textOf(rep, DiagnosticCode::K_LinkerDirectiveUnhonourable).find("declares no 'pe.embeddedManifest'"),
              std::string::npos)
        << diagnosticsOf(rep);
}

// ── THE PROGRAMS, RUN (Windows x86_64 hosts) ────────────────────────────────

namespace {

[[nodiscard]] bool onWindowsX64() {
    return test_support::currentHostOs() == "windows" && test_support::currentHostArch() == "x86_64";
}

}  // namespace

// Each image runs to 42, and the Windows resource loader reads its manifest back: resource 1 of type RT_MANIFEST,
// language 0x409, holding link.exe's text.
TEST(ManifestDependencyNative, EachImageRunsAndTheLoaderReadsItsManifest) {
    if (!onWindowsX64()) GTEST_SKIP() << "the images run on Windows x86_64 hosts only";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "manifest_dependency_native"};
    int k = 0;
    for (auto const& c : cells()) {
        SCOPED_TRACE(c.label);
        Built b;
        build(scratch.path() / std::to_string(k++), inputsOf(c), kPe, b);
        ASSERT_EQ(b.rc, 0) << b.diagnostics;
        auto const r = test_support::runBinary(b.image, test_support::kRunBudget, /*captureStdout=*/false);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_FALSE(r.timedOut);
        EXPECT_EQ(r.exitCode, 42u) << "the program runs with its dependencies activated";
#if defined(_WIN32)
        HMODULE const h = ::LoadLibraryExW(b.image.wstring().c_str(), nullptr,
                                           LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        ASSERT_NE(h, nullptr) << "the image loads as a resource file";
        HRSRC const found = ::FindResourceExW(h, MAKEINTRESOURCEW(24), MAKEINTRESOURCEW(1), 0x409);
        ASSERT_NE(found, nullptr) << "RT_MANIFEST 1, language 0x409";
        DWORD const size = ::SizeofResource(h, found);
        HGLOBAL const loaded = ::LoadResource(h, found);
        ASSERT_NE(loaded, nullptr);
        auto const* data = static_cast<char const*>(::LockResource(loaded));
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(std::string(data, size), referenceText(c.identities)) << "the loader reads link.exe's text";
        ::FreeLibrary(h);
#endif
    }
}

// The loader ACTIVATES the dependency: an image depending on an assembly no Windows installs is not started (the
// side-by-side configuration is incorrect, 14001), as link.exe's image of the same objects is not.
TEST(ManifestDependencyNative, AnImageDependingOnAMissingAssemblyIsNotStarted) {
    if (!onWindowsX64()) GTEST_SKIP() << "the images run on Windows x86_64 hosts only";
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "manifest_dependency_missing"};
    Built b;
    build(scratch.path(), {written("program.c", kProgram), fixture(fixtureName("missing"))}, kPe, b);
    ASSERT_EQ(b.rc, 0) << b.diagnostics;
    expectTheManifest(bytesOf(b.image), {kMissing}, 1u);
    auto const r = test_support::runBinary(b.image, test_support::kRunBudget, /*captureStdout=*/false);
    EXPECT_FALSE(r.spawned) << "the loader starts an image whose dependency it cannot activate";
    EXPECT_NE(r.diagnostic.find("GetLastError=14001"), std::string::npos) << r.diagnostic;
}
