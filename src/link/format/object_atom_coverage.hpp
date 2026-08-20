#pragma once

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"  // SymbolBinding / SymbolVisibility / isExternallyVisible

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Shared relocatable-object READER substrate: decide, in neutral coordinates,
// which defined symbols START a body -- and prove that no defined symbol's body
// was silently dropped. D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-
// NOT-ATOM.
//
// ★★★ THE FAILURE THIS ADDRESSES.
//
// The three relocatable-object readers (`elf/pe/macho::readRelocatableObject`)
// slice a section's bytes into ATOMS -- one `AssembledFunction` / `AssembledData`
// per defined symbol that starts a body. Which symbols START a body is decided
// per format, and the COFF and Mach-O readers both used to decide it by
// EXTERNAL-ness: a non-external defined section symbol was recorded as a
// bodiless `ModuleSymbol` on the reasoning that it is an interior `&&label` (a
// computed-goto block label), which is NOT an atom boundary and must never split
// the function that contains it. That reasoning is correct for a label and WRONG
// for a whole file-local (`static`) function -- and nlist_64 / IMAGE_SYMBOL carry
// no size field, so the two shapes are literally the same three numbers. ELF is
// unaffected only because `st_size` lets it slice any STT_FUNC with a non-empty
// extent regardless of BINDING.
//
// The consequence divided into a loud half and a silent half:
//   * CALLED file-local function -> loud. Nothing defines the relocation's
//     target atom, so the cross-CU resolve fails with `K_SymbolUndefined`.
//   * UNCALLED file-local function -> SILENT. Its bytes belong to no atom, so
//     the merge never sees them and the emitted section is short -- byte-
//     identical output across a source change that should have grown `.text`.
//
// ★★ THE ORDER THESE RUN IN IS THE DESIGN.
//
//   1. `uncoveredDefinedSymbolsThatStartAnAtom` -- the LAST-RESORT classifier.
//      For a symbol the format gives the reader no evidence about, GEOMETRY is
//      still evidence: see the soundness argument at that function. It RECOVERS
//      bodies rather than refusing them.
//   2. `resolveEqualOffsetAtomAliases` -- the IDENTITY rule, run once the
//      boundary set is FINAL and before a single byte is sliced. Two defined
//      symbols at one offset name ONE body under two names, not two bodies; it
//      picks which name owns the atom and maps the others onto it.
//   3. `everyDefinedSymbolIsCoveredByAnAtom` -- the POST-CONDITION. Run after
//      the reader has classified everything it can and re-sliced, it asserts
//      that nothing NAMED is left owning no bytes.
//
// ⓘ (2) USED TO BE THE WHOLE STORY, and reading it that way now is a mistake
// worth stating. When it landed it was a DETECTOR standing in for a fix that
// had not been made: every undecidable symbol was demoted and the guard made
// the resulting byte loss loud. As each reader learns to classify, its
// population of undecidable symbols shrinks and (2)'s role changes from
// detector to post-condition. On COFF that transition is COMPLETE -- see
// "WHAT THE POST-CONDITION STILL ASSERTS" below, which is not a rhetorical
// question in this repo.
//
// ★★ WHY THE CHECK IS ONE IMPLEMENTATION AND NOT THREE.
//
// The convergence is the point: all three readers reduce a wildly different
// on-disk shape to the SAME `(sectionKey, byteOffset)` staging before they slice
// atoms out of it. Both questions -- "does some reconstructed atom own the byte
// this defined symbol starts at?" and "which of the leftovers must start an atom
// of their own?" -- are asked entirely in those neutral coordinates, so they
// need no ELF/COFF/Mach-O knowledge and there is no `if (format == ...)` here or
// anywhere below. `sectionKey` is whatever section identity the caller already
// uses (an ELF `st_shndx`, a COFF 1-based ordinal, a Mach-O `n_sect`) widened to
// u32 and never interpreted; it only has to compare equal for two things in the
// same section. This mirrors `link/format/interior_block_symbol_va.hpp` on the
// WRITER side: three walkers, one helper, differing only in the values they
// already computed.
//
// ★ WHY THE TEST IS "COVERED BY AN ATOM" AND NOT "ATOMS COVER THE SECTION".
// The stronger phrasing would be wrong for ELF and would break the one reader
// that is already correct: inter-function ALIGNMENT PADDING in `.text` belongs to
// no function and is legitimately uncovered (`elf_object_reader.cpp`'s step-6.5
// gap atoms deliberately fill DATA sections only, for exactly this reason). A
// symbol-anchored test asks only about bytes somebody NAMED, which is precisely
// the set whose loss is a miscompile. The same restriction bounds the promotion
// pass: it recovers only bytes a symbol NAMES, and an ANONYMOUS uncovered region
// (a gcc `.rdata` jump table, which carries no symbol at all and is reached
// through the section symbol plus an addend) is not its business -- that is
// D-LK-COFF-READER-ANONYMOUS-GAP-ATOMS, and the readers still refuse loud when
// such a region carries relocations.
//
// ★ WHY A GENUINE INTERIOR LABEL PASSES. An `&&label` lies INSIDE the function
// that contains it, so the enclosing atom covers its offset, the promotion pass
// leaves it alone and the post-condition is silent -- no escape hatch, no format
// test, no name matching. That is the whole discrimination: a real interior
// label is covered, a demoted whole function is not.
//
// ⚠ KNOWN BOUNDARY, stated so nobody reads more into a green than it carries.
// A symbol that TRAILS an atom is invisible to BOTH functions here: the
// preceding atom runs to the end of the section (the COFF/Mach-O slicing rule
// when no further boundary symbol exists), so the trailing symbol's offset IS
// covered -- the post-condition stays silent and the promotion pass sees nothing
// to promote. Its bytes still reach the image, riding along inside the wrong
// atom rather than vanishing. Telling that case from an interior label is not a
// geometry question at all and never can be; it needs per-format evidence, and
// each reader closes it in its own vocabulary or not at all:
//   * COFF closes it fully. A file-local FUNCTION declares IMAGE_SYM_DTYPE_
//     FUNCTION, and a file-local DATA object is a class-STATIC symbol in a
//     non-code section -- both are classified as boundaries BEFORE slicing, so
//     position never matters (see `coff_object_reader.hpp` clause (7)).
//   * Mach-O closes it for an object that DECLARES
//     `MH_SUBSECTIONS_VIA_SYMBOLS`: every defined symbol then starts an atom
//     unless it says `N_ALT_ENTRY`, so position stops mattering there too.
//     An object WITHOUT that header flag keeps the narrow external-only rule,
//     and for those the boundary still binds -- a trailing local is absorbed
//     and nothing here can see it.
//
// ★★★ WHAT THE POST-CONDITION STILL ASSERTS ONCE A READER PROMOTES.
//
// A reader that promotes every uncovered symbol can never reach the refusal
// through its OWN staging -- by construction, what is left is covered. That
// would make the guard inert for that reader if the guard were only about the
// input, and this repo treats an inert guard as a defect. It is not inert,
// because it no longer asks a question about the OBJECT; it asks one about the
// READER:
//   * It is the ONLY thing that reds if the promotion pass computes the wrong
//     coordinates -- a mis-keyed section, an offset staged before the reader's
//     own section-relative conversion, a boundary added to the wrong section's
//     list. Every one of those produces atoms that exist but do not cover the
//     symbol they were minted for, and nothing else in the pipeline would
//     notice: the bytes would go to the wrong atom, not to no atom.
//   * It stays a LIVE detector for any reader that has NOT taken the promotion
//     pass, and Mach-O has not: it classifies from the object's own
//     `MH_SUBSECTIONS_VIA_SYMBOLS` declaration and deliberately stops there, so
//     an object that declares nothing still leaves genuinely unplaceable
//     symbols and the refusal is still the correct and reachable outcome.
//   * It is what a reader must keep passing as its classifier grows. A future
//     COFF/Mach-O arm that decides a symbol is NOT a boundary re-enters this
//     population immediately.
// Both properties are pinned by MUTANT, not by argument, in
// `tests/link/test_object_atom_coverage.cpp`.

namespace dss::link::format {

// A DEFINED, section-backed symbol that a reader recorded WITHOUT a body.
//
// Callers stage ONLY symbols in a section whose `SectionKind` RESOLVED -- real
// code/data the `AssembledModule` models. A symbol in an unmodeled metadata
// section (`.eh_frame`, `.debug$S`, `.pdata`) reconstructs no body BY DESIGN and
// is not a dropped body, so it is not a candidate. Callers also skip symbols
// that are section IDENTITIES rather than bodies (an ELF `STT_SECTION`, a COFF
// section-definition symbol).
struct BodilessDefinedSymbol {
    std::uint32_t sectionKey    = 0;  // caller's own section identity — opaque here
    std::uint64_t sectionOffset = 0;  // section-relative byte offset of the symbol
    // The symbol's KNOWN extent. `nullopt` means unknown — the usual case for
    // COFF `IMAGE_SYMBOL` and Mach-O `nlist_64`, which have no size field,
    // which is itself the reason those two readers cannot tell a local function
    // from a local label. A value comes either from the format's own field (ELF
    // `st_size`) or from a caller that can DERIVE the extent with certainty: a
    // symbol sitting exactly at its section's end has no room for a byte, so a
    // size-less format can still stage it as 0 and both functions below will
    // correctly treat it as a marker rather than a lost body.
    std::optional<std::uint64_t> declaredSize;
    std::string                  name;
    std::string                  sectionName;  // for the diagnostic only
};

// One reconstructed atom's byte extent within its section — the SAME
// `[start, start+len)` interval the reader already built to route relocation
// sites, restated in the neutral coordinates above.
struct ReconstructedAtomExtent {
    std::uint32_t sectionKey = 0;
    std::uint64_t start      = 0;
    std::uint64_t len        = 0;
};

// THE coverage predicate, stated once because two callers below ask it and a
// decision written twice is a decision that can disagree with itself. A
// zero-length atom covers nothing, INCLUDING its own start offset — it owns no
// bytes, so it can lose none.
[[nodiscard]] inline bool isCoveredByAnAtom(
    BodilessDefinedSymbol const&             sym,
    std::span<ReconstructedAtomExtent const> atoms) noexcept {
    for (auto const& atom : atoms) {
        if (atom.sectionKey != sym.sectionKey) continue;
        if (sym.sectionOffset >= atom.start
            && sym.sectionOffset - atom.start < atom.len) {
            return true;
        }
    }
    return false;
}

// A symbol the format declares to be ZERO bytes long is a MARKER, not a body —
// an ARM `$d`/`$x` mapping symbol, an empty object, a size-less text label —
// so both functions below skip it: there are no bytes to lose and nothing to
// promote. A `nullopt` extent is NOT treated as zero; it is UNKNOWN, and an
// unknown-extent symbol outside every atom is the exact shape of the dropped
// file-local function.
[[nodiscard]] inline bool isDeclaredEmptyMarker(
    BodilessDefinedSymbol const& sym) noexcept {
    return sym.declaredSize.has_value() && *sym.declaredSize == 0u;
}

// ═══ THE EQUAL-OFFSET ALIAS RULE ═══════════════════════════════════════════
//
// ★★★ THE FAILURE. Two defined symbols at the SAME offset of one section are
// two NAMES FOR ONE BODY, and every reader here used to mint an atom per
// boundary symbol -- so they came back as two byte-identical TWIN atoms over
// the same span. That is not merely wasteful:
//
//   * Each reader routes a relocation to its atom by scanning the section's
//     interval list for the first one containing the site. Twins have the
//     IDENTICAL interval, so EXACTLY ONE of them is ever found. The other
//     ships with its relocation never applied -- an un-patched `bl`/`call`
//     whose immediate is still 0, i.e. a branch to itself.
//   * Which twin wins is decided by the boundary set's sort order, so the
//     un-patched copy is routinely the EXTERNAL name -- the one every sibling
//     CU and every foreign linker resolves through.
//
// ✔MEASURED, and reachable from a THREE-LINE C file. clang 19
// (`-target arm64-apple-macos11 -c -O1`) emits a section-start label `ltmp0`
// at n_value 0 and the first function at n_value 0, with the function's own
// `bl` relocation inside the span they share. This is not an exotic shape: it
// is what clang emits for every ordinary translation unit. gcc/ELF reaches the
// same shape through a weak alias (`__attribute__((alias("g")))`), where the
// alias and its target share `st_value` and `st_size`. It is NOT reachable
// from DSS's own output, whose atoms all start at distinct offsets.
//
// ★★ THE MODEL: ONE ATOM, SEVERAL NAMES. The atom is minted once, for ONE of
// the names; every other name in the set becomes a `ModuleSymbol` row carrying
// THAT SAME SymbolId, and every relocation naming an aliased symbol is remapped
// onto it. Both halves are load-bearing and neither is optional:
//
//   * THE REMAP, because `buildCompoundIndex` declares a `SymbolKind` only for
//     ids that own a body (a function / data item / extern import). A
//     relocation naming an id that owns nothing is `K_SymbolUndefined` -- so
//     without the remap, collapsing the twins would trade a silent miscompile
//     for a spurious link failure.
//   * THE SHARED SymbolId ON THE ALIAS ROWS, because the writers' export walkers
//     (the ELF ET_DYN `.dynsym` set, the PE `.edata` table) look each
//     externally-visible `ModuleSymbol` up in the function/data tables and FAIL
//     LOUD on a row that "names neither a defined function nor a data item".
//     An alias row carrying its OWN id would trip that contract; carrying the
//     atom's id makes the two names export at one address, which is what an
//     alias IS.
//
// ⚠ CALLERS MUST APPEND THE ALIAS ROWS AFTER THE CANONICAL ONE. Every consumer
// of `AssembledModule::symbols` that maps id -> row (`ObjectSymbolNames`, the
// readers' own name lookups) keeps the FIRST row for an id, so a row order that
// puts an alias first would hand the atom the alias's name -- and for a LOCAL
// alias of an EXTERNAL function that silently demotes the emitted symbol to the
// internal `<prefix><id>` fallback. Order is part of the contract, not a detail.
//
// ★ WHY THIS IS ONE RULE AND NOT THREE. The three readers reach the shape by
// three routes -- ELF by two symbols sharing `st_value`, COFF and Mach-O by two
// boundary symbols at one section offset -- but the QUESTION is identical and is
// asked entirely in the `(sectionKey, offset)` coordinates the readers already
// stage. There is no format knowledge here and no `if (format == ...)`.

// One symbol a reader has decided STARTS an atom, in the neutral coordinates.
// `symbolId` is the reader's own `SymbolId::v` for it (an ELF/Mach-O symtab
// index, a COFF symbol-table index); `name` / `sectionName` are for the
// diagnostic only.
struct AtomStartCandidate {
    std::uint32_t sectionKey = 0;
    std::uint64_t offset     = 0;
    // The extent the FORMAT declares for this symbol (ELF `st_size`). `nullopt`
    // means the reader DERIVES the extent from the next boundary instead (COFF
    // `IMAGE_SYMBOL` and Mach-O `nlist_64` carry no size field) -- in which case
    // two candidates at one offset necessarily get the SAME extent and the
    // conflicting-extent refusal below cannot arise for them.
    std::optional<std::uint64_t> declaredExtent;
    std::uint32_t                symbolId   = 0;
    SymbolBinding                binding    = SymbolBinding::Global;
    SymbolVisibility             visibility = SymbolVisibility::Default;
    std::string                  name;
    std::string                  sectionName;  // for the diagnostic only
};

// ★ WHICH NAME OWNS THE ATOM. Ranked, and the ranking is an argument rather
// than a convenience:
//
//   1. EXTERNALLY VISIBLE beats module-private. The owning id is the only one
//      that gets a body, a `SymbolKind::Defined` row in the linker's compound
//      index, an address, and a `.symtab`/`.dynsym` entry under its real name.
//      Give the body to clang's local `ltmp0` and the external `_outer` becomes
//      a name with nothing behind it: the re-emitted object exports the body as
//      an internal `sym_<id>`, and every foreign reference to `_outer` fails to
//      resolve. Give it to `_outer` and the object is correct; `ltmp0` losing
//      its separate identity costs nothing, because a compiler-private section
//      label denotes exactly the address `_outer` already denotes.
//   2. STRONG beats WEAK among externally-visible names -- the genuine weak
//      alias (`void f() __attribute__((weak, alias("g"))))`). Both names are
//      real ABI, so this one is a tie-break rather than a correctness rule, and
//      it goes to the strong name because that is the definition the alias was
//      declared against; the weak name keeps its own `Weak` binding on its row
//      and still exports at the same address.
//   3. Otherwise the LOWEST symbol id -- the order the OBJECT listed them in.
//      This exists so the answer is TOTAL: `std::sort` is not stable, and a
//      reconstruction that depends on which of two equal elements the sort
//      happened to keep is a reconstruction that can differ between two runs
//      over the same bytes.
[[nodiscard]] inline bool outranksAsAtomIdentity(
    AtomStartCandidate const& a, AtomStartCandidate const& b) noexcept {
    bool const aVis = isExternallyVisible(a.binding, a.visibility);
    bool const bVis = isExternallyVisible(b.binding, b.visibility);
    if (aVis != bVis) return aVis;
    bool const aStrong = a.binding != SymbolBinding::Weak;
    bool const bStrong = b.binding != SymbolBinding::Weak;
    if (aStrong != bStrong) return aStrong;
    return a.symbolId < b.symbolId;
}

// ★★★ THE RESOLVER. Fills `ownerSymbolId` (parallel to `candidates`) with the
// symbol id that OWNS the atom each candidate starts -- its own id when it is
// the canonical name of its set, the canonical name's id otherwise. A caller
// mints an atom for candidate `i` iff `ownerSymbolId[i] == candidates[i].symbolId`,
// records a `ModuleSymbol` row under `ownerSymbolId[i]` either way, and remaps
// every relocation target through the same table.
//
// ★★ WHAT STILL FAILS LOUD, and why it is a DIFFERENT shape rather than a
// stricter alias. Two candidates at one offset whose DECLARED extents disagree
// are not two names for one body -- they are NESTED bodies, and no single atom
// is right for both: a relocation past the shorter extent belongs unambiguously
// to the longer symbol, while one inside the shorter is claimed by both. There
// is no answer to pick, only a guess to make, so the reader refuses and names
// both symbols. (This TIGHTENS a rule that used to wave equal-start pairs
// through unconditionally on the grounds that routing to either is
// offset-correct -- true only when the extents match, which is precisely what
// went unchecked.) Only a format with a real size field can produce it; a
// reader that derives extents from the next boundary cannot.
//
// Emits AT MOST ONE diagnostic and returns false; returns true with no
// diagnostics otherwise. `readerName` prefixes the message the way each reader
// already prefixes its own refusals.
[[nodiscard]] inline bool resolveEqualOffsetAtomAliases(
    std::span<AtomStartCandidate const> candidates,
    std::vector<std::uint32_t>&         ownerSymbolId,
    std::string_view                    readerName,
    DiagnosticReporter&                 reporter) {
    ownerSymbolId.assign(candidates.size(), 0u);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        ownerSymbolId[i] = candidates[i].symbolId;
    }

    // Group by (sectionKey, offset) WITHOUT reordering the caller's vector --
    // the caller's order is its atom order, and permuting it here would silently
    // permute the reconstruction.
    std::vector<std::size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (candidates[a].sectionKey != candidates[b].sectionKey)
            return candidates[a].sectionKey < candidates[b].sectionKey;
        if (candidates[a].offset != candidates[b].offset)
            return candidates[a].offset < candidates[b].offset;
        return candidates[a].symbolId < candidates[b].symbolId;
    });

    for (std::size_t g = 0; g < order.size();) {
        std::size_t h = g + 1;
        while (h < order.size()
               && candidates[order[h]].sectionKey == candidates[order[g]].sectionKey
               && candidates[order[h]].offset == candidates[order[g]].offset) {
            ++h;
        }
        if (h - g > 1) {
            std::size_t canonical = order[g];
            // The FIRST candidate in the group that declares an extent, kept as
            // the yardstick every later one is measured against. Comparing
            // CONSECUTIVE pairs instead would miss `{16, unknown, 8}` -- neither
            // adjacent pair has two known extents, and the conflict at the ends
            // would pass. No shipped reader mixes known and unknown extents in
            // one object, which is exactly why that hole would have gone unseen.
            std::size_t stated = h;
            for (std::size_t k = g; k < h; ++k) {
                if (candidates[order[k]].declaredExtent.has_value()) { stated = k; break; }
            }
            for (std::size_t k = g + 1; k < h; ++k) {
                AtomStartCandidate const& cur  = candidates[order[k]];
                AtomStartCandidate const& prev = candidates[order[stated < k ? stated : k - 1]];
                if (cur.declaredExtent.has_value() && prev.declaredExtent.has_value()
                    && *cur.declaredExtent != *prev.declaredExtent) {
                    dss::report(
                        reporter, DiagnosticCode::F_CorruptedBinary,
                        DiagnosticSeverity::Error,
                        std::string{readerName} + ": defined symbols '" + prev.name
                            + "' (" + std::to_string(*prev.declaredExtent)
                            + " bytes) and '" + cur.name + "' ("
                            + std::to_string(*cur.declaredExtent)
                            + " bytes) both start at offset "
                            + std::to_string(cur.offset) + " of section '"
                            + cur.sectionName
                            + "' but declare DIFFERENT extents -- they are nested "
                              "bodies, not two names for one body, and a relocation "
                              "in the range they disagree about cannot be attributed "
                              "to either without guessing. Refusing to reconstruct "
                              "one atom whose extent is wrong for one of them. "
                              "D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS.");
                    return false;
                }
                if (outranksAsAtomIdentity(cur, candidates[canonical])) {
                    canonical = order[k];
                }
            }
            for (std::size_t k = g; k < h; ++k) {
                ownerSymbolId[order[k]] = candidates[canonical].symbolId;
            }
        }
        g = h;
    }
    return true;
}

// ★★★ THE GEOMETRY FALLBACK. Given the atoms a reader HAS reconstructed and the
// defined symbols it could not classify, return (ascending indices into
// `bodiless`) the ones that must START an atom of their own. The caller adds
// them to its boundary set and RE-SLICES; everything downstream — body slicing,
// bounds checks, relocation routing — is the path an external symbol already
// takes, unchanged.
//
// ★ WHY IT IS SOUND, in the promote direction only.
//
// The demotion this exists to undo rests on one claim: "a non-external defined
// symbol is an interior block label". An interior label lies INSIDE the function
// that contains it, so IF that function was reconstructed, the label's offset is
// covered. Contrapositive: a defined symbol that NO reconstructed atom covers is
// not interior to any reconstructed atom, so the demotion's claim cannot be true
// of it. It names bytes that belong to nothing else — i.e. it starts a body.
//
// ⚠ THE PREMISE HAS A HOLE, AND THE RUN RULE IS WHERE IT IS PATCHED. The
// contrapositive only rules out being interior to a RECONSTRUCTED atom. Two
// uncovered symbols in the same section with no reconstructed atom between them
// are genuinely undecidable: `{f@0, L@4}` before an atom at 8 is either two
// bodies, or ONE body at 0 with a label at 4. So only the FIRST symbol of each
// uncovered run is promoted — it provably starts the run's bytes, since nothing
// at a lower offset in the run claims them — and the rest become interior to it
// once it is sliced. A symbol carrying a non-zero DECLARED size is exempt from
// the run rule: its extent is stated by the format, not inferred, so a later
// symbol beyond that extent is not inside it.
//
// ★ WHY THE UNDECIDABLE HALF MERGES RATHER THAN SPLITS — the asymmetry that
// picks the default. The two errors are not equally bad:
//   * MERGING two bodies into one atom keeps every byte, in order, in one
//     section. The result is a MISATTRIBUTION — the same defect class as the
//     KNOWN BOUNDARY above — and the image is still correct.
//   * SPLITTING one body at an interior label produces two atoms the linker may
//     lay out with padding between them, silently breaking every intra-function
//     relative reference that crosses the split. That is a miscompile.
// The same asymmetry is why the whole pass PROMOTES rather than refuses:
// promotion preserves the bytes (they become their own atom), demotion drops
// them. Between "possibly the wrong atom" and "no atom at all", only one of
// them can be a silent byte loss.
//
// ⓘ It cannot see the TRAILING case, deliberately and unavoidably — see KNOWN
// BOUNDARY above. Geometry has nothing to say about a symbol that IS covered.
[[nodiscard]] inline std::vector<std::size_t> uncoveredDefinedSymbolsThatStartAnAtom(
    std::span<BodilessDefinedSymbol const>   bodiless,
    std::span<ReconstructedAtomExtent const> atoms) {
    // Ascending (sectionKey, offset): promoting an earlier symbol is what
    // decides whether a later one is interior to it, so the walk order is part
    // of the rule and not an implementation detail. `stable_sort` keeps
    // equal-offset ALIASES in staging order, so the first-staged of an alias set
    // is the one promoted and the others land inside its atom.
    std::vector<std::size_t> order(bodiless.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) {
                         if (bodiless[a].sectionKey != bodiless[b].sectionKey)
                             return bodiless[a].sectionKey < bodiless[b].sectionKey;
                         return bodiless[a].sectionOffset < bodiless[b].sectionOffset;
                     });

    // The most recently promoted atom, as an [start, end) the following
    // candidates of the SAME section may fall inside. Offsets ascend, so one
    // slot is enough: a candidate past `runEnd` can never re-enter an older run.
    bool          haveRun    = false;
    std::uint32_t runSection = 0;
    std::uint64_t runStart   = 0;
    std::uint64_t runEnd     = 0;

    std::vector<std::size_t> promote;
    for (std::size_t idx : order) {
        BodilessDefinedSymbol const& sym = bodiless[idx];
        if (isDeclaredEmptyMarker(sym)) continue;
        if (isCoveredByAnAtom(sym, atoms)) continue;
        if (haveRun && runSection == sym.sectionKey
            && sym.sectionOffset >= runStart && sym.sectionOffset < runEnd) {
            continue;  // interior to a body promoted earlier in this run
        }

        promote.push_back(idx);

        // How far the newly promoted atom reaches. A DECLARED size is the
        // format's own statement and wins. Otherwise the atom runs to the next
        // RECONSTRUCTED atom start in the section — the COFF/Mach-O "next
        // strictly-greater boundary" slicing rule the caller will apply — or,
        // if there is none, to the end of the section, which no valid symbol
        // offset can exceed.
        haveRun    = true;
        runSection = sym.sectionKey;
        runStart   = sym.sectionOffset;
        if (sym.declaredSize.has_value()) {
            runEnd = sym.sectionOffset + *sym.declaredSize;
        } else {
            runEnd = std::numeric_limits<std::uint64_t>::max();
            for (auto const& atom : atoms) {
                if (atom.sectionKey != sym.sectionKey) continue;
                if (atom.start > sym.sectionOffset && atom.start < runEnd) {
                    runEnd = atom.start;
                }
            }
        }
    }

    std::sort(promote.begin(), promote.end());
    return promote;
}

// ★★★ THE ANONYMOUS-BYTE PASS. Every maximal byte range of one section that NO
// reconstructed atom owns, ascending. The caller mints one synthetic atom per
// range; this decides WHICH bytes, never what to build from them, so it stays
// free of every reader's buffer and section plumbing.
//
// ★ WHY THIS BELONGS NEXT TO THE COVERAGE QUESTION rather than in one reader.
// It is the SAME question asked about the complement: `everyDefinedSymbol...`
// asks "does an atom own the byte this SYMBOL starts at?", this asks "which
// bytes does no atom own at all?". ELF has answered the second since the c167
// gap-atom fold, because gcc reaches a string literal or a jump table through a
// SECTION symbol plus an addend and never names it -- so without the bytes the
// reference dangles. COFF has exactly the same shape (✔MEASURED: mingw gcc at
// `-O2` puts a switch's jump table in `.rdata` with NO symbol on it, reached
// from `.text` by a REL32 against the `.rdata` section symbol), and Mach-O's
// `L_` literal sections are the third instance. One rule, three callers.
//
// ⚠ CALLERS MUST RESTRICT THIS TO FILE-BACKED *DATA* SECTIONS WHOSE KIND IS
// DECLARED, and the restriction is not squeamishness -- each half of it is a
// measured failure:
//   * NOT Text. A `.text` gap is inter-function ALIGNMENT PADDING, which belongs
//     to no function legitimately. Fabricating a code atom from padding would
//     hand the linker bytes to lay out and, worse, give a corrupt code reference
//     somewhere to land instead of failing loud.
//   * NOT a kind guessed from FLAGS. `.eh_frame` is SHF_ALLOC PROGBITS and a
//     flags-based guess types it Rodata; a gap atom there un-skips its
//     relocations (whose targets are in `.text`) and mis-routes them.
//   * NOT zero-fill. There are no bytes to recover.
// The parameters carry none of that: `sectionSize` and the atoms are all this
// needs, and the caller has already decided the section qualifies.
//
// ⓘ INERT ON DSS'S OWN OUTPUT, by construction rather than by luck: DSS names
// every data item it writes, so its sections are fully covered and this returns
// nothing. It fires only on foreign objects.
[[nodiscard]] inline std::vector<ReconstructedAtomExtent> unownedByteRangesOfSection(
    std::uint32_t                            sectionKey,
    std::uint64_t                            sectionSize,
    std::span<ReconstructedAtomExtent const> atoms) {
    std::vector<ReconstructedAtomExtent> covered;
    for (auto const& a : atoms) {
        if (a.sectionKey == sectionKey) covered.push_back(a);
    }
    std::sort(covered.begin(), covered.end(),
              [](ReconstructedAtomExtent const& a, ReconstructedAtomExtent const& b) {
                  return a.start < b.start;
              });

    std::vector<ReconstructedAtomExtent> gaps;
    std::uint64_t cursor = 0;
    for (auto const& iv : covered) {
        if (iv.start > cursor) {
            gaps.push_back(ReconstructedAtomExtent{sectionKey, cursor, iv.start - cursor});
        }
        // `max`, not assignment: atoms may NEST -- a DATA symbol wholly inside
        // another (an ELF `.rodata` sub-object), which no reader guards against.
        // Advancing to `iv.start + iv.len` unconditionally would walk the cursor
        // BACKWARDS out of a nested atom and mint a gap over bytes the enclosing
        // atom already owns. (An EQUAL-start pair can no longer reach here --
        // `resolveEqualOffsetAtomAliases` collapses it to one atom, or refuses
        // it when the declared extents disagree.)
        cursor = std::max(cursor, iv.start + iv.len);
    }
    if (cursor < sectionSize) {
        gaps.push_back(ReconstructedAtomExtent{sectionKey, cursor, sectionSize - cursor});
    }
    return gaps;
}

// THE POST-CONDITION. Fail loud NAMING the symbol when a staged bodiless defined
// symbol's offset is owned by no reconstructed atom of its section.
//
// ★ CALLERS PASS *EVERY* STAGED CANDIDATE, INCLUDING THE ONES THEY PROMOTED,
// and that is the whole reason the check still has teeth on a reader that
// promotes. Passing only the leftovers would make it a tautology — the leftovers
// are, by the pass's own definition, the covered ones. Passing the promoted ones
// too turns it into the assertion that each promotion actually MATERIALISED as
// an atom at the coordinate it was filed under, which is the failure mode
// nothing else downstream can see.
//
// `readerName` prefixes the message the way each reader already prefixes its own
// refusals (e.g. "pe::readRelocatableObject"). Emits AT MOST ONE diagnostic (the
// first uncovered symbol) and returns false; returns true with no diagnostics
// when every staged symbol is covered.
//
// ★ `cause` IS THE CALLER'S OWN SENTENCE ABOUT WHY ITS CLASSIFIER COULD NOT
// PLACE THIS SYMBOL, and it is a parameter because the readers stopped agreeing.
// The message used to assert, for everyone, that "this reader treats a
// non-external defined symbol as an interior block label" — true of all three
// when the guard was written, and FALSE FOR COFF the moment the PE reader
// learned to classify. A shared diagnostic that describes a classifier is only
// as true as the classifiers stay identical, so each caller supplies its own
// clause and the DEFAULT stays true of any reader that has not diverged. It is a
// sentence FRAGMENT, spliced after "which no reconstructed atom covers -- ".
[[nodiscard]] inline bool everyDefinedSymbolIsCoveredByAnAtom(
    std::span<BodilessDefinedSymbol const>   bodiless,
    std::span<ReconstructedAtomExtent const> atoms,
    std::string_view                         readerName,
    DiagnosticReporter&                      reporter,
    std::string_view                         cause =
        "this reader treats a non-external defined symbol as an interior block "
        "label rather than an atom boundary, which is wrong for a file-local "
        "(`static`) function") {
    for (auto const& sym : bodiless) {
        if (isDeclaredEmptyMarker(sym)) continue;
        if (isCoveredByAnAtom(sym, atoms)) continue;
        dss::report(
            reporter, DiagnosticCode::F_ObjectReaderSymbolBodyDropped,
            DiagnosticSeverity::Error,
            std::string{readerName} + ": defined symbol '" + sym.name
                + "' lies at offset " + std::to_string(sym.sectionOffset)
                + " of section '" + sym.sectionName
                + "', which no reconstructed atom covers -- its bytes would be "
                  "silently dropped from the linked image. "
                + std::string{cause}
                + "; refusing to link a partially reconstructed object. "
                  "D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM.");
        return false;
    }
    return true;
}

} // namespace dss::link::format
