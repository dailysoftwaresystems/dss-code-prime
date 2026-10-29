#pragma once

#include "core/types/enum_name_table.hpp"   // EnumNameTable (the two closed sets a directive row states)

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// ── WHAT A LINKED UNIT ASKS OF THE LINK BEYOND ITS OWN SYMBOLS ──────────────
// P69 round 4 (D-LK-COFF-READER-SKIPPED-EVERY-LINKER-DIRECTIVE).
//
// A foreign COFF object hands its final linker requests about the IMAGE it
// joins -- `/STACK:`, `/HEAP:`, `/SUBSYSTEM:`, `/ENTRY:`, `/SECTION:`,
// `/RELEASE`, `/VERSION:`, `/BASE:`, `/ALIGN:`, `/FAILIFMISMATCH:`, `/DLL` -- and
// requests about OTHER units' symbols: `/EXPORT:` and `-exclude-symbols:` mean a
// definition anywhere in the link (✔MEASURED 2026-10-07: link.exe 14.44 and
// lld-link 19.1.5 export a name another object defines; lld-link -lldmingw
// excludes one). The object reader reads them against its format's directive
// vocabulary (`LinkerDirectiveMeaning`) into this record; `linker::link`
// decides them across the link's units with the precedence the format's
// vocabulary states for each option (`decideUnitLinkerRequests`) into the
// `DirectiveImageSettings` the image writer realizes, and a relocatable artifact
// hands every one it does not restate from its own rows on to its final linker,
// verbatim (`handOn`). Every unit but a foreign COFF object's carries an empty
// one.
//
// Lives in `core/types` because `AssembledModule` (asm) carries it and the
// linker and the PE writer consume it; neither tier owns the other's headers.
namespace dss {

struct VersionPair {
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    friend bool operator==(VersionPair const&, VersionPair const&) = default;
};

// `/STACK:` and `/HEAP:` -- a reserve and, when stated, a commit.
struct ReserveCommit {
    std::uint64_t                reserve = 0;
    std::optional<std::uint64_t> commit;
    friend bool operator==(ReserveCommit const&, ReserveCommit const&) = default;
};

// `/SUBSYSTEM:name[,major[.minor]]` -- the IMAGE_SUBSYSTEM value the format's
// vocabulary row maps the name to, and the version when stated.
struct SubsystemSetting {
    std::uint16_t              value = 0;
    std::optional<VersionPair> version;
    friend bool operator==(SubsystemSetting const&, SubsystemSetting const&) = default;
};

// `/SECTION:name,attributes` read as link.exe reads the attribute letters: the
// section's Characteristics become `(c & ~clearMask) | setMask`.
struct SectionAttributeRequest {
    std::string   section;
    std::uint32_t setMask   = 0;
    std::uint32_t clearMask = 0;
    std::string   spelled;    // the directive as the object wrote it
    friend bool operator==(SectionAttributeRequest const&, SectionAttributeRequest const&) = default;
};

struct UnitStackRequest            { ReserveCommit sizes; };
struct UnitHeapRequest             { ReserveCommit sizes; };
// `unsupported`: non-empty when the directive names a subsystem a reference
// linker takes and NO DSS image starts under (the vocabulary's
// `unsupportedSubsystems`), holding the row's reason. It is a subsystem request
// all the same: settled against the other units' by the option's precedence,
// and refused by name only where it would STAND -- a reference linker that
// takes the last `/SUBSYSTEM:` links a NATIVE one a later CONSOLE replaces.
struct UnitSubsystemRequest {
    SubsystemSetting setting;
    std::string      unsupported{};
    friend bool operator==(UnitSubsystemRequest const&, UnitSubsystemRequest const&) = default;
};
// `runtimeStartup`: the name is the C runtime's own startup (the format's
// vocabulary lists it), which in a DSS image is the target's startup.
// `unsupported`: non-empty when the name is a runtime startup NO DSS image
// provides (the vocabulary's `unsupportedStartups`), holding the row's reason.
// It is an ENTRY request all the same -- settled against the other units' by
// the option's precedence, and dropped from a member the archive search pulls
// like any entry -- and the image link refuses it by name only where it would
// stand (P69 send-back 5, review-xa4 NIT 7).
struct UnitEntryRequest {
    std::string symbol;
    bool        runtimeStartup = false;
    std::string unsupported{};
    friend bool operator==(UnitEntryRequest const&, UnitEntryRequest const&) = default;
};
struct UnitSectionAttributeRequest { SectionAttributeRequest request; };
struct UnitChecksumRequest         {};
struct UnitImageVersionRequest     { VersionPair version; };
struct UnitImageBaseRequest        { std::uint64_t address = 0; };
struct UnitSectionAlignmentRequest { std::uint32_t bytes = 0; };
struct UnitMismatchCheck           { std::string key; std::string value; };
struct UnitDllImageRequest         {};
// A request a reference linker honours and a DSS IMAGE cannot (the format's
// vocabulary row is `refused`, its `reason` saying why and naming the row): an
// image link refuses it by name; a relocatable artifact hands it on to the
// final linker, which can.
struct UnitUnhonourableRequest     { std::string reason; };

using UnitImageRequestValue = std::variant<UnitStackRequest, UnitHeapRequest, UnitSubsystemRequest,
                                           UnitEntryRequest, UnitSectionAttributeRequest,
                                           UnitChecksumRequest, UnitImageVersionRequest,
                                           UnitImageBaseRequest, UnitSectionAlignmentRequest,
                                           UnitMismatchCheck, UnitDllImageRequest,
                                           UnitUnhonourableRequest>;

// ── THE TWO ANSWERS A FORMAT STATES ABOUT A REQUEST ONE LINK DECIDES ACROSS ITS
//    UNITS (P69 send-back 5, review-xa4 MINOR 6) ────────────────────────────
// Which unit's request STANDS when two units of one link state one option, and
// what a link that makes a SHARED LIBRARY does with the request, are facts of
// the format's own linkers -- link.exe keeps the first `/STACK:` where lld-link
// takes the last (✔MEASURED 2026-10-07) -- so neither is code. The format's
// directive vocabulary states both per option
// (`PeLinkerDirectiveRow::unitPrecedence` / `inSharedLibrary`, required on every
// row whose request is decided across units and refused at load on any other),
// the object reader stamps each request with its row's answers, and the
// decision (`linker::decideUnitLinkerRequests`) reads them from the REQUEST.
enum class UnitRequestPrecedence : std::uint8_t {
    First,     // the first unit's request stands; a later one is dropped
    Last,      // the last unit's request stands; an earlier one is dropped
    InOrder,   // every request applies, in unit order: a section's attribute
               // edits compose; a setting that holds ONE value ends at the last
};
inline constexpr EnumNameTable<UnitRequestPrecedence, 3> kUnitRequestPrecedenceTable{{{
    { UnitRequestPrecedence::First,   "first"   },
    { UnitRequestPrecedence::Last,    "last"    },
    { UnitRequestPrecedence::InOrder, "inOrder" },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kUnitRequestPrecedenceTable);

enum class SharedLibraryDisposition : std::uint8_t {
    Honoured,  // a shared library takes the request as a program does
    Ignored,   // warned and dropped, with the row's reason
    Refused,   // refused by name, with the row's reason
};
inline constexpr EnumNameTable<SharedLibraryDisposition, 3> kSharedLibraryDispositionTable{{{
    { SharedLibraryDisposition::Honoured, "honoured" },
    { SharedLibraryDisposition::Ignored,  "ignored"  },
    { SharedLibraryDisposition::Refused,  "refused"  },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kSharedLibraryDispositionTable);

struct UnitImageRequest {
    std::string           spelled;   // the directive as the object wrote it, for diagnostics
    UnitImageRequestValue value;
    // The object or archive member that stated the directive -- `main.obj`,
    // `libdir.a(member.obj)` -- which every diagnostic about it names. Stamped
    // where the unit is read from its file (`nameStatingUnit`); empty for a unit
    // handed to the link in memory.
    std::string              unit{};
    // The stating row's two answers (above). A request no link settles across
    // units (a mismatch check, `/DLL`, one no DSS image can carry) keeps the
    // defaults, which nothing reads for it.
    UnitRequestPrecedence    precedence      = UnitRequestPrecedence::InOrder;
    SharedLibraryDisposition inSharedLibrary = SharedLibraryDisposition::Honoured;
    std::string              inSharedLibraryReason{};
};

// `/EXPORT:[exported=]internal[,DATA][,PRIVATE]` -- the image exports the
// definition named `internal` under `exported` (the same name unless renamed).
// PRIVATE keeps a name out of an import library only, which a DSS link does not
// write: the image's export table holds it all the same (✔MEASURED 2026-10-07,
// link.exe and lld-link). Two units exporting ONE name for two definitions are
// settled by the row's `precedence`, like an image setting.
struct UnitExportRequest {
    std::string internalName;
    std::string exportedName;
    bool        isData    = false;
    bool        isPrivate = false;
    std::string spelled;
    std::string              unit{};   // as `UnitImageRequest::unit`
    UnitRequestPrecedence    precedence      = UnitRequestPrecedence::InOrder;
    SharedLibraryDisposition inSharedLibrary = SharedLibraryDisposition::Honoured;
    std::string              inSharedLibraryReason{};
    friend bool operator==(UnitExportRequest const&, UnitExportRequest const&) = default;
};

struct UnitLinkerRequests {
    std::vector<UnitImageRequest>  image;     // in directive order
    std::vector<UnitExportRequest> exports;   // link-wide
    // Names a hide directive (`-exclude-symbols:`) states that THIS unit does
    // not define: link-wide, applied to whichever unit does. The ones it
    // defines are read back as `SymbolVisibility::Hidden` by its reader.
    std::vector<std::string>       hides;
    // The directive tokens a RELOCATABLE artifact holding this unit restates
    // verbatim, in directive order: every one whose request the artifact does
    // not restate from its own rows (a hide of its own definition, an include
    // of a name it does not define, an alternate name and a common's alignment
    // are restated from those).
    std::vector<std::string>       handOn;
    // What a link that makes an IMAGE says about a request it drops as the
    // reference linkers drop it (an option no reference honours in a directive,
    // a value link.exe replaces with its default): one message each, reported
    // as warnings by that link. A relocatable artifact hands the token on
    // instead, and says nothing.
    std::vector<std::string>       warnings;

    [[nodiscard]] bool empty() const noexcept {
        return image.empty() && exports.empty() && hides.empty() && handOn.empty() && warnings.empty();
    }
};

// Name the unit that stated `requests` on every one of them -- the object's
// file name, or `archive(member)` -- so each diagnostic a link makes of a
// request or a stored warning says WHOSE directive it is (P69 send-back 5,
// review-xa4 NIT 11). Called once per unit, where the unit is read from its
// file, before anything folds two units' requests together.
inline void nameStatingUnit(UnitLinkerRequests& requests, std::string_view unit) {
    for (auto& r : requests.image) r.unit = unit;
    for (auto& e : requests.exports) e.unit = unit;
    for (auto& w : requests.warnings) w = "'" + std::string{unit} + "': " + w;
}

// One link's units' requests, folded into the merged module's in UNIT order --
// the order the link's precedence is read from (the first or the last unit's
// request wins, per option). A handed-on token stated by two units is handed on
// once: every final linker reads a repeated identical token as the one.
inline void appendUnitLinkerRequests(UnitLinkerRequests& into, UnitLinkerRequests const& from) {
    into.image.insert(into.image.end(), from.image.begin(), from.image.end());
    into.exports.insert(into.exports.end(), from.exports.begin(), from.exports.end());
    into.hides.insert(into.hides.end(), from.hides.begin(), from.hides.end());
    into.warnings.insert(into.warnings.end(), from.warnings.begin(), from.warnings.end());
    for (auto const& tok : from.handOn) {
        bool seen = false;
        for (auto const& have : into.handOn) seen = seen || have == tok;
        if (!seen) into.handOn.push_back(tok);
    }
}

// What `linker::link` decided from a link's units' requests, for the IMAGE
// writer: each field is the WINNING request as the unit wrote it, and the writer
// realizes it against its own format's defaults (the rounding and the commit
// rules are the writer's, because they are its format's).
struct DirectiveImageSettings {
    std::optional<ReserveCommit>         stack;
    std::optional<ReserveCommit>         heap;
    std::optional<SubsystemSetting>      subsystem;
    std::optional<VersionPair>           imageVersion;
    std::optional<std::uint64_t>         imageBase;
    std::optional<std::uint32_t>         sectionAlignment;
    bool                                 checksum = false;
    std::vector<SectionAttributeRequest> sections;   // applied in order
    std::vector<UnitExportRequest>       exports;    // one per exported name

    [[nodiscard]] bool empty() const noexcept {
        return !stack.has_value() && !heap.has_value() && !subsystem.has_value() && !imageVersion.has_value()
            && !imageBase.has_value() && !sectionAlignment.has_value() && !checksum && sections.empty()
            && exports.empty();
    }
};

}  // namespace dss
