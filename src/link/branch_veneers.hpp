#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/branch_reloc_geometry.hpp"
#include "link/import_call_stub_layout.hpp"

#include <cstdint>
#include <optional>
#include <vector>

// ─────────────────────────────────────────────────────────────────────
// [[D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH]]
// (parent mechanism: [[D-LK-AARCH64-CALL26-BEYOND-RANGE-HAS-NO-VENEER]])
// LINK-TIER BRANCH VENEERS, DONE THE WAY THE REFERENCE LINKERS DO THEM
// ─────────────────────────────────────────────────────────────────────
//
// A PC-relative branch field has a reach, and a program can be bigger than it.
// When a branch cannot reach its target, the linker points it at a VENEER — a
// small synthetic function standing within reach of the branch, which carries
// control the rest of the way. Its sibling one tier down,
// [[D-CSUBSET-LONG-BRANCH]], answers the same wall INSIDE one function with
// assembler branch islands; the two tiers serve disjoint relocation classes and
// neither can stand in for the other.
//
// ★★★ EVERY CHOICE BELOW WAS READ OFF THE REFERENCES, MEASURED SEPARATELY —
// GNU ld 2.42, ld.lld 18.1.3 and ld64.lld 18.1.3 (lane `vn`, 2026-09-19; the
// transcripts are cited in the row):
//
//   * THE BODY is `adrp x16, T; add x16, x16, :lo12:T; br x16` — the body GNU
//     ld emits at every output kind within ±4 GiB, lld emits for PIE and shared
//     output, and ld64.lld emits always. PC-relative, so it needs no dynamic
//     relocation; 12 bytes; clobbers IP0 only. It is NOT spelled here: it is a
//     SEQUENCE the target declares (`linkVeneers.bodies`), assembled once per
//     link from the target's own opcodes into a template that every veneer
//     copies. Its reach is DERIVED from the relocations that template carries.
//   * WHICH BRANCHES and WHICH REGISTER are the ABI's, declared beside the body
//     (`linkVeneers.routableRelocations` / `.scratchRegisters`): AAELF64 lets a
//     linker veneer only CALL26/JUMP26-class branches, and AAPCS64 lets a
//     veneer corrupt only IP0/IP1 and the flags. A conditional branch is
//     REFUSED by both reference linkers, and by this pass.
//   * PLACEMENT is ld64.lld's single forward sweep (a new veneer goes at the
//     FURTHEST function boundary ahead within reach of its branch, and later
//     branches REUSE it while it stays in reach) plus ld.lld's fallback of the
//     boundary BEHIND the branch when none ahead is in reach — ✔MEASURED, lld
//     links a call at the top of a function longer than the reach, where GNU
//     ld and ld64.lld refuse. Where no boundary is in reach on either side,
//     every reference refuses, and so does this pass, by name.
//   * AN IMPORT'S STUB IS A TARGET LIKE ANY OTHER. Both ELF references measure
//     an import-bound call against its PLT entry; the writer here reports where
//     its stubs land (`link::ImportCallStubLayout`) and asserts that report
//     when it lays them out. This was the pass's one blind spot, and the one
//     every large image hit: the linker's own synthetic entry calls the
//     process-exit import from offset 0 of `.text`.
//
// ★★★ ONE PASS IS EXACT, AND THE ARGUMENT IS ARITHMETIC. Only the F branches
// farther than HALF their reach can ever receive a veneer, so veneers can add at
// most F × (body size) bytes anywhere. Every decision is taken against the
// reach minus exactly that margin M, so no later insertion can invalidate one:
// there is no fixed point to iterate to.
//
// ★★ THE ONE REGIME WHERE THAT IS NOT BY CONSTRUCTION: F × body > half the
// narrowest reach — on arm64 more than 5 592 405 far branches. There M is
// CAPPED at half the reach, every decision is taken against half the reach, and
// the exact verification that ends every link decides: the plan stands unless
// more than that half — 64 MiB of veneers on arm64 — lands between one branch
// and its aim, and past that it is refused by name, never emitted broken and
// never iterated. Each reference has an edge of its own: ld64.lld's single pass
// reserves 256 thunks of slop and is fatal past it, GNU ld's stub groups leave
// 1 MiB below the reach, ld.lld gives up after 30 passes.
//
// ★★ AND IT IS LINEAR. Branches are visited once, in text order; the boundary
// search is a pointer that only moves forward; reuse is an O(1) lookup per
// target; veneers are merged into the function list in ONE sweep instead of
// being inserted one `vector::insert` at a time. The work is COUNTED
// (`BranchVeneerWork`) and pinned by a test that doubles its input.
//
// The `applyExecRelocations` refusal stays exactly where it is, and stays the
// backstop: a veneer pass that got the arithmetic wrong is caught there,
// loudly, rather than emitting a branch to the wrong place.
namespace dss::linker {

// What the pass did, COUNTED — never timed. Every field is deterministic,
// host-independent and identical in Debug and Release, so a statement about
// them is a statement about the algorithm rather than about the machine.
struct DSS_EXPORT BranchVeneerWork {
    std::uint64_t sitesExamined    = 0;  // branch sites the placement sweep visited
    std::uint64_t sitesVerified    = 0;  // branch sites + veneers the exact check re-measured
    std::uint64_t farSites         = 0;  // sites farther than half their reach (the margin's base)
    std::uint64_t pointerAdvances  = 0;  // moves of the monotone boundary pointer — the only search
    std::uint64_t veneersPlaced    = 0;
    std::uint64_t veneersReused    = 0;  // sites served by a veneer an earlier site placed
    std::uint64_t behindPlacements = 0;  // veneers placed at the boundary BEHIND their site
};

// ── THE PLANNER'S MODEL ────────────────────────────────────────────────
//
// ⓘ PUBLIC SO THE ARITHMETIC CAN BE PINNED AT THE REAL REACH WITHOUT
// ALLOCATING FILLER. The planner reads function SIZES and branch sites, never
// instruction bytes, so a test can describe a 300 MiB `.text` in a few dozen
// numbers. `injectBranchVeneers` builds exactly this model from a module and
// runs exactly this planner; nothing is re-implemented for tests.

// A target stands for "a function of this module" or "an import's call stub".
inline constexpr std::uint32_t kVeneerTargetIsStub = 0xFFFFFFFFu;

struct DSS_EXPORT VeneerLayoutTarget {
    std::uint32_t function    = kVeneerTargetIsStub;  // function index, or the stub form
    std::uint64_t pastTextEnd = 0;  // stub form: upper bound of its distance past `.text`'s end
    std::int64_t  addend      = 0;  // the branch lands at (start + addend)
};

struct DSS_EXPORT VeneerLayoutSite {
    std::uint32_t function = 0;     // the function holding the branch field
    std::uint32_t offset   = 0;     // the field's byte offset within that function
    std::uint32_t target   = 0;     // index into `VeneerLayout::targets`
    std::uint32_t reach    = 0;     // index into `VeneerLayout::reaches`
    bool          routable = true;  // may this branch be routed through a veneer?
};

struct DSS_EXPORT VeneerLayout {
    std::vector<std::uint64_t>      functionSizes;
    std::vector<VeneerLayoutSite>   sites;    // ascending (function, offset)
    std::vector<VeneerLayoutTarget> targets;
    std::vector<link::RelocReach>   reaches;
    std::uint64_t                   veneerSize  = 0;  // the elected body's byte count
    link::RelocReach                veneerReach{};    // (target - veneer start) the body can encode
};

enum class VeneerPlanStatus : std::uint8_t {
    Ok,
    NotRoutable,        // a branch the ABI lets no linker veneer is out of reach
    NoBoundaryInReach,  // no function boundary within reach of the branch, on either side
    BodyOutOfReach,     // the veneer body cannot reach the target from where it must stand
    Malformed,          // the model itself is inconsistent (an internal-invariant breach)
    // (The capped-margin regime has no status of its own: the planner cannot
    // see it fail. `findVeneerPlanMisfit` does, on the plan it returns.)
};

struct DSS_EXPORT PlannedVeneer {
    std::uint32_t boundary = 0;  // stands immediately before function `boundary`
                                 // (== functionSizes.size(): after the last one)
    std::uint32_t target   = 0;  // index into `VeneerLayout::targets`
};

struct DSS_EXPORT VeneerPlan {
    VeneerPlanStatus           status       = VeneerPlanStatus::Ok;
    std::vector<std::int32_t>  siteVeneer;   // per site: -1 = reaches directly, else a veneer index
    std::vector<PlannedVeneer> veneers;      // in placement order
    std::uint32_t              failedSite   = 0;   // meaningful when status != Ok
    std::int64_t               failedDelta  = 0;   // that site's (target - site) distance
    std::uint64_t              margin       = 0;
    bool                       marginCapped = false;
    BranchVeneerWork           work;
};

// Decide, in one forward sweep, which branch sites need a veneer and where each
// veneer stands. Pure: reads the model, touches nothing else.
[[nodiscard]] DSS_EXPORT VeneerPlan planBranchVeneers(VeneerLayout const& layout);

// Re-measure a plan EXACTLY on the layout it produces (veneers inserted, every
// function moved by the veneers before it). Returns the first site — or veneer,
// flagged — whose field does not reach, or nullopt when every one fits. `work`
// (optional) accumulates `sitesVerified`.
struct DSS_EXPORT VeneerMisfit {
    bool          isVeneer = false;  // true: `index` names a veneer; false: a site
    std::uint32_t index    = 0;
    std::int64_t  delta    = 0;      // its (target - field) distance on the final layout
};
[[nodiscard]] DSS_EXPORT std::optional<VeneerMisfit>
findVeneerPlanMisfit(VeneerLayout const& layout, VeneerPlan const& plan,
                     BranchVeneerWork* work = nullptr);

// ── THE LINKER'S ENTRY POINTS ─────────────────────────────────────────

// Does any branch relocation of this module fail to reach its target on the
// layout the writer will build? Read-only and cheap — the linker asks BEFORE
// taking the copy-on-write clone of the input module (D-LK10-ENTRY-MODULE-COW),
// so a program that needs no veneer pays one scan and no copy. `stubs` is the
// format writer's own report of where each import's call stub lands.
[[nodiscard]] DSS_EXPORT bool
branchVeneersNeeded(AssembledModule const&            module,
                    TargetSchema const&               target,
                    link::ImportCallStubLayout const& stubs);

// Insert veneers so that every branch relocation reaches its target, re-pointing
// each routed relocation at the veneer standing in for it. Returns false (with a
// diagnostic naming the cause) when a branch cannot be carried — the target
// declares no veneer vocabulary, the branch is not one the ABI lets a linker
// route, no function boundary stands within its reach, or the body cannot reach
// the target from there.
[[nodiscard]] DSS_EXPORT bool
injectBranchVeneers(AssembledModule&                  module,
                    TargetSchema const&               target,
                    link::ImportCallStubLayout const& stubs,
                    DiagnosticReporter&               reporter,
                    BranchVeneerWork*                 work = nullptr);

} // namespace dss::linker
