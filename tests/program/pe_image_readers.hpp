#pragma once

// READERS OF A pe64 IMAGE, FOR PINS THAT READ WHAT THE DRIVER REALLY WROTE.
//
// Shared by `program/test_driver_argument_supply` (where they were born) and
// `program/test_guarded_region_examples_on_every_host`, so the two read an
// image's section table, its function table, its unwind codes and its scope
// tables with ONE decoder. Every reader is bounds-safe: a truncated or
// unexpected artifact yields zeros and empty lists, so a pin fails with its own
// message instead of crashing the suite.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace dss::test_support {

// ── Byte readers, shared by the image pins ─────────────────────────────────
// Bounds-safe: an out-of-range read yields 0 rather than UB, so a truncated or
// unexpected artifact makes a pin fail with its own message instead of
// crashing the suite.
[[nodiscard]] inline std::uint16_t rdU16(std::vector<std::uint8_t> const& b,
                                         std::size_t o) {
    if (o + 2 > b.size()) return 0;
    return static_cast<std::uint16_t>(static_cast<unsigned>(b[o])
                                      | (static_cast<unsigned>(b[o + 1]) << 8));
}
[[nodiscard]] inline std::uint32_t rdU32(std::vector<std::uint8_t> const& b,
                                         std::size_t o) {
    if (o + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(b[o])
           | (static_cast<std::uint32_t>(b[o + 1]) << 8)
           | (static_cast<std::uint32_t>(b[o + 2]) << 16)
           | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}
[[nodiscard]] inline std::uint64_t rdU64(std::vector<std::uint8_t> const& b,
                                         std::size_t o) {
    if (o + 8 > b.size()) return 0;
    return static_cast<std::uint64_t>(rdU32(b, o))
           | (static_cast<std::uint64_t>(rdU32(b, o + 4)) << 32);
}

[[nodiscard]] inline std::vector<std::uint8_t> readAllBytes(std::filesystem::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>()};
}

struct PeSection {
    std::string   name;
    std::uint32_t vaddr = 0, vsize = 0, rawPtr = 0, rawSize = 0;
};

[[nodiscard]] inline std::vector<PeSection>
peSections(std::vector<std::uint8_t> const& img) {
    std::vector<PeSection> out;
    if (img.size() < 0x40) return out;
    std::size_t const peOff = rdU32(img, 0x3C);
    if (peOff + 24 > img.size()) return out;
    if (!(img[peOff] == 'P' && img[peOff + 1] == 'E' && img[peOff + 2] == 0
          && img[peOff + 3] == 0)) {
        return out;
    }
    std::uint16_t const nsec    = rdU16(img, peOff + 6);
    std::uint16_t const optSize = rdU16(img, peOff + 20);
    std::size_t const   secOff  = peOff + 24 + optSize;
    for (std::uint16_t i = 0; i < nsec; ++i) {
        std::size_t const o = secOff + static_cast<std::size_t>(i) * 40;
        if (o + 40 > img.size()) break;
        PeSection s;
        for (std::size_t k = 0; k < 8 && img[o + k] != 0; ++k) {
            s.name.push_back(static_cast<char>(img[o + k]));
        }
        s.vsize   = rdU32(img, o + 8);
        s.vaddr   = rdU32(img, o + 12);
        s.rawSize = rdU32(img, o + 16);
        s.rawPtr  = rdU32(img, o + 20);
        out.push_back(std::move(s));
    }
    return out;
}

[[nodiscard]] inline std::size_t peRvaToOffset(std::vector<PeSection> const& secs,
                                               std::uint32_t                 rva) {
    for (auto const& s : secs) {
        std::uint32_t const span = std::max(s.vsize, s.rawSize);
        if (rva >= s.vaddr && rva < s.vaddr + span) {
            return static_cast<std::size_t>(rva - s.vaddr) + s.rawPtr;
        }
    }
    return 0;
}

struct PeScopeRecord {
    std::uint32_t begin  = 0;
    std::uint32_t end    = 0;
    std::uint32_t filter = 0;
    std::uint32_t target = 0;
};

// One function of a pe64 image whose unwind info names a language handler.
struct PeGuardedFunction {
    std::uint32_t              begin = 0;
    std::uint32_t              end   = 0;
    std::size_t                savedNonVolatile = 0;   // UWOP_PUSH_NONVOL / UWOP_SAVE_NONVOL(_FAR)
    std::vector<PeScopeRecord> records;                // the scope table, IN TABLE ORDER
};

[[nodiscard]] inline std::vector<PeGuardedFunction>
peGuardedFunctions(std::vector<std::uint8_t> const& img) {
    constexpr std::uint8_t kHandlerFlags    = 0x03;   // UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER
    constexpr std::size_t  kRuntimeFunction = 12;
    std::vector<PeGuardedFunction> out;
    auto const secs = peSections(img);
    for (auto const& s : secs) {
        if (s.name != ".pdata") continue;
        std::size_t const sz = std::min<std::size_t>(s.vsize, s.rawSize);
        for (std::size_t e = 0; e + kRuntimeFunction <= sz; e += kRuntimeFunction) {
            PeGuardedFunction f;
            f.begin = rdU32(img, s.rawPtr + e);
            f.end   = rdU32(img, s.rawPtr + e + 4);
            std::size_t const at = peRvaToOffset(secs, rdU32(img, s.rawPtr + e + 8));
            if (at == 0 || at + 4 > img.size()) continue;
            std::uint8_t const flags  = static_cast<std::uint8_t>(img[at] >> 3);
            std::size_t const  ncodes = img[at + 2];
            if ((flags & kHandlerFlags) == 0) continue;
            // The unwind codes: two bytes each, some followed by one or two
            // slots of operand.
            for (std::size_t i = 0; i < ncodes;) {
                std::uint8_t const b    = img[at + 4 + 2 * i + 1];
                std::uint8_t const op   = static_cast<std::uint8_t>(b & 0x0F);
                std::uint8_t const info = static_cast<std::uint8_t>(b >> 4);
                std::size_t slots = 1;
                if (op == 1) slots = info == 0 ? 2 : 3;        // UWOP_ALLOC_LARGE
                else if (op == 4 || op == 8) slots = 2;        // SAVE_NONVOL / SAVE_XMM128
                else if (op == 5 || op == 9) slots = 3;        // …_FAR
                if (op == 0 || op == 4 || op == 5) ++f.savedNonVolatile;
                i += slots;
            }
            // Past the codes (padded to an even count): the handler routine's
            // RVA, then its data — a count and that many four-field records.
            std::size_t const after = at + 4 + 2 * ((ncodes + 1) & ~std::size_t{1});
            if (after + 8 > img.size()) continue;
            std::uint32_t const count = rdU32(img, after + 4);
            if (count > 64 || after + 8 + std::size_t{16} * count > img.size()) continue;
            for (std::uint32_t k = 0; k < count; ++k) {
                std::size_t const r = after + 8 + std::size_t{16} * k;
                f.records.push_back(PeScopeRecord{rdU32(img, r), rdU32(img, r + 4),
                                                  rdU32(img, r + 8), rdU32(img, r + 12)});
            }
            out.push_back(std::move(f));
        }
    }
    return out;
}

struct PeUnwindCode {
    std::uint8_t  codeOffset = 0;   // the prologue byte the operation ENDS at
    std::uint8_t  op         = 0;
    std::uint8_t  info       = 0;
    std::uint32_t operand    = 0;   // DECODED: a save's slot offset from RSP
};

struct PeFunctionUnwind {
    std::uint32_t              begin  = 0;
    bool                       guards = false;   // names a language handler
    std::vector<PeUnwindCode>  codes;            // IN TABLE ORDER: descending offset
    std::vector<std::uint8_t>  prologue;         // its first SizeOfProlog bytes
};

constexpr std::uint8_t kPeUwopSaveXmm128    = 8;
constexpr std::uint8_t kPeUwopSaveXmm128Far = 9;

// Every function of a pe64 image, with its unwind codes decoded the way the
// x64 unwind-data documentation states them: a general save's node is its slot
// divided by 8, a vector save's by 16, and a FAR form carries the offset
// unscaled in two nodes, low word first.
[[nodiscard]] inline std::vector<PeFunctionUnwind>
peFunctionUnwinds(std::vector<std::uint8_t> const& img) {
    constexpr std::uint8_t kHandlerFlags    = 0x03;
    constexpr std::size_t  kRuntimeFunction = 12;
    std::vector<PeFunctionUnwind> out;
    auto const secs = peSections(img);
    for (auto const& s : secs) {
        if (s.name != ".pdata") continue;
        std::size_t const sz = std::min<std::size_t>(s.vsize, s.rawSize);
        for (std::size_t e = 0; e + kRuntimeFunction <= sz; e += kRuntimeFunction) {
            PeFunctionUnwind f;
            f.begin = rdU32(img, s.rawPtr + e);
            std::size_t const at   = peRvaToOffset(secs, rdU32(img, s.rawPtr + e + 8));
            std::size_t const code = peRvaToOffset(secs, f.begin);
            if (at == 0 || code == 0 || at + 4 > img.size()) continue;
            f.guards = ((img[at] >> 3) & kHandlerFlags) != 0;
            std::size_t const prolog = img[at + 1];
            std::size_t const ncodes = img[at + 2];
            if (code + prolog > img.size() || at + 4 + 2 * ncodes > img.size()) continue;
            f.prologue.assign(img.begin() + static_cast<std::ptrdiff_t>(code),
                              img.begin() + static_cast<std::ptrdiff_t>(code + prolog));
            for (std::size_t i = 0; i < ncodes;) {
                std::size_t const n = at + 4 + 2 * i;
                PeUnwindCode c;
                c.codeOffset = img[n];
                c.op         = static_cast<std::uint8_t>(img[n + 1] & 0x0F);
                c.info       = static_cast<std::uint8_t>(img[n + 1] >> 4);
                std::uint32_t const one = rdU16(img, n + 2);
                std::uint32_t const two = one | (static_cast<std::uint32_t>(rdU16(img, n + 4)) << 16);
                std::size_t slots = 1;
                if (c.op == 1)      { slots = c.info == 0 ? 2 : 3; c.operand = c.info == 0 ? one * 8u : two; }
                else if (c.op == 2) { c.operand = c.info * 8u + 8u; }
                else if (c.op == 4) { slots = 2; c.operand = one * 8u; }
                else if (c.op == kPeUwopSaveXmm128) { slots = 2; c.operand = one * 16u; }
                else if (c.op == 5 || c.op == kPeUwopSaveXmm128Far) { slots = 3; c.operand = two; }
                f.codes.push_back(c);
                i += slots;
            }
            out.push_back(std::move(f));
        }
    }
    return out;
}

// THE FIRST VALUE A HANDLER TAKES FROM OUTSIDE ITSELF, read off the image. A
// handler is entered by the unwinder, which hands over the frame and the
// registers a call preserves and nothing else — so the first register a handler
// READS WITHOUT HAVING WRITTEN IT must be one a call preserves, or the value
// must come out of the frame. Under the Win64 convention the general registers
// a call does NOT preserve are rax, rcx, rdx and r8..r11.
//
// The walk decodes the few instruction forms a handler opens with — forming a
// frame address (`LEA r, [rsp+d]`), `MOV r/m, r`, `MOV r, r/m`, `MOV r/m, imm32`,
// `MOV r, imm` — keeps the set of registers the handler has written, and stops
// at the first read of one it has not: a volatile register (the finding), a
// preserved one, or a load of a VALUE through RSP/RBP. Forming an address from
// the frame pointer takes no value and is walked past. Anything else ends the
// walk as `NotDecoded`, and the caller prints the bytes.
enum class HandlerTakes : std::uint8_t {
    NotDecoded,
    AVolatileRegister,     // a register the unwinder does not hand over
    APreservedRegister,
    AValueOfTheFrame,      // a load through RSP or RBP
    NothingFromOutside,    // the walk ended on a form that reads no register
};

struct HandlerFirstRead {
    HandlerTakes takes = HandlerTakes::NotDecoded;
    unsigned     reg   = 0;   // the register read
};

[[nodiscard]] inline HandlerFirstRead
firstValueAHandlerTakes(std::vector<std::uint8_t> const& img, std::size_t at) {
    HandlerFirstRead out;
    bool written[16] = {};
    auto const isFrame    = [](unsigned r) { return r == 4u || r == 5u; };
    auto const isVolatile = [](unsigned r) {
        return r == 0u || r == 1u || r == 2u || (r >= 8u && r <= 11u);
    };
    // A register the instruction reads: true when it ends the walk.
    auto const reads = [&](unsigned r) {
        if (isFrame(r) || written[r]) return false;
        out.reg   = r;
        out.takes = isVolatile(r) ? HandlerTakes::AVolatileRegister
                                  : HandlerTakes::APreservedRegister;
        return true;
    };
    std::size_t i = at;
    for (int step = 0; step < 8 && at != 0; ++step) {
        if (i + 2 > img.size()) return out;
        unsigned rex = 0;
        if ((img[i] & 0xF0u) == 0x40u) rex = img[i++];
        std::uint8_t const op = img[i++];
        if (op >= 0xB8u && op <= 0xBFu) {            // MOV r, imm32 / imm64
            written[(op & 7u) | ((rex & 1u) != 0u ? 8u : 0u)] = true;
            i += (rex & 8u) != 0u ? 8u : 4u;
            continue;
        }
        if (op != 0x8Du && op != 0x89u && op != 0x8Bu && op != 0xC7u) return out;
        if (i >= img.size()) return out;
        std::uint8_t const modrm = img[i++];
        unsigned const mod = modrm >> 6;
        unsigned const reg = ((modrm >> 3) & 7u) | ((rex & 4u) != 0u ? 8u : 0u);
        unsigned const rm  = modrm & 7u;
        // The memory operand's base and index, when it has them.
        bool     memory = mod != 3u, ripRelative = false;
        unsigned base = 16u, index = 16u;
        if (memory) {
            if (rm == 4u) {
                if (i >= img.size()) return out;
                std::uint8_t const sib = img[i++];
                unsigned const b = sib & 7u, x = (sib >> 3) & 7u;
                if (!(mod == 0u && b == 5u)) base = b | ((rex & 1u) != 0u ? 8u : 0u);
                if (x != 4u || (rex & 2u) != 0u) index = x | ((rex & 2u) != 0u ? 8u : 0u);
                if (mod == 0u && b == 5u) i += 4u;
            } else if (mod == 0u && rm == 5u) {
                ripRelative = true;
                i += 4u;
            } else {
                base = rm | ((rex & 1u) != 0u ? 8u : 0u);
            }
            i += mod == 1u ? 1u : mod == 2u ? 4u : 0u;
        }
        unsigned const rmReg = rm | ((rex & 1u) != 0u ? 8u : 0u);
        if (op == 0xC7u) i += 4u;                    // MOV r/m, imm32
        // What the instruction reads, the value first and the address after it.
        if (op == 0x89u && reads(reg)) return out;                   // the stored value
        if (op == 0x8Bu && !memory && reads(rmReg)) return out;      // a register copied
        if (memory && !ripRelative) {
            if (op == 0x8Bu && base < 16u && isFrame(base) && index == 16u) {
                out.reg   = base;
                out.takes = HandlerTakes::AValueOfTheFrame;
                return out;
            }
            if (base < 16u && reads(base)) return out;
            if (index < 16u && reads(index)) return out;
        }
        // What it writes.
        if (op == 0x8Du || op == 0x8Bu) written[reg] = true;
        if ((op == 0x89u || op == 0xC7u) && !memory) written[rmReg] = true;
        // A store to memory ends the opening statement.
        if ((op == 0x89u || op == 0xC7u) && memory) {
            out.takes = HandlerTakes::NothingFromOutside;
            return out;
        }
    }
    return out;
}

} // namespace dss::test_support
