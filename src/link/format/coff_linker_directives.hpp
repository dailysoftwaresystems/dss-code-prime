#pragma once

#include "core/types/extern_import.hpp"         // ExternImport -- the rows a unit's directives complete
#include "core/types/symbol_id_cursor.hpp"      // SymbolIdsRefused -- a refused fresh id, carried as a value
#include "core/types/unit_linker_requests.hpp"   // UnitImageRequest, UnitExportRequest
#include "link/object_format_schema.hpp"         // PeLinkerDirectives, LinkerDirectiveMeaning
#include "link/standing_fallback.hpp"             // standingFallback -- which of a name's fallbacks stands

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

// ── A COFF OBJECT'S LINKER DIRECTIVES, BOTH DIRECTIONS ─────────────────────
// P69 (the PE half of D-LK-PE-DLL-EXPORTS-THE-SHIPPED-RUNTIME-IT-LINKS, and
// D-LK-COFF-READER-SKIPPED-EVERY-LINKER-DIRECTIVE).
//
// A relocatable COFF object hands its final linker REQUESTS as text in a
// section of its own (`.drectve`, IMAGE_SCN_LNK_INFO): `/DEFAULTLIB:MSVCRT`,
// `/EXPORT:f`, `/INCLUDE:_tls_used`, ` -exclude-symbols:helper`,
// ` -aligncomm:"c",5`. COFF has no visibility field, so `-exclude-symbols` is
// how the ecosystem says "defined here, never exported from the image" (clang
// `--target=x86_64-w64-windows-gnu` writes it for `visibility("hidden")`).
//
// The TEXT is tokenized the way the COFF linkers tokenize it — Windows
// command-line rules (`cl::TokenizeWindowsCommandLine`, which lld-link applies
// to `.drectve`): whitespace separates, a double quote groups, `2n` backslashes
// before a quote are `n` backslashes and a toggle, `2n+1` are `n` backslashes
// and a literal quote, any other backslash is itself. A UTF-8 byte-order mark
// opens the text at most once, and a NUL is whitespace (an assembler may pad the
// section). Each token is `<prefix><option>[:<value>]`; the format document
// declares the prefixes and every option it gives a meaning
// (`PeLinkerDirectives`): one the link honours, one it refuses by name with the
// reason (`refused`), or one it drops with the reason dropping it is right
// (`ignored`). An option the document does not declare is one no reference
// linker honours in a directive (link.exe warns LNK4229 and links, GNU ld warns
// "unrecognized" and links, ✔MEASURED 2026-10-07): it is WARNED and ignored —
// never dropped in silence. P69 round 4.
namespace dss::pe {

// ── THE NAME OF A WEAK EXTERNAL'S DEFAULT RECORD ─────────────────────────────
// (P69 fold 3b, the review of fold 3a, F11: it was spelled separately in the
// writer, twice, and in the reader.) COFF names no default: a weak external's
// default is any record, and the format says nothing of what it is called. The
// GNU-family producers name it after the weak name — MinGW gcc
// `.weak.<name>.<the unit's first strong symbol>`, clang
// `.weak.<name>.default.<...>` (✔MEASURED 2026-10-08 on their objects) — so the
// spelling is the PRODUCERS' convention, not a fact of the format, and it is
// stated here and not in a format document: DSS's writer names its own default
// in it (`.weak.<name>.default`, then `.<the unit's first strong definition>`
// where it has one, as gcc does), and the reader recognizes a default of the
// family by it.
inline constexpr std::string_view kWeakDefaultRecordPrefix = ".weak.";
// Every name a default of the weak name `weakName` takes begins with this.
[[nodiscard]] inline std::string weakDefaultRecordFamily(std::string_view weakName) {
    return std::string{kWeakDefaultRecordPrefix} + std::string{weakName} + ".";
}
// The name DSS's writer gives the default of `weakName` (before any suffix).
[[nodiscard]] inline std::string dssWeakDefaultRecordName(std::string_view weakName) {
    return weakDefaultRecordFamily(weakName) + "default";
}

// `alternateName`: `name=fallback` — a reference to `name` that nothing
// defines resolves to `fallback`.
struct CoffAlternateName {
    std::string name;
    std::string fallback;
    friend bool operator==(CoffAlternateName const&, CoffAlternateName const&) = default;
};

// `commonAlignment`: `name,log2` — the COMMON symbol `name` aligned to
// 2^log2 bytes.
struct CoffCommonAlignment {
    std::string   name;
    std::uint8_t  log2 = 0;
    friend bool operator==(CoffCommonAlignment const&, CoffCommonAlignment const&) = default;
};

struct CoffDirectiveRequests {
    std::vector<std::string>         hidden;            // `hideSymbols`: Hidden wherever defined
    std::vector<UnitExportRequest>   exports;           // `exportSymbol`: exported, link-wide
    std::vector<std::string>         included;          // `includeSymbol`: the link must define it
    std::vector<CoffAlternateName>   alternates;        // `alternateName`
    std::vector<CoffCommonAlignment> commonAlignments;  // `commonAlignment`
    std::vector<UnitImageRequest>    image;             // the IMAGE requests, in directive order
    std::vector<std::string>         dependentLibraries;   // `dependentLibrary`: a library to search (P69 fold 3f)
    // One message per option no reference honours in a directive: the reader
    // reports each as a warning (`K_LinkerDirectiveIgnored`) and links.
    std::vector<std::string>         warnings;
    // Every token a relocatable artifact restates VERBATIM (requoted where its
    // text needs it): the exports, the image requests, the `ignored` options and
    // the warned ones. A hide, an include, an alternate name and a common's
    // alignment are restated from the rows they become instead.
    std::vector<std::string>         handOn;
};

// The largest `commonAlignment` exponent: 2^13 = 8192 bytes, the widest
// alignment a COFF section header can state (IMAGE_SCN_ALIGN_8192BYTES).
inline constexpr std::uint8_t kMaxCoffCommonAlignmentLog2 = 13;

// ⓘ The alignment the COFF linkers give a COMMON that no `-aligncomm:` names is
// the DOCUMENT's (`commonAlignmentFromSizeAtMost` — link.exe's and lld-link's —
// and `commonAlignmentEveryLinkerGivesAtMost` — GNU ld's, the least — read
// through `ObjectFormatSchema::commonsStatement`): the COFF reader aligns a
// common at least as the first, and the COFF writer states a common's alignment
// in a directive exactly where it exceeds the second. No number of either lives
// here.

// The meanings whose requests ONE LINK DECIDES ACROSS ITS UNITS: the nine image
// settings, each held once by an image, and the exports, one per exported name.
// A row of one of these states which unit's request stands and what a shared
// library does with it (`PeLinkerDirectiveRow::unitPrecedence` /
// `inSharedLibrary`: the loader requires both here and refuses either on any
// other row), and the parse stamps each request with its row's answers. P69
// send-back 5 (review-xa4 MINOR 6).
[[nodiscard]] constexpr bool isDecidedAcrossUnits(LinkerDirectiveMeaning meaning) noexcept {
    switch (meaning) {
        case LinkerDirectiveMeaning::ExportSymbol:
        case LinkerDirectiveMeaning::StackSize:
        case LinkerDirectiveMeaning::HeapSize:
        case LinkerDirectiveMeaning::Subsystem:
        case LinkerDirectiveMeaning::EntryPoint:
        case LinkerDirectiveMeaning::SectionAttributes:
        case LinkerDirectiveMeaning::ImageChecksum:
        case LinkerDirectiveMeaning::ImageVersion:
        case LinkerDirectiveMeaning::ImageBase:
        case LinkerDirectiveMeaning::SectionAlignment:
            return true;
        case LinkerDirectiveMeaning::HideSymbols:
        case LinkerDirectiveMeaning::IncludeSymbol:
        case LinkerDirectiveMeaning::AlternateName:
        case LinkerDirectiveMeaning::CommonAlignment:
        case LinkerDirectiveMeaning::MismatchCheck:
        case LinkerDirectiveMeaning::DllImage:
        case LinkerDirectiveMeaning::ManifestDependency:   // a link-wide set: nothing is dropped
        case LinkerDirectiveMeaning::DependentLibrary:     // a library to search: an image ignores it
        case LinkerDirectiveMeaning::Refused:
        case LinkerDirectiveMeaning::Ignored:
            return false;
    }
    return false;
}

namespace detail {

[[nodiscard]] inline std::vector<std::string> tokenizeWindowsCommandLine(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    auto const space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0'; };
    while (i < s.size()) {
        while (i < s.size() && space(s[i])) ++i;
        if (i >= s.size()) break;
        std::string tok;
        bool quoted = false;
        while (i < s.size() && (quoted || !space(s[i]))) {
            if (s[i] == '\\') {
                std::size_t n = 0;
                while (i < s.size() && s[i] == '\\') { ++n; ++i; }
                if (i < s.size() && s[i] == '"') {
                    tok.append(n / 2, '\\');
                    if (n % 2 == 1) { tok.push_back('"'); ++i; }
                    // an even run leaves the quote to toggle below
                } else {
                    tok.append(n, '\\');
                }
                continue;
            }
            if (s[i] == '"') {
                // `""` inside a quoted run is one literal quote (the MSVC CRT rule).
                if (quoted && i + 1 < s.size() && s[i + 1] == '"') { tok.push_back('"'); i += 2; continue; }
                quoted = !quoted;
                ++i;
                continue;
            }
            tok.push_back(s[i++]);
        }
        out.push_back(std::move(tok));
    }
    return out;
}

[[nodiscard]] inline bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t k = 0; k < a.size(); ++k) {
        if (std::tolower(static_cast<unsigned char>(a[k])) != std::tolower(static_cast<unsigned char>(b[k]))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::vector<std::string> splitOnComma(std::string_view v) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t k = 0; k <= v.size(); ++k) {
        if (k == v.size() || v[k] == ',') {
            out.emplace_back(v.substr(start, k - start));
            start = k + 1;
        }
    }
    return out;
}

// A number in "decimal or C-language notation", as the Microsoft linker
// documents its size and address options: `0x`/`0X` hexadecimal, a leading `0`
// octal, otherwise decimal. Nothing else (no sign, no suffix, no blank).
[[nodiscard]] inline std::optional<std::uint64_t> parseCNumber(std::string_view s) {
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
    } else if (s.size() > 1 && s[0] == '0') {
        base = 8;
        s.remove_prefix(1);
    }
    if (s.empty()) return std::nullopt;
    std::uint64_t v = 0;
    auto const [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
    return v;
}

// `major[.minor]`, each a decimal u16 (`6.02` -> 6, 2; `6.1` -> 6, 1: link.exe
// writes 6.01, ✔MEASURED 2026-10-07).
[[nodiscard]] inline std::optional<VersionPair> parseVersionPair(std::string_view s) {
    auto const decimalU16 = [](std::string_view d) -> std::optional<std::uint16_t> {
        if (d.empty() || d.size() > 5) return std::nullopt;
        unsigned v = 0;
        auto const [end, ec] = std::from_chars(d.data(), d.data() + d.size(), v, 10);
        if (ec != std::errc{} || end != d.data() + d.size() || v > 0xFFFFu) return std::nullopt;
        return static_cast<std::uint16_t>(v);
    };
    std::size_t const dot = s.find('.');
    auto const major = decimalU16(s.substr(0, dot));
    if (!major.has_value()) return std::nullopt;
    if (dot == std::string_view::npos) return VersionPair{*major, 0};
    auto const minor = decimalU16(s.substr(dot + 1));
    if (!minor.has_value()) return std::nullopt;
    return VersionPair{*major, *minor};
}

// `reserve[,commit]` (`/STACK:`, `/HEAP:`). A reserve of 0 is link.exe's to take
// (it writes the default commit there, ✔MEASURED 2026-10-07), so it parses.
[[nodiscard]] inline std::optional<ReserveCommit> parseReserveCommit(std::string_view v) {
    auto const parts = splitOnComma(v);
    if (parts.empty() || parts.size() > 2) return std::nullopt;
    auto const reserve = parseCNumber(parts[0]);
    if (!reserve.has_value()) return std::nullopt;
    ReserveCommit rc{*reserve, std::nullopt};
    if (parts.size() == 2) {
        auto const commit = parseCNumber(parts[1]);
        if (!commit.has_value()) return std::nullopt;
        rc.commit = *commit;
    }
    return rc;
}

// The section Characteristics bits `/SECTION:` attribute letters name
// (PE/COFF 4.1, "Section Flags").
inline constexpr std::uint32_t kScnMemDiscardable = 0x02000000u;
inline constexpr std::uint32_t kScnMemNotCached   = 0x04000000u;
inline constexpr std::uint32_t kScnMemNotPaged    = 0x08000000u;
inline constexpr std::uint32_t kScnMemShared      = 0x10000000u;
inline constexpr std::uint32_t kScnMemExecute     = 0x20000000u;
inline constexpr std::uint32_t kScnMemRead        = 0x40000000u;
inline constexpr std::uint32_t kScnMemWrite       = 0x80000000u;

// `attributes` of `/SECTION:name,attributes`, read as link.exe reads it
// (✔MEASURED 2026-10-07, link.exe 14.44, 28 arms): letters E R W S D K P in
// either case; `!` TOGGLES negation for the letters after it (`!RW` clears
// both, `!K!P` negates K only); an un-negated E, R or W REPLACES the E/R/W
// bits with the un-negated ones (`.text,R` drops E), while a negated one
// clears its bit alone; S and D set (negated: clear); K and P mean CACHEABLE
// and PAGEABLE, so they clear NOT_CACHED / NOT_PAGED (negated: set them).
// nullopt for an empty string or any other character (LNK1137).
[[nodiscard]] inline std::optional<std::pair<std::uint32_t, std::uint32_t>>
parseSectionAttributes(std::string_view a) {
    if (a.empty()) return std::nullopt;
    std::uint32_t set = 0, clear = 0, positiveAccess = 0;
    bool negated = false;
    for (char const raw : a) {
        char const c = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
        if (c == '!') {
            negated = !negated;
            continue;
        }
        std::uint32_t bit = 0;
        switch (c) {
            case 'E': bit = kScnMemExecute; break;
            case 'R': bit = kScnMemRead; break;
            case 'W': bit = kScnMemWrite; break;
            case 'S': bit = kScnMemShared; break;
            case 'D': bit = kScnMemDiscardable; break;
            case 'K': bit = kScnMemNotCached; break;
            case 'P': bit = kScnMemNotPaged; break;
            default: return std::nullopt;
        }
        bool const access = c == 'E' || c == 'R' || c == 'W';
        bool const inverted = c == 'K' || c == 'P';   // the letter names the bit's absence
        bool const raise = inverted ? negated : !negated;
        if (access && !negated) positiveAccess |= bit;
        if (raise) {
            set |= bit;
            clear &= ~bit;
        } else {
            clear |= bit;
            set &= ~bit;
        }
    }
    if (positiveAccess != 0u) {
        std::uint32_t const accessBits = kScnMemExecute | kScnMemRead | kScnMemWrite;
        clear |= accessBits & ~positiveAccess;
        set = (set & ~accessBits) | positiveAccess;
    }
    return std::pair{set, clear};
}

// A token spelled back so the Windows command-line tokenizer reads it as ONE
// token with the same text: unchanged when it holds no whitespace and no
// quote, else quoted with each quote (and the backslashes before it) escaped.
[[nodiscard]] inline std::string requoteDirectiveToken(std::string_view tok) {
    bool const plain = std::none_of(tok.begin(), tok.end(), [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\0';
    });
    if (plain && !tok.empty()) return std::string{tok};
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char const c : tok) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');   // before the closing quote
    out.push_back('"');
    return out;
}

}  // namespace detail

// Read one object's directive text against the format's vocabulary.
[[nodiscard]] inline std::expected<CoffDirectiveRequests, std::string>
parseCoffLinkerDirectives(std::string_view text, PeLinkerDirectives const& vocab) {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    CoffDirectiveRequests out;
    for (auto const& tok : detail::tokenizeWindowsCommandLine(text)) {
        if (tok.empty() || vocab.optionPrefixes.find(tok.front()) == std::string::npos) {
            return std::unexpected(std::format(
                "directive '{}' does not begin with an option prefix the format declares ('{}')", tok,
                vocab.optionPrefixes));
        }
        std::string_view const body{std::string_view{tok}.substr(1)};
        std::size_t const colon = body.find(':');
        std::string_view const option = body.substr(0, colon);
        std::string_view const value = colon == std::string_view::npos ? std::string_view{} : body.substr(colon + 1);
        PeLinkerDirectiveRow const* row = nullptr;
        for (auto const& r : vocab.directives) {
            if (detail::equalsIgnoringCase(r.option, option)) row = &r;
        }
        std::string const handOnText = detail::requoteDirectiveToken(tok);
        if (row == nullptr) {
            // No reference linker honours it in a directive: link.exe warns LNK4229 and links, GNU ld warns
            // "unrecognized" and links (✔MEASURED 2026-10-07), so a DSS link warns and links too.
            out.warnings.push_back(std::format(
                "directive '{}' asks the final linker for '{}', which the format's directive vocabulary does not "
                "list and no reference linker honours in a directive (link.exe warns LNK4229, GNU ld "
                "\"unrecognized\"): it is ignored",
                tok, option));
            out.handOn.push_back(handOnText);
            continue;
        }
        std::string const spelled = std::string{tok.front()} + std::string{option} + ":";
        // A request the link decides ACROSS its units carries its row's two answers to that decision. The format's
        // loader requires both on such a row, so one that states none is a vocabulary built in memory: refused
        // rather than given a precedence nobody stated.
        bool const acrossUnits = isDecidedAcrossUnits(row->meaning);
        if (acrossUnits && (!row->unitPrecedence.has_value() || !row->inSharedLibrary.has_value())) {
            return std::unexpected(std::format(
                "directive '{}' is decided across the link's units, and the format's vocabulary row for '{}' states "
                "no 'unitPrecedence' or no 'inSharedLibrary': there is no precedence to assume",
                tok, row->option));
        }
        auto const image = [&](UnitImageRequestValue v) {
            UnitImageRequest request{tok, std::move(v)};
            if (acrossUnits) {
                request.precedence            = *row->unitPrecedence;
                request.inSharedLibrary       = *row->inSharedLibrary;
                request.inSharedLibraryReason = row->inSharedLibraryReason;
            }
            out.image.push_back(std::move(request));
            out.handOn.push_back(handOnText);
        };
        auto const noValue = [&]() -> std::optional<std::string> {
            if (colon == std::string_view::npos) return std::nullopt;
            return std::format("directive '{}' takes no value ('{}{}')", tok, tok.front(), option);
        };
        switch (row->meaning) {
            case LinkerDirectiveMeaning::Ignored:
                out.handOn.push_back(handOnText);
                break;
            case LinkerDirectiveMeaning::DependentLibrary:
                // A library to search (P69 fold 3f): the unit's, restated by the relocatable writer from the field
                // (`CoffDirectiveStatements::dependentLibraries`), so it is not handed on as well. One that names
                // no library asks nothing a final linker could search: handed on as it stands (clang writes
                // `/DEFAULTLIB:.lib` for `comment(lib)` with no string, ✔MEASURED P69 fold 3f).
                if (value.empty()) {
                    out.handOn.push_back(handOnText);
                    break;
                }
                out.dependentLibraries.emplace_back(value);
                break;
            case LinkerDirectiveMeaning::Refused:
                // Refused by the link that makes an IMAGE (`linker::link`); a relocatable artifact hands it on to
                // its final linker, which honours it.
                image(UnitUnhonourableRequest{row->reason});
                break;
            case LinkerDirectiveMeaning::HideSymbols:
                for (auto& name : detail::splitOnComma(value)) {
                    if (name.empty()) {
                        return std::unexpected(std::format("directive '{}' names an empty symbol", tok));
                    }
                    out.hidden.push_back(std::move(name));
                }
                break;
            case LinkerDirectiveMeaning::ExportSymbol: {
                // `[exported=]internal[,DATA][,PRIVATE]` (✔MEASURED 2026-10-07: link.exe and lld-link export a
                // rename under its new name only, and keep a PRIVATE one in the image's table). An ordinal
                // (`,@n`), NONAME and a forwarder (`exported=dll.function`) ask for an export table DSS does not
                // build.
                auto parts = detail::splitOnComma(value);
                if (parts.empty() || parts[0].empty()) {
                    return std::unexpected(std::format("directive '{}' names no symbol to export", tok));
                }
                UnitExportRequest e;
                e.spelled = tok;
                std::string_view const head = parts[0];
                std::size_t const eq = head.find('=');
                e.exportedName = std::string{head.substr(0, eq)};
                e.internalName = eq == std::string_view::npos ? e.exportedName : std::string{head.substr(eq + 1)};
                if (e.exportedName.empty() || e.internalName.empty()
                    || e.internalName.find('=') != std::string::npos) {
                    return std::unexpected(std::format(
                        "directive '{}' must read '{}[<exported>=]<name>', two non-empty names", tok, spelled));
                }
                // A forwarder, an ordinal and NONAME ask for an export table a DSS image does not write: refused
                // by the link that makes an image, handed on by a relocatable artifact. A FORWARDER is spelled
                // with its `=` (`exported=dll.function`): ✔MEASURED 2026-10-08 (link.exe 14.44.35228 and lld-link
                // 19.1.5 on clang 19.1.5 objects), `/export:f.v2` -- in a directive and on the command line --
                // exports the SYMBOL `f.v2` at its address, a dotted name nothing defines is refused as an
                // unresolved symbol, and only `fwd=kernel32.Sleep` is "forwarded to". So a dotted name with no `=`
                // is a symbol's name like any other (review-xa4 NIT 8).
                if (eq != std::string_view::npos && e.internalName.find('.') != std::string::npos) {
                    image(UnitUnhonourableRequest{std::format(
                        "it asks for a FORWARDER export (to '{}'), which a DSS image's export table does not "
                        "write; '{}[<exported>=]<name>[,DATA][,PRIVATE]' is honoured",
                        e.internalName, spelled)});
                    break;
                }
                std::optional<std::string> unassigned;
                for (std::size_t k = 1; k < parts.size(); ++k) {
                    std::string_view const attr = parts[k];
                    bool const ordinal = attr.size() > 1 && attr.front() == '@'
                                      && std::all_of(attr.begin() + 1, attr.end(), [](char c) {
                                             return std::isdigit(static_cast<unsigned char>(c)) != 0;
                                         });
                    if (detail::equalsIgnoringCase(attr, "DATA")) {
                        e.isData = true;
                    } else if (detail::equalsIgnoringCase(attr, "PRIVATE")) {
                        e.isPrivate = true;
                    } else if (ordinal || detail::equalsIgnoringCase(attr, "NONAME")) {
                        if (!unassigned.has_value()) unassigned = std::string{attr};
                    } else {
                        return std::unexpected(std::format(
                            "directive '{}' must read '{}[<exported>=]<name>[,@<ordinal>[,NONAME]][,DATA][,PRIVATE]'"
                            " ('{}' is none of those)",
                            tok, spelled, attr));
                    }
                }
                if (unassigned.has_value()) {
                    image(UnitUnhonourableRequest{std::format(
                        "it asks for an export by ORDINAL or NONAME ('{}'), which a DSS export table does not "
                        "assign; '{}[<exported>=]<name>[,DATA][,PRIVATE]' is honoured",
                        *unassigned, spelled)});
                    break;
                }
                e.precedence            = *row->unitPrecedence;
                e.inSharedLibrary       = *row->inSharedLibrary;
                e.inSharedLibraryReason = row->inSharedLibraryReason;
                out.exports.push_back(std::move(e));
                out.handOn.push_back(handOnText);
                break;
            }
            case LinkerDirectiveMeaning::StackSize:
            case LinkerDirectiveMeaning::HeapSize: {
                // A stack commit above its reserve is lld-link's to take (link.exe refuses it, LNK1229; lld-link
                // writes it and the image runs); a heap's no reference takes (link.exe refuses it and lld-link
                // takes no `/HEAP:` directive). ✔MEASURED 2026-10-07.
                bool const heap = row->meaning == LinkerDirectiveMeaning::HeapSize;
                auto const rc = detail::parseReserveCommit(value);
                if (!rc.has_value() || (heap && rc->commit.has_value() && *rc->commit > rc->reserve)) {
                    return std::unexpected(std::format(
                        "directive '{}' must read '{}<reserve>[,<commit>]', each in decimal or C notation{}", tok,
                        spelled,
                        heap ? ", the commit no larger than the reserve (link.exe refuses a larger one, LNK1229)"
                             : ""));
                }
                if (heap) {
                    image(UnitHeapRequest{*rc});
                } else {
                    image(UnitStackRequest{*rc});
                }
                break;
            }
            case LinkerDirectiveMeaning::Subsystem: {
                std::size_t const comma = value.find(',');
                std::string_view const name = value.substr(0, comma);
                PeDirectiveNamedValue const* known = nullptr;
                for (auto const& s : row->subsystems) {
                    if (detail::equalsIgnoringCase(s.name, name)) known = &s;
                }
                if (known == nullptr) {
                    // A subsystem a reference names and no DSS image starts under: refused by the link that makes
                    // an image, handed on by a relocatable artifact (its final linker may have the startup).
                    PeDirectiveNamedValue const* unsupported = nullptr;
                    for (auto const& u : row->unsupportedSubsystems) {
                        if (detail::equalsIgnoringCase(u.name, name)) unsupported = &u;
                    }
                    if (unsupported != nullptr) {
                        // A SUBSYSTEM request all the same (`UnitSubsystemRequest::unsupported`): settled against
                        // the other units' by the option's precedence, and refused where it would stand.
                        image(UnitSubsystemRequest{
                            SubsystemSetting{},
                            std::format("it names the subsystem '{}': {}", name, unsupported->reason)});
                        break;
                    }
                    std::string listed;
                    for (auto const& s : row->subsystems) listed += (listed.empty() ? "" : ", ") + s.name;
                    return std::unexpected(std::format(
                        "directive '{}' names the subsystem '{}', which the format's directive vocabulary does not "
                        "list (it lists {}): a DSS image starts in its target's own startup, which runs under those",
                        tok, name, listed));
                }
                SubsystemSetting setting{known->value, std::nullopt};
                if (comma != std::string_view::npos) {
                    auto const version = detail::parseVersionPair(value.substr(comma + 1));
                    if (!version.has_value()) {
                        return std::unexpected(std::format(
                            "directive '{}' must read '{}<name>[,<major>[.<minor>]]'", tok, spelled));
                    }
                    auto const below = [](VersionPair a, VersionPair b) {
                        return a.major < b.major || (a.major == b.major && a.minor < b.minor);
                    };
                    if (row->minimumVersion.has_value() && below(*version, *row->minimumVersion)) {
                        // link.exe warns LNK4010 and keeps its default version (✔MEASURED 2026-10-07: 5.01 for
                        // x64), and so does a DSS link -- the subsystem itself still stands.
                        out.warnings.push_back(std::format(
                            "directive '{}' states subsystem version {}.{:02}, below the {}.{:02} the format's "
                            "linker takes for this machine: the format's default version is kept (link.exe "
                            "LNK4010)",
                            tok, version->major, version->minor, row->minimumVersion->major,
                            row->minimumVersion->minor));
                    } else {
                        setting.version = *version;
                    }
                }
                // The startups this subsystem starts a program at, by their verbs (`subsystemStartups`, P69 fold
                // 3f): what the request ranks first in the program's entry election where it stands.
                std::vector<EntryMaterialization> verbs;
                for (auto const& s : row->subsystemStartups) {
                    if (detail::equalsIgnoringCase(s.subsystem, known->name)) verbs = s.verbs;
                }
                image(UnitSubsystemRequest{setting, {}, std::move(verbs)});
                break;
            }
            case LinkerDirectiveMeaning::EntryPoint: {
                if (value.empty() || value.find(',') != std::string_view::npos) {
                    return std::unexpected(std::format(
                        "directive '{}' must name exactly one symbol ('{}<name>')", tok, spelled));
                }
                // A runtime startup no DSS image provides: refused by the link that makes an image, handed on by a
                // relocatable artifact (its final linker links that runtime).
                PeDirectiveNamedValue const* unsupported = nullptr;
                for (auto const& u : row->unsupportedStartups) {
                    if (u.name == value) unsupported = &u;
                }
                if (unsupported != nullptr) {
                    // An ENTRY request all the same (`UnitEntryRequest::unsupported`, review-xa4 NIT 7): dropped
                    // and warned for a member the archive search pulls, like any entry; settled against the other
                    // units' by the option's precedence; refused where it would stand.
                    image(UnitEntryRequest{
                        std::string{value}, false,
                        std::format("it names the C runtime's '{}' startup: {}", value, unsupported->reason)});
                    break;
                }
                // The C runtime's own startup carries the verbs it calls an entry with: the standing one restricts
                // the program's entry election to them (P69 fold 3e, class C's N5).
                auto const startup = std::find_if(row->runtimeStartups.begin(), row->runtimeStartups.end(),
                                                  [&](PeRuntimeStartup const& s) { return s.name == value; });
                if (startup != row->runtimeStartups.end()) {
                    image(UnitEntryRequest{std::string{value}, true, {}, startup->verbs});
                } else {
                    image(UnitEntryRequest{std::string{value}, false});
                }
                break;
            }
            case LinkerDirectiveMeaning::SectionAttributes: {
                std::size_t const comma = value.find(',');
                std::string_view const section = value.substr(0, comma);
                std::string_view const attrs =
                    comma == std::string_view::npos ? std::string_view{} : value.substr(comma + 1);
                auto const masks = attrs.find(',') == std::string_view::npos
                                       ? detail::parseSectionAttributes(attrs)
                                       : std::nullopt;
                if (section.empty() || !masks.has_value()) {
                    return std::unexpected(std::format(
                        "directive '{}' must read '{}<section>,<attributes>' with attributes of the letters "
                        "E R W S D K P and '!' (link.exe refuses any other, and `ALIGN=` without /DRIVER: "
                        "LNK1117 / LNK1137)",
                        tok, spelled));
                }
                image(UnitSectionAttributeRequest{
                    SectionAttributeRequest{std::string{section}, masks->first, masks->second, tok}});
                break;
            }
            case LinkerDirectiveMeaning::ImageChecksum:
            case LinkerDirectiveMeaning::DllImage:
                if (auto const bad = noValue()) return std::unexpected(*bad);
                if (row->meaning == LinkerDirectiveMeaning::ImageChecksum) {
                    image(UnitChecksumRequest{});
                } else {
                    image(UnitDllImageRequest{});
                }
                break;
            case LinkerDirectiveMeaning::ImageVersion: {
                auto const version = detail::parseVersionPair(value);
                if (!version.has_value()) {
                    return std::unexpected(std::format(
                        "directive '{}' must read '{}<major>[.<minor>]'", tok, spelled));
                }
                image(UnitImageVersionRequest{*version});
                break;
            }
            case LinkerDirectiveMeaning::ImageBase: {
                // link.exe refuses a base that is not a multiple of 64 KiB (LNK1224) and takes any multiple, 0
                // included (LNK4281 warns of ASLR, the image runs): ✔MEASURED 2026-10-07.
                auto const address = detail::parseCNumber(value);
                if (!address.has_value() || (*address % 0x10000u) != 0u) {
                    return std::unexpected(std::format(
                        "directive '{}' must read '{}<address>': a multiple of 64 KiB in decimal or C notation "
                        "(link.exe refuses any other, LNK1224; DSS takes no '@file' form)",
                        tok, spelled));
                }
                image(UnitImageBaseRequest{*address});
                break;
            }
            case LinkerDirectiveMeaning::SectionAlignment: {
                auto const bytes = detail::parseCNumber(value);
                if (!bytes.has_value()) {
                    return std::unexpected(std::format(
                        "directive '{}' must read '{}<bytes>' in decimal or C notation", tok, spelled));
                }
                if (*bytes == 0u || (*bytes & (*bytes - 1u)) != 0u || *bytes > 0x80000000u) {
                    // link.exe warns LNK4043 on one that is not a power of two and keeps its default
                    // (✔MEASURED 2026-10-07: 0x3000), and so does a DSS link.
                    out.warnings.push_back(std::format(
                        "directive '{}' asks for a section alignment of {} bytes, which is not a power of two: the "
                        "format's own alignment is kept (link.exe LNK4043)",
                        tok, *bytes));
                    out.handOn.push_back(handOnText);
                    break;
                }
                image(UnitSectionAlignmentRequest{static_cast<std::uint32_t>(*bytes)});
                break;
            }
            case LinkerDirectiveMeaning::ManifestDependency:
                // `<identity>`: the attributes of the manifest's `assemblyIdentity`, which link.exe and lld-link copy
                // verbatim (✔MEASURED 2026-11-17); cl writes the value, or the whole directive, quoted, which the
                // tokenizer has already unwrapped.
                if (value.empty()) {
                    return std::unexpected(std::format(
                        "directive '{}' must read '{}<identity>': the attributes of the assembly the image depends on",
                        tok, spelled));
                }
                image(UnitManifestDependencyRequest{std::string{value}});
                break;
            case LinkerDirectiveMeaning::MismatchCheck: {
                std::size_t const eq = value.find('=');
                if (eq == std::string_view::npos || eq == 0) {
                    return std::unexpected(std::format(
                        "directive '{}' must read '{}<key>=<value>'", tok, spelled));
                }
                image(UnitMismatchCheck{std::string{value.substr(0, eq)}, std::string{value.substr(eq + 1)}});
                break;
            }
            case LinkerDirectiveMeaning::IncludeSymbol:
                // ONE symbol, as link.exe's and lld-link's own `/INCLUDE:` take it.
                if (value.empty() || value.find(',') != std::string_view::npos) {
                    return std::unexpected(std::format(
                        "directive '{}' must name exactly one symbol ('{}<name>')", tok, spelled));
                }
                out.included.emplace_back(value);
                break;
            case LinkerDirectiveMeaning::AlternateName: {
                std::size_t const eq = value.find('=');
                if (eq == std::string_view::npos || eq == 0 || eq + 1 == value.size()
                    || value.find('=', eq + 1) != std::string_view::npos) {
                    return std::unexpected(std::format(
                        "directive '{}' must read '{}<name>=<fallback>', two non-empty names", tok, spelled));
                }
                out.alternates.push_back(
                    CoffAlternateName{std::string{value.substr(0, eq)}, std::string{value.substr(eq + 1)}});
                break;
            }
            case LinkerDirectiveMeaning::CommonAlignment: {
                // `name,log2` — GNU as and clang's windows-gnu target both write the name QUOTED
                // (` -aligncomm:"c32",5`, ✔MEASURED 2026-10-06: mingw-w64 gcc 13.2.0 and clang 18.1.3), which
                // the tokenizer has already unwrapped; a bare name reads the same.
                auto const parts = detail::splitOnComma(value);
                unsigned log2 = 0;
                bool parsed = false;
                if (parts.size() == 2 && !parts[0].empty() && !parts[1].empty()) {
                    auto const* const first = parts[1].data();
                    auto const* const last = first + parts[1].size();
                    auto const [end, ec] = std::from_chars(first, last, log2);
                    parsed = ec == std::errc{} && end == last;
                }
                if (!parsed || log2 > kMaxCoffCommonAlignmentLog2) {
                    return std::unexpected(std::format(
                        "directive '{}' must read '{}<name>,<n>' with n in [0, {}]: the common's alignment "
                        "as a power of two, at most the 8192 bytes a COFF section can state",
                        tok, spelled, static_cast<unsigned>(kMaxCoffCommonAlignmentLog2)));
                }
                out.commonAlignments.push_back(
                    CoffCommonAlignment{parts[0], static_cast<std::uint8_t>(log2)});
                break;
            }
        }
    }
    return out;
}

namespace detail {

// The first row the vocabulary declares with `meaning` — the option the writer
// spells it with — or an error naming what is missing.
[[nodiscard]] inline std::expected<PeLinkerDirectiveRow const*, std::string>
writerRowFor(PeLinkerDirectives const& vocab, LinkerDirectiveMeaning meaning, std::string_view what) {
    for (auto const& r : vocab.directives) {
        if (r.meaning == meaning) {
            if (vocab.optionPrefixes.empty()) break;
            return &r;
        }
    }
    return std::unexpected(std::format("the format's directive vocabulary declares no '{}' option to state {} "
                                       "with",
                                       kLinkerDirectiveMeaningTable.name(meaning), what));
}

// A name the writer spells verbatim: one holding whitespace, a comma, a quote
// or a NUL — or, in an `alternateName`, an `=` — reads back as something else.
[[nodiscard]] inline std::optional<std::string>
unspellableName(std::string_view what, std::string_view name, std::string_view option, bool refuseEquals) {
    for (char const c : name) {
        if (c == ' ' || c == '\t' || c == ',' || c == '"' || c == '\0' || (refuseEquals && c == '=')) {
            return std::format("{} '{}' cannot be spelled in a '{}' directive: a name holding whitespace, a "
                               "comma{} or a quote reads back as a different list",
                               what, name, option, refuseEquals ? ", an '='" : "");
        }
    }
    return std::nullopt;
}

}  // namespace detail

// What a relocatable object hands its final linker — the requests a DSS writer
// states (the reader's `CoffDirectiveRequests`, the subset an object of DSS's
// can carry): the definitions its visibility hides, the names an `/INCLUDE:`
// required of the object it was read from, the fallbacks it carries, and the
// alignment of each COMMON it hands on that the final linker would otherwise
// align less (`CommonsStatement::statesItsAlignment`); and, VERBATIM, every other
// directive of a foreign unit it holds (`UnitLinkerRequests::handOn`: an
// export, an image request, an ignored or warned option), so its final linker
// is asked what the unit asked.
struct CoffDirectiveStatements {
    std::span<std::string const>         hidden;
    std::span<std::string const>         included;
    std::span<CoffAlternateName const>   alternates;
    std::span<CoffCommonAlignment const> commonAlignments;
    std::span<std::string const>         handOn = {};
    // P69 fold 3f: the libraries the object asks its final linker to search (`UnitLinkerRequests::
    // dependentLibraries`), each written with the vocabulary's `dependentLibrary` option (`-defaultlib`), its name
    // QUOTED as cl quotes it (`/DEFAULTLIB:"user32"`, ✔MEASURED P69 fold 3f, run 20261117-143416-a9be43b7).
    std::span<std::string const>         dependentLibraries = {};
};

// The writer's directive text: ` <prefix><option>:<value>` per request, hidden
// then included then alternates then common alignments then libraries, each in
// the order given, with the FIRST declared prefix and the first option declared for the
// meaning, then each handed-on token as it was read (empty when there is
// nothing to state). Every value reads back through `parseCoffLinkerDirectives`
// as the request it states.
[[nodiscard]] inline std::expected<std::string, std::string>
coffLinkerDirectiveText(CoffDirectiveStatements const& s, PeLinkerDirectives const& vocab) {
    std::string text;
    auto const state = [&](LinkerDirectiveMeaning meaning, std::string_view what, auto const& values,
                           auto const& spell) -> std::optional<std::string> {
        if (values.empty()) return std::nullopt;
        auto const row = detail::writerRowFor(vocab, meaning, std::format("a {}", what));
        if (!row.has_value()) return row.error();
        for (auto const& v : values) {
            auto spelled = spell(what, v, (*row)->option);
            if (!spelled.has_value()) return spelled.error();
            text += std::format(" {}{}:{}", vocab.optionPrefixes.front(), (*row)->option, *spelled);
        }
        return std::nullopt;
    };
    auto const plainName = [](std::string_view what, std::string const& name,
                              std::string_view option) -> std::expected<std::string, std::string> {
        if (auto const bad = detail::unspellableName(what, name, option, /*refuseEquals=*/false)) {
            return std::unexpected(*bad);
        }
        return name;
    };
    auto const alternate = [](std::string_view what, CoffAlternateName const& a,
                              std::string_view option) -> std::expected<std::string, std::string> {
        for (std::string const* n : {&a.name, &a.fallback}) {
            if (auto const bad = detail::unspellableName(what, *n, option, /*refuseEquals=*/true)) {
                return std::unexpected(*bad);
            }
        }
        return a.name + "=" + a.fallback;
    };
    if (auto const e = state(LinkerDirectiveMeaning::HideSymbols, "hidden definition", s.hidden, plainName)) {
        return std::unexpected(*e);
    }
    if (auto const e = state(LinkerDirectiveMeaning::IncludeSymbol, "required symbol", s.included, plainName)) {
        return std::unexpected(*e);
    }
    if (auto const e = state(LinkerDirectiveMeaning::AlternateName, "fallback name", s.alternates, alternate)) {
        return std::unexpected(*e);
    }
    auto const commonAlignment = [](std::string_view what, CoffCommonAlignment const& c,
                                    std::string_view option) -> std::expected<std::string, std::string> {
        if (auto const bad = detail::unspellableName(what, c.name, option, /*refuseEquals=*/false)) {
            return std::unexpected(*bad);
        }
        if (c.log2 > kMaxCoffCommonAlignmentLog2) {
            return std::unexpected(std::format("{} of '{}' is 2^{} bytes, past the 2^{} a COFF section can state",
                                               what, c.name, static_cast<unsigned>(c.log2),
                                               static_cast<unsigned>(kMaxCoffCommonAlignmentLog2)));
        }
        return std::format("{},{}", c.name, static_cast<unsigned>(c.log2));
    };
    if (auto const e = state(LinkerDirectiveMeaning::CommonAlignment, "common's alignment", s.commonAlignments,
                             commonAlignment)) {
        return std::unexpected(*e);
    }
    // A library, quoted as cl quotes it: the tokenizer reads `"..."` inside a token as the text between, so a name
    // holding a space is one library. A backslash is a path's and stands as written, save the ones that END the name,
    // doubled so the closing quote stays a quote (the Windows command-line rule `detail::requoteDirectiveToken`
    // follows). An empty name, or one holding a quote or a NUL -- which no Windows file name holds -- has no
    // spelling that reads back as itself.
    auto const quotedLibrary = [](std::string_view what, std::string const& name,
                                  std::string_view option) -> std::expected<std::string, std::string> {
        if (name.empty() || name.find('"') != std::string::npos || name.find('\0') != std::string::npos) {
            return std::unexpected(std::format(
                "{} '{}' cannot be spelled in a '{}' directive: an empty name, or one holding a quote or a NUL, "
                "reads back as another",
                what, name, option));
        }
        std::size_t const kept = name.find_last_not_of('\\') + 1;   // 0 when the name is backslashes alone
        return "\"" + name + std::string(name.size() - kept, '\\') + "\"";
    };
    if (auto const e = state(LinkerDirectiveMeaning::DependentLibrary, "library to search", s.dependentLibraries,
                             quotedLibrary)) {
        return std::unexpected(*e);
    }
    for (auto const& tok : s.handOn) {
        if (tok.empty() || tok.find('\0') != std::string::npos) {
            return std::unexpected(std::string{"a handed-on directive is empty or holds a NUL"});
        }
        text += " " + tok;
    }
    return text;
}

// The writer's text for these hidden definitions alone: ` <prefix><option>:<name>`
// per name, in the order given (empty when there are none).
[[nodiscard]] inline std::expected<std::string, std::string>
coffHideDirectiveText(std::span<std::string const> names, PeLinkerDirectives const& vocab) {
    return coffLinkerDirectiveText(CoffDirectiveStatements{names, {}, {}, {}}, vocab);
}

// ONE request as a relocatable artifact hands it on (`UnitLinkerRequests::
// handOn`): the writer's text for `one` without the separator that begins
// every token of a list — or WHY it has none, in words that are true of the
// case. (The COFF reader's two sites that need a token each tested the text
// themselves and said "its spelling is empty" of any text that was not one
// token, empty or not.)
[[nodiscard]] inline std::expected<std::string, std::string>
coffDirectiveToken(CoffDirectiveStatements const& one, PeLinkerDirectives const& vocab) {
    auto text = coffLinkerDirectiveText(one, vocab);
    if (!text.has_value()) return text;
    if (text->empty()) {
        return std::unexpected(std::string{"it states no request, so there is no directive to write"});
    }
    if (text->front() != ' ' || text->find(' ', 1) != std::string::npos) {
        return std::unexpected(
            std::format("the vocabulary spells it '{}', which is not ONE directive token", *text));
    }
    return text->substr(1);
}

// ── ONE UNIT'S DIRECTIVES, FOLDED AND APPLIED: THE ONE HOME (P69 fold 3f) ────
//
// What a unit's directives make of it is ONE rule however the directives reach a
// link: an object's directive sections, read by the COFF object reader, and a
// COMPILED unit's `#pragma comment(linker, ...)` texts, applied by the compile
// pipeline (`pe::applyUnitLinkerDirectives`) -- so a unit compiled to an object
// and the same unit compiled straight into a link carry one module. Two steps:
//   * the FOLD: every text the unit states, parsed against the vocabulary
//     (`parseCoffLinkerDirectives`) and gathered in order -- the requests the
//     link decides across its units (exports, image requests, libraries, the
//     tokens a relocatable artifact hands on, the warnings), and the ones judged
//     against what the unit defines (hides, includes, alternate names, common
//     alignments), each once in the order first stated;
//   * the COMPLETION: the latter applied to the unit's rows (`ExternImport`), once
//     what it defines is known.
// What a hide makes of the unit's OWN definition is the caller's: the reader
// reads the definition back Hidden as it reads its record, a compiled unit's is
// stated on its `ModuleSymbol`.

// One `/alternatename:` a unit states, and where it stated it -- named in the warning when another statement of the
// name stands over it (`linker::standingFallback`).
struct CoffStatedAlternate {
    CoffAlternateName alternate;
    std::string       statedBy;   // "the object's '.drectve'", "the unit's `#pragma comment(linker, ...)`"
};

// Every directive text one unit states, folded.
struct CoffUnitDirectives {
    std::vector<std::string>                      hidden;       // in directive order, once each
    std::vector<std::string>                      included;     // in directive order, once each
    std::vector<CoffStatedAlternate>              alternates;   // in directive order, once each: the one that stands
    std::unordered_map<std::string, std::uint8_t> commonAlignmentLog2;   // the largest asked, per name
    std::unordered_set<std::string>               hiddenSet;    // `hidden`, for lookup
    // The requests the link decides across its units, and the ones a relocatable artifact hands on.
    UnitLinkerRequests                            requests;
};

// Fold one directive text into `into`. `who` names the unit in a refusal ("the object's", "the unit's"),
// `carrier` the text ("'.drectve'"), `warningPrefix` begins each warning the link that makes an image says;
// `reporter` hears which of two fallbacks of one name stands. The error is the refusal's text: the directives cannot
// be honoured.
[[nodiscard]] inline std::expected<void, std::string>
foldCoffLinkerDirectives(CoffUnitDirectives& into, std::string_view text, PeLinkerDirectives const& vocab,
                         std::string_view who, std::string_view carrier, std::string_view warningPrefix,
                         DiagnosticReporter& reporter) {
    auto const requests = parseCoffLinkerDirectives(text, vocab);
    if (!requests.has_value()) {
        return std::unexpected(std::format("{} linker directives ({}) cannot be honoured: {}.", who, carrier,
                                           requests.error()));
    }
    // Said by the link that makes an image (`decideUnitLinkerRequests`); a relocatable artifact hands the token on
    // to its final linker instead.
    for (auto const& warning : requests->warnings) {
        into.requests.warnings.push_back(std::string{warningPrefix} + warning);
    }
    for (auto const& name : requests->hidden) {
        if (into.hiddenSet.insert(name).second) into.hidden.push_back(name);
    }
    into.requests.image.insert(into.requests.image.end(), requests->image.begin(), requests->image.end());
    into.requests.exports.insert(into.requests.exports.end(), requests->exports.begin(), requests->exports.end());
    into.requests.handOn.insert(into.requests.handOn.end(), requests->handOn.begin(), requests->handOn.end());
    // A library two directives name is searched once (link.exe searches a repeated `/DEFAULTLIB:` once).
    for (auto const& lib : requests->dependentLibraries) {
        auto& libs = into.requests.dependentLibraries;
        if (std::find(libs.begin(), libs.end(), lib) == libs.end()) libs.push_back(lib);
    }
    for (auto const& name : requests->included) {
        if (std::find(into.included.begin(), into.included.end(), name) == into.included.end()) {
            into.included.push_back(name);
        }
    }
    // ★ Of two directives of THIS unit for one name, the first in the order the unit states them stands (P69 H6c2,
    // ✔MEASURED u8: link.exe takes the first of one object's directive section, silently) -- one statement each, at
    // one place, so the one met first stands, and the other is warned about (`linker::standingFallback`).
    std::string const statedBy = std::format("{} {}", who, carrier);
    for (auto const& alt : requests->alternates) {
        auto const prior = std::find_if(into.alternates.begin(), into.alternates.end(),
                                        [&](CoffStatedAlternate const& a) { return a.alternate.name == alt.name; });
        if (prior == into.alternates.end()) {
            into.alternates.push_back({alt, statedBy});
        } else if (prior->alternate.fallback != alt.fallback) {
            linker::FallbackStatement standing{prior->alternate.fallback, linker::FallbackSource::Directive,
                                               std::nullopt, prior->statedBy};
            linker::standingFallback(alt.name, standing,
                                     {alt.fallback, linker::FallbackSource::Directive, std::nullopt, statedBy},
                                     reporter);
            prior->alternate.fallback = standing.fallback;
        }
    }
    for (auto const& c : requests->commonAlignments) {
        auto& log2 = into.commonAlignmentLog2[c.name];
        log2 = std::max(log2, c.log2);
    }
    // ⓘ One name both hidden and exported is EXPORTED: an explicit export wins over an exclusion, which only keeps
    // a name out of the automatic exports (✔MEASURED 2026-10-07, lld-link -lldmingw and GNU ld 2.42 on one object
    // stating both). The definition reads back Hidden and the image writer exports it by name all the same.
    return {};
}

// What the completion asks of the unit: what it DEFINES, and its rows.
struct CoffDirectiveRowsOf {
    // Does the unit make an EXTERNAL definition of the name? A COMMON is one: it defines storage, though it is a row.
    std::function<bool(std::string const&)> definesExternally;
    // Was the name's hide answered by the unit's own definition (read back, or stated, Hidden)?
    std::function<bool(std::string const&)> hideAnswered;
    // The unit's reference rows, and a fresh id for a row no record of the unit owns (the unit's id space's door):
    // nullopt when that door REFUSED the id -- reported there, by name -- which refuses the completion (P69 f26e, B1).
    std::vector<ExternImport>*                        rows = nullptr;
    std::function<std::optional<SymbolId>()>          freshId;
};

// The completion's refusal: its text, for the caller to report; or `SymbolIdsRefused`, a fresh row's id the unit's
// door refused, which the door has reported by name -- the caller refuses its output and says nothing more.
using CoffDirectivesRefusal = std::variant<std::string, SymbolIdsRefused>;

// The folded requests applied against what the unit defines: the alternate names, the includes, the exports'
// internal names, the hides of another unit's definitions. `requests` is the unit's (its exports are read, its
// hides and hand-on tokens grow); `reporter` hears a directive a weak external's own default stands over. The error
// is the refusal (`CoffDirectivesRefusal`).
[[nodiscard]] inline std::expected<void, CoffDirectivesRefusal>
completeCoffLinkerDirectives(CoffUnitDirectives const& d, CoffDirectiveRowsOf const& unit, UnitLinkerRequests& requests,
                             PeLinkerDirectives const& vocab, std::string_view who, DiagnosticReporter& reporter) {
    std::vector<ExternImport>& rows = *unit.rows;
    auto const rowNamed = [&](std::string const& name) -> std::optional<std::size_t> {
        for (std::size_t k = 0; k < rows.size(); ++k) {
            if (rows[k].mangledName == name) return k;
        }
        return std::nullopt;
    };
    // nullptr: the fresh id was REFUSED (reported by name at the door); the completion is refused.
    auto const freshRow = [&](std::string const& name) -> ExternImport* {
        std::optional<SymbolId> const id = unit.freshId();
        if (!id) return nullptr;
        ExternImport row;
        row.symbol      = *id;
        row.mangledName = name;
        // A directive states no code-vs-data kind; the definition the link finds decides it (the `__imp_X` fold's
        // rule, `ExternKindOrigin`).
        row.kindOrigin  = ExternKindOrigin::Pending;
        rows.push_back(std::move(row));
        return &rows.back();
    };

    // `/alternatename:A=B` (`LinkerDirectiveMeaning::AlternateName`), judged as the reference linkers judge it --
    // link-wide: ANY reference to A that nothing in the link defines resolves to B, and an A defined anywhere wins
    // (✔MEASURED 2026-10-06, link.exe 14.51 and lld-link 18). Only the link knows which, so the unit STATES it
    // rather than decides it: an A the unit defines makes the directive a no-op; otherwise A's reference -- its own
    // row, or a row that carries the directive for the other units' references when no code here names A -- gets the
    // fallback (`ExternImport::fallbackName`, decided by `linker::link`), and a B the unit does not define gets a row
    // of its own, so B is bound to its library and its archive member is pulled like any name. ⚠ NOT a second name
    // of B's body: that would bind THIS unit's own references to A to B even where another unit defines A, which no
    // reference linker does.
    for (auto const& [alt, statedBy] : d.alternates) {
        if (unit.definesExternally(alt.name)) continue;
        if (auto const row = rowNamed(alt.name); row.has_value()) {
            ExternImport& named = rows[*row];
            if (named.binding == SymbolBinding::Weak) {
                // ★ A WEAK EXTERNAL'S OWN DEFAULT STANDS OVER THE DIRECTIVE (P69 H6c2, ✔MEASURED u11: a MinGW gcc
                // object whose own `.drectve` gives its weak `aa` the fallback `cc` -- link.exe and GNU ld take the
                // weak's default, a defined one and the absolute 0 alike; GNU ld does not read the directive). The
                // row's default -- another undefined symbol, or none, the absolute 0 -- is kept, and the directive
                // is dropped and warned about (`linker::standingFallback`). Nothing refers to the dropped fallback,
                // so it gets no row of its own.
                linker::FallbackStatement standing{named.fallbackName, linker::FallbackSource::WeakDefault,
                                                   std::nullopt, "its weak external record"};
                linker::standingFallback(alt.name, standing,
                                         {alt.fallback, linker::FallbackSource::Directive, std::nullopt, statedBy},
                                         reporter);
                named.fallbackName = standing.fallback;
                continue;
            }
            named.fallbackName = alt.fallback;
        } else {
            ExternImport* const fresh = freshRow(alt.name);
            if (fresh == nullptr) return std::unexpected(SymbolIdsRefused{});   // refused by name at the door
            fresh->fallbackName = alt.fallback;
        }
        if (!unit.definesExternally(alt.fallback) && !rowNamed(alt.fallback).has_value()) {
            if (freshRow(alt.fallback) == nullptr) return std::unexpected(SymbolIdsRefused{});   // refused at the door
        }
    }

    // `/INCLUDE:X` (`LinkerDirectiveMeaning::IncludeSymbol`): a name the unit defines needs nothing more; any other
    // is a REQUIRED reference of the unit -- its own row when it has one. A name the vocabulary lists among the
    // platform C runtime's own symbols (`runtimeSymbols`: `_tls_used`, the TLS directory cl's TLS-CALLBACK idiom pulls
    // in beside a `.CRT$XL*` callback pointer) is refused by name with the row's reason: no DSS link defines it, and
    // the callback table it serves is one DSS does not carry, so neither an image nor a re-emitted object could keep
    // the callback the unit asked to run.
    PeLinkerDirectiveRow const* includeRow = nullptr;
    for (auto const& r : vocab.directives) {
        if (r.meaning == LinkerDirectiveMeaning::IncludeSymbol) includeRow = &r;
    }
    for (auto const& name : d.included) {
        if (unit.definesExternally(name)) {
            // The unit's OWN definition: the link needs nothing more of it, and no row states the request -- so a
            // relocatable artifact restates it in the vocabulary's own spelling (`handOn`), or its final linker
            // would never be asked to keep the definition the unit asked it to keep (P69 send-back 5, review-xa4
            // NIT 9).
            auto const token =
                coffDirectiveToken(CoffDirectiveStatements{{}, std::span<std::string const>{&name, 1}, {}, {}}, vocab);
            if (!token.has_value()) {
                return std::unexpected(std::format(
                    "{} include directive names its own definition '{}', and it cannot be handed on: {}.", who,
                    name, token.error()));
            }
            requests.handOn.push_back(*token);
            continue;
        }
        if (includeRow != nullptr) {
            for (auto const& runtime : includeRow->runtimeSymbols) {
                if (runtime.name == name) {
                    return std::unexpected(std::format(
                        "{} linker directive `/INCLUDE:{}` asks for a symbol of the platform's C runtime that no "
                        "DSS link defines: {}",
                        who, name, runtime.reason));
                }
            }
        }
        if (auto const row = rowNamed(name); row.has_value()) {
            rows[*row].requiredByDirective = true;
        } else {
            ExternImport* const fresh = freshRow(name);
            if (fresh == nullptr) return std::unexpected(SymbolIdsRefused{});   // refused by name at the door
            fresh->requiredByDirective = true;
        }
    }

    // `/EXPORT:` is a reference too: link.exe and lld-link pull the archive member that defines an exported name
    // nothing else names, and bind one only an import library defines (✔MEASURED 2026-10-07: `/EXPORT:fn` beside a
    // member defining `fn`, and `/EXPORT:puts` alone, each exported by both). So the internal name of an export the
    // unit does not define is a REQUIRED reference of it, as an `/INCLUDE:` makes one: the link's archive search and
    // library binding reach it, and the export then finds it.
    for (auto const& e : requests.exports) {
        if (unit.definesExternally(e.internalName)) continue;
        if (auto const row = rowNamed(e.internalName); row.has_value()) {
            rows[*row].requiredByDirective = true;
        } else {
            ExternImport* const fresh = freshRow(e.internalName);
            if (fresh == nullptr) return std::unexpected(SymbolIdsRefused{});   // refused by name at the door
            fresh->requiredByDirective = true;
        }
    }

    // A hide directive naming what the unit does not define hides that name WHEREVER the link defines it --
    // link-wide, as lld-link -lldmingw takes another object's `-exclude-symbols:` (✔MEASURED 2026-10-07); one that
    // names nothing anywhere is silent there too. The link applies it (`UnitLinkerRequests::hides`), and a
    // relocatable artifact restates it in the vocabulary's own spelling (`handOn`).
    for (auto const& name : d.hidden) {
        if (unit.hideAnswered(name)) continue;
        auto const token =
            coffDirectiveToken(CoffDirectiveStatements{std::span<std::string const>{&name, 1}, {}, {}, {}}, vocab);
        if (!token.has_value()) {
            return std::unexpected(std::format(
                "{} hide directive names '{}', which another unit must define, and it cannot be handed on: {}.", who,
                name, token.error()));
        }
        requests.hides.push_back(name);
        requests.handOn.push_back(*token);
    }
    return {};
}

}  // namespace dss::pe
