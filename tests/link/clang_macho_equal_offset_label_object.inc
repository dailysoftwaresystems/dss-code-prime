#pragma once

// Golden fixture: a REAL clang-produced Mach-O 64-bit MH_OBJECT (arm64) whose
// section-start label sits at the SAME offset as the first function, with that
// function's own relocation INSIDE the span the two share. Embedded byte-for-
// byte so the equal-offset alias cross-check is DETERMINISTIC and CI-portable
// (genuine clang bytes on every host, no toolchain and no macOS at test time).
// D-LINK-EQUAL-OFFSET-DEFINED-SYMBOLS-BECOME-TWIN-ATOMS.
//
// ★★★ WHY A SECOND CLANG FIXTURE RATHER THAN A REUSE OF THE FIRST.
// `clang_macho_subsections_object.inc` already carries the `ltmp0`-at-offset-0
// shape, and it is what first showed the twin. It cannot witness the DEFECT,
// though: its only relocation lies at 0x08, inside `_entry`, and NOT inside the
// span `ltmp0` and `_outer` share. The twin is inert there. This object is the
// same shape WITH the relocation in the shared span, which is the difference
// between "two atoms are byte-identical" and "one of them ships un-patched".
//
// PROVENANCE, stated exactly. Producer: Ubuntu clang 19.1.1, invoked as
//   clang-19 -target arm64-apple-macos11 -c -O1
//            -fno-unwind-tables -fno-asynchronous-unwind-tables f.c -o f.o
// (the two unwind flags keep `__LD,__compact_unwind` out of the object, whose
//  r_extern=0 section-relative relocations are a separate, already-anchored
//  reader gap -- D-LK-MACHO-STATIC-SECTION-RELATIVE-RELOC -- and would make
//  this fixture about that instead of about symbol identity)
// ⚠ This is UPSTREAM LLVM's MachO object writer cross-emitting from Linux, NOT
// Apple's own clang on a Mac -- said plainly because a reference control that
// does not match the target has cost this project two cycles before. What it
// witnesses is a WIRE decision (where the producer puts its symbols), and the
// operator MEASURED `/usr/bin/clang -c` on real Apple Silicon 2026-08-20
// emitting the same header flags on every ordinary `.o`. Nothing here is used
// to adjudicate RUNTIME behaviour.
//
// SOURCE -- three lines of ordinary C, no attributes, no module-level asm.
// That is the point: this is not an exotic object, it is what clang emits for
// a translation unit whose first function calls something.
//
//     extern int sink(int);
//     int outer(int x) { return sink(x) + 1; }
//     int entry(int x) { return outer(x); }
//
// WHAT THE BYTES CONTAIN (✔MEASURED from the file itself, not from the source):
//   mach_header_64.flags = 0x00002000 (MH_SUBSECTIONS_VIA_SYMBOLS)
//   one section, __TEXT,__text, addr 0, size 0x30, two relocations
//   nlist_64[4]:
//     [0] ltmp0   n_type=0x0E local  n_sect=1  n_value=0x00
//     [1] _entry  n_type=0x0F EXT    n_sect=1  n_value=0x18
//     [2] _outer  n_type=0x0F EXT    n_sect=1  n_value=0x00   <- SAME as ltmp0
//     [3] _sink   n_type=0x01 UNDEF  n_sect=0  n_value=0x00
//   reloc[0]: r_address=0x20 -> symbol #3 (_sink), ARM64_RELOC_BRANCH26
//   reloc[1]: r_address=0x08 -> symbol #3 (_sink), ARM64_RELOC_BRANCH26
//             ^^^^^^^^^^^^^^ INSIDE [0x00, 0x18), the span ltmp0 and _outer
//             share. This is the byte the whole fixture exists for.
//
// ★ THE UN-PATCHED COPY IS NOT A THEORY -- IT IS IN THESE BYTES. `__text`
// offset 0x08 holds `00 00 00 94`, a BRANCH26 whose immediate is still ZERO,
// because a relocatable object stores the branch un-patched and the linker
// fills it in. A reconstruction that gives the reloc to one twin and not the
// other therefore emits a `bl` to imm 0 -- a branch to ITSELF, an infinite
// loop -- under whichever name lost the routing. With the boundary set sorted
// by (offset, symbol index) the loser is `_outer`: symbol #2, behind `ltmp0`
// at #0. The EXTERNAL name, the one every sibling CU and every foreign linker
// resolves through, is exactly the one that would ship broken.

#include <cstdint>
#include <vector>

namespace dss::test {

// The object described above, byte for byte.
inline std::vector<std::uint8_t> clangMachoEqualOffsetLabelObject() {
    return {
        207,250,237,254,12,0,0,1,0,0,0,0,1,0,0,0,
        4,0,0,0,24,1,0,0,0,32,0,0,0,0,0,0,
        25,0,0,0,152,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        48,0,0,0,0,0,0,0,56,1,0,0,0,0,0,0,
        48,0,0,0,0,0,0,0,7,0,0,0,7,0,0,0,
        1,0,0,0,0,0,0,0,95,95,116,101,120,116,0,0,
        0,0,0,0,0,0,0,0,95,95,84,69,88,84,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        48,0,0,0,0,0,0,0,56,1,0,0,2,0,0,0,
        104,1,0,0,2,0,0,0,0,4,0,128,0,0,0,0,
        0,0,0,0,0,0,0,0,50,0,0,0,24,0,0,0,
        1,0,0,0,0,0,11,0,0,0,0,0,0,0,0,0,
        2,0,0,0,24,0,0,0,120,1,0,0,4,0,0,0,
        184,1,0,0,32,0,0,0,11,0,0,0,80,0,0,0,
        0,0,0,0,1,0,0,0,1,0,0,0,2,0,0,0,
        3,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,253,123,191,169,253,3,0,145,
        0,0,0,148,0,4,0,17,253,123,193,168,192,3,95,214,
        253,123,191,169,253,3,0,145,0,0,0,148,0,4,0,17,
        253,123,193,168,192,3,95,214,32,0,0,0,3,0,0,45,
        8,0,0,0,3,0,0,45,21,0,0,0,14,1,0,0,
        0,0,0,0,0,0,0,0,1,0,0,0,15,1,0,0,
        24,0,0,0,0,0,0,0,8,0,0,0,15,1,0,0,
        0,0,0,0,0,0,0,0,15,0,0,0,1,0,0,0,
        0,0,0,0,0,0,0,0,0,95,101,110,116,114,121,0,
        95,111,117,116,101,114,0,95,115,105,110,107,0,108,116,109,
        112,48,0,0,0,0,0,0,
    };
}

} // namespace dss::test
