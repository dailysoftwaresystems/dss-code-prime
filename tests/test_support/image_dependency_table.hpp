#pragma once

// The DEPENDENCY TABLE of an emitted image — the list of libraries the image
// tells its loader it needs — recovered from the bytes, one extractor per
// object format:
//
//   ELF     `.dynamic` DT_NEEDED  → names in `.dynstr`
//   PE      IMAGE_IMPORT_DESCRIPTOR.Name (data directory #1)
//   Mach-O  LC_LOAD_DYLIB dylib name
//
// D-LK-ELF-EMITS-ONE-DT-NEEDED-WHEN-TWO-LIBRARIES-ARE-REFERENCED. These live
// in shared test support rather than in one test file because the defect they
// guard is a DIVERGENCE BETWEEN FORMATS: DSS believed libtcl8.6.so exported
// the zlib names it merely imports, so libz.so.1 never reached the ELF
// writer, and no test anywhere compared what the three writers did with the
// same resolved-library set. A pin that answers "did EVERY resolved library
// get recorded?" is only meaningful if all three formats can be asked the
// same question, which means all three extractors must be reachable from any
// tier's test — the reader tier, the writer tier, and the end-to-end driver.
//
// Each returns the names in EMISSION ORDER, and returns empty on a buffer it
// cannot parse. Empty is safe for the assertions built on them because they
// assert PRESENCE of a specific name: an extractor that silently stopped
// working reds every pin that uses it rather than passing vacuously.
//
// Deliberately NOT substring scans. A scan of `.idata` for "beta.dll" cannot
// tell a genuine dependency row from an incidental string in a neighbouring
// blob, so it would stay green on exactly the defect this pins; each
// extractor follows its format's real pointer chain.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace dss::test_support {

namespace image_deps_detail {

[[nodiscard]] inline std::uint16_t rdU16(std::vector<std::uint8_t> const& b,
                                         std::size_t off) {
    if (off + 2 > b.size()) return 0;
    return static_cast<std::uint16_t>(b[off] | (b[off + 1] << 8));
}

[[nodiscard]] inline std::uint32_t rdU32(std::vector<std::uint8_t> const& b,
                                         std::size_t off) {
    if (off + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(b[off])
         | (static_cast<std::uint32_t>(b[off + 1]) << 8)
         | (static_cast<std::uint32_t>(b[off + 2]) << 16)
         | (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

[[nodiscard]] inline std::uint64_t rdU64(std::vector<std::uint8_t> const& b,
                                         std::size_t off) {
    if (off + 8 > b.size()) return 0;
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8)
          | static_cast<std::uint64_t>(b[off + static_cast<std::size_t>(i)]);
    return v;
}

// A NUL-terminated string at `off`, bounded by the buffer. An out-of-range
// offset yields empty rather than reading past the end — a dependency row
// that points nowhere must surface as "no name", never as a crash and never
// as some neighbouring string.
[[nodiscard]] inline std::string rdCStr(std::vector<std::uint8_t> const& b,
                                        std::size_t off) {
    std::string s;
    while (off < b.size() && b[off] != 0)
        s.push_back(static_cast<char>(b[off++]));
    return s;
}

}  // namespace image_deps_detail

// ── ELF: the DT_NEEDED list ──────────────────────────────────────
//
// Walks the section header table for `.dynamic` + `.dynstr` BY NAME (not by
// a hardcoded index — the exec, PIE, and `.so` shapes carry different section
// counts), then every 16-byte Elf64_Dyn whose d_tag == DT_NEEDED (1).
[[nodiscard]] inline std::vector<std::string>
elfNeededLibraries(std::vector<std::uint8_t> const& b) {
    using namespace image_deps_detail;
    std::vector<std::string> out;
    if (b.size() < 64) return out;
    std::uint64_t const shoff     = rdU64(b, 0x28);
    std::uint16_t const shentsize = rdU16(b, 0x3A);
    std::uint16_t const shnum     = rdU16(b, 0x3C);
    std::uint16_t const shstrndx  = rdU16(b, 0x3E);
    if (shoff == 0 || shentsize == 0 || shnum == 0) return out;

    auto shdrOff = [&](std::size_t i) {
        return static_cast<std::size_t>(shoff)
             + i * static_cast<std::size_t>(shentsize);
    };
    std::uint64_t const shstrOff = rdU64(b, shdrOff(shstrndx) + 0x18);
    std::uint64_t dynamicOff = 0, dynamicSize = 0, dynstrOff = 0;
    for (std::size_t i = 0; i < shnum; ++i) {
        auto const o = shdrOff(i);
        auto const name =
            rdCStr(b, static_cast<std::size_t>(shstrOff) + rdU32(b, o + 0));
        if (name == ".dynamic") {
            dynamicOff  = rdU64(b, o + 0x18);
            dynamicSize = rdU64(b, o + 0x20);
        } else if (name == ".dynstr") {
            dynstrOff = rdU64(b, o + 0x18);
        }
    }
    if (dynamicOff == 0 || dynstrOff == 0) return out;
    constexpr std::uint64_t kDtNull   = 0;
    constexpr std::uint64_t kDtNeeded = 1;
    for (std::uint64_t e = 0; e + 16 <= dynamicSize; e += 16) {
        auto const at  = static_cast<std::size_t>(dynamicOff + e);
        auto const tag = rdU64(b, at);
        if (tag == kDtNull) break;
        if (tag == kDtNeeded)
            out.push_back(rdCStr(
                b, static_cast<std::size_t>(dynstrOff + rdU64(b, at + 8))));
    }
    return out;
}

// ── PE: the IMAGE_IMPORT_DESCRIPTOR DllName list ─────────────────
//
// Follows the real chain: e_lfanew → COFF header → optional header (PE32+
// only) → data directory #1 → RVA-to-file-offset via the section table →
// each 20-byte descriptor's Name RVA at +12.
[[nodiscard]] inline std::vector<std::string>
peImportedLibraries(std::vector<std::uint8_t> const& b) {
    using namespace image_deps_detail;
    std::vector<std::string> out;
    if (b.size() < 0x40) return out;
    std::size_t const peOff = rdU32(b, 0x3C);
    if (peOff + 24 > b.size() || rdU32(b, peOff) != 0x00004550u) return out;
    std::size_t   const coffOff = peOff + 4;
    std::uint16_t const numSecs = rdU16(b, coffOff + 2);
    std::uint16_t const optSize = rdU16(b, coffOff + 16);
    std::size_t   const optOff  = coffOff + 20;
    if (rdU16(b, optOff) != 0x020Bu) return out;   // PE32+ magic
    std::uint32_t const importRva = rdU32(b, optOff + 112 + 1 * 8);
    if (importRva == 0) return out;

    std::size_t const secTabOff = optOff + optSize;
    auto rvaToFile = [&](std::uint32_t rva) -> std::size_t {
        for (std::size_t i = 0; i < numSecs; ++i) {
            std::size_t   const s    = secTabOff + i * 40;
            std::uint32_t const va   = rdU32(b, s + 12);
            std::uint32_t const vsz  = rdU32(b, s + 8);
            std::uint32_t const rsz  = rdU32(b, s + 16);
            std::uint32_t const raw  = rdU32(b, s + 20);
            std::uint32_t const span = std::max(vsz, rsz);
            if (span != 0 && rva >= va && rva < va + span)
                return static_cast<std::size_t>(raw) + (rva - va);
        }
        return 0;
    };

    std::size_t desc = rvaToFile(importRva);
    if (desc == 0) return out;
    for (; desc + 20 <= b.size(); desc += 20) {
        std::uint32_t const nameRva = rdU32(b, desc + 12);
        // The array is terminated by an all-zero descriptor.
        if (rdU32(b, desc + 0) == 0 && nameRva == 0 && rdU32(b, desc + 16) == 0)
            break;
        std::size_t const nameOff = rvaToFile(nameRva);
        if (nameOff == 0) break;
        out.push_back(rdCStr(b, nameOff));
    }
    return out;
}

// ── Mach-O: the LC_LOAD_DYLIB path list ──────────────────────────
[[nodiscard]] inline std::vector<std::string>
machoLoadedDylibs(std::vector<std::uint8_t> const& b) {
    using namespace image_deps_detail;
    std::vector<std::string> out;
    if (b.size() < 32) return out;
    constexpr std::uint32_t kLcLoadDylib = 0x0Cu;
    std::uint32_t const ncmds = rdU32(b, 16);
    std::size_t off = 32;
    for (std::uint32_t i = 0; i < ncmds && off + 8 <= b.size(); ++i) {
        std::uint32_t const cmd     = rdU32(b, off);
        std::uint32_t const cmdsize = rdU32(b, off + 4);
        if (cmdsize == 0) break;
        if (cmd == kLcLoadDylib)
            out.push_back(rdCStr(b, off + rdU32(b, off + 8)));
        off += cmdsize;
    }
    return out;
}

// How many times `needle` appears in a recovered dependency table. Callers
// assert on this rather than on `size()` so a table with the right NUMBER of
// WRONG entries fails.
[[nodiscard]] inline std::size_t
dependencyOccurrences(std::vector<std::string> const& table,
                      std::string const&              needle) {
    return static_cast<std::size_t>(
        std::count(table.begin(), table.end(), needle));
}

// The table rendered for a failure message.
[[nodiscard]] inline std::string
joinDependencies(std::vector<std::string> const& table) {
    std::string s;
    for (auto const& e : table) {
        if (!s.empty()) s += ", ";
        s += e;
    }
    return s;
}

}  // namespace dss::test_support
