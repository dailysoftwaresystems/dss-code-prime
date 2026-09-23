#include "link/runpath.hpp"

#include <algorithm>
#include <format>
#include <string>

namespace dss {

namespace {

// `${ORIGIN}` as the WHOLE entry, or followed by `/` — the only position both
// loaders expand their spelling in (see the header). Returns the length of the
// token when it leads, else 0.
[[nodiscard]] std::size_t leadingOriginTokenLength(std::string_view entry) noexcept {
    if (!entry.starts_with(kRunpathOriginToken)) return 0;
    std::string_view const rest = entry.substr(kRunpathOriginToken.size());
    if (rest.empty() || rest.front() == '/') return kRunpathOriginToken.size();
    return 0;
}

// The one sentence every portable refusal ends with, so none of them is a dead
// end: WHERE a spelling the manifest refuses is still accepted.
constexpr std::string_view kPortableRemedy =
    " A project manifest builds for many targets, so it may say only what every "
    "loader reads alike: an absolute directory ('/opt/app/lib') or one rooted at "
    "'${ORIGIN}' (the directory of the image that carries the path — written "
    "'$ORIGIN' on ELF and '@loader_path' on Mach-O by each format's own "
    "document). A loader-specific spelling ('$LIB', '$PLATFORM', "
    "'@executable_path', a ':'-separated list, a relative directory) is accepted "
    "VERBATIM through the CLI flag '--rpath', where gcc's -rpath semantics "
    "apply to the one target that invocation builds.";

} // namespace

std::string_view runpathUnsupportedExplanation(RunpathUnsupportedReason r) noexcept {
    switch (r) {
        case RunpathUnsupportedReason::ApplicationDirectorySearch:
            return "This format's loader searches the directory the "
                   "APPLICATION was loaded from (then the system directories "
                   "and the search path), so a library placed beside the "
                   "executable is found with nothing recorded — which is why "
                   "an image of this format has no field for a runpath at all";
        case RunpathUnsupportedReason::Unspecified:
            break;
    }
    return "This format declares no 'runpathUnsupportedReason' either, so no "
           "specific account of where its loader looks can be given";
}

std::vector<RunpathDeclarationProblem>
runpathDeclarationProblems(RunpathDeclaration const& decl) {
    std::vector<RunpathDeclarationProblem> problems;
    auto const fail = [&](std::string_view key, std::string message) {
        problems.push_back({key, std::move(message)});
    };

    // `origin` — required by every carrier: without it a leading `${ORIGIN}`
    // could not be written at all, and the image would carry the portable
    // token literally, which no loader expands as the image's directory.
    if (decl.origin.empty()) {
        fail("origin",
             "'runpath.origin' must name how this format spells the directory "
             "of the image that carries the path ('$ORIGIN' for ELF, "
             "'@loader_path' for Mach-O): a leading '${ORIGIN}' is written in "
             "that spelling, and without one it could not be written at all");
    } else if (decl.origin.find('\0') != std::string::npos) {
        fail("origin",
             "'runpath.origin' holds a NUL byte — every carrier stores "
             "NUL-terminated strings, so the spelling would be cut short");
    }

    switch (decl.carrier) {
        case RunpathCarrier::Unspecified:
            fail("carrier",
                 std::format("the runpath declaration names no carrier — "
                             "declare one of '{}' / '{}'",
                             runpathCarrierName(RunpathCarrier::ElfDynamicEntry),
                             runpathCarrierName(RunpathCarrier::MachoLoadCommand)));
            break;
        case RunpathCarrier::ElfDynamicEntry:
            if (decl.dynamicTag != kElfDtRunpath && decl.dynamicTag != kElfDtRpath) {
                fail("dynamicTag",
                     std::format("'runpath.dynamicTag' is {}, but a runpath is "
                                 "carried only by DT_RUNPATH ({}) or DT_RPATH "
                                 "({}) — the two gABI tags whose value is the "
                                 "string-table offset of a library search path. "
                                 "Any other tag would make the loader read the "
                                 "directory list as something else",
                                 decl.dynamicTag, kElfDtRunpath, kElfDtRpath));
            }
            if (decl.separator.size() != 1 || decl.separator.front() == '\0') {
                fail("separator",
                     "'runpath.separator' must be exactly ONE non-NUL character "
                     "— the character this format's loader splits the single "
                     "recorded string on (':' for ELF)");
            } else if (decl.origin.find(decl.separator.front())
                       != std::string::npos) {
                fail("origin",
                     std::format("'runpath.origin' ('{}') contains the "
                                 "separator '{}', so every recorded origin "
                                 "would be split in two by the loader",
                                 decl.origin, decl.separator));
            }
            break;
        case RunpathCarrier::MachoLoadCommand:
            if (decl.loadCommand != kMachoLcRpath) {
                fail("loadCommand",
                     std::format("'runpath.loadCommand' is {:#x}, but a runpath "
                                 "is carried only by LC_RPATH ({:#x}) — the one "
                                 "load command whose body is a search path "
                                 "(`rpath_command`)",
                                 decl.loadCommand, kMachoLcRpath));
            }
            if (!decl.separator.empty()) {
                fail("separator",
                     "'runpath.separator' is declared, but this carrier records "
                     "one load command PER path and never joins them — dyld "
                     "keeps a separator character as part of ONE path");
            }
            break;
    }
    return problems;
}

std::optional<std::string> runpathEntryRefusal(std::string_view entry) {
    if (entry.empty()) {
        return std::string{
            "an EMPTY runpath entry names no directory, and no reference makes "
            "one work: GNU ld records 'RUNPATH []' and the program still exits "
            "127 run from the library's own directory, while clang's driver "
            "drops the empty argument so ld.lld takes the NEXT argument as the "
            "path. Name the directory, or drop the entry"};
    }
    if (entry.find('\0') != std::string_view::npos) {
        return std::string{
            "a runpath entry holds a NUL byte, and it cannot be written: every "
            "carrier (ELF's .dynstr string, Mach-O's LC_RPATH path) stores a "
            "NUL-terminated string, so the loader would read only the part "
            "before it"};
    }
    return std::nullopt;
}

std::optional<std::string> portableRunpathEntryRefusal(std::string_view entry) {
    if (auto refusal = runpathEntryRefusal(entry)) return refusal;

    std::size_t const tokenLength = leadingOriginTokenLength(entry);
    if (tokenLength == 0 && entry.starts_with(kRunpathOriginToken)) {
        return std::format(
            "runpath entry '{}' continues '${{ORIGIN}}' with something other "
            "than '/', and neither loader expands its spelling there (dyld "
            "needs '/' or the end after '@loader_path'; glibc leaves "
            "'$ORIGINx' unexpanded) — write '${{ORIGIN}}/<dir>'.{}",
            entry, kPortableRemedy);
    }
    std::string_view const rest = entry.substr(tokenLength);
    if (rest.find('$') != std::string_view::npos) {
        return std::format(
            "runpath entry '{}' holds a '$' other than a leading '${{ORIGIN}}' "
            "(ELF's own '$ORIGIN' included — the portable spelling is "
            "'${{ORIGIN}}', which each format's document writes its own way), "
            "and loaders disagree about any other: glibc expands '$LIB' and "
            "'$PLATFORM', while musl ignores the WHOLE runpath when any '$' is "
            "not ORIGIN.{}",
            entry, kPortableRemedy);
    }
    if (tokenLength == 0 && entry.starts_with('@')) {
        return std::format(
            "runpath entry '{}' begins with '@', a spelling only dyld reads "
            "('@loader_path', '@executable_path', '@rpath') — the portable "
            "spelling of '@loader_path' is '${{ORIGIN}}', and '@executable_path' "
            "has no ELF equivalent at all.{}",
            entry, kPortableRemedy);
    }
    if (tokenLength == 0 && !entry.starts_with('/')) {
        return std::format(
            "runpath entry '{}' is RELATIVE, and a relative runpath is resolved "
            "against the working directory of whatever process loads the image, "
            "not against the image — it names no fixed place.{}",
            entry, kPortableRemedy);
    }
    if (entry.find(':') != std::string_view::npos) {
        return std::format(
            "runpath entry '{}' holds a ':', which ELF's loader splits the list "
            "on and dyld keeps as part of ONE path — it would name different "
            "directories on different targets. Write each directory as its own "
            "entry.{}",
            entry, kPortableRemedy);
    }
    return std::nullopt;
}

std::vector<std::string>
recordedRunpaths(std::span<std::string const> requested,
                 RunpathDeclaration const&    decl) {
    std::vector<std::string> recorded;
    recorded.reserve(requested.size());
    for (std::string const& entry : requested) {
        std::size_t const tokenLength = leadingOriginTokenLength(entry);
        std::string written = tokenLength == 0
            ? entry
            : decl.origin + entry.substr(tokenLength);
        if (std::find(recorded.begin(), recorded.end(), written)
            != recorded.end()) {
            continue;   // an exact duplicate — the first occurrence is kept
        }
        recorded.push_back(std::move(written));
    }
    return recorded;
}

} // namespace dss
