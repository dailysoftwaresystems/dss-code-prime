#pragma once

// Golden fixture: a REAL, **STOCK** Apple-clang Mach-O 64-bit MH_OBJECT
// (x86_64), embedded byte-for-byte. The reader test reads GENUINE Apple bytes on
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
//   /usr/bin/clang -arch x86_64 -c stock.c -o stock.o
// -- no optimisation flag, no unwind flag, no flag of any kind beyond `-c`.
// (`-arch x86_64` names the SLICE, not a behaviour: the host
// is Apple Silicon, so the x86_64 object must name its arch.)
// ⚠ Unlike its neighbour this is APPLE's own clang on a real Mac, not upstream
// LLVM cross-emitting from Linux, and it is used to adjudicate RUNTIME behaviour
// as well as wire shape: the archive built from these very bytes was linked by
// DSS on Windows and the resulting Mach-O exec RAN NATIVELY ON THAT MAC, exit 42
// (2026-08-24).
// md5(stock.o) = 4d96adda0953dd6126824e1085edd2da, 944 bytes.
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
//   mach_header_64.cputype = 0x01000007 (x86_64), .filetype = 1 (MH_OBJECT),
//     .flags = 0x00002000 (MH_SUBSECTIONS_VIA_SYMBOLS)
//   __TEXT,__text            addr 0x00 size 0x55 align 2^4 nreloc 2
//                            flags 0x80000400
//   __LD,__compact_unwind    addr 0x58 size 0x60 align 2^3 nreloc 3
//                            flags 0x02000000 (S_ATTR_DEBUG)
//   __TEXT,__eh_frame        addr 0xB8 size 0x90 align 2^3 nreloc 0
//                            flags 0x6800000B
//   nlist_64[3]: _dss_stock_add / _dss_stock_answer / _dss_stock_leaf, ALL
//     n_type=0x0F EXTERNAL in n_sect=1 -- and NOT ONE LOCAL LABEL.
//
// ★★ THE HALF ITS arm64 SIBLING CANNOT REACH. With no local label anywhere,
// this object's `__LD,__compact_unwind` arrives at the reader as a section that
// carries THREE relocations and no symbol -- so it is the fixture that exercises
// the RELOCATION half of the fix, where the arm64 sibling
// (`apple_clang_macho_compact_unwind_arm64_object.inc`) exercises the SYMBOL
// half through its `ltmp1`. It also carries a `__TEXT,__eh_frame` its sibling
// does not, which is a second producer choice and not an edit: on x86_64 clang
// emits the DWARF table alongside the compact one.

#include <cstdint>
#include <vector>

namespace dss::test {

// The object described above, byte for byte.
inline std::vector<std::uint8_t> appleClangMachoCompactUnwindX86_64Object() {
    return {
        207,250,237,254,7,0,0,1,3,0,0,0,1,0,0,0,
        4,0,0,0,184,1,0,0,0,32,0,0,0,0,0,0,
        25,0,0,0,56,1,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        72,1,0,0,0,0,0,0,216,1,0,0,0,0,0,0,
        72,1,0,0,0,0,0,0,7,0,0,0,7,0,0,0,
        3,0,0,0,0,0,0,0,95,95,116,101,120,116,0,0,
        0,0,0,0,0,0,0,0,95,95,84,69,88,84,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        85,0,0,0,0,0,0,0,216,1,0,0,4,0,0,0,
        32,3,0,0,2,0,0,0,0,4,0,128,0,0,0,0,
        0,0,0,0,0,0,0,0,95,95,99,111,109,112,97,99,
        116,95,117,110,119,105,110,100,95,95,76,68,0,0,0,0,
        0,0,0,0,0,0,0,0,88,0,0,0,0,0,0,0,
        96,0,0,0,0,0,0,0,48,2,0,0,3,0,0,0,
        48,3,0,0,3,0,0,0,0,0,0,2,0,0,0,0,
        0,0,0,0,0,0,0,0,95,95,101,104,95,102,114,97,
        109,101,0,0,0,0,0,0,95,95,84,69,88,84,0,0,
        0,0,0,0,0,0,0,0,184,0,0,0,0,0,0,0,
        144,0,0,0,0,0,0,0,144,2,0,0,3,0,0,0,
        0,0,0,0,0,0,0,0,11,0,0,104,0,0,0,0,
        0,0,0,0,0,0,0,0,50,0,0,0,24,0,0,0,
        1,0,0,0,0,0,26,0,0,5,26,0,0,0,0,0,
        2,0,0,0,24,0,0,0,72,3,0,0,3,0,0,0,
        120,3,0,0,56,0,0,0,11,0,0,0,80,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,
        3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,85,72,137,229,137,125,252,139,
        69,252,131,192,1,93,195,144,85,72,137,229,72,131,236,16,
        137,125,252,137,117,248,139,125,252,232,0,0,0,0,137,69,
        244,139,69,244,3,69,248,72,131,196,16,93,195,102,102,46,
        15,31,132,0,0,0,0,0,85,72,137,229,191,20,0,0,
        0,190,21,0,0,0,232,0,0,0,0,93,195,0,0,0,
        0,0,0,0,0,0,0,0,15,0,0,0,0,0,0,1,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        16,0,0,0,0,0,0,0,37,0,0,0,0,0,0,1,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        64,0,0,0,0,0,0,0,21,0,0,0,0,0,0,1,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        20,0,0,0,0,0,0,0,1,122,82,0,1,120,16,1,
        16,12,7,8,144,1,0,0,36,0,0,0,28,0,0,0,
        40,255,255,255,255,255,255,255,15,0,0,0,0,0,0,0,
        0,65,14,16,134,2,67,13,6,0,0,0,0,0,0,0,
        36,0,0,0,68,0,0,0,16,255,255,255,255,255,255,255,
        37,0,0,0,0,0,0,0,0,65,14,16,134,2,67,13,
        6,0,0,0,0,0,0,0,36,0,0,0,108,0,0,0,
        24,255,255,255,255,255,255,255,21,0,0,0,0,0,0,0,
        0,65,14,16,134,2,67,13,6,0,0,0,0,0,0,0,
        79,0,0,0,0,0,0,45,34,0,0,0,2,0,0,45,
        64,0,0,0,1,0,0,6,32,0,0,0,1,0,0,6,
        0,0,0,0,1,0,0,6,35,0,0,0,15,1,0,0,
        16,0,0,0,0,0,0,0,1,0,0,0,15,1,0,0,
        64,0,0,0,0,0,0,0,19,0,0,0,15,1,0,0,
        0,0,0,0,0,0,0,0,0,95,100,115,115,95,115,116,
        111,99,107,95,97,110,115,119,101,114,0,95,100,115,115,95,
        115,116,111,99,107,95,108,101,97,102,0,95,100,115,115,95,
        115,116,111,99,107,95,97,100,100,0,0,0,0,0,0,0,
    };
}

} // namespace dss::test
