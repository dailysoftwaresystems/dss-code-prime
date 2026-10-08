// P69 round 4 (D-LK-COFF-READER-SKIPPED-EVERY-LINKER-DIRECTIVE; review-xa3 MAJOR 1, MINOR 3, MINOR 4) — what a COFF
// object's linker directives ask of the IMAGE, from the parse to the header.
//
// ✔MEASURED 2026-10-07 (link.exe 14.44.35228 and lld-link 19.1.5 on cl 19.44 /Od /MD /GS- objects, probe logs under
// `.orchestrators/p69/work/xa/r4probe`): both linkers HONOUR a directive's `/STACK:`, `/SUBSYSTEM:`, `/ENTRY:`,
// `/SECTION:` and `/EXPORT:` in an EXE, which a round-3 DSS link dropped in silence; link.exe also takes `/HEAP:`,
// `/VERSION:`, `/BASE:`, `/ALIGN:` and `/DLL`, and lld-link `/RELEASE`. Each is pinned here at the tier it lives in:
//   * THE PARSE (`pe::parseCoffLinkerDirectives`): every image request reads as link.exe reads it, `/SECTION:`'s
//     letters by link.exe's own rule; what link.exe warns about and replaces (a subsystem version below its minimum,
//     an alignment that is not a power of two) is warned and replaced; what a DSS image cannot carry (`/MERGE:`, a
//     wide or GUI runtime startup, a NATIVE subsystem, an ordinal export) is a request the IMAGE link refuses.
//   * THE DECISION (`linker::link` -> `decideUnitLinkerRequests`): the precedence link.exe gives each option across
//     units — the first `/STACK:`, `/HEAP:`, `/BASE:`, `/ENTRY:`; the last `/SUBSYSTEM:`, `/VERSION:`, `/ALIGN:`;
//     `/SECTION:` in order; the program's own `--stack-reserve` over all of them.
//   * THE WRITER (`pe::encodeExec`): the optional header, the section headers, the export table (an EXE's own, a
//     DLL's additions), the checksum, the entry; and a relocatable artifact hands every request on verbatim.
//   * THE READER: an object's own directives reach the link (a round trip through DSS's writer), and the TLS-callback
//     idiom's `/INCLUDE:_tls_used` is refused naming the table it serves (MINOR 4).
// The runtime witnesses are the corpus examples `examples/c/pe_foreign_directive_*`, which link cl's own objects.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/unit_linker_requests.hpp"
#include "link/format/coff_linker_directives.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/pe.hpp"
#include "link/image_request.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link/unit_linker_decisions.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> format;
};

[[nodiscard]] Loaded load(char const* stem) {
    Loaded l;
    auto t = TargetSchema::loadShipped("x86_64");
    auto f = ObjectFormatSchema::loadShipped(stem);
    EXPECT_TRUE(t.has_value() && f.has_value()) << stem;
    if (t.has_value()) l.target = *t;
    if (f.has_value()) l.format = *f;
    return l;
}

[[nodiscard]] PeLinkerDirectives const& vocabulary() {
    static Loaded const l = load("pe64-x86_64-windows");
    static PeLinkerDirectives const empty;
    if (!l.format || !l.format->pe().linkerDirectives.has_value()) {
        ADD_FAILURE() << "pe64-x86_64-windows declares pe.linkerDirectives";
        return empty;
    }
    return *l.format->pe().linkerDirectives;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s;
}

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& rep, DiagnosticCode code) {
    auto const all = rep.all();
    return static_cast<std::size_t>(
        std::count_if(all.begin(), all.end(), [&](ParseDiagnostic const& d) { return d.code == code; }));
}

[[nodiscard]] bool sawText(DiagnosticReporter const& rep, std::string_view text) {
    auto const all = rep.all();
    return std::any_of(all.begin(), all.end(),
                       [&](ParseDiagnostic const& d) { return d.actual.find(text) != std::string::npos; });
}

// ── A PE image read back off its bytes ─────────────────────────────────────
struct PeImage {
    std::vector<std::uint8_t> b;
    std::size_t               opt   = 0;   // the optional header
    std::size_t               secs  = 0;   // the first section header
    std::uint16_t             nSecs = 0;

    [[nodiscard]] std::uint64_t rd(std::size_t off, int width) const {
        std::uint64_t v = 0;
        for (int i = 0; i < width; ++i) v |= static_cast<std::uint64_t>(b.at(off + i)) << (i * 8);
        return v;
    }
    // PE32+ optional-header fields, by their offset in the header (PE/COFF §3.4).
    [[nodiscard]] std::uint32_t entry() const { return static_cast<std::uint32_t>(rd(opt + 16, 4)); }
    [[nodiscard]] std::uint64_t imageBase() const { return rd(opt + 24, 8); }
    [[nodiscard]] std::uint32_t sectionAlignment() const { return static_cast<std::uint32_t>(rd(opt + 32, 4)); }
    [[nodiscard]] std::uint16_t u16(std::size_t field) const { return static_cast<std::uint16_t>(rd(opt + field, 2)); }
    [[nodiscard]] std::uint32_t checksum() const { return static_cast<std::uint32_t>(rd(opt + 64, 4)); }
    [[nodiscard]] std::uint16_t subsystem() const { return u16(68); }
    [[nodiscard]] std::uint64_t stackReserve() const { return rd(opt + 72, 8); }
    [[nodiscard]] std::uint64_t stackCommit() const { return rd(opt + 80, 8); }
    [[nodiscard]] std::uint64_t heapReserve() const { return rd(opt + 88, 8); }
    [[nodiscard]] std::uint64_t heapCommit() const { return rd(opt + 96, 8); }

    struct Section {
        std::string   name;
        std::uint32_t va = 0, vsize = 0, rawSize = 0, raw = 0, chars = 0;
    };
    [[nodiscard]] std::vector<Section> sections() const {
        std::vector<Section> out;
        for (std::uint16_t i = 0; i < nSecs; ++i) {
            std::size_t const o = secs + i * 40u;
            Section s;
            for (std::size_t k = 0; k < 8 && b.at(o + k) != 0; ++k) s.name.push_back(static_cast<char>(b[o + k]));
            s.vsize   = static_cast<std::uint32_t>(rd(o + 8, 4));
            s.va      = static_cast<std::uint32_t>(rd(o + 12, 4));
            s.rawSize = static_cast<std::uint32_t>(rd(o + 16, 4));
            s.raw     = static_cast<std::uint32_t>(rd(o + 20, 4));
            s.chars   = static_cast<std::uint32_t>(rd(o + 36, 4));
            out.push_back(s);
        }
        return out;
    }
    [[nodiscard]] std::optional<Section> section(std::string_view name) const {
        for (auto const& s : sections()) {
            if (s.name == name) return s;
        }
        return std::nullopt;
    }
    // The file offset of an RVA inside a section's raw data.
    [[nodiscard]] std::optional<std::size_t> fileOffset(std::uint32_t rva) const {
        for (auto const& s : sections()) {
            if (rva >= s.va && rva < s.va + std::max(s.vsize, s.rawSize) && rva - s.va < s.rawSize) {
                return s.raw + (rva - s.va);
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] std::vector<std::uint8_t> bytesAt(std::uint32_t rva, std::size_t n) const {
        auto const off = fileOffset(rva);
        if (!off.has_value() || *off + n > b.size()) return {};
        return {b.begin() + static_cast<std::ptrdiff_t>(*off), b.begin() + static_cast<std::ptrdiff_t>(*off + n)};
    }
    // The export table, name -> RVA (data directory 0).
    [[nodiscard]] std::map<std::string, std::uint32_t> exports() const {
        std::map<std::string, std::uint32_t> out;
        auto const dirRva = static_cast<std::uint32_t>(rd(opt + 112, 4));
        if (dirRva == 0) return out;
        auto const dir = fileOffset(dirRva);
        if (!dir.has_value()) return out;
        auto const nNames = static_cast<std::uint32_t>(rd(*dir + 24, 4));
        auto const eat    = fileOffset(static_cast<std::uint32_t>(rd(*dir + 28, 4)));
        auto const names  = fileOffset(static_cast<std::uint32_t>(rd(*dir + 32, 4)));
        auto const ords   = fileOffset(static_cast<std::uint32_t>(rd(*dir + 36, 4)));
        if (!eat || !names || !ords) return out;
        for (std::uint32_t i = 0; i < nNames; ++i) {
            auto const nameAt = fileOffset(static_cast<std::uint32_t>(rd(*names + 4u * i, 4)));
            if (!nameAt) continue;
            std::string name;
            for (std::size_t p = *nameAt; p < b.size() && b[p] != 0; ++p) name.push_back(static_cast<char>(b[p]));
            auto const ord = static_cast<std::uint16_t>(rd(*ords + 2u * i, 2));
            out[name] = static_cast<std::uint32_t>(rd(*eat + 4u * ord, 4));
        }
        return out;
    }
};

[[nodiscard]] std::optional<PeImage> peImage(std::vector<std::uint8_t> bytes) {
    if (bytes.size() < 0x40) return std::nullopt;
    PeImage img;
    img.b = std::move(bytes);
    auto const peOff = static_cast<std::size_t>(img.rd(0x3C, 4));
    if (peOff + 24 > img.b.size()) return std::nullopt;
    img.nSecs = static_cast<std::uint16_t>(img.rd(peOff + 6, 2));
    auto const optSize = static_cast<std::size_t>(img.rd(peOff + 20, 2));
    img.opt  = peOff + 24;
    img.secs = img.opt + optSize;
    return img;
}

// The PE image checksum, written here independently of the writer's (PE/COFF §3.4.2: the file's 16-bit words with
// the carry folded, the CheckSum field read as zero, plus the length).
[[nodiscard]] std::uint32_t independentChecksum(PeImage const& img) {
    std::size_t const field = img.opt + 64;
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < img.b.size(); i += 2) {
        std::uint32_t w = img.b[i] | (i + 1 < img.b.size() ? img.b[i + 1] << 8 : 0);
        if (i == field || i == field + 2) w = 0;
        sum += w;
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    sum = (sum & 0xFFFF) + (sum >> 16);
    return sum + static_cast<std::uint32_t>(img.b.size());
}

// ── Units ───────────────────────────────────────────────────────────────────
constexpr std::uint8_t kMainBody[]  = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};   // mov eax, 42 ; ret
constexpr std::uint8_t kEntryBody[] = {0xB8, 0x0D, 0x00, 0x00, 0x00, 0xC3};   // mov eax, 13 ; ret

[[nodiscard]] AssembledFunction function(std::uint32_t id, std::vector<std::uint8_t> body) {
    AssembledFunction fn;
    fn.symbol = SymbolId{id};
    fn.bytes  = std::move(body);
    return fn;
}

// The program's own unit: `main` (returns 42), and `dss_main_defined` (returns 5).
[[nodiscard]] AssembledModule programUnit() {
    AssembledModule m;
    m.cuId              = CompilationUnitId{1};
    m.expectedFuncCount = 2;
    m.functions.push_back(function(1, {std::begin(kMainBody), std::end(kMainBody)}));
    m.functions.push_back(function(2, {0xB8, 0x05, 0x00, 0x00, 0x00, 0xC3}));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global, SymbolVisibility::Default});
    m.symbols.push_back(
        ModuleSymbol{SymbolId{2}, "dss_main_defined", SymbolBinding::Global, SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    return m;
}

// A "foreign" unit stating `requests`: `dss_internal` (returns 2), `dss_entry` (returns 13), `dss_hidden_fn` (Hidden,
// returns 3), and the datum `dss_datum` (7) in `.data`.
[[nodiscard]] AssembledModule requestingUnit(std::uint32_t cu, UnitLinkerRequests requests) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{cu};
    m.expectedFuncCount = 3;
    m.functions.push_back(function(1, {0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3}));
    m.functions.push_back(function(2, {std::begin(kEntryBody), std::end(kEntryBody)}));
    m.functions.push_back(function(3, {0xB8, 0x03, 0x00, 0x00, 0x00, 0xC3}));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_internal", SymbolBinding::Global, SymbolVisibility::Default});
    m.symbols.push_back(ModuleSymbol{SymbolId{2}, "dss_entry", SymbolBinding::Global, SymbolVisibility::Default});
    m.symbols.push_back(ModuleSymbol{SymbolId{3}, "dss_hidden_fn", SymbolBinding::Global, SymbolVisibility::Hidden});
    AssembledData d;
    d.symbol    = SymbolId{4};
    d.section   = DataSectionKind::Data;
    d.bytes     = {7, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    m.dataItems.push_back(d);
    m.symbols.push_back(ModuleSymbol{SymbolId{4}, "dss_datum", SymbolBinding::Global, SymbolVisibility::Default});
    m.linkerRequests = std::move(requests);
    return m;
}

// Another unit stating `requests`, beside the requesting one: one function of its own name (`dss_unit<cu>`, returning
// `cu`), so units never define one name twice.
[[nodiscard]] AssembledModule extraUnit(std::uint32_t cu, UnitLinkerRequests requests) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{cu};
    m.expectedFuncCount = 1;
    m.functions.push_back(function(1, {0xB8, static_cast<std::uint8_t>(cu), 0x00, 0x00, 0x00, 0xC3}));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_unit" + std::to_string(cu), SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.linkerRequests = std::move(requests);
    return m;
}

// The requests a directive text states, as the reader hands them to its unit.
[[nodiscard]] UnitLinkerRequests requestsOf(std::string_view text) {
    auto const parsed = pe::parseCoffLinkerDirectives(text, vocabulary());
    EXPECT_TRUE(parsed.has_value()) << (parsed.has_value() ? std::string{} : parsed.error());
    UnitLinkerRequests r;
    if (!parsed.has_value()) return r;
    r.image    = parsed->image;
    r.exports  = parsed->exports;
    r.handOn   = parsed->handOn;
    r.warnings = parsed->warnings;
    return r;
}

struct Linked {
    LinkedImage        image;
    DiagnosticReporter rep;
    std::optional<PeImage> pe;
};

[[nodiscard]] std::unique_ptr<Linked> linkWith(char const* stem, std::vector<AssembledModule> const& units,
                                               ImageRequest const& request = {}) {
    auto out = std::make_unique<Linked>();
    auto const l = load(stem);
    if (!l.target || !l.format) return out;
    out->image = linker::link(std::span<AssembledModule const>{units}, *l.target, *l.format, out->rep, request);
    if (!out->image.bytes.empty()) out->pe = peImage(out->image.bytes);
    return out;
}

[[nodiscard]] std::unique_ptr<Linked> linkExe(std::vector<AssembledModule> const& units,
                                              ImageRequest const& request = {}) {
    return linkWith("pe64-x86_64-windows-exec", units, request);
}

[[nodiscard]] std::unique_ptr<Linked> linkDll(std::vector<AssembledModule> const& units) {
    return linkWith("pe64-x86_64-windows-dll", units);
}

}  // namespace

// ══ The parse ═══════════════════════════════════════════════════════════════

TEST(CoffDirectiveImageRequests, EachImageRequestReadsAsLinkExeReadsIt) {
    auto const r = pe::parseCoffLinkerDirectives(
        " /STACK:0x10000000,0x20000 /stack:010000000 /HEAP:0x200000 /SUBSYSTEM:WINDOWS,6.02 /subsystem:console"
        " /ENTRY:mainCRTStartup /ENTRY:dss_entry /SECTION:.data,RWS /RELEASE /VERSION:3.5 /BASE:0"
        " /ALIGN:0x2000 /FAILIFMISMATCH:dss_key=one /DLL",
        vocabulary());
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->image.size(), 14u);
    auto const& i = r->image;
    auto const* stack = std::get_if<UnitStackRequest>(&i[0].value);
    ASSERT_NE(stack, nullptr);
    EXPECT_EQ(stack->sizes, (ReserveCommit{0x10000000u, 0x20000u}));
    EXPECT_EQ(i[0].spelled, "/STACK:0x10000000,0x20000");
    ASSERT_NE(std::get_if<UnitStackRequest>(&i[1].value), nullptr);
    EXPECT_EQ(std::get<UnitStackRequest>(i[1].value).sizes, (ReserveCommit{0x200000u, std::nullopt}))
        << "a leading 0 is octal, as link.exe reads it (010000000 = 0x200000, ✔MEASURED 2026-10-07)";
    EXPECT_EQ(std::get<UnitHeapRequest>(i[2].value).sizes, (ReserveCommit{0x200000u, std::nullopt}));
    EXPECT_EQ(std::get<UnitSubsystemRequest>(i[3].value).setting, (SubsystemSetting{2, VersionPair{6, 2}}));
    EXPECT_EQ(std::get<UnitSubsystemRequest>(i[4].value).setting, (SubsystemSetting{3, std::nullopt}))
        << "names and options compare without regard to case";
    EXPECT_TRUE(std::get<UnitEntryRequest>(i[5].value).runtimeStartup);
    EXPECT_FALSE(std::get<UnitEntryRequest>(i[6].value).runtimeStartup);
    EXPECT_EQ(std::get<UnitEntryRequest>(i[6].value).symbol, "dss_entry");
    auto const& section = std::get<UnitSectionAttributeRequest>(i[7].value).request;
    EXPECT_EQ(section.section, ".data");
    EXPECT_EQ(section.setMask, 0xD0000000u) << "R | W | S";
    ASSERT_NE(std::get_if<UnitChecksumRequest>(&i[8].value), nullptr);
    EXPECT_EQ(std::get<UnitImageVersionRequest>(i[9].value).version, (VersionPair{3, 5}));
    EXPECT_EQ(std::get<UnitImageBaseRequest>(i[10].value).address, 0u)
        << "link.exe takes /BASE:0 (LNK4281 is an ASLR warning, the image runs, ✔MEASURED 2026-10-07)";
    EXPECT_EQ(std::get<UnitSectionAlignmentRequest>(i[11].value).bytes, 0x2000u);
    EXPECT_EQ(std::get<UnitMismatchCheck>(i[12].value).key, "dss_key");
    ASSERT_NE(std::get_if<UnitDllImageRequest>(&i[13].value), nullptr);
    EXPECT_TRUE(r->warnings.empty());
    EXPECT_EQ(r->handOn.size(), 14u) << "every image request is handed on by a relocatable artifact";
}

// What link.exe warns about and replaces with its default is warned and replaced: a subsystem version below the
// minimum it takes for x64 (5.01 -> LNK4010, the default 6.00 kept; 5.02 taken), an alignment that is not a power of
// two (0x3000 -> LNK4043, the default kept). ✔MEASURED 2026-10-07.
TEST(CoffDirectiveImageRequests, WhatLinkExeReplacesWithItsDefaultIsWarnedAndReplaced) {
    auto const r = pe::parseCoffLinkerDirectives(
        " /SUBSYSTEM:CONSOLE,5.01 /SUBSYSTEM:CONSOLE,5.02 /ALIGN:0x3000", vocabulary());
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->image.size(), 2u) << "the alignment link.exe replaces is no request";
    EXPECT_EQ(std::get<UnitSubsystemRequest>(r->image[0].value).setting, (SubsystemSetting{3, std::nullopt}))
        << "the subsystem stands; its version is the format's default";
    EXPECT_EQ(std::get<UnitSubsystemRequest>(r->image[1].value).setting, (SubsystemSetting{3, VersionPair{5, 2}}));
    ASSERT_EQ(r->warnings.size(), 2u);
    EXPECT_NE(r->warnings[0].find("LNK4010"), std::string::npos) << r->warnings[0];
    EXPECT_NE(r->warnings[1].find("LNK4043"), std::string::npos) << r->warnings[1];
    EXPECT_EQ(r->handOn.size(), 3u) << "all three are handed on as written";
}

// What a DSS IMAGE cannot carry is a request the image link refuses — `/MERGE:`, `/MANIFESTDEPENDENCY:`, a runtime
// startup DSS provides no counterpart of, a subsystem no DSS image starts under, an ordinal, NONAME or forwarder
// export — and the parse reads on, so a relocatable artifact can hand each on to a final linker that can.
TEST(CoffDirectiveImageRequests, WhatADssImageCannotCarryIsARequestTheImageLinkRefuses) {
    auto const r = pe::parseCoffLinkerDirectives(
        " /MERGE:.rdata=.text \"/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls'\""
        " /ENTRY:wmainCRTStartup /SUBSYSTEM:NATIVE /EXPORT:f,@3 /EXPORT:g,@4,NONAME /EXPORT:h=otherdll.h",
        vocabulary());
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->image.size(), 7u);
    std::vector<std::string> reasons;
    for (auto const& req : r->image) {
        auto const* u = std::get_if<UnitUnhonourableRequest>(&req.value);
        ASSERT_NE(u, nullptr) << req.spelled;
        reasons.push_back(u->reason);
    }
    EXPECT_NE(reasons[0].find("changes what a program MEANS"), std::string::npos) << reasons[0];
    EXPECT_NE(reasons[0].find("D-LK-PE-IMAGE-CANNOT-HONOUR-EVERY-LINKER-DIRECTIVE"), std::string::npos);
    EXPECT_NE(reasons[1].find("manifest"), std::string::npos) << reasons[1];
    EXPECT_NE(reasons[2].find("calls `wmain`"), std::string::npos) << reasons[2];
    EXPECT_NE(reasons[3].find("NtProcessStartup"), std::string::npos) << reasons[3];
    EXPECT_NE(reasons[4].find("ORDINAL or NONAME ('@3')"), std::string::npos) << reasons[4];
    EXPECT_NE(reasons[5].find("ORDINAL or NONAME"), std::string::npos) << reasons[5];
    EXPECT_NE(reasons[6].find("FORWARDER"), std::string::npos) << reasons[6];
    EXPECT_TRUE(r->exports.empty()) << "an export the image cannot carry is no export of it";
    EXPECT_EQ(r->handOn.size(), 7u);
}

// `/SECTION:name,attributes` read as link.exe reads the letters (✔MEASURED 2026-10-07, link.exe 14.44, 28 arms in
// r4probe/m1, m2 and m3/sect_neg.bat): each row is the characteristics link.exe wrote for the section, from its own.
TEST(CoffDirectiveImageRequests, SectionAttributeLettersFollowLinkExesRule) {
    struct Arm {
        char const*   attrs;
        std::uint32_t before;
        std::uint32_t after;
    };
    Arm const kArms[] = {
        {"RWS", 0xC0000040u, 0xD0000040u},   {"rws", 0xC0000040u, 0xD0000040u},
        {"R", 0x60000020u, 0x40000020u},     // `.text,R` drops E: an un-negated access letter REPLACES E/R/W
        {"W", 0x40000040u, 0x80000040u},     // `.rdata,W` drops R
        {"E", 0xC0000040u, 0x20000040u},     // `.data,E` drops R and W
        {"!RW", 0xC0000040u, 0x00000040u},   // `!` toggles negation for the letters after it
        {"!WS", 0xD0000040u, 0x40000040u},
        {"!KP", 0xC0000040u, 0xCC000040u},   // K and P mean CACHEABLE / PAGEABLE: negated, they SET NOT_*
        {"!PK", 0xC0000040u, 0xCC000040u},
        {"!K!P", 0xC0000040u, 0xC4000040u},  // the second `!` toggles negation off again: P un-negated
        {"!R!W", 0x40000040u, 0x80000040u},  // R cleared, then W un-negated replaces the access bits
        {"S!W", 0xC0000040u, 0x50000040u},   // (link.exe also warns LNK4223: shared without write)
        {"D", 0xC0000040u, 0xC2000040u},
        {"!D", 0xC2000040u, 0xC0000040u},
    };
    for (auto const& arm : kArms) {
        SCOPED_TRACE(arm.attrs);
        auto const masks = pe::detail::parseSectionAttributes(arm.attrs);
        ASSERT_TRUE(masks.has_value());
        EXPECT_EQ((arm.before & ~masks->second) | masks->first, arm.after)
            << std::hex << "set 0x" << masks->first << " clear 0x" << masks->second;
    }
    for (char const* bad : {"", "X", "RWX", "ALIGN=16", "R W"}) {
        SCOPED_TRACE(bad);
        EXPECT_FALSE(pe::detail::parseSectionAttributes(bad).has_value()) << "link.exe refuses it (LNK1137)";
    }
}

// ══ The decision and the header ═════════════════════════════════════════════

// `/STACK:` — the FIRST unit's, whole; rounded up to 4 as link.exe rounds it; a reserve below the default commit
// with no commit stated becomes the commit; a stated commit above the reserve is written as stated (lld-link's
// answer — that image runs; link.exe refuses it); the program's own `--stack-reserve` wins over every unit's.
TEST(CoffDirectiveImageRequests, TheFirstUnitsStackAndTheProgramsOwnRequestWin) {
    {
        auto const l = linkExe({programUnit(), requestingUnit(2, requestsOf(" /STACK:0x10000000,0x20000")),
                                extraUnit(3, requestsOf(" /STACK:0x30000000,0x40000"))});
        ASSERT_TRUE(l->pe.has_value()) << diagnosticsOf(l->rep);
        EXPECT_EQ(l->pe->stackReserve(), 0x10000000u) << "link.exe keeps the FIRST object's (✔MEASURED 2026-10-07)";
        EXPECT_EQ(l->pe->stackCommit(), 0x20000u);
    }
    struct Edge {
        char const*   text;
        std::uint64_t reserve, commit;
        char const*   why;
    };
    Edge const kEdges[] = {
        {" /STACK:0x100001,0x1001", 0x100004u, 0x1004u, "link.exe rounds both up to 4"},
        {" /STACK:0x10", 0x1000u, 0x1000u, "a reserve below the default commit, none stated, becomes the commit"},
        {" /STACK:0", 0x1000u, 0x1000u, "so does a reserve of 0"},
        {" /STACK:0x800,0x400", 0x800u, 0x400u, "a stated commit below the reserve stands"},
        {" /STACK:0x100000,0x200000", 0x100000u, 0x200000u, "a stated commit above the reserve is lld-link's"},
    };
    for (auto const& e : kEdges) {
        SCOPED_TRACE(e.why);
        auto const l = linkExe({programUnit(), requestingUnit(2, requestsOf(e.text))});
        ASSERT_TRUE(l->pe.has_value()) << diagnosticsOf(l->rep);
        EXPECT_EQ(l->pe->stackReserve(), e.reserve);
        EXPECT_EQ(l->pe->stackCommit(), e.commit);
    }
    {
        ImageRequest program;
        program.stackReserveBytes = 0x400000u;
        auto const l = linkExe({programUnit(), requestingUnit(2, requestsOf(" /STACK:0x10000000,0x20000"))}, program);
        ASSERT_TRUE(l->pe.has_value()) << diagnosticsOf(l->rep);
        EXPECT_EQ(l->pe->stackReserve(), 0x400000u) << "the program's own request wins";
        EXPECT_EQ(l->pe->stackCommit(), 0x1000u) << "whole: the commit is the format's, as link.exe's command line";
    }
    {   // CONTROL: no request, the format's own.
        auto const l = linkExe({programUnit(), requestingUnit(2, {})});
        ASSERT_TRUE(l->pe.has_value()) << diagnosticsOf(l->rep);
        EXPECT_EQ(l->pe->stackReserve(), 0x100000u);
        EXPECT_EQ(l->pe->stackCommit(), 0x1000u);
    }
}

// `/HEAP:` (the first), `/SUBSYSTEM:` (the last; a version sets the subsystem AND operating-system versions),
// `/VERSION:` (the last), `/BASE:` (the first), `/ALIGN:` (the last; every section then starts at a multiple of it,
// `.text` at the first), `/RELEASE` (the PE checksum over the file): each lands in the header, with link.exe's
// precedence (✔MEASURED 2026-10-07, r4probe/m3).
TEST(CoffDirectiveImageRequests, TheHeaderRequestsLandWithLinkExesPrecedence) {
    auto const l = linkExe(
        {programUnit(),
         requestingUnit(2, requestsOf(" /HEAP:0x200000,0x10000 /SUBSYSTEM:CONSOLE /VERSION:1.0 /BASE:0x200000000"
                                      " /ALIGN:0x4000 /RELEASE")),
         extraUnit(3, requestsOf(" /HEAP:0x300000 /SUBSYSTEM:WINDOWS,6.02 /VERSION:3.5 /BASE:0x300000000"
                                      " /ALIGN:0x2000"))});
    ASSERT_TRUE(l->pe.has_value()) << diagnosticsOf(l->rep);
    auto const& pe = *l->pe;
    EXPECT_EQ(pe.heapReserve(), 0x200000u) << "the FIRST /HEAP:";
    EXPECT_EQ(pe.heapCommit(), 0x10000u);
    EXPECT_EQ(pe.subsystem(), 2u) << "the LAST /SUBSYSTEM:";
    EXPECT_EQ(pe.u16(48), 6u);
    EXPECT_EQ(pe.u16(50), 2u) << "the subsystem version";
    EXPECT_EQ(pe.u16(40), 6u);
    EXPECT_EQ(pe.u16(42), 2u) << "and the operating-system version, as link.exe sets both";
    EXPECT_EQ(pe.u16(44), 3u);
    EXPECT_EQ(pe.u16(46), 5u) << "the LAST /VERSION:";
    EXPECT_EQ(pe.imageBase(), 0x200000000u) << "the FIRST /BASE:";
    EXPECT_EQ(pe.sectionAlignment(), 0x2000u) << "the LAST /ALIGN:";
    auto const secs = pe.sections();
    ASSERT_FALSE(secs.empty());
    EXPECT_EQ(secs.front().va, 0x2000u) << "`.text` at the first address the alignment allows past the headers";
    for (auto const& s : secs) EXPECT_EQ(s.va % 0x2000u, 0u) << s.name;
    EXPECT_NE(pe.checksum(), 0u);
    EXPECT_EQ(pe.checksum(), independentChecksum(pe)) << "the PE algorithm over the finished file";
    // CONTROL: without the requests, the document's own header.
    auto const plain = linkExe({programUnit(), requestingUnit(2, {})});
    ASSERT_TRUE(plain->pe.has_value()) << diagnosticsOf(plain->rep);
    EXPECT_EQ(plain->pe->subsystem(), 3u);
    EXPECT_EQ(plain->pe->sectionAlignment(), 0x1000u);
    EXPECT_EQ(plain->pe->sections().front().va, 0x1000u);
    EXPECT_EQ(plain->pe->checksum(), 0u);
    EXPECT_EQ(plain->pe->u16(44), 0u);
}

// An alignment below the format's own (its page) asks for the layout whose file offsets equal its addresses, which
// link.exe makes (✔MEASURED 2026-10-07: /ALIGN:0x800 runs) and DSS's writer does not: refused by name.
TEST(CoffDirectiveImageRequests, AnAlignmentBelowThePageIsRefusedByName) {
    auto const l = linkExe({programUnit(), requestingUnit(2, requestsOf(" /ALIGN:0x800"))});
    EXPECT_TRUE(l->image.bytes.empty());
    EXPECT_EQ(countCode(l->rep, DiagnosticCode::K_LinkerDirectiveUnhonourable), 1u) << diagnosticsOf(l->rep);
    EXPECT_TRUE(sawText(l->rep, "section alignment of 2048 bytes")) << diagnosticsOf(l->rep);
}

// `/SECTION:` rewrites the named section's Characteristics, in order across units (RWS then R -> S|R, as link.exe);
// a name the image has no section of is warned and changes nothing (LNK4039; names are case-sensitive).
TEST(CoffDirectiveImageRequests, ASectionRequestRewritesTheNamedSectionInOrder) {
    auto const plain = linkExe({programUnit(), requestingUnit(2, {})});
    ASSERT_TRUE(plain->pe.has_value()) << diagnosticsOf(plain->rep);
    auto const data0 = plain->pe->section(".data");
    ASSERT_TRUE(data0.has_value());
    EXPECT_EQ(data0->chars, 0xC0000040u);

    auto const l = linkExe({programUnit(), requestingUnit(2, requestsOf(" /SECTION:.data,RWS /SECTION:.DATA,R")),
                            extraUnit(3, requestsOf(" /SECTION:.data,R"))});
    ASSERT_TRUE(l->pe.has_value()) << diagnosticsOf(l->rep);
    auto const data = l->pe->section(".data");
    ASSERT_TRUE(data.has_value());
    EXPECT_EQ(data->chars, 0x50000040u) << "RWS, then R replacing the access bits: S | R";
    EXPECT_EQ(countCode(l->rep, DiagnosticCode::K_LinkerDirectiveIgnored), 1u) << diagnosticsOf(l->rep);
    EXPECT_TRUE(sawText(l->rep, "names section '.DATA', which this image does not have")) << diagnosticsOf(l->rep);
    EXPECT_EQ(l->pe->section(".text")->chars, plain->pe->section(".text")->chars) << "no other section changes";
}

// An EXE exports what its units request — LINK-WIDE (another unit's definition), a rename under its new name ALONE,
// a datum, a PRIVATE one, a HIDDEN one (an explicit export wins over a hide) and, for a name only a library defines,
// the image's import thunk — as link.exe and lld-link export each (✔MEASURED 2026-10-07, r4probe/m3, m4).
TEST(CoffDirectiveImageRequests, AnExeExportsWhatItsUnitsRequest) {
    AssembledModule caller = programUnit();
    // `main` calls `puts`, so the image imports it and has a thunk for it.
    caller.functions[0].bytes = {0xE8, 0x00, 0x00, 0x00, 0x00, 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
    Relocation call;
    call.offset = 1;
    call.target = SymbolId{9};
    call.kind   = RelocationKind{1};
    caller.functions[0].relocations.push_back(call);
    ExternImport puts;
    puts.symbol      = SymbolId{9};
    puts.mangledName = "puts";
    puts.libraryPath = "ucrtbase.dll";
    caller.externImports.push_back(puts);
    auto const l = linkExe(
        {caller, requestingUnit(2, requestsOf(" /EXPORT:dss_renamed=dss_internal /EXPORT:dss_datum,DATA"
                                              " /EXPORT:dss_main_defined /EXPORT:dss_hidden_fn,PRIVATE /EXPORT:puts"))});
    ASSERT_TRUE(l->pe.has_value()) << diagnosticsOf(l->rep);
    auto const exports = l->pe->exports();
    std::vector<std::string> names;
    for (auto const& [n, rva] : exports) names.push_back(n);
    EXPECT_EQ(names, (std::vector<std::string>{"dss_datum", "dss_hidden_fn", "dss_main_defined", "dss_renamed", "puts"}))
        << "`dss_internal` is exported under its new name alone, and an EXE exports nothing it was not asked to";
    EXPECT_EQ(l->pe->bytesAt(exports.at("dss_renamed"), 6), (std::vector<std::uint8_t>{0xB8, 0x02, 0, 0, 0, 0xC3}));
    EXPECT_EQ(l->pe->bytesAt(exports.at("dss_main_defined"), 6),
              (std::vector<std::uint8_t>{0xB8, 0x05, 0, 0, 0, 0xC3}))
        << "another unit's definition (link-wide)";
    EXPECT_EQ(l->pe->bytesAt(exports.at("dss_hidden_fn"), 6), (std::vector<std::uint8_t>{0xB8, 0x03, 0, 0, 0, 0xC3}));
    EXPECT_EQ(l->pe->bytesAt(exports.at("dss_datum"), 4), (std::vector<std::uint8_t>{7, 0, 0, 0}));
    auto const thunk = l->pe->bytesAt(exports.at("puts"), 2);
    EXPECT_EQ(thunk, (std::vector<std::uint8_t>{0xFF, 0x25})) << "the import thunk `jmp [rip+disp32]`";
    // CONTROL: the same units with no export request write no export table.
    auto const plain = linkExe({caller, requestingUnit(2, {})});
    ASSERT_TRUE(plain->pe.has_value()) << diagnosticsOf(plain->rep);
    EXPECT_TRUE(plain->pe->exports().empty());
    EXPECT_FALSE(plain->pe->section(".edata").has_value());
}

// An export of a name NOTHING in the link defines is refused by name, as link.exe and lld-link refuse it (LNK2001,
// ✔MEASURED 2026-10-07); a second export of one name for ANOTHER definition is warned and the first stands (LNK4197).
TEST(CoffDirectiveImageRequests, AnExportOfNothingIsRefusedAndASecondOneIsWarned) {
    {
        auto const l = linkExe({programUnit(), requestingUnit(2, requestsOf(" /EXPORT:nobody_defines_this"))});
        EXPECT_TRUE(l->image.bytes.empty());
        EXPECT_GE(countCode(l->rep, DiagnosticCode::K_SymbolUndefined), 1u) << diagnosticsOf(l->rep);
        EXPECT_TRUE(sawText(l->rep, "exports 'nobody_defines_this', which no unit of the link defines"))
            << diagnosticsOf(l->rep);
    }
    {
        auto const l = linkExe({programUnit(), requestingUnit(2, requestsOf(" /EXPORT:dup=dss_internal")),
                                extraUnit(3, requestsOf(" /EXPORT:dup=dss_entry"))});
        ASSERT_TRUE(l->pe.has_value()) << diagnosticsOf(l->rep);
        EXPECT_EQ(countCode(l->rep, DiagnosticCode::K_LinkerDirectiveIgnored), 1u) << diagnosticsOf(l->rep);
        EXPECT_TRUE(sawText(l->rep, "LNK4197")) << diagnosticsOf(l->rep);
        EXPECT_EQ(l->pe->bytesAt(l->pe->exports().at("dup"), 6),
                  (std::vector<std::uint8_t>{0xB8, 0x02, 0, 0, 0, 0xC3}))
            << "the first stands";
    }
}

// A DLL still exports every visible definition, and adds what its units request: a rename adds a name, an export of
// a hidden definition wins over the hide, and a hide of ANOTHER unit's definition keeps it out (link-wide).
TEST(CoffDirectiveImageRequests, ADllAddsTheRequestedExportsAndHonoursAForeignHide) {
    AssembledModule lib;
    lib.cuId              = CompilationUnitId{1};
    lib.expectedFuncCount = 2;
    lib.functions.push_back(function(1, {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3}));
    lib.functions.push_back(function(2, {0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3}));
    lib.symbols.push_back(ModuleSymbol{SymbolId{1}, "lib_api", SymbolBinding::Global, SymbolVisibility::Default});
    lib.symbols.push_back(ModuleSymbol{SymbolId{2}, "lib_other", SymbolBinding::Global, SymbolVisibility::Default});
    UnitLinkerRequests requests = requestsOf(" /EXPORT:lib_alias=lib_api /EXPORT:dss_hidden_fn");
    requests.hides = {"lib_other"};
    auto const l = linkDll({lib, requestingUnit(2, requests)});
    ASSERT_TRUE(l->pe.has_value()) << diagnosticsOf(l->rep);
    std::vector<std::string> names;
    for (auto const& [n, rva] : l->pe->exports()) names.push_back(n);
    EXPECT_EQ(names, (std::vector<std::string>{"dss_datum", "dss_entry", "dss_hidden_fn", "dss_internal", "lib_alias",
                                               "lib_api"}))
        << "every visible definition, the rename's new name beside the old, the hidden one asked for by name -- and "
           "not `lib_other`, which another unit's hide excludes";
}

// `/ENTRY:` naming a FUNCTION of the link starts the image there, with no startup (both linkers: the function's
// return value is the exit code, ✔MEASURED 2026-10-07); a runtime startup is the target's own, byte for byte; a later,
// different `/ENTRY:` is warned and the first stands (link.exe LNK4258).
TEST(CoffDirectiveImageRequests, AnEntryNamingAFunctionStartsTheImageThere) {
    auto const plain = linkExe({programUnit(), requestingUnit(2, {})});
    ASSERT_TRUE(plain->pe.has_value()) << diagnosticsOf(plain->rep);
    auto const l = linkExe({programUnit(), requestingUnit(2, requestsOf(" /ENTRY:dss_entry")),
                            extraUnit(3, requestsOf(" /ENTRY:dss_internal"))});
    ASSERT_TRUE(l->pe.has_value()) << diagnosticsOf(l->rep);
    EXPECT_EQ(l->pe->bytesAt(l->pe->entry(), 6), (std::vector<std::uint8_t>{std::begin(kEntryBody),
                                                                            std::end(kEntryBody)}))
        << "the image starts at `dss_entry` itself";
    EXPECT_NE(plain->pe->bytesAt(plain->pe->entry(), 6),
              (std::vector<std::uint8_t>{std::begin(kEntryBody), std::end(kEntryBody)}))
        << "CONTROL: without the request it starts in the target's startup";
    EXPECT_EQ(countCode(l->rep, DiagnosticCode::K_LinkerDirectiveIgnored), 1u) << diagnosticsOf(l->rep);
    EXPECT_TRUE(sawText(l->rep, "LNK4258")) << diagnosticsOf(l->rep);

    auto const runtime = linkExe({programUnit(), requestingUnit(2, requestsOf(" /ENTRY:mainCRTStartup"))});
    ASSERT_TRUE(runtime->pe.has_value()) << diagnosticsOf(runtime->rep);
    EXPECT_EQ(runtime->image.bytes, plain->image.bytes) << "the C runtime's startup IS the target's: the same image";
}

TEST(CoffDirectiveImageRequests, AnEntryNamingADatumOrNothingIsRefused) {
    for (auto const& [text, words] : {std::pair{" /ENTRY:dss_datum", "it is a datum, not a function"},
                                      std::pair{" /ENTRY:nobody", "no unit of the link defines a function of that "
                                                                  "name"}}) {
        SCOPED_TRACE(text);
        auto const l = linkExe({programUnit(), requestingUnit(2, requestsOf(text))});
        EXPECT_TRUE(l->image.bytes.empty());
        EXPECT_EQ(countCode(l->rep, DiagnosticCode::K_LinkerDirectiveUnhonourable), 1u) << diagnosticsOf(l->rep);
        EXPECT_TRUE(sawText(l->rep, words)) << diagnosticsOf(l->rep);
    }
}

// A member the ARCHIVE SEARCH pulls names no entry: link.exe fixes the entry before it searches an archive, and GNU ld
// reads no MS `/ENTRY:` (✔MEASURED 2026-10-07, r4probe/m8: a cl member stating `/ENTRY:f`, pulled from an archive,
// starts `main` under both; lld-link alone starts `f`). The static-link pull drops each entry request of a member it
// pulled — warned when it named a function, silent when it named the C runtime's startup — and keeps the token for a
// relocatable artifact; the image then starts in the target's startup, and the link says why. The pipeline's half (an
// object input keeps its entry; the same object pulled from an archive does not) is the pair of corpus examples
// `pe_foreign_directive_entry` and `pe_foreign_directive_entry_from_an_archive_member_warned`.
TEST(CoffDirectiveImageRequests, APulledArchiveMembersEntryIsDroppedAndSaid) {
    UnitLinkerRequests pulled = requestsOf(" /ENTRY:dss_entry /STACK:0x200000 /ENTRY:mainCRTStartup");
    linker::dropPulledMemberEntryRequests(pulled);
    ASSERT_EQ(pulled.image.size(), 1u) << "both entry requests leave; the stack stays";
    EXPECT_NE(std::get_if<UnitStackRequest>(&pulled.image[0].value), nullptr);
    ASSERT_EQ(pulled.warnings.size(), 1u) << "only the one naming a function is said";
    EXPECT_NE(pulled.warnings[0].find("'/ENTRY:dss_entry'"), std::string::npos) << pulled.warnings[0];
    EXPECT_EQ(pulled.handOn.size(), 3u) << "a relocatable artifact still hands every token on";

    auto const plain = linkExe({programUnit(), requestingUnit(2, {})});
    ASSERT_TRUE(plain->pe.has_value()) << diagnosticsOf(plain->rep);
    auto const l = linkExe({programUnit(), requestingUnit(2, pulled)});
    ASSERT_TRUE(l->pe.has_value()) << diagnosticsOf(l->rep);
    EXPECT_EQ(l->pe->entry(), plain->pe->entry()) << "the image starts in the target's startup";
    EXPECT_EQ(l->pe->stackReserve(), 0x200000u);
    EXPECT_EQ(countCode(l->rep, DiagnosticCode::K_LinkerDirectiveIgnored), 1u) << diagnosticsOf(l->rep);
    EXPECT_TRUE(sawText(l->rep, "the archive search pulled that member")) << diagnosticsOf(l->rep);
}

// The requests that mean nothing for a shared library or for a program, each said: a DLL takes no entry (refused: a
// DSS DLL has none), and reads no stack or heap size from its own header (warned and dropped; link.exe writes a copy
// nothing reads); a program refuses `/DLL` (the build's target decides what a DSS link makes).
TEST(CoffDirectiveImageRequests, WhatADllOrAProgramCannotTakeIsSaid) {
    {
        auto const l = linkDll({requestingUnit(2, requestsOf(" /ENTRY:dss_entry"))});
        EXPECT_TRUE(l->image.bytes.empty());
        EXPECT_TRUE(sawText(l->rep, "a DSS shared library has no entry")) << diagnosticsOf(l->rep);
    }
    {
        auto const plainDll = linkDll({requestingUnit(2, {})});
        ASSERT_TRUE(plainDll->pe.has_value()) << diagnosticsOf(plainDll->rep);
        auto const l = linkDll({requestingUnit(2, requestsOf(" /STACK:0x10000000 /HEAP:0x200000 /DLL"))});
        ASSERT_TRUE(l->pe.has_value()) << diagnosticsOf(l->rep);
        EXPECT_EQ(countCode(l->rep, DiagnosticCode::K_LinkerDirectiveIgnored), 2u) << diagnosticsOf(l->rep);
        EXPECT_EQ(l->pe->stackReserve(), plainDll->pe->stackReserve());
        EXPECT_EQ(l->pe->heapReserve(), plainDll->pe->heapReserve());
    }
    {
        auto const l = linkExe({programUnit(), requestingUnit(2, requestsOf(" /DLL"))});
        EXPECT_TRUE(l->image.bytes.empty());
        EXPECT_TRUE(sawText(l->rep, "asks for a DLL, and this build makes a program")) << diagnosticsOf(l->rep);
    }
}

// A `/FAILIFMISMATCH:` disagreement is WARNED and linked: link.exe and lld-link refuse it, GNU ld links it, and the
// disjunction decides acceptance (✔MEASURED 2026-10-06/07). One value stated twice is silent, as under both.
TEST(CoffDirectiveImageRequests, AMismatchIsWarnedAndLinked) {
    auto const agree = linkExe({programUnit(), requestingUnit(2, requestsOf(" /FAILIFMISMATCH:k=one")),
                                extraUnit(3, requestsOf(" /FAILIFMISMATCH:k=one"))});
    ASSERT_TRUE(agree->pe.has_value()) << diagnosticsOf(agree->rep);
    EXPECT_EQ(countCode(agree->rep, DiagnosticCode::K_LinkerDirectiveIgnored), 0u) << diagnosticsOf(agree->rep);
    auto const l = linkExe({programUnit(), requestingUnit(2, requestsOf(" /FAILIFMISMATCH:k=one")),
                            extraUnit(3, requestsOf(" /FAILIFMISMATCH:k=two"))});
    ASSERT_TRUE(l->pe.has_value()) << diagnosticsOf(l->rep);
    EXPECT_EQ(countCode(l->rep, DiagnosticCode::K_LinkerDirectiveIgnored), 1u) << diagnosticsOf(l->rep);
    EXPECT_TRUE(sawText(l->rep, "disagree on 'k'")) << diagnosticsOf(l->rep);
}

// ══ Refused by an image, handed on by an object ═════════════════════════════

// A request a DSS image cannot carry is refused BY NAME by the link that makes one — with the row's reason, which
// names the row — and an option no reference honours is warned there (MINOR 3); the same unit linked into a
// RELOCATABLE artifact is written, every token handed on verbatim to the final linker, and nothing is said.
TEST(CoffDirectiveImageRequests, RefusedByAnImageHandedOnByAnObject) {
    UnitLinkerRequests const requests =
        requestsOf(" /MERGE:.rdata=.text /LARGEADDRESSAWARE /STACK:0x10000000 /EXPORT:dss_internal");
    {
        auto const l = linkExe({programUnit(), requestingUnit(2, requests)});
        EXPECT_TRUE(l->image.bytes.empty());
        EXPECT_EQ(countCode(l->rep, DiagnosticCode::K_LinkerDirectiveUnhonourable), 1u) << diagnosticsOf(l->rep);
        EXPECT_TRUE(sawText(l->rep, "'/MERGE:.rdata=.text' cannot be honoured by this image")) << diagnosticsOf(l->rep);
        EXPECT_TRUE(sawText(l->rep, "D-LK-PE-IMAGE-CANNOT-HONOUR-EVERY-LINKER-DIRECTIVE")) << diagnosticsOf(l->rep);
        EXPECT_EQ(countCode(l->rep, DiagnosticCode::K_LinkerDirectiveIgnored), 1u) << diagnosticsOf(l->rep);
        EXPECT_TRUE(sawText(l->rep, "'LARGEADDRESSAWARE'")) << diagnosticsOf(l->rep);
    }
    {
        auto const l = linkWith("pe64-x86_64-windows", {requestingUnit(2, requests)});
        ASSERT_FALSE(l->image.bytes.empty()) << diagnosticsOf(l->rep);
        EXPECT_EQ(l->rep.errorCount(), 0u) << diagnosticsOf(l->rep);
        EXPECT_EQ(countCode(l->rep, DiagnosticCode::K_LinkerDirectiveIgnored), 0u)
            << "a relocatable artifact says nothing: its final linker decides";
        std::string const text{l->image.bytes.begin(), l->image.bytes.end()};
        for (char const* tok : {"/MERGE:.rdata=.text", "/LARGEADDRESSAWARE", "/STACK:0x10000000", "/EXPORT:dss_internal"}) {
            EXPECT_NE(text.find(tok), std::string::npos) << tok << " is handed on verbatim";
        }
    }
}

// ══ Through the reader ══════════════════════════════════════════════════════

// The round trip a foreign object takes: a unit's tokens written into a COFF object's `.drectve` by DSS's own writer
// (the static-archive repack path) read back as the same requests, and a link of the object honours them.
TEST(CoffDirectiveImageRequests, AnObjectsDirectivesReachTheLinkThroughTheReader) {
    auto const obj = load("pe64-x86_64-windows");
    ASSERT_TRUE(obj.target && obj.format);
    UnitLinkerRequests written;
    written.handOn = {"/STACK:0x10000000,0x20000", "/SUBSYSTEM:WINDOWS", "/EXPORT:dss_renamed=dss_internal"};
    DiagnosticReporter rep;
    auto const bytes = pe::encode(requestingUnit(2, written), *obj.target, *obj.format, rep);
    ASSERT_FALSE(bytes.empty()) << diagnosticsOf(rep);
    auto read = pe::readRelocatableObject(bytes, *obj.target, *obj.format, rep, CompilationUnitId{2});
    ASSERT_TRUE(read.has_value()) << diagnosticsOf(rep);
    EXPECT_EQ(read->linkerRequests.handOn, written.handOn);
    ASSERT_EQ(read->linkerRequests.image.size(), 2u);
    ASSERT_EQ(read->linkerRequests.exports.size(), 1u);
    auto const l = linkExe({programUnit(), *read});
    ASSERT_TRUE(l->pe.has_value()) << diagnosticsOf(l->rep);
    EXPECT_EQ(l->pe->stackReserve(), 0x10000000u);
    EXPECT_EQ(l->pe->subsystem(), 2u);
    EXPECT_EQ(l->pe->exports().count("dss_renamed"), 1u);
}

// MINOR 4: cl's TLS-CALLBACK idiom (`/INCLUDE:_tls_used` beside a `.CRT$XL*` callback pointer) runs its callback
// under link.exe and lld-link (✔MEASURED 2026-10-07, r4probe/m6: 42, and 0 without the includes). No DSS link defines
// `_tls_used` and the reader does not carry a foreign `.CRT$XL*` table, so the object is refused by name, the message
// naming the table and the row that carries it — not "link the library that defines it", which no library could.
TEST(CoffDirectiveImageRequests, TheTlsCallbackIdiomIsRefusedNamingItsTable) {
    auto const obj = load("pe64-x86_64-windows");
    ASSERT_TRUE(obj.target && obj.format);
    UnitLinkerRequests written;
    written.handOn = {"/INCLUDE:_tls_used"};
    DiagnosticReporter rep;
    auto const bytes = pe::encode(requestingUnit(2, written), *obj.target, *obj.format, rep);
    ASSERT_FALSE(bytes.empty()) << diagnosticsOf(rep);
    DiagnosticReporter readRep;
    auto const read = pe::readRelocatableObject(bytes, *obj.target, *obj.format, readRep, CompilationUnitId{2});
    EXPECT_FALSE(read.has_value());
    EXPECT_EQ(countCode(readRep, DiagnosticCode::K_LinkerDirectiveUnhonourable), 1u) << diagnosticsOf(readRep);
    EXPECT_TRUE(sawText(readRep, ".CRT$XL")) << diagnosticsOf(readRep);
    EXPECT_TRUE(sawText(readRep, "D-LK-FOREIGN-OBJECT-INITIALIZER-TABLES-NEVER-RUN")) << diagnosticsOf(readRep);
    // CONTROL: an include of a name the object DEFINES needs nothing more, and one of an ordinary name is a required
    // reference, as before.
    written.handOn = {"/INCLUDE:dss_internal", "/INCLUDE:puts"};
    auto const ok = pe::encode(requestingUnit(2, written), *obj.target, *obj.format, rep);
    DiagnosticReporter okRep;
    auto const okRead = pe::readRelocatableObject(ok, *obj.target, *obj.format, okRep, CompilationUnitId{2});
    ASSERT_TRUE(okRead.has_value()) << diagnosticsOf(okRep);
    bool required = false;
    for (auto const& e : okRead->externImports) required = required || (e.mangledName == "puts" && e.requiredByDirective);
    EXPECT_TRUE(required);
}
