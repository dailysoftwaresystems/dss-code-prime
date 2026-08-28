#pragma once

#include "core/types/target_schema.hpp"

#include <cstdint>
#include <string>
#include <string_view>

// ═══════════════════════════════════════════════════════════════════════
// "Which of this target's relocation rows patches an UNWIND TABLE's
// code-pointer field?" — asked ONCE, for every format.
//
// D-LK-PE-OBJ-ARM-CARRIES-NO-UNWIND-INFO.
// ═══════════════════════════════════════════════════════════════════════
//
// An unwind table is a table of ADDRESSES OF CODE stored in DATA. In a
// relocatable object none of those addresses is known, so every one of
// them is a relocation — and each format's table states its pointers in
// its own coordinate space:
//
//   * DWARF `.eh_frame`  — the FDE's `initial_location`, a
//                          `DW_EH_PE_pcrel|sdata4`: 32-bit, PC-RELATIVE,
//                          no implicit bias.  ELF and Mach-O.
//   * Win64 `.pdata`     — a `RUNTIME_FUNCTION`'s three fields and the
//     / `.xdata`           UNWIND_INFO handler field, all RVAs: 32-bit,
//                          IMAGE-RELATIVE, no implicit bias.  PE.
//
// ★ TWO DESCRIPTIONS, ONE SCANNER — AND THAT SPLIT IS THE POINT.
//   Before this file, `dwarf_cfi.hpp` owned the whole answer for ELF and
//   `pe.cpp` was about to grow a second copy for PE: two functions
//   re-deciding what "ambiguous" means, what "absent" means, and what to
//   say about either.  The part that genuinely differs between the two
//   formats is the FIELD'S SHAPE — four scalars — and that is now the
//   only thing a caller supplies.  The matching rule, the two-rows
//   refusal and the no-rows refusal have ONE implementation, so a third
//   encoding (ARM EHABI's `.ARM.exidx`, PE-ARM64's packed `.pdata` v2)
//   arrives as a `constexpr` shape and a psABI row, never as a third
//   scanner.
//
// ★ THE RELOCATION IS FOUND BY WHAT IT DOES, NOT BY WHO IS ASKING.
//   A `switch (machine)` or a name match on "imagerel32" would be a
//   second owner for a fact the target's own relocation table already
//   states in full: `formulaKind`, `pcRelative`, `imageRelative`,
//   `addendBias` and `widthBytes` together identify the row exactly.  A
//   target that grows a third architecture inherits the lookup by
//   declaring the row, and never by editing this file.
//
// ⚠ AMBIGUITY IS A REFUSAL, NOT A FIRST MATCH.  Two rows answering one
//   description means the schema no longer identifies a relocation;
//   silently picking either writes a table that is wrong for half the
//   reasons the other row exists, and an unwinder follows a wrong table
//   without complaining.  ✔This is not hypothetical on x86_64: `abs32`
//   and `imagerel32` agree on formula, width, bias and non-pc-relativity
//   and differ ONLY in `imageRelative`, so dropping that one field from
//   the description makes this scanner ambiguous immediately.
namespace dss::link::format {

// The four scalars that identify an unwind-table pointer field in a
// target's relocation vocabulary, plus the prose a refusal needs.
// `tls` is excluded unconditionally rather than being a field: a
// thread-pointer offset is not an address at all, so no unwind table can
// ever want one, and making it selectable would only let a caller ask
// for something meaningless.
struct UnwindPointerFieldShape {
    bool          pcRelative    = false;
    bool          imageRelative = false;
    std::uint8_t  widthBytes    = 4;
    std::int32_t  addendBias    = 0;
    // "the DWARF FDE `initial_location`" — names the FIELD in a refusal.
    std::string_view fieldName;
    // "32-bit PC-relative, no implicit addend bias" — the description in
    // the reader's own words, so the two-rows message says what the two
    // rows were confused ABOUT.
    std::string_view shapeSpelling;
    // "x86_64: R_X86_64_PC32; aarch64: R_AARCH64_PREL32" — where the
    // missing row comes from, so the absence message is actionable.
    std::string_view psabiHint;
    // The anchor a reader should follow. ONE LINE, never wrapped.
    std::string_view anchorId;
};

// DWARF CFI (`.eh_frame`) — ELF and Mach-O.
inline constexpr UnwindPointerFieldShape kDwarfFdePointerField{
    /*pcRelative=*/true,
    /*imageRelative=*/false,
    /*widthBytes=*/4,
    /*addendBias=*/0,
    /*fieldName=*/"the DWARF FDE `initial_location`",
    /*shapeSpelling=*/"32-bit PC-relative, no implicit addend bias",
    /*psabiHint=*/"x86_64: R_X86_64_PC32; aarch64: R_AARCH64_PREL32",
    /*anchorId=*/"D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS"};

// Win64 SEH (`.pdata` RUNTIME_FUNCTION + `.xdata` UNWIND_INFO) — PE.
inline constexpr UnwindPointerFieldShape kWin64RuntimeFunctionPointerField{
    /*pcRelative=*/false,
    /*imageRelative=*/true,
    /*widthBytes=*/4,
    /*addendBias=*/0,
    /*fieldName=*/"a Win64 `RUNTIME_FUNCTION` / `UNWIND_INFO` RVA field",
    /*shapeSpelling=*/"32-bit image-relative, no implicit addend bias",
    /*psabiHint=*/"x86_64: IMAGE_REL_AMD64_ADDR32NB",
    /*anchorId=*/"D-LK-PE-OBJ-ARM-CARRIES-NO-UNWIND-INFO"};

// The UNIQUE relocation row matching `shape`, or nullptr with `errorOut`
// set — never a first match, never a guess.
[[nodiscard]] inline TargetRelocationInfo const*
unwindPointerRelocationOf(TargetSchema const&            target,
                          UnwindPointerFieldShape const& shape,
                          std::string&                   errorOut) {
    TargetRelocationInfo const* found = nullptr;
    for (auto const& r : target.relocations()) {
        if (r.formulaKind != RelocFormulaKind::Linear
            || r.pcRelative != shape.pcRelative
            || r.imageRelative != shape.imageRelative
            || r.addendBias != shape.addendBias
            || r.widthBytes != shape.widthBytes
            || r.tls) {
            continue;
        }
        if (found != nullptr) {
            errorOut = "target '" + std::string(target.name())
                     + "' declares TWO relocations matching "
                     + std::string(shape.fieldName) + "'s description ("
                     + std::string(shape.shapeSpelling) + "): '"
                     + found->name + "' and '" + r.name
                     + "'. One of them is wrong for this field and nothing "
                       "here can tell which, so no unwind-table relocation "
                       "is emitted rather than a coin-flip an unwinder "
                       "would follow into the wrong function ("
                     + std::string(shape.anchorId) + ")";
            return nullptr;
        }
        found = &r;
    }
    if (found == nullptr) {
        errorOut = "target '" + std::string(target.name())
                 + "' declares no relocation matching "
                 + std::string(shape.fieldName) + "'s description ("
                 + std::string(shape.shapeSpelling)
                 + "), so that field in a relocatable object cannot be "
                   "relocated. This is a per-target psABI row ("
                 + std::string(shape.psabiHint)
                 + ") and it belongs in that target's `*.target.json`. "
                   "Reusing a neighbouring row whose bias or coordinate "
                   "space differs would bake that difference into a DATA "
                   "field and point the table somewhere the unwinder "
                   "still follows ("
                 + std::string(shape.anchorId) + ")";
    }
    return found;
}

}  // namespace dss::link::format
