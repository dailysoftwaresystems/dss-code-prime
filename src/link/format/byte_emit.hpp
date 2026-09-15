#pragma once

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_pass_util.hpp"
#include "link/object_format_schema.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Shared little-endian byte-emit helpers for object-format walkers
// (`elf.cpp`, `pe.cpp`, future `macho.cpp`). Hoisted from the per-
// walker copies — every walker writes little-endian POD records,
// and the byte-by-byte append loop is byte-identical across them.
//
// Header-only `inline` so each walker pays no link cost. Lives in
// `dss::link::format::detail` so the namespace identifies "format
// walker substrate" without escaping to the public surface.

namespace dss::link::format::detail {

inline void appendU8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}

inline void appendU16LE(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

inline void appendU32LE(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

inline void appendU64LE(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

// Big-endian (network-byte-order) append helpers.
//
// WHY these exist alongside the LE variants: every Mach-O POD record
// (mach_header_64, load commands, nlist_64, the chained-fixups payload)
// is LITTLE-endian — so the LE helpers above serve the whole walker.
// The Apple code-signature SuperBlob is the ONE exception: every field
// in the `CS_SuperBlob` / `CS_BlobIndex` / `CS_CodeDirectory` headers is
// BIG-endian (Apple's `cs_blobs.h` stores them via `htonl` / `OSSwapHostToBigInt*`).
// This is the universal codesign gotcha — emit a magic or length LE and
// the kernel's `cs_validate_csblob` rejects the signature. Hoisted here
// (not buried in `macho_codesign.cpp`) so the BE/LE pair lives in one
// place and a future format that needs BE records can reuse it.
inline void appendU32BE(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i)
        out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

inline void appendU64BE(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i)
        out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

inline void appendI16LE(std::vector<std::uint8_t>& out, std::int16_t v) {
    appendU16LE(out, static_cast<std::uint16_t>(v));
}

inline void appendI64LE(std::vector<std::uint8_t>& out, std::int64_t v) {
    appendU64LE(out, static_cast<std::uint64_t>(v));
}

// Positional (read/write-at-offset) helpers — patch sites need
// in-place mutation, not append. Hoisted from `exec_reloc_apply.hpp`
// at D-LK6-1 post-fold #1 (3-consumer trip: 3 ARM64 formula arms read
// + OR + write the 32-bit instruction word, vs Linear's append-style
// overwrite — but Linear can use these too when widthBytes==4).
//
// Bounds-check via `assert` — every caller pre-validates the offset
// against the buffer's expected size (`applyExecRelocations` rules
// 3 + 4), so the assertion is a defense-in-depth that catches caller
// bugs (mis-populated `funcTextStart`, off-by-one in a future patcher)
// while staying zero-cost in release. (silent-failure audit HIGH-2
// post-fold #2.)
inline std::uint32_t
readU32LEAt(std::vector<std::uint8_t> const& buf, std::size_t off) noexcept {
    assert(off + 4 <= buf.size() && "readU32LEAt: offset overruns buffer");
    return  static_cast<std::uint32_t>(buf[off + 0])
         | (static_cast<std::uint32_t>(buf[off + 1]) <<  8)
         | (static_cast<std::uint32_t>(buf[off + 2]) << 16)
         | (static_cast<std::uint32_t>(buf[off + 3]) << 24);
}

inline void
writeU32LEAt(std::vector<std::uint8_t>& buf, std::size_t off, std::uint32_t v) noexcept {
    assert(off + 4 <= buf.size() && "writeU32LEAt: offset overruns buffer");
    buf[off + 0] = static_cast<std::uint8_t>(v        & 0xFFu);
    buf[off + 1] = static_cast<std::uint8_t>((v >>  8) & 0xFFu);
    buf[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    buf[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

// Round `v` up to the nearest multiple of `a`.
//
// ★★ PRECONDITION: `a` IS A POSITIVE POWER OF TWO. The body is the
// power-of-two BITMASK `(v + a - 1) & ~(a - 1)`, exact for such an `a`
// and silently WRONG for any other — ✔COMPUTED, not guessed:
// `alignUp(4, 3)` returns 4 where the multiple of 3 is 6, and
// `alignUp(600, 600)` returns 1192 where the answer is 600 itself. The
// result is not merely unrounded; it can be LARGER than the correct one,
// so a "close enough" reading of the failure is wrong too.
//
// ⚠⚠ THIS COMMENT USED TO CLAIM THE OPPOSITE, AND THE CLAIM WAS FALSE
// THE DAY IT WAS WRITTEN. It read *"not required to be a power of two —
// handled with the modulo-cycle form to keep the contract simple"*
// while the body was, and still is, the mask. A caller who read it and
// trusted it got wrong bytes with no diagnostic. Nothing gates a
// comment, which is why a corrected sentence alone was never the plan —
// see the precondition assert below, which was written, WORKED, found a
// real violator on its first run, and now SHIPS.
//
// ★ WHY THE CONTRACT WAS NARROWED RATHER THAN THE BODY GENERALISED —
// ✔MEASURED over EVERY call site, not chosen by preference. Each caller
// in `elf.cpp`, `pe.cpp`, `macho.cpp` and `macho_chained_fixups.cpp`
// passes either a small literal (2/4/8/16) or an alignment that some
// OTHER part of the same writer also feeds to `std::countr_zero`:
// `section_64.align` and PE's `IMAGE_SCN_ALIGN_*` nibble are LOG2
// fields, so a non-power-of-two alignment cannot be expressed in the
// container at all, whatever this function returns. Generalising the
// body would have made ONE step of a multi-step encoding correct and
// left the rest wrong — a partial fix that reads as a complete one, and
// one that moves the failure downstream of the last place that can
// still name it. `Alignment::alignUp` in `core/types/alignment.hpp`,
// this project's power-of-two alignment OWNER, states the same
// precondition over the same mask; this is the raw-integer sibling for
// values that arrive from a format document rather than through that
// newtype, and it now says so instead of contradicting it.
//
// ★ MOST OF THE PRECONDITION IS ENFORCED ALWAYS-ON, AT EACH VALUE'S OWN
// BOUNDARY — stated here because a reader who does not know that would
// reasonably add a second, and a fact with an owner does not get one.
// ✔MEASURED, each guard read at its source: PE's `sectionAlignment` and
// `fileAlignment` are `isPow2`-checked by `pe_backend`'s validate();
// Mach-O's `segmentPageSize` by `macho_backend`'s; every
// `section_64.align` by `sectionAlignLog2` in `macho.cpp`; and every
// ITEM alignment by the `Alignment` newtype's construction-time check.
//
// ⚠⚠⚠ AND THE WORD WAS **MOST**, NOT ALL — THE PRECONDITION WAS VIOLATED
// BY A SHIPPED CALLER UNTIL P65, AND THIS PARAGRAPH'S FIRST DRAFT SAID
// OTHERWISE. It read *"a release build cannot reach this function with a
// bad `a` through any shipped path"*, reasoned from the guard inventory
// above. The precondition `assert` below was written to state the
// contract, and it FIRED ON ITS FIRST RUN — four link tests aborted at
// once (`link/test_elf_writer`, `link/test_elf_exec_writer`,
// `link/test_relocatable_object_reader`,
// `link/test_elf_image_symtab_partition`). ✔THE OFFENDING CALL WAS
// LOCATED, not guessed, with a temporary `std::source_location` default
// parameter: `elf::encode`'s STATIC layout evaluated
// `alignUp(roSpanEndVa, pageAlignStatic)` with `pageAlignStatic =
// fmt.elf().pageAlign`, and the RELOCATABLE and STATICLIB documents
// declare no `pageAlign` at all — so `a` arrived as **0**.
//
// ★ `alignUp(v, 0)` RETURNS 0, ALWAYS: `~(0 - 1)` is `~0xFFFF…FFFF`,
// i.e. zero, so the mask erases the value entirely. ⓘ AND IT HAD BEEN
// CORRECT BY ACCIDENT, which is the part worth pausing on — every
// consumer of that VA chain sits inside the ELF walker's `if (isExec)`
// block and every `sh_addr` it reaches is written `isExec ? va : 0`, so
// the collapsed answer was the right one on the only path that reached
// it. That is the same shape as
// [[D-LINK-ELF-IMAGE-OVERALIGNED-DATA-PLACED-AT-ALIGNED-FILE-OFFSET]]
// itself: arithmetic that is right for a reason nobody chose, one
// document key away from being wrong.
//
// ⇒ FIXED AT THE CALLER IN P65, AND THE ASSERT NOW SHIPS —
// [[D-LINK-ELF-STATIC-EXEC-VA-CHAIN-ROUNDS-TO-AN-UNDECLARED-PAGE]].
// ⚠ THE CALLER WAS NOT CLAMPED, AND THAT WAS DECIDED BY MEASUREMENT
// RATHER THAN BY TASTE. `std::max<std::uint64_t>(1, pageAlignStatic)`
// was the other candidate: it defines the ill-formed call away instead
// of removing it, and evaluated side by side with what shipped, on a
// real relocatable object, it turned ONE meaningless non-zero address on
// that path into THREE. The walker now computes those addresses only
// when the document describes an IMAGE, where `elf_backend`'s validate()
// has always refused an ET_EXEC or ET_DYN document that declares no
// `elf.pageAlign` and refused any `pageAlign` that is not a positive
// power of two. The argument is therefore guaranteed at the DOCUMENT
// boundary — the same distributed enforcement the inventory above
// describes — rather than repaired at the call, which is what makes the
// assert below a statement about a contract nobody breaks instead of a
// second owner of a fact.
//
// ★ AND IT IS CHEAPER THAN IT LOOKS: in a constant-evaluated call the
// assert is a COMPILE error, so every literal alignment in every walker
// is checked at build time at zero run-time cost — the same posture as
// `readU32LEAt`/`writeU32LEAt` above. ⓘ A whole-suite sweep with the
// `std::source_location` probe (2136 tests) found NO OTHER violating
// caller in any walker, which is why the probe is not kept: it is worth
// re-adding for a diagnosis, not for a fact now written down.
//
// Hoisted from per-walker lambdas in `elf.cpp` / `pe.cpp` / `macho.cpp`
// (3+ consumers trip the D-LK4-9 / D-LK4-11 hoist threshold;
// code-simplifier REQUIRED fold, LK7 post-fold review).
[[nodiscard]] inline constexpr std::uint64_t
alignUp(std::uint64_t v, std::uint64_t a) noexcept {
    assert(a != 0 && (a & (a - 1)) == 0
           && "alignUp: alignment must be a positive power of two");
    return (v + a - 1) & ~(a - 1);
}

// Shared diagnostic-emit shorthand for format walkers. Identical
// shape to ML6/ML7's pass-side helpers; centralizes `report` so
// every walker speaks the same K_* dialect.
inline void emit(DiagnosticReporter& reporter, DiagnosticCode code,
                 std::string msg) {
    dss::report(reporter, code, DiagnosticSeverity::Error,
                          std::move(msg));
}

// Resolve a SectionKind row in the format schema, fail-loud if
// missing. The walker treats absent declarations as a configuration
// error rather than substituting a default — silent defaults are
// exactly the silent-failure class the substrate discipline rejects.
// Returns nullptr + emits `K_NoMatchingObjectFormat` if not found.
//
// `walkerName` (e.g. "ELF writer" / "PE writer") prefixes the
// diagnostic so a multi-format build pinpoints the failing walker.
[[nodiscard]] inline ObjectFormatSectionInfo const*
requireSection(ObjectFormatSchema const& fmt, SectionKind kind,
               std::string_view walkerName,
               DiagnosticReporter& reporter) {
    auto const* s = fmt.sectionByKind(kind);
    if (s == nullptr) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{walkerName}
                 + " requires section kind '"
                 + std::string{sectionKindName(kind)}
                 + "' but object format '"
                 + std::string{fmt.name()}
                 + "' does not declare one");
    }
    return s;
}

} // namespace dss::link::format::detail
