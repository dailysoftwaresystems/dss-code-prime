#include "link/format/macho_codesign.hpp"

#include "core/crypto/sha256.hpp"
#include "link/format/byte_emit.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace dss::macho::detail {

namespace {

// ── On-wire constants (Apple `cs_blobs.h` / `<Security/CSCommon.h>`) ──
constexpr std::uint32_t kCsMagicEmbeddedSignature = 0xFADE0CC0u;
constexpr std::uint32_t kCsMagicCodeDirectory     = 0xFADE0C02u;
constexpr std::uint32_t kCsCodeDirectoryVersion   = 0x00020400u;
constexpr std::uint32_t kCsAdhoc                  = 0x00000002u;  // flags
constexpr std::uint32_t kCsSlotCodeDirectory      = 0u;           // BlobIndex.type
constexpr std::uint8_t  kCsHashTypeSha256         = 2u;
constexpr std::uint8_t  kCsHashSizeSha256         = 32u;
constexpr std::uint8_t  kCsPlatformNotPlatform    = 0u;
// execSegFlags is caller-supplied since c153 (D-LK3-3): the header's
// kCsExecSegFlagsMainBinary / kCsExecSegFlagsNone constants — an
// MH_EXECUTE main binary sets MAIN_BINARY; an MH_DYLIB sets none.

// CS_SuperBlob fixed header (magic + length + count) and one
// CS_BlobIndex (type + offset). Exactly one sub-blob (the CD).
constexpr std::uint32_t kSuperBlobHeaderSize = 12u;  // 3 × u32
constexpr std::uint32_t kBlobIndexSize       = 8u;   // 2 × u32
constexpr std::uint32_t kSuperBlobPrefixSize =
    kSuperBlobHeaderSize + kBlobIndexSize;           // = 20

// CS_CodeDirectory fixed-header length at version 0x20400. The version
// gates which trailing fields are present; 0x20400 is the first that
// carries execSegBase/Limit/Flags. Field map (see the header docblock):
//   base v0x20000 ......... 44 bytes  (through spare2)
//   + scatterOffset (0x20100) .. +4
//   + teamOffset    (0x20200) .. +4
//   + spare3 + codeLimit64 (0x20300) .. +12
//   + execSegBase/Limit/Flags (0x20400) .. +24
//   = 88 bytes
constexpr std::uint32_t kCodeDirectoryFixedHeaderLen = 88u;

// Code-slot count = ceil(codeLimit / pageSize). pageSize > 0 by
// precondition (power-of-two, schema-validated).
[[nodiscard]] std::uint32_t
codeSlotCount(std::uint32_t codeLimit, std::uint32_t pageSize) {
    return (codeLimit + pageSize - 1u) / pageSize;
}

// CodeDirectory blob length for the given identifier + slot count.
[[nodiscard]] std::uint32_t
codeDirectoryLength(std::uint32_t nCodeSlots, std::string_view identifier) {
    return kCodeDirectoryFixedHeaderLen
         + static_cast<std::uint32_t>(identifier.size()) + 1u  // + NUL
         + nCodeSlots * kCsHashSizeSha256;
}

} // namespace

std::uint32_t
adHocCodeSignatureSize(std::uint32_t    codeLimit,
                       std::uint32_t    pageSize,
                       std::string_view identifier) {
    std::uint32_t const nCodeSlots = codeSlotCount(codeLimit, pageSize);
    return kSuperBlobPrefixSize
         + codeDirectoryLength(nCodeSlots, identifier);
}

std::vector<std::uint8_t>
buildAdHocCodeSignature(std::span<std::uint8_t const> signedBytes,
                        std::uint32_t                 codeLimit,
                        std::uint32_t                 pageSize,
                        std::string_view              identifier,
                        std::uint64_t                 execSegLimit,
                        std::uint64_t                 execSegFlags) {
    using namespace dss::link::format::detail;

    std::uint32_t const nCodeSlots = codeSlotCount(codeLimit, pageSize);
    std::uint32_t const cdLength    =
        codeDirectoryLength(nCodeSlots, identifier);
    std::uint32_t const totalLength = kSuperBlobPrefixSize + cdLength;

    // Offsets WITHIN the CodeDirectory blob: [fixed header]
    //   [identifier + NUL][code-slot hashes].
    std::uint32_t const identOffset = kCodeDirectoryFixedHeaderLen;
    std::uint32_t const hashOffset  =
        identOffset + static_cast<std::uint32_t>(identifier.size()) + 1u;
    // The CodeDirectory begins right after the SuperBlob prefix.
    std::uint32_t const cdSuperBlobOffset = kSuperBlobPrefixSize;

    std::vector<std::uint8_t> out;
    out.reserve(totalLength);

    // ── CS_SuperBlob (all fields BIG-ENDIAN) ────────────────────────
    appendU32BE(out, kCsMagicEmbeddedSignature);
    appendU32BE(out, totalLength);
    appendU32BE(out, 1u);                       // count
    // CS_BlobIndex[0]
    appendU32BE(out, kCsSlotCodeDirectory);     // type
    appendU32BE(out, cdSuperBlobOffset);        // offset to CD

    // ── CS_CodeDirectory (all header fields BIG-ENDIAN) ─────────────
    appendU32BE(out, kCsMagicCodeDirectory);
    appendU32BE(out, cdLength);
    appendU32BE(out, kCsCodeDirectoryVersion);
    appendU32BE(out, kCsAdhoc);                 // flags
    appendU32BE(out, hashOffset);
    appendU32BE(out, identOffset);
    appendU32BE(out, 0u);                       // nSpecialSlots
    appendU32BE(out, nCodeSlots);
    appendU32BE(out, codeLimit);
    out.push_back(kCsHashSizeSha256);           // hashSize  (u8)
    out.push_back(kCsHashTypeSha256);           // hashType  (u8)
    out.push_back(kCsPlatformNotPlatform);      // platform  (u8)
    out.push_back(static_cast<std::uint8_t>(    // pageSize  (u8, log2)
        std::countr_zero(pageSize)));
    appendU32BE(out, 0u);                       // spare2
    appendU32BE(out, 0u);                       // scatterOffset
    appendU32BE(out, 0u);                       // teamOffset
    appendU32BE(out, 0u);                       // spare3
    appendU64BE(out, 0u);                       // codeLimit64
    appendU64BE(out, 0u);                       // execSegBase
    appendU64BE(out, execSegLimit);             // execSegLimit
    appendU64BE(out, execSegFlags);             // execSegFlags

    // identifier C-string (NUL-terminated) at identOffset.
    out.insert(out.end(), identifier.begin(), identifier.end());
    out.push_back(0u);

    // Code-slot page hashes at hashOffset. Page i covers
    // [i*pageSize, min((i+1)*pageSize, codeLimit)). The last page is
    // short when codeLimit is not a multiple of pageSize. Raw SHA-256
    // digest bytes (already big-endian per FIPS 180-4).
    for (std::uint32_t i = 0; i < nCodeSlots; ++i) {
        std::uint32_t const start = i * pageSize;
        std::uint32_t const len   =
            std::min(pageSize, codeLimit - start);
        std::array<std::uint8_t, 32> const digest =
            dss::crypto::sha256(signedBytes.subspan(start, len));
        out.insert(out.end(), digest.begin(), digest.end());
    }

    return out;
}

} // namespace dss::macho::detail
