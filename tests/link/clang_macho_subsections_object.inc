#pragma once

// Golden fixture: a REAL clang-produced Mach-O 64-bit MH_OBJECT (arm64),
// embedded byte-for-byte so the reader's foreign-`.o` symbol-classification
// cross-check is DETERMINISTIC and CI-portable -- it reads GENUINE clang bytes
// on every host with NO toolchain (and no macOS) dependency at test time. Same
// justification, and the same shape, as `gcc_lib_c164_object.inc` /
// `gcc_section_relative_c167.inc` for the ELF reader; this is the FIRST
// checked-in clang Mach-O object in the tree.
// D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-BLOCK-LABEL-NOT-ATOM.
//
// PROVENANCE, stated exactly. Producer: Ubuntu clang 19.1.1, invoked as
//   clang-19 -target arm64-apple-macos11 -c -O1
//            -fno-unwind-tables -fno-asynchronous-unwind-tables f.c -o f.o
// (the two unwind flags keep `__LD,__compact_unwind` out of the object, whose
//  r_extern=0 section-relative relocations are a separate, already-anchored
//  reader gap -- D-LK-MACHO-STATIC-SECTION-RELATIVE-RELOC -- and would make
//  this fixture about that instead of about symbol classification)
// ⚠ This is UPSTREAM LLVM's MachO object writer cross-emitting from Linux, NOT
// Apple's own clang on a Mac -- said plainly because a reference control that
// does not match the target has cost this project two cycles before. What it
// is being used to witness is a WIRE FORMAT decision (which n_desc bit, and
// the mach_header flag), and on that the two agree: the operator MEASURED
// `/usr/bin/clang -c` on real Apple Silicon 2026-08-20 emitting header
// flags = 0x00002000 on every ordinary `.o`, which is what these bytes carry.
// Nothing here is used to adjudicate RUNTIME behaviour.
//
// SOURCE. The module-level asm exists because `.alt_entry` has no C spelling,
// and an N_ALT_ENTRY symbol from a real producer is half the point. Written
// here without the C string-literal quoting so this comment stays free of
// stray double quotes; the asm body is one directive/instruction per line:
//
//     __asm__(  .text
//               .globl _outer
//               .p2align 2
//               _outer:
//                 mov w0, #7
//               .alt_entry _outer_alt
//               _outer_alt:
//                 ret            );
//     __attribute__((noinline)) static int helper(int x) { return x + 1; }
//     int entry(int x) { return helper(x); }
//
// WHAT THE BYTES CONTAIN (✔MEASURED from the file itself, not from the source):
//   mach_header_64.flags = 0x00002000 (MH_SUBSECTIONS_VIA_SYMBOLS)
//   one section, __TEXT,__text, addr 0, size 20, one relocation
//   nlist_64[5]:
//     [0] ltmp0       n_type=0x0E local  n_sect=1 n_desc=0x0000  n_value=0x00
//     [1] _outer_alt  n_type=0x0E local  n_sect=1 n_desc=0x0200  n_value=0x04
//     [2] _helper     n_type=0x0E local  n_sect=1 n_desc=0x0000  n_value=0x0C
//     [3] _entry      n_type=0x0F EXT    n_sect=1 n_desc=0x0000  n_value=0x08
//     [4] _outer      n_type=0x0F EXT    n_sect=1 n_desc=0x0000  n_value=0x00
//   reloc: r_address=0x08 -> symbol #2 (_helper), ARM64_RELOC_BRANCH26,
//          r_pcrel=1 r_length=2 r_extern=1, packed nativeId 0x25000000
//
// ★ THE DISCRIMINATING PAIR IS IN ONE FILE, FROM ONE PRODUCER: `_helper` and
// `_outer_alt` are BOTH non-external defined `__text` symbols -- identical
// n_type, identical n_sect, and nlist_64 has no size field -- and they differ
// ONLY in n_desc. `_helper` is a whole file-local function whose bytes must
// survive; `_outer_alt` is an interior entry point that must NOT split the
// atom it lives in. Any reader that classifies by N_EXT gets both wrong.
//
// ⚠ 0x0200 IS N_ALT_ENTRY. 0x0020 IS N_NO_DEAD_STRIP AND IS A DIFFERENT
// THING -- ✔MEASURED with this same compiler: `__attribute__((used))` on a
// static function emits n_desc=0x0020, and reading that bit as alt-entry would
// drop the body of exactly the `static` helpers that carry `used`.

#include <cstdint>
#include <vector>

namespace dss::test {

// The object described above, byte for byte.
inline std::vector<std::uint8_t> clangMachoSubsectionsObject() {
    return {
        207,250,237,254,12,0,0,1,0,0,0,0,1,0,0,0,
        4,0,0,0,24,1,0,0,0,32,0,0,0,0,0,0,
        25,0,0,0,152,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        20,0,0,0,0,0,0,0,56,1,0,0,0,0,0,0,
        20,0,0,0,0,0,0,0,7,0,0,0,7,0,0,0,
        1,0,0,0,0,0,0,0,95,95,116,101,120,116,0,0,
        0,0,0,0,0,0,0,0,95,95,84,69,88,84,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        20,0,0,0,0,0,0,0,56,1,0,0,2,0,0,0,
        80,1,0,0,1,0,0,0,0,4,0,128,0,0,0,0,
        0,0,0,0,0,0,0,0,50,0,0,0,24,0,0,0,
        1,0,0,0,0,0,11,0,0,0,0,0,0,0,0,0,
        2,0,0,0,24,0,0,0,88,1,0,0,5,0,0,0,
        168,1,0,0,40,0,0,0,11,0,0,0,80,0,0,0,
        0,0,0,0,3,0,0,0,3,0,0,0,2,0,0,0,
        5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,224,0,128,82,192,3,95,214,
        0,0,0,20,0,4,0,17,192,3,95,214,0,0,0,0,
        8,0,0,0,2,0,0,45,34,0,0,0,14,1,0,0,
        0,0,0,0,0,0,0,0,8,0,0,0,14,1,0,2,
        4,0,0,0,0,0,0,0,26,0,0,0,14,1,0,0,
        12,0,0,0,0,0,0,0,1,0,0,0,15,1,0,0,
        8,0,0,0,0,0,0,0,19,0,0,0,15,1,0,0,
        0,0,0,0,0,0,0,0,0,95,101,110,116,114,121,0,
        95,111,117,116,101,114,95,97,108,116,0,95,111,117,116,101,
        114,0,95,104,101,108,112,101,114,0,108,116,109,112,48,0,
    };
}

} // namespace dss::test
