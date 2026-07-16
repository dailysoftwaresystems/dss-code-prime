#pragma once

#include "core/export.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// D-LK7-ADHOC-CODESIGN-MACHO (increment 2/2) — ad-hoc Mach-O code-
// signature builder. Pure byte-emit sub-builder (no reporter, no
// schema, no I/O — modelled on `macho_chained_fixups.hpp`'s
// `buildChainedFixupsPayload`) so the SuperBlob / CodeDirectory byte
// structure is independently unit-testable, decoupled from the
// macho.cpp integration fold.
//
// macOS (and especially Apple Silicon, where AMFI hard-enforces it)
// refuses to exec a Mach-O without a valid code signature. An "ad-hoc"
// signature carries NO CMS blob, NO certificate, and NO identity — just
// a CodeDirectory whose `flags` field sets CS_ADHOC and whose code-slot
// hashes the kernel verifies against the on-disk page bytes. That is
// sufficient for `spctl`-disabled / locally-built binaries to run.
//
// ── On-wire format (Apple `<Security/CSCommon.h>` + `cs_blobs.h`) ──
// EVERY header field below is BIG-ENDIAN (the universal codesign
// gotcha — all OTHER Mach-O structs are little-endian). The page-hash
// payload bytes are raw SHA-256 digests (already big-endian per
// FIPS 180-4, byte-for-byte from `dss::crypto::sha256`).
//
//   CS_SuperBlob (CSMAGIC_EMBEDDED_SIGNATURE = 0xFADE0CC0):
//     [ 0.. 3] magic   = 0xFADE0CC0
//     [ 4.. 7] length  = total blob length
//     [ 8..11] count   = 1
//     then count × CS_BlobIndex:
//       [ 0.. 3] type   = 0 (CSSLOT_CODEDIRECTORY)
//       [ 4.. 7] offset = byte offset from SuperBlob start to the CD
//
//   CS_CodeDirectory (CSMAGIC_CODEDIRECTORY = 0xFADE0C02), version
//   0x00020400 (supports the execSeg fields Apple Silicon checks):
//     [ 0.. 3] magic          = 0xFADE0C02
//     [ 4.. 7] length         = CD blob length
//     [ 8..11] version        = 0x00020400
//     [12..15] flags          = 0x00000002 (CS_ADHOC)
//     [16..19] hashOffset     = offset (within CD) of the first code slot
//     [20..23] identOffset    = offset (within CD) of the identifier
//     [24..27] nSpecialSlots  = 0
//     [28..31] nCodeSlots     = ceil(codeLimit / pageSize)
//     [32..35] codeLimit      = signed byte count covered (= the file
//                               offset where the signature begins)
//     [36]     hashSize       = 32 (SHA-256 digest length)
//     [37]     hashType       = 2 (CS_HASHTYPE_SHA256)
//     [38]     platform       = 0 (not a platform binary)
//     [39]     pageSize       = log2(pageSize) (12 for 4096)
//     [40..43] spare2         = 0
//     [44..47] scatterOffset  = 0
//     [48..51] teamOffset     = 0
//     [52..55] spare3         = 0
//     [56..63] codeLimit64    = 0 (codeLimit fits in 32 bits here)
//     [64..71] execSegBase    = 0 (__TEXT starts at file offset 0)
//     [72..79] execSegLimit   = file size of the __TEXT segment
//     [80..87] execSegFlags   = caller-supplied: CS_EXECSEG_MAIN_BINARY
//                               (1) for an MH_EXECUTE main binary; 0
//                               for an MH_DYLIB (codesign ground truth
//                               — a dylib is NOT the main binary, and
//                               Apple's codesign never sets the flag
//                               on one; c153 D-LK3-3)
//     [88..]   identifier C-string (NUL-terminated) at identOffset
//     [..]     nCodeSlots × 32-byte SHA-256 page hashes at hashOffset
//   (nSpecialSlots == 0 ⇒ no negative special slots precede hashOffset.)
//
// Layout order inside the CD blob: [fixed 88-byte header]
//   [identifier + NUL][code-slot hashes]. identOffset therefore equals
// the fixed-header length, and hashOffset = identOffset + ident + 1.

namespace dss::macho::detail {

// CodeDirectory execSegFlags values (Apple `cs_blobs.h`). The caller
// (macho.cpp) picks per image flavor: MAIN_BINARY on MH_EXECUTE, 0 on
// MH_DYLIB — matching what `codesign -s -` stamps on each (c153,
// D-LK3-3; a MAIN_BINARY-flagged dylib is a shape Apple's tooling
// never produces, so this substrate does not ship it either).
inline constexpr std::uint64_t kCsExecSegFlagsNone       = 0;
inline constexpr std::uint64_t kCsExecSegFlagsMainBinary = 1;

// Exact byte length of the ad-hoc signature blob the builder produces
// for the given parameters. The macho.cpp reservation derives its size
// from THIS (never a hand-typed constant) so the reservation and the
// built blob match to the byte — `buildAdHocCodeSignature` over the
// same arguments returns a vector of exactly this size.
//
//   = CS_SuperBlob(8) + 1 × CS_BlobIndex(8)
//   + CodeDirectory( fixedHeaderLen(88) + identifier.size() + 1
//                    + nCodeSlots × 32 )
// where nCodeSlots = ceil(codeLimit / pageSize).
//
// PRECONDITION: pageSize > 0 (a power of two; enforced at schema load).
[[nodiscard]] DSS_EXPORT std::uint32_t
adHocCodeSignatureSize(std::uint32_t   codeLimit,
                       std::uint32_t   pageSize,
                       std::string_view identifier);

// Assemble the ad-hoc CodeDirectory + SuperBlob over `signedBytes`
// (the file bytes [0, codeLimit) — i.e. everything from the mach header
// up to where the signature itself begins). Page i's hash is
// SHA-256 over signedBytes[i*pageSize, min((i+1)*pageSize, codeLimit)).
//
// `execSegLimit` is the file size of the __TEXT segment (the execSeg
// the kernel maps as the image's executable region). `execSegFlags`
// is the CodeDirectory execSeg flag word — `kCsExecSegFlagsMainBinary`
// for an MH_EXECUTE, `kCsExecSegFlagsNone` for an MH_DYLIB (see the
// constants above).
//
// The returned vector's size equals `adHocCodeSignatureSize(codeLimit,
// pageSize, identifier)` exactly (the flags do not affect the length).
//
// PRECONDITION: signedBytes.size() >= codeLimit, pageSize > 0.
[[nodiscard]] DSS_EXPORT std::vector<std::uint8_t>
buildAdHocCodeSignature(std::span<std::uint8_t const> signedBytes,
                        std::uint32_t                 codeLimit,
                        std::uint32_t                 pageSize,
                        std::string_view              identifier,
                        std::uint64_t                 execSegLimit,
                        std::uint64_t                 execSegFlags);

} // namespace dss::macho::detail
