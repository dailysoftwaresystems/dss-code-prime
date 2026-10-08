#pragma once

#include <cstdint>
#include <optional>
#include <string>
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
// decides them across the link's units with the precedence link.exe gives each
// (`decideUnitLinkerRequests`) into the `DirectiveImageSettings` the image
// writer realizes, and a relocatable artifact hands every one it does not
// restate from its own rows on to its final linker, verbatim (`handOn`). Every
// unit but a foreign COFF object's carries an empty one.
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
struct UnitSubsystemRequest        { SubsystemSetting setting; };
// `runtimeStartup`: the name is the C runtime's own startup (the format's
// vocabulary lists it), which in a DSS image is the target's startup.
struct UnitEntryRequest            { std::string symbol; bool runtimeStartup = false; };
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

struct UnitImageRequest {
    std::string           spelled;   // the directive as the object wrote it, for diagnostics
    UnitImageRequestValue value;
};

// `/EXPORT:[exported=]internal[,DATA][,PRIVATE]` -- the image exports the
// definition named `internal` under `exported` (the same name unless renamed).
// PRIVATE keeps a name out of an import library only, which a DSS link does not
// write: the image's export table holds it all the same (✔MEASURED 2026-10-07,
// link.exe and lld-link).
struct UnitExportRequest {
    std::string internalName;
    std::string exportedName;
    bool        isData    = false;
    bool        isPrivate = false;
    std::string spelled;
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
    // not restate from its own rows (a hide of its own definition, an include,
    // an alternate name and a common's alignment are restated from those).
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
