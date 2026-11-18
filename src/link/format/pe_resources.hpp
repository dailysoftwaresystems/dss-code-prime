#pragma once

// ── THE RESOURCES A PE IMAGE EMBEDS ─────────────────────────────────────────
// P69 fold 3f (D-LK-PE-IMAGE-CANNOT-HONOUR-EVERY-LINKER-DIRECTIVE item (2)): the manifest an image embeds for the
// side-by-side assemblies its units depend on (`/MANIFESTDEPENDENCY:`), as ONE resource in a section of its own.
//
// ✔MEASURED 2026-11-17 (probe-reference-cc runs 20261117-140648-927055da, 20261117-140952-a037b4db, 20261117-142310-025e5c2e, 20261117-142455-10b66d4b: cl 19.52 + link.exe 14.52 /MANIFEST:EMBED,
// clang 18.1.3 + lld-link 18.1.3 /manifest:embed): both references write the same tree -- three one-entry
// directories (type, name, language) at section offsets 0x0, 0x18 and 0x30, the data entry at 0x48, the text at
// 0x60 (the tree's end rounded up to 16), and the section's size the text's end rounded up to 8; every directory's
// Characteristics, TimeDateStamp and version 0; the data entry's code page and reserved word as the document says.
// The tree's shape is PE/COFF's (§6.9), so its record sizes are this header's; the resource's coordinates and the
// text are the image document's (`pe.embeddedManifest`).

#include "link/format/byte_emit.hpp"
#include "link/object_format_schema.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dss::pe {

inline constexpr std::size_t   kResourceDirectorySize        = 16;   // IMAGE_RESOURCE_DIRECTORY
inline constexpr std::size_t   kResourceDirectoryEntrySize   = 8;    // IMAGE_RESOURCE_DIRECTORY_ENTRY
inline constexpr std::size_t   kResourceDataEntrySize        = 16;   // IMAGE_RESOURCE_DATA_ENTRY
inline constexpr std::uint32_t kResourceSubdirectory         = 0x80000000u;   // an entry naming a directory
// Both references' placement (✔MEASURED, the header): the text past the tree at a multiple of 16, the section's
// size a multiple of 8.
inline constexpr std::size_t   kResourceDataAlignment        = 16;
inline constexpr std::size_t   kResourceSectionSizeAlignment = 8;

// The manifest's text: the document's head lines, then its dependency lines once per identity, in the order given,
// with `{identity}` replaced by the identity verbatim, then its tail lines -- each line ended by the document's line
// end. The identities are the link's decided set: distinct, in the order the units first state them.
[[nodiscard]] inline std::string
embeddedManifestText(PeEmbeddedManifest const& manifest, std::span<std::string const> identities) {
    std::string out;
    auto const line = [&](std::string_view text) {
        out.append(text);
        out.append(manifest.lineEnd);
    };
    for (auto const& l : manifest.head) line(l);
    for (auto const& identity : identities) {
        for (auto const& l : manifest.dependency) {
            std::size_t const at = l.find(kEmbeddedManifestIdentity);
            if (at == std::string::npos) {
                line(l);
                continue;
            }
            line(l.substr(0, at) + identity + l.substr(at + kEmbeddedManifestIdentity.size()));
        }
    }
    for (auto const& l : manifest.tail) line(l);
    return out;
}

// The resource section's bytes, laid out at image RVA `sectionRva`: the one-leaf tree naming the manifest, then its
// text, padded to the section's size. The data entry holds the text's RVA, which is why the section's RVA is an
// argument.
[[nodiscard]] inline std::vector<std::uint8_t>
encodeEmbeddedManifestSection(PeEmbeddedManifest const&     manifest,
                              std::span<std::string const> identities,
                              std::uint32_t                sectionRva) {
    using link::format::detail::alignUp;
    using link::format::detail::appendU16LE;
    using link::format::detail::appendU32LE;
    std::string const text = embeddedManifestText(manifest, identities);
    std::size_t const directory = kResourceDirectorySize + kResourceDirectoryEntrySize;
    std::size_t const nameDirectory = directory;
    std::size_t const languageDirectory = 2 * directory;
    std::size_t const dataEntry = 3 * directory;
    std::size_t const textOffset =
        static_cast<std::size_t>(alignUp(dataEntry + kResourceDataEntrySize, kResourceDataAlignment));
    std::vector<std::uint8_t> out;
    // One directory of one id entry: Characteristics, TimeDateStamp, MajorVersion, MinorVersion, then no named entry
    // and one id entry -- its id, and where it points.
    auto const oneEntryDirectory = [&](std::uint32_t id, std::uint32_t target) {
        appendU32LE(out, 0);
        appendU32LE(out, 0);
        appendU16LE(out, 0);
        appendU16LE(out, 0);
        appendU16LE(out, 0);
        appendU16LE(out, 1);
        appendU32LE(out, id);
        appendU32LE(out, target);
    };
    oneEntryDirectory(manifest.resourceType, kResourceSubdirectory | static_cast<std::uint32_t>(nameDirectory));
    oneEntryDirectory(manifest.resourceId, kResourceSubdirectory | static_cast<std::uint32_t>(languageDirectory));
    oneEntryDirectory(manifest.language, static_cast<std::uint32_t>(dataEntry));
    appendU32LE(out, sectionRva + static_cast<std::uint32_t>(textOffset));   // the text's RVA
    appendU32LE(out, static_cast<std::uint32_t>(text.size()));
    appendU32LE(out, manifest.codePage);
    appendU32LE(out, 0);                                                      // reserved
    out.resize(textOffset, 0);
    out.insert(out.end(), text.begin(), text.end());
    out.resize(static_cast<std::size_t>(alignUp(out.size(), kResourceSectionSizeAlignment)), 0);
    return out;
}

}  // namespace dss::pe
