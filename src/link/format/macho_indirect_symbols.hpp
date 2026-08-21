#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>

// The Mach-O IMAGE writer's indirect-symbol-table invariant, as a pure
// predicate over the bytes that were actually emitted — so it can be EXERCISED
// by a test instead of only read.
//
// ★★ WHY THIS REPLACED TWO CHECKS THAT COULD NOT FIRE. `encodeExecDynamic`
// briefly carried two refusal arms that read like belts and were tautologies:
// `nlistBytes.size() % kNlist64Size != 0` and
// `numberOfSymbols != numDefs + numExterns`. Both compared quantities derived
// from the SAME `nlistBytes` — `numDefs` is that buffer's length before the
// undefined band, `numberOfSymbols` its length after, and the band in between
// appends exactly `numExterns` records of exactly `kNlist64Size` bytes — so
// the second reduces to `x != x`. The comment on it called it "the
// `minted == emitted` belt", which claims it compares two INDEPENDENTLY
// produced numbers the way the MH_OBJECT writer's `nextSymIdx !=
// numberOfSymbols` genuinely does. A guard that cannot fire, labelled as one
// that can, is worse than no guard: it spends the reader's trust and returns
// nothing.
//
// ★ WHAT IS ACTUALLY AT RISK, and it is not a count. Every `indirectSyms`
// entry is an INDEX INTO the nlist that the `__stubs` and `__got` bands are
// read through: entry k names the symbol whose address slot k must be bound
// to. If that index is wrong — a mis-derived band origin, a compaction added
// to one of the two loops but not the other, an extern list that stops
// agreeing with the emitted undefined band — the slot binds to the wrong
// symbol. Nothing fails to load; a call to `puts` enters some other function.
// So the invariant worth checking is not "do the counts add up" but "does each
// index, followed into the table that was really written, land on an UNDEFINED
// EXTERNAL symbol". That is a comparison between something MINTED (the index
// arithmetic in the indirect-symbol pass) and something EMITTED (the nlist
// record at that offset), and it can fail.
//
// This is Mach-O's record layout, not shared vocabulary: it belongs to the
// Mach-O writer exactly as `stbForBinding` belongs to the ELF one.

namespace dss::link::format {

// nlist_64: n_strx(4) n_type(1) n_sect(1) n_desc(2) n_value(8).
inline constexpr std::size_t  kMachoNlist64Size   = 16;
inline constexpr std::size_t  kMachoNlistTypeOff  = 4;
// N_UNDF (0x00) | N_EXT (0x01) — an undefined external symbol, the only thing
// a stub or __got slot may be bound through.
inline constexpr std::uint8_t kMachoNTypeUndefExt = 0x01;

// Returns an empty string when every indirect-symbol entry resolves to an
// undefined external symbol in `nlistBytes`; otherwise a sentence naming the
// offending entry, the index it carried, and what that index actually landed
// on — for the caller's diagnostic.
[[nodiscard]] inline std::string
machoIndirectSymbolBreach(std::span<std::uint8_t const>  nlistBytes,
                          std::span<std::uint32_t const> indirectSyms) {
    std::size_t const nsyms = nlistBytes.size() / kMachoNlist64Size;
    if (nlistBytes.size() % kMachoNlist64Size != 0) {
        return "the nlist_64 table is " + std::to_string(nlistBytes.size())
               + " bytes, which is not a whole number of "
               + std::to_string(kMachoNlist64Size) + "-byte records";
    }
    for (std::size_t k = 0; k < indirectSyms.size(); ++k) {
        std::uint32_t const idx = indirectSyms[k];
        if (idx >= nsyms) {
            return "indirect entry #" + std::to_string(k) + " names symbol #"
                   + std::to_string(idx) + " but the table holds only "
                   + std::to_string(nsyms) + " symbols";
        }
        std::uint8_t const nType =
            nlistBytes[static_cast<std::size_t>(idx) * kMachoNlist64Size
                       + kMachoNlistTypeOff];
        if (nType != kMachoNTypeUndefExt) {
            // `{:#04x}`, not `"0x" + to_string(...)`: the first draft of this
            // message concatenated a DECIMAL rendering behind a `0x` prefix and
            // reported n_type 0x0F as "0x15". A diagnostic that states a byte
            // in the wrong base is a false statement about the artifact, and
            // this one exists to be read by someone chasing a mis-bound stub.
            return "indirect entry #" + std::to_string(k) + " names symbol #"
                   + std::to_string(idx)
                   + std::format(", whose n_type is {:#04x}", nType)
                   + " rather than N_UNDF|N_EXT — a stub or __got slot would "
                     "bind to a DEFINED symbol instead of its import";
        }
    }
    return {};
}

} // namespace dss::link::format
