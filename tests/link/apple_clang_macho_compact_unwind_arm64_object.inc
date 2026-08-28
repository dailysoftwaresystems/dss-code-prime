#pragma once

// Golden fixture: a REAL, **STOCK** Apple-clang Mach-O 64-bit MH_OBJECT
// (arm64), embedded byte-for-byte. The reader test reads GENUINE Apple bytes on
// every host with NO toolchain (and no macOS) dependency at test time -- the same
// justification and the same shape as `clang_macho_subsections_object.inc`
// beside it.
// D-LK-MACHO-COMPACT-UNWIND-SECTION-REFUSED-BLOCKS-EVERY-STOCK-MACOS-ARCHIVE.
//
// ★★ **STOCK IS THE WHOLE POINT, AND IT IS WHAT ITS NEIGHBOUR IS NOT.**
// `clang_macho_subsections_object.inc` records in its own provenance block that
// it was built with `-fno-unwind-tables -fno-asynchronous-unwind-tables`
// specifically to keep `__LD,__compact_unwind` OUT -- a legitimate way to isolate
// one defect from another, and precisely the narrowing that left the SHIPPING
// case untested. THIS object was built by asking clang for nothing at all, so it
// carries the section every ordinary macOS `.o` carries.
//
// PROVENANCE, stated exactly. Producer: **Apple** clang 21.0.0
// (clang-2100.1.1.101), Target: arm64-apple-darwin25.5.0, on real Apple Silicon
// (macOS 26.5.2, arm64), invoked as
//   /usr/bin/clang -c stock.c -o stock.o
// -- no optimisation flag, no unwind flag, no flag of any kind beyond `-c`.
//
// ⚠ Unlike its neighbour this is APPLE's own clang on a real Mac, not upstream
// LLVM cross-emitting from Linux, and it is used to adjudicate RUNTIME behaviour
// as well as wire shape: the archive built from these very bytes was linked by
// DSS on Windows and the resulting Mach-O exec RAN NATIVELY ON THAT MAC, exit 42
// (2026-08-24).
// md5(stock.o) = ed4b505578a8b4ce76af61c299b5a941, 784 bytes.
//
// SOURCE, verbatim:
//     int dss_stock_leaf(int x) { return x + 1; }
//     int dss_stock_add(int a, int b) { int t = dss_stock_leaf(a); return t + b; }
//     int dss_stock_answer(void) { return dss_stock_add(20, 21); }
// `dss_stock_add` and `dss_stock_answer` are NON-LEAF, which is what makes clang
// describe a frame for them.
//
// WHAT THE BYTES CONTAIN (✔MEASURED from the file itself with a decoder written
// from the format description, not from `macho_object_reader.cpp`):
//   mach_header_64.cputype = 0x0100000C (arm64), .filetype = 1 (MH_OBJECT),
//     .flags = 0x00002000 (MH_SUBSECTIONS_VIA_SYMBOLS)
//   __TEXT,__text            addr 0x00 size 0x6C align 2^2 nreloc 2
//                            flags 0x80000400
//   __LD,__compact_unwind    addr 0x70 size 0x60 align 2^3 nreloc 3
//                            flags 0x02000000 (S_ATTR_DEBUG)
//     -> 0x60 / 32 = THREE records, one per function:
//        [0] fnStart=0x00 len=0x18 enc=0x04000000 personality=0 lsda=0
//        [1] fnStart=0x18 len=0x38 enc=0x04000000 personality=0 lsda=0
//        [2] fnStart=0x50 len=0x1C enc=0x04000000 personality=0 lsda=0
//     -> its 3 relocations are all r_extern=0 (SECTION-INDEX, r_symbolnum=1 =
//        __text), r_type=0 (ARM64_RELOC_UNSIGNED), r_length=3, r_pcrel=0
//   nlist_64[5]:
//     [0] ltmp0             n_type=0x0E local n_sect=1 n_desc=0x0000 n_value=0x00
//     [1] ltmp1             n_type=0x0E local n_sect=2 n_desc=0x0000 n_value=0x70
//     [2] _dss_stock_add    n_type=0x0F EXT   n_sect=1 n_desc=0x0000 n_value=0x18
//     [3] _dss_stock_answer n_type=0x0F EXT   n_sect=1 n_desc=0x0000 n_value=0x50
//     [4] _dss_stock_leaf   n_type=0x0F EXT   n_sect=1 n_desc=0x0000 n_value=0x00
//   NO `__eh_frame`: every frame here is compact-encodable, so this object's
//   ONLY unwind description is the compact one.
//
// ★★ THE PAIR IS THE INSTRUMENT, AND THE TWO HALVES DISAGREE BY PRODUCER
// CHOICE, NOT BY EDITING. This object carries `ltmp1`, a local label at offset 0
// of `__LD,__compact_unwind`, which under MH_SUBSECTIONS_VIA_SYMBOLS reads
// exactly like an atom boundary -- so it is the fixture that exercises the
// SYMBOL half of the fix. Its x86_64 sibling
// (`apple_clang_macho_compact_unwind_x86_64_object.inc`) carries NO local label
// at all, so its `__compact_unwind` reaches the reader as a reloc-bearing
// section with no symbol in it -- the RELOCATION half. Neither object alone
// covers both, and both came out of the same `clang -c`.

#include <cstdint>
#include <vector>

namespace dss::test {

// The object described above, byte for byte.
inline std::vector<std::uint8_t> appleClangMachoCompactUnwindArm64Object() {
    return {
        207,250,237,254,12,0,0,1,0,0,0,0,1,0,0,0,
        4,0,0,0,104,1,0,0,0,32,0,0,0,0,0,0,
        25,0,0,0,232,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        208,0,0,0,0,0,0,0,136,1,0,0,0,0,0,0,
        208,0,0,0,0,0,0,0,7,0,0,0,7,0,0,0,
        2,0,0,0,0,0,0,0,95,95,116,101,120,116,0,0,
        0,0,0,0,0,0,0,0,95,95,84,69,88,84,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        108,0,0,0,0,0,0,0,136,1,0,0,2,0,0,0,
        88,2,0,0,2,0,0,0,0,4,0,128,0,0,0,0,
        0,0,0,0,0,0,0,0,95,95,99,111,109,112,97,99,
        116,95,117,110,119,105,110,100,95,95,76,68,0,0,0,0,
        0,0,0,0,0,0,0,0,112,0,0,0,0,0,0,0,
        96,0,0,0,0,0,0,0,248,1,0,0,3,0,0,0,
        104,2,0,0,3,0,0,0,0,0,0,2,0,0,0,0,
        0,0,0,0,0,0,0,0,50,0,0,0,24,0,0,0,
        1,0,0,0,0,0,26,0,0,5,26,0,0,0,0,0,
        2,0,0,0,24,0,0,0,128,2,0,0,5,0,0,0,
        208,2,0,0,64,0,0,0,11,0,0,0,80,0,0,0,
        0,0,0,0,2,0,0,0,2,0,0,0,3,0,0,0,
        5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,255,67,0,209,224,15,0,185,
        232,15,64,185,0,5,0,17,255,67,0,145,192,3,95,214,
        255,131,0,209,253,123,1,169,253,67,0,145,160,195,31,184,
        225,11,0,185,160,195,95,184,0,0,0,148,224,7,0,185,
        232,7,64,185,233,11,64,185,0,1,9,11,253,123,65,169,
        255,131,0,145,192,3,95,214,253,123,191,169,253,3,0,145,
        128,2,128,82,161,2,128,82,0,0,0,148,253,123,193,168,
        192,3,95,214,0,0,0,0,0,0,0,0,0,0,0,0,
        24,0,0,0,0,16,0,2,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,24,0,0,0,0,0,0,0,
        56,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,80,0,0,0,0,0,0,0,
        28,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,96,0,0,0,2,0,0,45,
        48,0,0,0,4,0,0,45,64,0,0,0,1,0,0,6,
        32,0,0,0,1,0,0,6,0,0,0,0,1,0,0,6,
        56,0,0,0,14,1,0,0,0,0,0,0,0,0,0,0,
        50,0,0,0,14,2,0,0,112,0,0,0,0,0,0,0,
        35,0,0,0,15,1,0,0,24,0,0,0,0,0,0,0,
        1,0,0,0,15,1,0,0,80,0,0,0,0,0,0,0,
        19,0,0,0,15,1,0,0,0,0,0,0,0,0,0,0,
        0,95,100,115,115,95,115,116,111,99,107,95,97,110,115,119,
        101,114,0,95,100,115,115,95,115,116,111,99,107,95,108,101,
        97,102,0,95,100,115,115,95,115,116,111,99,107,95,97,100,
        100,0,108,116,109,112,49,0,108,116,109,112,48,0,0,0,
    };
}

} // namespace dss::test
