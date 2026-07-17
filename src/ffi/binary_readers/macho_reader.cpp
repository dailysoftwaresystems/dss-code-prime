#include "ffi/binary_readers/macho_reader.hpp"

#include "core/cpp_invariants.hpp"  // arithmetic-right-shift static_assert
#include "core/types/parse_diagnostic.hpp"
#include "ffi/binary_readers/reader_common.hpp"

#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss::ffi {

namespace {

// -- Mach-O 64 layout constants ----------------------------------
// Audited against Apple's `<mach-o/loader.h>` + `<mach-o/nlist.h>`
// (Darwin xnu source -- public). Field offsets are reproduced here
// rather than #include-pulled to keep `src/ffi/` free of host-OS
// SDK dependencies (the reader runs on Linux / Windows / macOS hosts
// over a Mach-O byte buffer -- host agnostic by construction).
//
//   mach_header_64 (32 bytes):
//     [ 0.. 3] magic       (0xFEEDFACF -- 64-bit LE)
//     [ 4.. 7] cputype     (skipped -- surfaced via libraryPathLabel)
//     [ 8..11] cpusubtype  (skipped)
//     [12..15] filetype    (skipped -- MH_DYLIB / MH_EXECUTE / etc.)
//     [16..19] ncmds       (count of load commands)
//     [20..23] sizeofcmds  (total byte size of load commands)
//     [24..27] flags       (skipped)
//     [28..31] reserved    (always 0)
//
//   load_command (every LC, 8 bytes preamble):
//     [ 0.. 3] cmd
//     [ 4.. 7] cmdsize
//
//   LC_SYMTAB command (LC_SYMTAB == 0x2, cmdsize == 24):
//     [ 0.. 3] cmd (= LC_SYMTAB)
//     [ 4.. 7] cmdsize (= 24)
//     [ 8..11] symoff   (file offset to symbol table)
//     [12..15] nsyms    (count of nlist_64 entries)
//     [16..19] stroff   (file offset to string table)
//     [20..23] strsize  (size of string table in bytes)
//
//   LC_DYSYMTAB command (LC_DYSYMTAB == 0xB, cmdsize == 80) --
//   we use 8 fields (the external-defined slice index/count); the
//   remaining 64 bytes (local syms, undefs, ToC, mod table, refs,
//   indirect syms) are skipped (deferred surfaces).
//     [16..19] iextdefsym  (start index in `LC_SYMTAB` table)
//     [20..23] nextdefsym  (count of externally-defined entries)
//
//   LC_SEGMENT_64 command (LC_SEGMENT_64 == 0x19) -- segment_command_64
//   header is 72 bytes, followed by `nsects` section_64 records
//   (80 bytes each):
//     segment_command_64:
//       [ 8..23] segname (16 bytes)
//       [24..31] vmaddr  (u64 -- the segment's load VA)
//       [40..47] fileoff (u64 -- the segment's file offset)
//       [48..55] filesize(u64)
//       [64..67] nsects  (u32)
//     section_64 (each):
//       [ 0..15] sectname (16 bytes)
//       [16..31] segname  (16 bytes)
//       [32..39] addr  (u64 -- the section's load VA)
//       [40..47] size  (u64)
//       [64..67] flags (u32 -- S_* type + S_ATTR_* attributes)
//
//   LC_DYLD_INFO / LC_DYLD_INFO_ONLY (dyld_info_command) -- the classic
//   dyld metadata command. dlsym resolves the EXPORT TRIE at
//   export_off/export_size (NOT the nlist symtab):
//     [40..43] export_off  (file offset to the export trie)
//     [44..47] export_size (byte size of the export trie)
//
//   LC_DYLD_EXPORTS_TRIE (linkedit_data_command) -- the modern
//   (chained-fixups era) home of the export trie, split out of
//   LC_DYLD_INFO:
//     [ 8..11] dataoff   (file offset to the export trie)
//     [12..15] datasize  (byte size of the export trie)
//
//   LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB / LC_REEXPORT_DYLIB
//   (dylib_command) -- names a dependent dylib. Reexport terminals in
//   the export trie address these by 1-based ordinal:
//     [ 8..11] name.offset (u32 -- byte offset from the LC start to the
//                           NUL-terminated dylib path)
//
//   nlist_64 (16 bytes each):
//     [ 0.. 3] n_strx   (index into string table; 0 = unnamed)
//     [ 4]     n_type   (low 3 bits = type-or-flags; high bits stab)
//     [ 5]     n_sect   (1-based section index; 0 = NO_SECT)
//     [ 6.. 7] n_desc   (weak-def flags; skipped -- D-FF1-MACHO-WEAK-DEF)
//     [ 8..15] n_value  (symbol VA)

constexpr std::size_t   kMachOHeaderSize  = 32;
constexpr std::size_t   kMachOLcPreamble  = 8;
constexpr std::size_t   kMachOSymtabCmdSz = 24;
constexpr std::size_t   kMachONlist64Sz   = 16;
constexpr std::size_t   kMachOSegCmd64Hdr = 72;   // segment_command_64 header
constexpr std::size_t   kMachOSection64Sz = 80;   // one section_64 record

constexpr std::uint32_t kMachOMagic64     = 0xFEEDFACFu;

constexpr std::uint32_t kLcSymtab           = 0x2u;
constexpr std::uint32_t kLcDysymtab         = 0xBu;
constexpr std::uint32_t kLcSegment64        = 0x19u;
constexpr std::uint32_t kLcReqDyld          = 0x80000000u;  // LC_REQ_DYLD
constexpr std::uint32_t kLcDyldInfo         = 0x22u;
constexpr std::uint32_t kLcDyldInfoOnly     = 0x22u | kLcReqDyld;       // 0x80000022
constexpr std::uint32_t kLcDyldExportsTrie  = 0x33u | kLcReqDyld;       // 0x80000033
// The dylib-load commands, in the order they appear, define the 1-based
// library ordinal space that reexport terminals (and two-level binds)
// index. ALL five kinds count -- omitting any shifts every later ordinal
// and would name the wrong dependent dylib for a reexport target.
constexpr std::uint32_t kLcLoadDylib        = 0xCu;
constexpr std::uint32_t kLcLoadWeakDylib    = 0x18u | kLcReqDyld;       // 0x80000018
constexpr std::uint32_t kLcReexportDylib    = 0x1Fu | kLcReqDyld;       // 0x8000001F
constexpr std::uint32_t kLcLazyLoadDylib    = 0x20u;                    // no LC_REQ_DYLD
constexpr std::uint32_t kLcLoadUpwardDylib  = 0x23u | kLcReqDyld;       // 0x80000023

// section_64.flags attribute bits (Apple `<mach-o/loader.h>`). A
// section carrying either instruction attribute is CODE; nm/otool key
// on exactly these bits (the writer stamps `__text` with both). This is
// the Mach-O twin of the c159 PE reader's IMAGE_SCN_MEM_EXECUTE test.
constexpr std::uint32_t kSAttrPureInstructions = 0x80000000u;
constexpr std::uint32_t kSAttrSomeInstructions = 0x00000400u;

// EXPORT_SYMBOL_FLAGS_* (the trie terminal payload flags, `<mach-o/loader.h>`).
constexpr std::uint64_t kExportKindMask        = 0x03u;  // regular/tls/absolute
constexpr std::uint64_t kExportWeakDefinition  = 0x04u;  // D-FF1-MACHO-WEAK-DEF
constexpr std::uint64_t kExportReexport        = 0x08u;  // Forwarder analog
constexpr std::uint64_t kExportStubAndResolver = 0x10u;  // ifunc-like (2 addrs)

// ULEB of a u64 is at most 10 bytes (9 * 7 = 63 bits + one carry bit).
constexpr unsigned kMaxUlebBytes = 10u;

// Export-trie name-explosion guard (memory safety over untrusted bytes).
// A LEGAL export trie's TOTAL materialized name bytes -- summed over every
// accumulated prefix the walk pushes -- is ~linear in the trie size: edges
// are shared and distinct exports don't repeat names, so a real dylib runs
// ~1-3x the trie size. A crafted "caterpillar" trie (a deep spine whose
// every node also terminates, so the prefixes grow linearly with depth) is
// structurally VALID -- distinct node offsets, NUL-terminated edges, all
// child offsets < trie size, no cycle -- yet drives Theta(size^2) name
// materialization: a ~1MB dylib inflates to ~GBs and OOM-kills the process.
// The reader's whole job is untrusted real dylibs, so this is a live DoS.
// FAIL LOUD once the running total exceeds this multiple of the trie size
// (32x = a >10x margin over a real dylib; the quadratic blows past it
// almost immediately, so the pre-fail allocation stays bounded at
// ~32 * trieSize). This is a legal-trie structural invariant, in the same
// family as the child-offset < trie-size discipline -- NOT a workaround.
// A single accumulated name is separately capped at the trie size (a legal
// name is the concatenation of its path edges, which cannot out-length the
// trie bytes that encode them).
constexpr std::uint64_t kMaxTrieNameBytesMultiple = 32u;

// n_type masks (Apple `<mach-o/nlist.h>`):
//   N_STAB == 0xE0 -- stab debugging entries (skip wholesale)
//   N_PEXT == 0x10 -- private extern (visibility = Hidden)
//   N_TYPE == 0x0E -- type mask: N_UNDF=0, N_ABS=2, N_SECT=0xE, N_PBUD=0xC, N_INDR=0xA
//   N_EXT  == 0x01 -- external bit (low bit; symbol is exported)
constexpr std::uint8_t  kNStabMask        = 0xE0u;
constexpr std::uint8_t  kNPextBit         = 0x10u;
constexpr std::uint8_t  kNTypeMask        = 0x0Eu;
constexpr std::uint8_t  kNExtBit          = 0x01u;

constexpr std::uint8_t  kNTypeUndf        = 0x00u;  // undefined (imported, not exported)
constexpr std::uint8_t  kNTypeSect        = 0x0Eu;  // defined in section -- the export case

// POD wrapper over the raw `n_type` byte with named accessors. The
// raw `&`/`==` idiom is repeated across the symbol-walk loop + the
// visibility helper; the type pins the bit semantics into the type
// system rather than living in repeated bit-ops + comments.
struct NType {
    std::uint8_t raw;
    [[nodiscard]] constexpr bool isStab() const noexcept {
        return (raw & kNStabMask) != 0u;
    }
    [[nodiscard]] constexpr bool isPrivateExtern() const noexcept {
        return (raw & kNPextBit) != 0u;
    }
    [[nodiscard]] constexpr bool isExternal() const noexcept {
        return (raw & kNExtBit) != 0u;
    }
    [[nodiscard]] constexpr std::uint8_t typeBits() const noexcept {
        return raw & kNTypeMask;
    }
    [[nodiscard]] constexpr bool isSectionDefined() const noexcept {
        return typeBits() == kNTypeSect;
    }
    [[nodiscard]] constexpr bool isUndefined() const noexcept {
        return typeBits() == kNTypeUndf;
    }
    [[nodiscard]] constexpr SymbolVisibility toVisibility() const noexcept {
        return isPrivateExtern() ? SymbolVisibility::Hidden
                                 : SymbolVisibility::Default;
    }
};

// One parsed section_64, in 1-based n_sect ordinal order across all
// segments -- the coordinate every export address / nlist n_sect
// resolves into for kind classification.
struct SectionInfo {
    std::uint64_t addr  = 0;
    std::uint64_t size  = 0;
    std::uint32_t flags = 0;
};

// Section-membership -> SymbolKind. A section carrying instruction
// attributes (S_ATTR_PURE_INSTRUCTIONS / S_ATTR_SOME_INSTRUCTIONS) is
// code -> Function; every other mapped section is data -> Object. This
// mirrors nm/otool and the c159 PE section-executability heuristic.
// LIMITS (documented, matching the PE twin's scope):
//   * `__TEXT,__const` rodata carries no instruction attrs -> Object (a
//     read-only global is data; the ELF STT_OBJECT / PE data precedent).
//   * thread-local sections (S_THREAD_LOCAL_*) are non-instruction data
//     -> Object today; the S_THREAD_LOCAL_* -> SymbolKind::Tls refinement
//     is the deferred richer taxonomy (anchor D-FF1-MACHO-SECT-KIND).
[[nodiscard]] constexpr SymbolKind
kindForSectionFlags(std::uint32_t flags) noexcept {
    return (flags & (kSAttrPureInstructions | kSAttrSomeInstructions)) != 0u
             ? SymbolKind::Function
             : SymbolKind::Object;
}

// Bounded ULEB128 decode over `[off, end)`. Returns {value, bytesRead}
// or nullopt when the encoding runs off `end` (no terminator byte) or
// overflows u64 (> 10 bytes / the 10th byte carries bits above 63). The
// export trie is untrusted bytes -- a truncated or hostile ULEB must
// never read past the trie region.
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::size_t>>
readUleb128(std::span<std::uint8_t const> bytes,
            std::size_t off, std::size_t end) noexcept {
    std::uint64_t result = 0;
    unsigned      shift  = 0;
    std::size_t   i      = off;
    for (unsigned n = 0; n < kMaxUlebBytes; ++n) {
        if (i >= end) return std::nullopt;        // ran off the region
        std::uint8_t const b = bytes[i++];
        // 10th byte (n==9): only bit 63 may be set (value 0x01).
        if (n + 1u == kMaxUlebBytes && (b & 0x7Fu) > 0x01u) {
            return std::nullopt;                  // overflow past u64
        }
        result |= static_cast<std::uint64_t>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0u) {
            return std::make_pair(result, i - off);
        }
        shift += 7u;
    }
    return std::nullopt;                          // no terminator in 10 bytes
}

// The leaf (basename) of a dylib path -- e.g. "/usr/lib/libSystem.B.dylib"
// -> "libSystem.B.dylib". Used to compose a reexport's forwardTarget
// (the "<dylib>.<symbol>" form, the Mach-O twin of the PE forwarder's
// "DLL.Symbol").
[[nodiscard]] std::string dylibLeaf(std::string const& path) {
    auto const slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Walk the Mach-O EXPORT TRIE (`[trieStart, trieEnd)`), emitting one
// ImportSurface row per terminal export. This is the exact inverse of
// the c153 dylib writer's `encodeExportTrie` (radix trie: uleb
// terminalSize, terminal payload, u8 childCount, then childCount edges
// of { chars + NUL, uleb childNodeOffset }). It is what dlsym itself
// walks.
//
// Fail-loud vs skip+warn split (the c159 discipline): STRUCTURAL trie
// corruption (a ULEB / edge-string / child-offset that runs past the
// trie region, a child offset >= the trie size, or a back-edge cycle)
// fails loud -- a broken link makes the whole export surface
// untrustworthy, exactly like the PE reader's truncated-EAT hard-fail.
// PER-ENTRY corruption (a terminal whose resolved address lands in no
// section) is skipped + counted (the caller emits one summarizing
// F_BinaryReaderPartialCorruption Warning), never silently dropped.
[[nodiscard]] std::expected<void, BinaryReadError>
walkExportTrie(std::span<std::uint8_t const>    bytes,
               std::size_t                      trieStart,
               std::size_t                      trieEnd,
               std::string_view                 libraryPathLabel,
               std::vector<SectionInfo> const&  sections,
               std::uint64_t                    machHeaderVa,
               std::vector<std::string> const&  dylibLeaves,
               DiagnosticReporter&              reporter,
               std::vector<ImportSurface>&      out,
               std::uint32_t&                   corruptedSkips) {
    std::size_t const trieSize = trieEnd - trieStart;
    // Name-explosion guard (see kMaxTrieNameBytesMultiple): bound the TOTAL
    // materialized name bytes across the whole walk. trieSize <= file size
    // (an on-disk offset span), so the multiply cannot overflow u64.
    std::uint64_t const nameByteBudget =
        static_cast<std::uint64_t>(trieSize) * kMaxTrieNameBytesMultiple;
    std::uint64_t totalNameBytes = 0;

    // Resolve an in-image VA to its containing section index, else nullopt.
    auto const sectionForVa =
        [&](std::uint64_t va) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < sections.size(); ++i) {
            if (va >= sections[i].addr
             && va <  sections[i].addr + sections[i].size) {
                return i;
            }
        }
        return std::nullopt;
    };

    // Iterative DFS. The visited-offset set is the cycle / DoS guard: a
    // corrupted trie whose child offset points back at an ancestor would
    // otherwise loop forever. Each node offset is visited at most once
    // (the set bounds total work at <= trieSize nodes).
    std::vector<std::pair<std::size_t, std::string>> stack;
    stack.emplace_back(trieStart, std::string{});
    std::unordered_set<std::size_t> visited;

    while (!stack.empty()) {
        auto [nodeOff, prefix] = std::move(stack.back());
        stack.pop_back();

        if (nodeOff >= trieEnd) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "Mach-O reader: export-trie node offset "
                + std::to_string(nodeOff) + " runs past the trie region "
                "(end " + std::to_string(trieEnd) + ")", reporter));
        }
        if (!visited.insert(nodeOff).second) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "Mach-O reader: export trie contains a cycle (node offset "
                + std::to_string(nodeOff) + " revisited) -- a corrupted "
                "child back-edge", reporter));
        }

        auto const term = readUleb128(bytes, nodeOff, trieEnd);
        if (!term) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "Mach-O reader: export-trie terminalSize ULEB at offset "
                + std::to_string(nodeOff) + " runs past the trie region",
                reporter));
        }
        std::uint64_t const terminalSize = term->first;
        std::size_t const   payloadOff   = nodeOff + term->second;
        // terminalSize must fit before the trie end; childrenOff follows it.
        if (terminalSize > static_cast<std::uint64_t>(trieEnd - payloadOff)) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "Mach-O reader: export-trie terminal payload (size "
                + std::to_string(terminalSize) + ") at offset "
                + std::to_string(payloadOff) + " exceeds the trie region",
                reporter));
        }
        std::size_t const childrenOff =
            payloadOff + static_cast<std::size_t>(terminalSize);

        if (terminalSize > 0u && prefix.empty()) {
            // An export reached with an EMPTY accumulated name (a
            // root-as-terminal, or an empty-edge chain). Not a usable
            // ImportSurface row -- the nlist path skip+counts an empty
            // resolved name, so mirror that here for parity rather than
            // emitting a nameless row. The terminal payload region is
            // already bounds-validated (terminalSize fits the trie), so
            // leaving it uninterpreted reads nothing out of range.
            ++corruptedSkips;
        } else if (terminalSize > 0u) {
            std::size_t const payloadEnd = childrenOff;
            auto const fl = readUleb128(bytes, payloadOff, payloadEnd);
            if (!fl) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "Mach-O reader: export-trie terminal flags ULEB for '"
                    + prefix + "' runs past the terminal payload", reporter));
            }
            std::uint64_t const flags = fl->first;
            std::size_t         p     = payloadOff + fl->second;

            ImportSurface row;
            row.mangledName = prefix;
            row.libraryPath = std::string{libraryPathLabel};
            // The export trie only ever lists externally-visible symbols
            // (hidden symbols are excluded by construction), so every
            // terminal is Default/External. Weak-def (kExportWeakDefinition)
            // surfacing as SymbolLinkage::Weak is gated: D-FF1-MACHO-WEAK-DEF.
            row.visibility = SymbolVisibility::Default;
            row.linkage    = SymbolLinkage::External;

            if ((flags & kExportReexport) != 0u) {
                // Reexport (the PE-forwarder analog): payload is
                // uleb(ordinal) then a NUL-terminated import name (the
                // symbol's name in the target dylib; empty == same name).
                auto const ord = readUleb128(bytes, p, payloadEnd);
                if (!ord) {
                    return std::unexpected(emitAndReturn(
                        BinaryReadErrorKind::CorruptedBinary,
                        "Mach-O reader: reexport ordinal ULEB for '" + prefix
                        + "' runs past the terminal payload", reporter));
                }
                p += ord->second;
                std::size_t s = p;
                while (s < payloadEnd && bytes[s] != 0u) ++s;
                if (s >= payloadEnd) {
                    return std::unexpected(emitAndReturn(
                        BinaryReadErrorKind::CorruptedBinary,
                        "Mach-O reader: reexport import-name for '" + prefix
                        + "' is not NUL-terminated within the terminal "
                        "payload", reporter));
                }
                std::string importName{
                    reinterpret_cast<char const*>(&bytes[p]),
                    static_cast<std::size_t>(s - p)};
                std::string target =
                    importName.empty() ? prefix : std::move(importName);
                // 1-based ordinal into the dependent-dylib list; resolve to
                // "<dylib-leaf>.<symbol>" when in range (SELF_LIBRARY==0 and
                // the special negative ordinals leave the bare symbol name).
                std::uint64_t const ord1 = ord->first;
                if (ord1 >= 1u && ord1 <= dylibLeaves.size()) {
                    target = dylibLeaves[static_cast<std::size_t>(ord1 - 1u)]
                             + "." + target;
                }
                row.kind          = SymbolKind::Forwarder;
                row.forwardTarget = std::move(target);
                out.push_back(std::move(row));
            } else {
                // Regular / stub-and-resolver: uleb(address); a
                // stub-and-resolver carries a SECOND uleb (the resolver
                // offset) we step over (both live in __text -- the address
                // resolves to code either way).
                auto const addr = readUleb128(bytes, p, payloadEnd);
                if (!addr) {
                    return std::unexpected(emitAndReturn(
                        BinaryReadErrorKind::CorruptedBinary,
                        "Mach-O reader: export address ULEB for '" + prefix
                        + "' runs past the terminal payload", reporter));
                }
                p += addr->second;
                if ((flags & kExportStubAndResolver) != 0u) {
                    auto const res = readUleb128(bytes, p, payloadEnd);
                    if (!res) {
                        return std::unexpected(emitAndReturn(
                            BinaryReadErrorKind::CorruptedBinary,
                            "Mach-O reader: stub-and-resolver resolver ULEB "
                            "for '" + prefix + "' runs past the terminal "
                            "payload", reporter));
                    }
                    p += res->second;
                }
                std::uint64_t const va = machHeaderVa + addr->first;
                if (sections.empty()) {
                    // No section table (a degenerate image -- real Mach-O
                    // always carries __TEXT). No membership signal exists,
                    // so floor to the dominant export kind (Function),
                    // matching the v1 contract. Documented floor, not a
                    // per-entry corruption skip.
                    row.kind = SymbolKind::Function;
                    out.push_back(std::move(row));
                } else if (auto const si = sectionForVa(va)) {
                    row.kind = kindForSectionFlags(sections[*si].flags);
                    out.push_back(std::move(row));
                } else {
                    // An export address inside a section table that maps
                    // it nowhere -- per-entry corruption: skip + count
                    // (mirrors the PE reader's "EAT RVA in no section").
                    ++corruptedSkips;
                }
            }
        }

        // Children: u8 count, then `count` edges of { chars + NUL,
        // uleb childNodeOffset (relative to trieStart) }.
        if (childrenOff >= trieEnd) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "Mach-O reader: export-trie childCount byte at offset "
                + std::to_string(childrenOff) + " runs past the trie region",
                reporter));
        }
        std::uint8_t const childCount = bytes[childrenOff];
        std::size_t        c          = childrenOff + 1u;
        for (unsigned k = 0; k < childCount; ++k) {
            std::size_t s = c;
            while (s < trieEnd && bytes[s] != 0u) ++s;
            if (s >= trieEnd) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "Mach-O reader: export-trie child edge string at offset "
                    + std::to_string(c) + " is not NUL-terminated within "
                    "the trie region", reporter));
            }
            std::string edge{reinterpret_cast<char const*>(&bytes[c]),
                             static_cast<std::size_t>(s - c)};
            c = s + 1u;
            auto const childOff = readUleb128(bytes, c, trieEnd);
            if (!childOff) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "Mach-O reader: export-trie child node-offset ULEB at "
                    + std::to_string(c) + " runs past the trie region",
                    reporter));
            }
            c += childOff->second;
            if (childOff->first >= static_cast<std::uint64_t>(trieSize)) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "Mach-O reader: export-trie child node offset "
                    + std::to_string(childOff->first) + " is past the trie "
                    "size " + std::to_string(trieSize), reporter));
            }
            // Materialize the child's accumulated name under the
            // memory-safety caps BEFORE pushing (the caterpillar-trie DoS
            // surface -- see kMaxTrieNameBytesMultiple). Per-name cap: a
            // legal name is its path edges concatenated, so it cannot
            // out-length the trie. Total cap: the running sum across the
            // whole walk stays a bounded multiple of the trie size.
            std::string childPrefix = prefix + edge;
            if (childPrefix.size() > trieSize) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "Mach-O reader: export-trie accumulated name (length "
                    + std::to_string(childPrefix.size())
                    + ") exceeds the trie size " + std::to_string(trieSize)
                    + " -- a name cannot be longer than the trie that "
                      "encodes it", reporter));
            }
            totalNameBytes += childPrefix.size();
            if (totalNameBytes > nameByteBudget) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "Mach-O reader: export-trie total materialized name "
                    "bytes (" + std::to_string(totalNameBytes) + ") exceed "
                    + std::to_string(kMaxTrieNameBytesMultiple)
                    + "x the trie size " + std::to_string(trieSize)
                    + " -- name-explosion / quadratic-blowup guard "
                      "(caterpillar trie)", reporter));
            }
            stack.emplace_back(
                trieStart + static_cast<std::size_t>(childOff->first),
                std::move(childPrefix));
        }
    }
    return {};
}

} // namespace

std::expected<std::vector<ImportSurface>, BinaryReadError>
readMacho(std::span<std::uint8_t const> bytes,
          std::string_view              libraryPathLabel,
          DiagnosticReporter&           reporter) {
    // -- mach_header_64 + magic --
    if (bytes.size() < kMachOHeaderSize) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "Mach-O reader: file is shorter than mach_header_64 (32 bytes)",
            reporter));
    }
    std::uint32_t const magic = readU32(bytes, 0);
    if (magic != kMachOMagic64) {
        // guessFormat routes only 0xFEEDFACF here; this arm guards a
        // TOCTOU between guess and read on the same byte buffer.
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "Mach-O reader: header magic is not 0xFEEDFACF "
            "(64-bit LE Mach-O)", reporter));
    }
    std::uint32_t const ncmds      = readU32(bytes, 16);
    std::uint32_t const sizeofcmds = readU32(bytes, 20);
    if (rangeExceedsBuffer(kMachOHeaderSize, sizeofcmds, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "Mach-O reader: sizeofcmds=" + std::to_string(sizeofcmds)
            + " runs past EOF (file size " + std::to_string(bytes.size())
            + ")", reporter));
    }

    // -- Walk load commands. Collect the symbol tables, the section
    //    table (for kind classification), the export trie (the dlsym
    //    surface), and the dependent-dylib list (for reexport targets). --
    std::optional<std::size_t> symtabOff;
    std::optional<std::size_t> dysymtabOff;
    std::optional<std::size_t> exportTrieOff;    // file offset of the trie
    std::optional<std::size_t> exportTrieSize;   // byte size of the trie
    std::vector<SectionInfo>   sections;
    std::vector<std::string>   dylibLeaves;
    std::optional<std::uint64_t> machHeaderVa;   // vmaddr of the header segment

    std::size_t       lcOff = kMachOHeaderSize;
    std::size_t const lcEnd = kMachOHeaderSize + sizeofcmds;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (rangeExceedsBuffer(lcOff, kMachOLcPreamble, bytes.size())
         || lcOff + kMachOLcPreamble > lcEnd) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "Mach-O reader: load command #" + std::to_string(i)
                + " preamble runs past sizeofcmds region", reporter));
        }
        std::uint32_t const cmd     = readU32(bytes, lcOff + 0);
        std::uint32_t const cmdsize = readU32(bytes, lcOff + 4);
        if (cmdsize < kMachOLcPreamble) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "Mach-O reader: load command #" + std::to_string(i)
                + " cmdsize=" + std::to_string(cmdsize)
                + " is smaller than the 8-byte preamble (corrupted)",
                reporter));
        }
        if (rangeExceedsBuffer(lcOff, cmdsize, bytes.size())
         || lcOff + cmdsize > lcEnd) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "Mach-O reader: load command #" + std::to_string(i)
                + " body (cmdsize=" + std::to_string(cmdsize)
                + ") runs past sizeofcmds region", reporter));
        }
        // Every field read below is inside [lcOff, lcOff+cmdsize), which
        // the check above proved lies inside the buffer.
        if (cmd == kLcSymtab) {
            if (cmdsize < kMachOSymtabCmdSz) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "Mach-O reader: LC_SYMTAB cmdsize=" + std::to_string(cmdsize)
                    + " (expected 24)", reporter));
            }
            symtabOff = lcOff;
        } else if (cmd == kLcDysymtab) {
            // Minimum: preamble (8) + iextdefsym/nextdefsym (2 x u32 at
            // +16) = 24 bytes. The full struct is 80 bytes; the rest is
            // deferred surfaces.
            if (cmdsize < kMachOLcPreamble + 16u) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "Mach-O reader: LC_DYSYMTAB cmdsize=" + std::to_string(cmdsize)
                    + " is too small to hold iextdefsym/nextdefsym",
                    reporter));
            }
            dysymtabOff = lcOff;
        } else if (cmd == kLcSegment64) {
            // Parse the section_64 records for classification. A segment
            // whose cmdsize is too small to hold the 72-byte header (a
            // corrupt filler LC) is tolerated -- it simply contributes no
            // sections; the LC-region walk above already fails loud on a
            // genuinely out-of-bounds body. Record the header segment's
            // vmaddr (fileoff==0 && filesize>0 -- the segment the
            // mach_header lives in; export-trie addresses are relative to
            // it). __PAGEZERO (filesize==0) is excluded.
            if (cmdsize >= kMachOSegCmd64Hdr) {
                std::uint64_t const vmaddr   = readU64(bytes, lcOff + 24);
                std::uint64_t const fileoff  = readU64(bytes, lcOff + 40);
                std::uint64_t const filesize = readU64(bytes, lcOff + 48);
                std::uint32_t const nsects   = readU32(bytes, lcOff + 64);
                if (fileoff == 0u && filesize > 0u && !machHeaderVa) {
                    machHeaderVa = vmaddr;
                }
                // Bound the section count by what actually fits in cmdsize
                // (already proved in-buffer); an inconsistent nsects that
                // over-claims is clamped rather than read out of bounds.
                std::uint64_t const roomForSects =
                    (cmdsize - kMachOSegCmd64Hdr) / kMachOSection64Sz;
                std::uint64_t const nToRead =
                    (static_cast<std::uint64_t>(nsects) < roomForSects)
                        ? static_cast<std::uint64_t>(nsects) : roomForSects;
                for (std::uint64_t s = 0; s < nToRead; ++s) {
                    std::size_t const secOff = lcOff + kMachOSegCmd64Hdr
                        + static_cast<std::size_t>(s) * kMachOSection64Sz;
                    SectionInfo info;
                    info.addr  = readU64(bytes, secOff + 32);
                    info.size  = readU64(bytes, secOff + 40);
                    info.flags = readU32(bytes, secOff + 64);
                    sections.push_back(info);
                }
            }
        } else if (cmd == kLcDyldInfo || cmd == kLcDyldInfoOnly) {
            // dyld_info_command: export_off @ +40, export_size @ +44.
            if (cmdsize >= 48u) {
                std::uint32_t const eoff = readU32(bytes, lcOff + 40);
                std::uint32_t const esz  = readU32(bytes, lcOff + 44);
                if (esz > 0u) {
                    exportTrieOff  = static_cast<std::size_t>(eoff);
                    exportTrieSize = static_cast<std::size_t>(esz);
                }
            }
        } else if (cmd == kLcDyldExportsTrie) {
            // linkedit_data_command: dataoff @ +8, datasize @ +12. The
            // modern (chained-fixups era) home of the export trie.
            if (cmdsize >= 16u) {
                std::uint32_t const doff = readU32(bytes, lcOff + 8);
                std::uint32_t const dsz  = readU32(bytes, lcOff + 12);
                if (dsz > 0u) {
                    exportTrieOff  = static_cast<std::size_t>(doff);
                    exportTrieSize = static_cast<std::size_t>(dsz);
                }
            }
        } else if (cmd == kLcLoadDylib   || cmd == kLcLoadWeakDylib
                || cmd == kLcReexportDylib || cmd == kLcLazyLoadDylib
                || cmd == kLcLoadUpwardDylib) {
            // dylib_command: name.offset @ +8, path at lcOff + name.offset.
            std::uint32_t const nameOff = (cmdsize >= 12u)
                ? readU32(bytes, lcOff + 8) : 0u;
            std::string path;
            if (nameOff >= kMachOLcPreamble && nameOff < cmdsize) {
                path = readNulTerminated(bytes, lcOff,
                                         lcOff + cmdsize, nameOff);
            }
            dylibLeaves.push_back(dylibLeaf(path));
        }
        lcOff += cmdsize;
    }

    // -- Export-trie path (the dlsym surface) --------------------------
    // When the image carries an export trie, it is the authoritative
    // export surface (what dlsym resolves + the only place reexports
    // live). Walk it, classifying each terminal's address by section
    // membership. The nlist walk below is the fallback for images with
    // no trie (a relocatable .o, an ancient dylib, or a minimal fixture).
    if (exportTrieOff && exportTrieSize) {
        std::uint64_t const trieOff  = *exportTrieOff;
        std::uint64_t const trieSize = *exportTrieSize;
        if (rangeExceedsBuffer(trieOff, trieSize, bytes.size())) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "Mach-O reader: export trie (off=" + std::to_string(trieOff)
                + " + " + std::to_string(trieSize) + " bytes) runs past EOF",
                reporter));
        }
        std::vector<ImportSurface> out;
        std::uint32_t corruptedSkips = 0;
        auto const walked = walkExportTrie(
            bytes, static_cast<std::size_t>(trieOff),
            static_cast<std::size_t>(trieOff + trieSize),
            libraryPathLabel, sections, machHeaderVa.value_or(0),
            dylibLeaves, reporter, out, corruptedSkips);
        if (!walked) return std::unexpected(walked.error());
        if (corruptedSkips > 0) {
            dss::report(reporter,
                DiagnosticCode::F_BinaryReaderPartialCorruption,
                DiagnosticSeverity::Warning,
                "Mach-O reader: '" + std::string{libraryPathLabel}
                + "': skipped " + std::to_string(corruptedSkips)
                + " export-trie terminals whose address resolved to no "
                  "section (out-of-section export pointer). Surfaced "
                + std::to_string(out.size()) + " valid exports.");
        }
        return out;
    }

    // -- LC_SYMTAB (nlist) fallback ------------------------------------
    if (!symtabOff) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::SectionNotFound,
            "Mach-O reader: no LC_SYMTAB found "
            "(stripped binary?)", reporter));
    }

    std::uint32_t const symoff  = readU32(bytes, *symtabOff +  8);
    std::uint32_t const nsyms   = readU32(bytes, *symtabOff + 12);
    std::uint32_t const stroff  = readU32(bytes, *symtabOff + 16);
    std::uint32_t const strsize = readU32(bytes, *symtabOff + 20);

    std::uint64_t const symtabBytes =
        static_cast<std::uint64_t>(nsyms) * static_cast<std::uint64_t>(kMachONlist64Sz);
    if (rangeExceedsBuffer(symoff, symtabBytes, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "Mach-O reader: symbol table (symoff=" + std::to_string(symoff)
            + " + " + std::to_string(symtabBytes) + " bytes for "
            + std::to_string(nsyms) + " entries) runs past EOF", reporter));
    }
    if (rangeExceedsBuffer(stroff, strsize, bytes.size())) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "Mach-O reader: string table (stroff=" + std::to_string(stroff)
            + " + " + std::to_string(strsize) + " bytes) runs past EOF",
            reporter));
    }

    // Filter strategy: if LC_DYSYMTAB present + nextdefsym > 0, use its
    // [iextdefsym, iextdefsym+nextdefsym) slice (the canonical
    // externally-defined symbols). Otherwise scan ALL of LC_SYMTAB and
    // filter via (N_EXT set) && (N_TYPE == N_SECT).
    std::uint32_t walkStart = 0;
    std::uint32_t walkEnd   = nsyms;
    bool          dysymtabFilter = false;
    if (dysymtabOff) {
        std::uint32_t const iextdefsym = readU32(bytes, *dysymtabOff + 16);
        std::uint32_t const nextdefsym = readU32(bytes, *dysymtabOff + 20);
        if (nextdefsym > 0u) {
            std::uint64_t const sliceEnd =
                static_cast<std::uint64_t>(iextdefsym)
                + static_cast<std::uint64_t>(nextdefsym);
            if (sliceEnd > static_cast<std::uint64_t>(nsyms)) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "Mach-O reader: LC_DYSYMTAB extdef slice "
                    "[" + std::to_string(iextdefsym) + ", "
                    + std::to_string(sliceEnd) + ") exceeds nsyms="
                    + std::to_string(nsyms), reporter));
            }
            walkStart      = iextdefsym;
            walkEnd        = static_cast<std::uint32_t>(sliceEnd);
            dysymtabFilter = true;
        }
        // nextdefsym == 0 falls through to the N_EXT walk (uniform).
    }

    std::vector<ImportSurface> out;
    out.reserve(walkEnd - walkStart);
    // D-FF1-PARTIAL-CORRUPTION-MACHO: skip cases collapse into one
    // Warning summarizing partial loss (same counter pattern as ELF+PE).
    std::uint32_t corruptedNameSkips = 0;
    for (std::uint32_t i = walkStart; i < walkEnd; ++i) {
        std::size_t const symOff = static_cast<std::size_t>(symoff)
                                  + static_cast<std::size_t>(i) * kMachONlist64Sz;
        std::uint32_t const n_strx = readU32(bytes, symOff + 0);
        NType const   nt{bytes[symOff + 4]};
        std::uint8_t const n_sect = bytes[symOff + 5];

        // Cheapest gate first: unnamed entries (by-design, not corruption).
        if (n_strx == 0u) continue;
        // Stab debug entries -- never exports. Applied to BOTH paths
        // (defense-in-depth: a malformed LC_DYSYMTAB could include stab
        // entries inside its extdef slice).
        if (nt.isStab()) continue;
        // Defense-in-depth N_TYPE filter: even on the dysymtabFilter path,
        // require N_SECT. A malformed slice with N_UNDF/N_ABS/N_INDR
        // entries would otherwise surface garbage n_value/kind.
        if (!nt.isSectionDefined()) continue;
        // Fallback walk only: also require the N_EXT bit. LC_DYSYMTAB
        // membership is the authoritative external-visibility signal when
        // present; the bit on individual rows can be inconsistent.
        if (!dysymtabFilter && !nt.isExternal()) continue;

        ImportSurface row;
        row.mangledName = readNulTerminated(bytes,
                                            static_cast<std::size_t>(stroff),
                                            static_cast<std::size_t>(stroff + strsize),
                                            n_strx);
        if (row.mangledName.empty()) {
            // n_strx was non-zero but the string-table read returned empty
            // -- out-of-range or truncated string table.
            ++corruptedNameSkips;
            continue;
        }
        row.libraryPath = std::string{libraryPathLabel};
        // Kind classification by n_sect -> section membership (the Mach-O
        // twin of the PE reader's EAT-RVA-section heuristic). n_sect is
        // 1-based; a defined N_SECT symbol always names a real section.
        if (sections.empty()) {
            // No section table (a minimal / stripped image with no
            // LC_SEGMENT_64). No membership signal -- floor to Function
            // (the v1 contract + the dominant export kind). Documented.
            row.kind = SymbolKind::Function;
        } else if (n_sect >= 1u
                && static_cast<std::size_t>(n_sect) <= sections.size()) {
            row.kind = kindForSectionFlags(sections[n_sect - 1u].flags);
        } else {
            // A defined symbol whose n_sect indexes no parsed section --
            // per-entry corruption: skip + count (mirrors the trie path
            // and the PE reader's unmapped-address skip).
            ++corruptedNameSkips;
            continue;
        }
        row.visibility  = nt.toVisibility();
        // Weak-def (n_desc & N_WEAK_DEF) surfacing as SymbolLinkage::Weak
        // is gated -- anchor D-FF1-MACHO-WEAK-DEF.
        row.linkage     = SymbolLinkage::External;
        out.push_back(std::move(row));
    }

    if (corruptedNameSkips > 0) {
        dss::report(reporter,
            DiagnosticCode::F_BinaryReaderPartialCorruption,
            DiagnosticSeverity::Warning,
            "Mach-O reader: '" + std::string{libraryPathLabel}
            + "': skipped " + std::to_string(corruptedNameSkips)
            + " LC_SYMTAB entries with corrupted name indices "
              "(non-zero n_strx resolved to empty string -- possibly "
              "truncated string table or out-of-bounds name offset) or "
              "an out-of-range n_sect. Surfaced " + std::to_string(out.size())
            + " valid symbols.");
    }

    return out;
}

} // namespace dss::ffi
