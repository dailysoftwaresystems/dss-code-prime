#pragma once

// READERS OF A pe64 IMAGE, FOR PINS THAT READ WHAT THE DRIVER REALLY WROTE.
//
// Shared by `program/test_driver_argument_supply` (where they were born) and
// `program/test_guarded_region_examples_on_every_host`, so the two read an
// image's section table, its function table, its unwind codes, its scope
// tables and its handlers' instructions with ONE decoder. Every reader is bounds-safe: a truncated or
// unexpected artifact yields zeros and empty lists, so a pin fails with its own
// message instead of crashing the suite.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
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

// ── ONE x86-64 INSTRUCTION, AS A HANDLER WALK READS IT ──────────────────────
//
// Registers are numbered 0..15 for the general ones (rax, rcx, rdx, rbx, rsp,
// rbp, rsi, rdi, r8..r15) and 16..31 for xmm0..xmm15, so one 32-bit set holds
// what an instruction reads or writes in both files. The decoder knows the forms
// a compiler writes for a handler's statements and for the code after them —
// moves, address forming, the integer arithmetic, shifts, sign and zero
// extension, conditional moves and sets, the scalar and packed SSE moves,
// arithmetic and conversions, and every jump, call and return — and answers
// nothing (`std::nullopt`) for any other form, so a walk that meets one says so
// instead of guessing what it reads.
//
// Only what a walk asks is decoded: which registers are READ (a value used,
// an address's base and index included), which are WRITTEN, whether a memory
// operand is loaded or stored, and where control goes. A scalar SSE operation
// whose destination keeps its upper lanes (`movsd xmm, xmm`, `cvtsi2sd`,
// `sqrtsd`) is not counted as reading the destination — the value it produces
// does not depend on it — and an 8- or 16-bit write counts as a write of the
// whole register.
constexpr unsigned kX64Xmm        = 16;   // xmm n is register kX64Xmm + n
constexpr unsigned kX64NoRegister = 32;

// Under the Win64 convention: what a call does not preserve, and the frame.
constexpr std::uint32_t kWin64Volatile = 0x00000F07u                 // rax rcx rdx r8..r11
                                         | (std::uint32_t{0x3F} << kX64Xmm);   // xmm0..xmm5
constexpr std::uint32_t kX64Frame      = 0x00000030u;                // rsp rbp

[[nodiscard]] constexpr std::uint32_t x64Bit(unsigned r) {
    return r < 32u ? (std::uint32_t{1} << r) : 0u;
}

[[nodiscard]] inline std::string x64RegisterName(unsigned r) {
    constexpr char const* kLow[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"};
    if (r < 8u) return kLow[r];
    if (r < kX64Xmm) return "r" + std::to_string(r);
    if (r < kX64NoRegister) return "xmm" + std::to_string(r - kX64Xmm);
    return "(no register)";
}

enum class X64Flow : std::uint8_t {
    Next,           // falls through to the next instruction
    Jump,           // a direct unconditional jump to `target`
    Branch,         // a direct conditional jump: to `target`, or on
    Call,           // a call, direct or not: control comes back to the next instruction
    Return,
    IndirectJump,   // through a register or memory: the bytes do not say where
    Trap,           // `ud2`, `int3`
};

struct X64Instruction {
    std::size_t   length      = 0;
    std::uint32_t reads       = 0;   // registers whose value is used
    std::uint32_t writes      = 0;
    bool          load        = false;   // a value read through the memory operand
    bool          store       = false;   // a value written through it
    bool          ripRelative = false;
    unsigned      base        = kX64NoRegister;   // the memory operand's base and index
    unsigned      index       = kX64NoRegister;
    unsigned      loadTo      = kX64NoRegister;   // the register a load fills
    unsigned      leaTo       = kX64NoRegister;   // LEA's destination
    unsigned      copyTo      = kX64NoRegister;   // a register-to-register MOV
    unsigned      copyFrom    = kX64NoRegister;
    X64Flow       flow        = X64Flow::Next;
    std::size_t   target      = 0;   // a direct jump's or call's target, as an offset into the bytes
};

[[nodiscard]] inline std::optional<X64Instruction>
decodeX64(std::vector<std::uint8_t> const& img, std::size_t at) {
    X64Instruction d;
    std::size_t    i   = at;
    bool           p66 = false, pF2 = false, pF3 = false;
    while (i < img.size() && (img[i] == 0x66u || img[i] == 0xF2u || img[i] == 0xF3u
                              || img[i] == 0x2Eu || img[i] == 0x3Eu)) {
        p66 = p66 || img[i] == 0x66u;
        pF2 = pF2 || img[i] == 0xF2u;
        pF3 = pF3 || img[i] == 0xF3u;
        ++i;
    }
    unsigned rex = 0;
    if (i < img.size() && (img[i] & 0xF0u) == 0x40u) rex = img[i++];
    if (i >= img.size()) return std::nullopt;
    bool const     rexW = (rex & 8u) != 0u;
    unsigned const rexR = (rex & 4u) != 0u ? 8u : 0u;
    unsigned const rexX = (rex & 2u) != 0u ? 8u : 0u;
    unsigned const rexB = (rex & 1u) != 0u ? 8u : 0u;
    unsigned       op   = img[i++];
    bool           twoByte = false;
    if (op == 0x0Fu) {
        if (i >= img.size()) return std::nullopt;
        op      = img[i++];
        twoByte = true;
    }

    // The ModRM operand: `reg`, and `rm` as a register or a memory operand.
    unsigned reg = 0, rm = 0;
    bool     memory = false;
    auto const modrm = [&]() -> bool {
        if (i >= img.size()) return false;
        unsigned const b   = img[i++];
        unsigned const mod = b >> 6;
        unsigned const low = b & 7u;
        reg = ((b >> 3) & 7u) | rexR;
        if (mod == 3u) {
            rm = low | rexB;
            return true;
        }
        memory = true;
        if (low == 4u) {
            if (i >= img.size()) return false;
            unsigned const sib = img[i++];
            unsigned const sb = sib & 7u, sx = (sib >> 3) & 7u;
            if (!(mod == 0u && sb == 5u)) d.base = sb | rexB;
            if (sx != 4u || rexX != 0u) d.index = sx | rexX;
            if (mod == 0u && sb == 5u) i += 4u;
        } else if (mod == 0u && low == 5u) {
            d.ripRelative = true;
            i += 4u;
        } else {
            d.base = low | rexB;
        }
        i += mod == 1u ? 1u : mod == 2u ? 4u : 0u;
        d.reads |= x64Bit(d.base) | x64Bit(d.index);
        return true;
    };
    // Without a REX prefix an 8-bit register number 4..7 is ah, ch, dh or bh.
    auto const gp = [&](unsigned r, bool byte) {
        return byte && rex == 0u && r >= 4u && r <= 7u ? r - 4u : r;
    };
    auto const regOf = [&](bool xmm, bool byte) { return xmm ? kX64Xmm + reg : gp(reg, byte); };
    auto const readRm = [&](bool xmm, bool byte) {
        if (memory) d.load = true;
        else        d.reads |= x64Bit(xmm ? kX64Xmm + rm : gp(rm, byte));
    };
    auto const writeRm = [&](bool xmm, bool byte) {
        if (memory) d.store = true;
        else        d.writes |= x64Bit(xmm ? kX64Xmm + rm : gp(rm, byte));
    };
    auto const fill = [&](unsigned r) {   // `r` is written, and filled by the load if there is one
        d.writes |= x64Bit(r);
        if (memory) d.loadTo = r;
    };
    std::size_t const immz = p66 ? 2u : 4u;   // an "imm32" under the operand-size prefix
    auto const relative = [&](std::size_t n) -> bool {
        if (i + n > img.size()) return false;
        std::int64_t const v = n == 1u ? std::int64_t{static_cast<std::int8_t>(img[i])}
                                       : std::int64_t{static_cast<std::int32_t>(rdU32(img, i))};
        i += n;
        d.target = static_cast<std::size_t>(static_cast<std::int64_t>(i) + v);
        return true;
    };

    if (!twoByte) {
        if (op < 0x40u && (op & 7u) <= 5u) {                 // ADD OR ADC SBB AND SUB XOR CMP
            unsigned const form = op & 7u, group = op & 0xF8u;
            bool const     compare = group == 0x38u;
            bool const     byte    = form == 0u || form == 2u || form == 4u;
            if (form <= 3u) {
                if (!modrm()) return std::nullopt;
                unsigned const r = regOf(false, byte);
                // `xor r, r` and `sub r, r` write a zero and read nothing.
                bool const zero = (group == 0x30u || group == 0x28u) && !memory && rm == reg;
                if (!zero) {
                    readRm(false, byte);
                    d.reads |= x64Bit(r);
                }
                if (form <= 1u) {
                    if (!compare) writeRm(false, byte);
                } else if (!compare) {
                    fill(r);
                }
            } else {
                d.reads |= x64Bit(0);
                if (!compare) d.writes |= x64Bit(0);
                i += form == 4u ? 1u : immz;
            }
        } else if (op >= 0x50u && op <= 0x57u) {            // PUSH r
            d.reads |= x64Bit((op & 7u) | rexB);
        } else if (op >= 0x58u && op <= 0x5Fu) {            // POP r
            d.writes |= x64Bit((op & 7u) | rexB);
        } else if (op == 0x63u) {                           // MOVSXD
            if (!modrm()) return std::nullopt;
            readRm(false, false);
            fill(regOf(false, false));
        } else if (op == 0x69u || op == 0x6Bu) {            // IMUL r, r/m, imm
            if (!modrm()) return std::nullopt;
            readRm(false, false);
            fill(regOf(false, false));
            i += op == 0x6Bu ? 1u : immz;
        } else if (op >= 0x70u && op <= 0x7Fu) {            // Jcc rel8
            if (!relative(1)) return std::nullopt;
            d.flow = X64Flow::Branch;
        } else if (op == 0x80u || op == 0x81u || op == 0x83u) {   // group 1 with an immediate
            if (!modrm()) return std::nullopt;
            bool const byte = op == 0x80u;
            readRm(false, byte);
            if ((reg & 7u) != 7u) writeRm(false, byte);     // /7 is CMP
            i += op == 0x81u ? immz : 1u;
        } else if (op == 0x84u || op == 0x85u) {            // TEST r/m, r
            if (!modrm()) return std::nullopt;
            readRm(false, op == 0x84u);
            d.reads |= x64Bit(regOf(false, op == 0x84u));
        } else if (op == 0x88u || op == 0x89u) {            // MOV r/m, r
            if (!modrm()) return std::nullopt;
            bool const byte = op == 0x88u;
            d.reads |= x64Bit(regOf(false, byte));
            writeRm(false, byte);
            if (!memory && !byte) {
                d.copyTo   = rm;
                d.copyFrom = reg;
            }
        } else if (op == 0x8Au || op == 0x8Bu) {            // MOV r, r/m
            if (!modrm()) return std::nullopt;
            bool const byte = op == 0x8Au;
            readRm(false, byte);
            fill(regOf(false, byte));
            if (!memory && !byte) {
                d.copyTo   = reg;
                d.copyFrom = rm;
            }
        } else if (op == 0x8Du) {                           // LEA
            if (!modrm() || !memory) return std::nullopt;
            d.writes |= x64Bit(regOf(false, false));
            d.leaTo = regOf(false, false);
        } else if (op == 0x90u && rexB == 0u) {             // NOP
        } else if (op == 0x98u) {                           // CWDE / CDQE
            d.reads  |= x64Bit(0);
            d.writes |= x64Bit(0);
        } else if (op == 0x99u) {                           // CDQ / CQO
            d.reads  |= x64Bit(0);
            d.writes |= x64Bit(2);
        } else if (op == 0xA8u || op == 0xA9u) {            // TEST al/eax, imm
            d.reads |= x64Bit(0);
            i += op == 0xA8u ? 1u : immz;
        } else if (op >= 0xB0u && op <= 0xB7u) {            // MOV r8, imm8
            d.writes |= x64Bit(gp((op & 7u) | rexB, true));
            i += 1u;
        } else if (op >= 0xB8u && op <= 0xBFu) {            // MOV r, imm
            d.writes |= x64Bit((op & 7u) | rexB);
            i += rexW ? 8u : immz;
        } else if (op == 0xC0u || op == 0xC1u || (op >= 0xD0u && op <= 0xD3u)) {   // shifts, rotates
            if (!modrm()) return std::nullopt;
            bool const byte = op == 0xC0u || op == 0xD0u || op == 0xD2u;
            readRm(false, byte);
            writeRm(false, byte);
            if (op == 0xD2u || op == 0xD3u) d.reads |= x64Bit(1);   // by cl
            if (op == 0xC0u || op == 0xC1u) i += 1u;
        } else if (op == 0xC2u || op == 0xC3u) {            // RET
            d.flow = X64Flow::Return;
            if (op == 0xC2u) i += 2u;
        } else if (op == 0xC6u || op == 0xC7u) {            // MOV r/m, imm
            if (!modrm() || (reg & 7u) != 0u) return std::nullopt;
            writeRm(false, op == 0xC6u);
            i += op == 0xC6u ? 1u : immz;
        } else if (op == 0xCCu) {                           // INT3
            d.flow = X64Flow::Trap;
        } else if (op == 0xE8u || op == 0xE9u) {            // CALL / JMP rel32
            if (!relative(4)) return std::nullopt;
            d.flow = op == 0xE8u ? X64Flow::Call : X64Flow::Jump;
        } else if (op == 0xEBu) {                           // JMP rel8
            if (!relative(1)) return std::nullopt;
            d.flow = X64Flow::Jump;
        } else if (op == 0xF6u || op == 0xF7u) {            // group 3
            if (!modrm()) return std::nullopt;
            bool const     byte = op == 0xF6u;
            unsigned const n    = reg & 7u;
            readRm(false, byte);
            if (n <= 1u) {                                  // TEST r/m, imm
                i += byte ? 1u : immz;
            } else if (n <= 3u) {                           // NOT, NEG
                writeRm(false, byte);
            } else {                                        // MUL, IMUL, DIV, IDIV
                std::uint32_t const pair = byte ? x64Bit(0) : (x64Bit(0) | x64Bit(2));
                d.reads  |= n <= 5u ? x64Bit(0) : pair;
                d.writes |= pair;
            }
        } else if (op == 0xFEu || op == 0xFFu) {            // groups 4 and 5
            if (!modrm()) return std::nullopt;
            unsigned const n = reg & 7u;
            if (n <= 1u) {                                  // INC, DEC
                readRm(false, op == 0xFEu);
                writeRm(false, op == 0xFEu);
            } else if (op == 0xFFu && n == 2u) {            // CALL r/m
                readRm(false, false);
                d.flow = X64Flow::Call;
            } else if (op == 0xFFu && n == 4u) {            // JMP r/m
                readRm(false, false);
                d.flow = X64Flow::IndirectJump;
            } else if (op == 0xFFu && n == 6u) {            // PUSH r/m
                readRm(false, false);
            } else {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    } else {
        bool const scalar = pF2 || pF3;
        // The vector moves: MOVUPS/MOVUPD/MOVSS/MOVSD, MOVAPS/MOVAPD, MOVDQA/MOVDQU and
        // MOVQ, each as the load into a register and as the store out of one.
        bool const vectorLoad  = op == 0x10u || (op == 0x28u && !scalar)
                                 || (op == 0x6Fu && (p66 || pF3)) || (op == 0x7Eu && pF3);
        bool const vectorStore = op == 0x11u || (op == 0x29u && !scalar)
                                 || (op == 0x7Fu && (p66 || pF3)) || (op == 0xD6u && p66);
        if (op == 0x0Bu) {                                  // UD2
            d.flow = X64Flow::Trap;
        } else if (op == 0x1Fu) {                           // NOP r/m: its operand is not read
            if (!modrm()) return std::nullopt;
            d.reads = 0;
        } else if (vectorLoad) {
            if (!modrm()) return std::nullopt;
            readRm(true, false);
            fill(regOf(true, false));
        } else if (vectorStore) {
            if (!modrm()) return std::nullopt;
            d.reads |= x64Bit(regOf(true, false));
            writeRm(true, false);
        } else if (op == 0x2Au && scalar) {                 // CVTSI2SS / CVTSI2SD
            if (!modrm()) return std::nullopt;
            readRm(false, false);
            d.writes |= x64Bit(regOf(true, false));
        } else if ((op == 0x2Cu || op == 0x2Du) && scalar) {    // CVT(T)SS2SI / CVT(T)SD2SI
            if (!modrm()) return std::nullopt;
            readRm(true, false);
            fill(regOf(false, false));
        } else if ((op == 0x2Eu || op == 0x2Fu) && !scalar) {   // (U)COMISS / (U)COMISD
            if (!modrm()) return std::nullopt;
            readRm(true, false);
            d.reads |= x64Bit(regOf(true, false));
        } else if (op >= 0x40u && op <= 0x4Fu) {            // CMOVcc: the destination is kept or replaced
            if (!modrm()) return std::nullopt;
            readRm(false, false);
            d.reads |= x64Bit(regOf(false, false));
            fill(regOf(false, false));
        } else if (op == 0x51u || op == 0x5Au || op == 0x5Bu) {   // SQRT, the float conversions
            if (!modrm()) return std::nullopt;
            readRm(true, false);
            fill(regOf(true, false));
        } else if ((op >= 0x54u && op <= 0x59u) || (op >= 0x5Cu && op <= 0x5Fu)
                   || (p66 && (op == 0xD4u || op == 0xDBu || op == 0xEBu || op == 0xEFu
                               || op == 0xFAu || op == 0xFBu || op == 0xFEu))) {
            // AND/ANDN/OR/XOR, ADD/MUL/SUB/MIN/DIV/MAX, and the integer PADD/PAND/POR/PXOR/PSUB
            if (!modrm()) return std::nullopt;
            bool const zero = (op == 0x57u || op == 0xEFu) && !memory && rm == reg;   // x ^ x
            if (!zero) {
                readRm(true, false);
                d.reads |= x64Bit(regOf(true, false));
            }
            d.writes |= x64Bit(regOf(true, false));
        } else if (op == 0x6Eu && p66) {                    // MOVD/MOVQ xmm, r/m
            if (!modrm()) return std::nullopt;
            readRm(false, false);
            d.writes |= x64Bit(regOf(true, false));
        } else if (op == 0x7Eu && p66) {                    // MOVD/MOVQ r/m, xmm
            if (!modrm()) return std::nullopt;
            d.reads |= x64Bit(regOf(true, false));
            writeRm(false, false);
        } else if (op >= 0x80u && op <= 0x8Fu) {            // Jcc rel32
            if (!relative(4)) return std::nullopt;
            d.flow = X64Flow::Branch;
        } else if (op >= 0x90u && op <= 0x9Fu) {            // SETcc
            if (!modrm()) return std::nullopt;
            writeRm(false, true);
        } else if (op == 0xAFu) {                           // IMUL r, r/m
            if (!modrm()) return std::nullopt;
            readRm(false, false);
            d.reads |= x64Bit(regOf(false, false));
            fill(regOf(false, false));
        } else if (op == 0xB6u || op == 0xB7u || op == 0xBEu || op == 0xBFu) {   // MOVZX / MOVSX
            if (!modrm()) return std::nullopt;
            readRm(false, op == 0xB6u || op == 0xBEu);
            fill(regOf(false, false));
        } else {
            return std::nullopt;
        }
    }
    if (i > img.size()) return std::nullopt;
    d.length = i - at;
    return d;
}

// THE FIRST VALUE A HANDLER TAKES FROM OUTSIDE ITSELF, read off the image. A
// handler is entered by the unwinder, which hands over the frame and the
// registers a call preserves and nothing else — so the first register a handler
// READS WITHOUT HAVING WRITTEN IT must be one a call preserves, or the value
// must come out of the frame.
//
// This walk asks it of the handler's OPENING STATEMENT only: straight-line
// instructions from the handler's address, keeping the set of registers the
// handler has written and the set that hold an address of the frame (formed
// from rsp or rbp), and stopping at the first read of a register it has not
// written (a volatile one is the finding, and is named before a preserved one
// the same instruction reads), at a load of a VALUE out of the frame, or at the
// first store to memory (the end of the statement: `NothingFromOutside`). A
// form `decodeX64` does not know, or a jump, ends it as `NotDecoded`, and the
// caller prints the bytes. `everyValueAHandlerTakes` below asks the same
// question of every path to the function's return.
enum class HandlerTakes : std::uint8_t {
    NotDecoded,
    AVolatileRegister,     // a register the unwinder does not hand over
    APreservedRegister,
    AValueOfTheFrame,      // a load through rsp, rbp, or a register holding an address of the frame
    NothingFromOutside,    // the statement ended on a store that read no register from outside
};

struct HandlerFirstRead {
    HandlerTakes takes = HandlerTakes::NotDecoded;
    unsigned     reg   = 0;   // the register read: 0..15 general, 16..31 xmm
};

[[nodiscard]] inline unsigned x64LowestRegister(std::uint32_t set) {
    for (unsigned r = 0; r < 32u; ++r) {
        if ((set & x64Bit(r)) != 0u) return r;
    }
    return kX64NoRegister;
}

[[nodiscard]] inline HandlerFirstRead
firstValueAHandlerTakes(std::vector<std::uint8_t> const& img, std::size_t at) {
    HandlerFirstRead out;
    std::uint32_t    written = 0, frameAddress = 0;
    std::size_t      i = at;
    for (int step = 0; step < 8 && at != 0; ++step) {
        auto const d = decodeX64(img, i);
        if (!d || d->flow != X64Flow::Next) return out;
        std::uint32_t const fresh = d->reads & ~written & ~kX64Frame;
        if (fresh != 0u) {
            bool const volatileOne = (fresh & kWin64Volatile) != 0u;
            out.reg   = x64LowestRegister(volatileOne ? fresh & kWin64Volatile : fresh);
            out.takes = volatileOne ? HandlerTakes::AVolatileRegister
                                    : HandlerTakes::APreservedRegister;
            return out;
        }
        bool const throughFrame =
            d->base != kX64NoRegister && ((kX64Frame | frameAddress) & x64Bit(d->base)) != 0u;
        if (d->load && !d->ripRelative && throughFrame && d->index == kX64NoRegister) {
            out.reg   = d->base;
            out.takes = HandlerTakes::AValueOfTheFrame;
            return out;
        }
        std::uint32_t frame = frameAddress & ~d->writes;
        if (d->leaTo != kX64NoRegister && d->index == kX64NoRegister && throughFrame) {
            frame |= x64Bit(d->leaTo);
        }
        if (d->copyTo != kX64NoRegister && ((kX64Frame | frameAddress) & x64Bit(d->copyFrom)) != 0u) {
            frame |= x64Bit(d->copyTo);
        }
        written |= d->writes;
        frameAddress = frame;
        if (d->store) {
            out.takes = HandlerTakes::NothingFromOutside;
            return out;
        }
        i += d->length;
    }
    return out;
}

// ── EVERY VALUE A HANDLER TAKES FROM OUTSIDE ITSELF ─────────────────────────
//
// The same question asked of EVERY PATH from the handler's address to the
// function's return — the handler's statements and the code after them, which
// reads what the handler and the code before the region left — in BOTH
// register files. Under the Win64 convention the registers a call does not
// preserve are rax, rcx, rdx, r8..r11 and xmm0..xmm5: a read of one the path
// has not written is the finding (`volatileReads`), and a read of a call-
// preserved one, or a load out of the frame, is a value the unwinder did hand
// over.
//
// An unconditional jump is followed and a conditional one followed both ways;
// a call is walked past, and writes every register a call does not preserve; a
// path ends at a return, at a jump whose target the bytes do not hold, at a
// trap, or at a jump out of the function. A block reached again with a
// register unwritten that was written when it was first walked is walked again
// with the smaller set, so a loop is walked to its fixed point. A form the
// decoder does not know ends its path and is said (`undecodedAt`), and so is a
// walk longer than its budget: the statement "no handler reads such a
// register" is only as wide as the paths decoded to their end.
struct HandlerRead {
    unsigned    reg = kX64NoRegister;   // 0..15 general, 16..31 xmm
    std::size_t at  = 0;                // the reading instruction's offset
};

struct HandlerWalk {
    std::vector<HandlerRead>   volatileReads;          // the finding
    std::size_t                preservedGeneral = 0;   // reads of rbx, rsi, rdi, r12..r15 not written first
    std::size_t                preservedVector  = 0;   // ... of xmm6..xmm15
    std::size_t                frameLoads       = 0;   // values loaded out of the frame
    std::size_t                instructions     = 0;   // decoded, over every path
    std::optional<std::size_t> undecodedAt;            // the first form the decoder did not know
    bool                       overBudget       = false;
    [[nodiscard]] bool decodedToItsEnd() const { return !undecodedAt.has_value() && !overBudget; }
};

[[nodiscard]] inline HandlerWalk
everyValueAHandlerTakes(std::vector<std::uint8_t> const& img, std::size_t at,
                        std::size_t functionBegin, std::size_t functionEnd) {
    constexpr std::size_t kBudget = 4096;   // instructions, over every path
    struct Path {
        std::size_t   at;
        std::uint32_t written;
        std::uint32_t frameAddress;
    };
    HandlerWalk                          out;
    std::vector<Path>                    work{Path{at, 0u, 0u}};
    std::map<std::size_t, std::uint32_t> walkedWith;   // an instruction -> the written set it was walked with
    while (!work.empty()) {
        Path p = work.back();
        work.pop_back();
        for (;;) {
            if (p.at < functionBegin || p.at >= functionEnd) break;   // a jump out of the function
            if (auto const it = walkedWith.find(p.at); it != walkedWith.end()) {
                if ((it->second & ~p.written) == 0u) break;   // nothing unwritten now that was written then
                p.written &= it->second;
                it->second = p.written;
            } else {
                walkedWith.emplace(p.at, p.written);
            }
            if (++out.instructions > kBudget) {
                out.overBudget = true;
                return out;
            }
            auto const d = decodeX64(img, p.at);
            if (!d) {
                if (!out.undecodedAt) out.undecodedAt = p.at;
                break;
            }
            for (unsigned r = 0; r < 32u; ++r) {
                std::uint32_t const bit = x64Bit(r);
                if ((d->reads & bit) == 0u || (kX64Frame & bit) != 0u || (p.written & bit) != 0u) continue;
                if ((kWin64Volatile & bit) != 0u) out.volatileReads.push_back(HandlerRead{r, p.at});
                else if (r >= kX64Xmm)           ++out.preservedVector;
                else                             ++out.preservedGeneral;
            }
            bool const throughFrame =
                d->base != kX64NoRegister && ((kX64Frame | p.frameAddress) & x64Bit(d->base)) != 0u;
            if (d->load && !d->ripRelative && throughFrame) ++out.frameLoads;
            std::uint32_t frame = p.frameAddress & ~d->writes;
            if (d->leaTo != kX64NoRegister && d->index == kX64NoRegister && throughFrame) {
                frame |= x64Bit(d->leaTo);
            }
            if (d->copyTo != kX64NoRegister
                && ((kX64Frame | p.frameAddress) & x64Bit(d->copyFrom)) != 0u) {
                frame |= x64Bit(d->copyTo);
            }
            p.written      |= d->writes;
            p.frameAddress  = frame;
            std::size_t const next = p.at + d->length;
            if (d->flow == X64Flow::Next) {
                p.at = next;
            } else if (d->flow == X64Flow::Jump) {
                p.at = d->target;
            } else if (d->flow == X64Flow::Branch) {
                work.push_back(Path{d->target, p.written, p.frameAddress});
                p.at = next;
            } else if (d->flow == X64Flow::Call) {
                p.written      |= kWin64Volatile;
                p.frameAddress &= ~kWin64Volatile;
                p.at = next;
            } else {
                break;   // a return, an indirect jump, a trap
            }
        }
    }
    return out;
}

} // namespace dss::test_support
