# In-tree Assembler — Sub-Plan (13)

> Owns instruction encoding for v1 ISAs. Translates [LIR](./12-mir-lir-plan%20-%20ok.md) (post-regalloc, post-frame-materialization, physical-register-only) into raw machine bytes + relocation records consumed by the [in-tree linker](./14-linker-plan%20-%20tbd.md). Per the hermetic-compiler invariant, **no GAS / MASM / llvm-mc invocation** — we own every byte.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **planned.** v1 production-critical. |
| Predecessors  | ✅ [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20ok.md) (LIR shape — ML1–ML8 closed 2026-05-29; `.dsslir` round-trip lossless; remaining ML7 cycle 2 = ARM64-stackPointer + ABI goldens, only blocking if AS1 targets ARM64 first). |
| Successors    | ⏳ [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) consumes (bytes, relocations) per section. ⏳ [`15-debug-info-plan`](./15-debug-info-plan%20-%20tbd.md) consumes (byte-offset → SourceSpan) maps. |
| Scope         | **Bounded.** AS1–AS6. v1 ISAs: x86_64 + ARM64. WASM bytecode + SPIR-V words live in their own plans (18 / 17). |

---

## 1. Motivation

Hermetic per [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1.1 — no shelling to GAS / MASM / llvm-mc. Why its own plan (vs. folding into the linker): **encoding correctness** (bits → ISA-legal) and **layout correctness** (sections + relocations + load) are independently testable concerns. Encoding bugs are silent and disastrous — a wrong REX prefix produces a different valid instruction. Pin them with per-instruction hex roundtrips.

---

## 2. Design

### 2.1 Files

```
src/asm/
├── asm.hpp                # Public Assembler API
├── asm_buffer.hpp / .cpp  # Section bytes + reloc list
├── relocation.hpp         # Per-(arch×format) relocation type enum
├── x86_64/
│   ├── encoder.hpp / .cpp # Instruction → bytes
│   ├── encoding_table.cpp # SDM-derived encoding rows
│   └── disasm.hpp / .cpp  # Round-trip oracle (test-only)
├── arm64/
│   ├── encoder.hpp / .cpp
│   ├── encoding_table.cpp # ARM ARM-derived encoding rows
│   └── disasm.hpp / .cpp  # Round-trip oracle (test-only)
└── tests/
    └── (per-instruction hex roundtrip pins)
```

### 2.2 Input contract

`Assembler::encode(LirFunction const&) → (bytes: vector<uint8_t>, relocations: vector<Relocation>)`

LIR inputs are guaranteed:
- All operands are physical registers, immediates, or memory operands (base+offset+index+scale).
- Stack frame is materialized; spill/reload slots resolved to memory operands.
- Calling convention is lowered — no `call` operand is symbolic except for cross-translation-unit references (those produce a relocation).

### 2.3 Output contract

Per-section byte buffers + a list of relocations. Each relocation:

```cpp
struct Relocation {
    uint64_t offset;        // Within the section bytes
    SymbolId target;        // What symbol this references
    RelocationKind kind;    // Per (arch × format) — see §2.5
    int64_t addend;         // ABI-specific
};
```

### 2.4 v1 instruction subset

**x86_64** (Intel SDM Vol 2 derived):
- Data movement: `MOV` (reg/imm/mem/reg64-imm64/sign+zero ext via MOVSX/MOVZX), `LEA`, `PUSH`, `POP`, `XCHG`
- Arithmetic: `ADD`, `SUB`, `IMUL` (2/3-op forms), `IDIV`, `DIV`, `NEG`, `INC`, `DEC`, `CMP`
- Bitwise: `AND`, `OR`, `XOR`, `NOT`, `SHL`/`SHR`/`SAR`, `ROL`/`ROR`
- Control: `JMP` (rel32/rel8), `Jcc` (16 cond codes), `CALL` (rel32 + indirect), `RET`, `RET imm16`
- Comparison setup: `TEST`, `SETcc`
- Floating-point: SSE2 baseline (`MOVSS`/`MOVSD`/`ADDSS`/`SUBSS`/`MULSS`/`DIVSS` + double variants + `UCOMISS`/`UCOMISD`)
- AVX / AVX-512: **reserved post-v1**

**ARM64** (ARMv8-A ARM derived):
- Data movement: `MOV` (reg/imm/extended-imm via MOVZ/MOVK/MOVN), `LDR`/`LDRB`/`LDRH`/`LDRSW`/`STR`/`STRB`/`STRH`, `LDP`/`STP` (pair), `LDUR`/`STUR` (unscaled)
- Arithmetic: `ADD`/`SUB` (reg/imm/shifted-reg/extended-reg), `MUL`, `MADD`, `MSUB`, `SDIV`, `UDIV`, `NEG`, `CMP`/`CMN`
- Bitwise: `AND`, `ORR`, `EOR`, `BIC`, `MVN`, `LSL`/`LSR`/`ASR`, `RBIT`, `CLZ`
- Control: `B` (cond + uncond), `BL`, `BR`, `BLR`, `RET`, `CBZ`/`CBNZ`, `TBZ`/`TBNZ`
- Comparison setup: `CSEL`, `CSINC`, `CSET`
- Floating-point: NEON scalar baseline (`FMOV`, `FADD`/`FSUB`/`FMUL`/`FDIV`, `FCMP`)
- SIMD vectors: **reserved post-v1**

### 2.5 Relocation taxonomy

Per (arch × format). Format owned by [`14-linker-plan`](./14-linker-plan%20-%20tbd.md); kinds emitted here.

| Arch | Format | Kinds |
|------|--------|-------|
| x86_64 | ELF (SysV) | `R_X86_64_64`, `R_X86_64_PC32`, `R_X86_64_PLT32`, `R_X86_64_GOTPCREL`, `R_X86_64_TPOFF32` |
| x86_64 | PE/COFF | `IMAGE_REL_AMD64_ADDR32`, `ADDR64`, `REL32`, `REL32_1`..`REL32_5` |
| x86_64 | Mach-O | `X86_64_RELOC_BRANCH`, `SIGNED`, `GOT`, `GOT_LOAD`, `SUBTRACTOR`, `UNSIGNED` |
| ARM64 | ELF | `R_AARCH64_ABS64`, `R_AARCH64_CALL26`, `R_AARCH64_ADR_PREL_PG_HI21`, `R_AARCH64_ADD_ABS_LO12_NC`, `R_AARCH64_LDST{8,16,32,64,128}_ABS_LO12_NC` |
| ARM64 | PE/COFF | `IMAGE_REL_ARM64_ADDR64`, `BRANCH26`, `PAGEBASE_REL21`, `REL21`, `PAGEOFFSET_12L` |
| ARM64 | Mach-O | `ARM64_RELOC_BRANCH26`, `PAGE21`, `PAGEOFF12`, `UNSIGNED` |

The assembler emits the (arch-correct) kind; the linker resolves it per (format) into the final byte mutation.

### 2.6 Encoding tables

**Hand-written C++** in v1 (open question §4.1 default). Each instruction has a corresponding row in `encoding_table.cpp` containing:
- Opcode bytes (with placeholder bits for ModR/M, SIB, imm)
- Operand-shape constraints (which register classes / addressing modes are valid)
- Prefix selection rules (REX, VEX, OF prefix for 16-bit)
- Relocation-class hint (for symbol references)

For x86_64, ~80 distinct instructions × few encoding variants ≈ 300 encoding rows.  
For ARM64, ~70 distinct instructions × few encoding variants ≈ 200 rows. Manageable.

If a third ISA lands post-v1 (RISC-V, ARMv7-Thumb), revisit table generation from a JSON spec.

### 2.7 Disassembler round-trip oracle

Test-only. After encoding instruction `I` to bytes `B`, disassemble `B` and compare the mnemonic+operands to the original `I`. Catches encoder bugs that produce valid-but-wrong instructions.

Reference outputs (objdump / llvm-mc) are TEST ORACLES only — golden hex snapshots live in fixtures; the production pipeline does not invoke objdump.

### 2.8 Source-position mapping output

**Diagnostic namespace** for this plan: `A_*` — `A_EncodingInvalid` (operand-shape mismatch), `A_RelocationKindUnknown`, `A_RoundTripMismatch` (disassembler oracle disagrees with encoder), `A_InstructionUnimplemented` (LIR opcode not in the v1 subset). Fail-loud per `*Fatal` discipline.

Alongside bytes + relocations, the encoder emits a `(byte_offset → LirInstId)` map per function. Combined with LIR's own `LirAttribute<SourceSpan>` (populated during MIR→LIR lowering per [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20ok.md) §2.7), this gives us a `(byte_offset → SourceSpan)` chain consumed by [`15-debug-info-plan`](./15-debug-info-plan%20-%20tbd.md). Each IR layer owns its own per-arena attribute family — the chain composes via `byte_offset → LirInstId → SourceSpan`, never via cross-arena `HirAttribute` reads (which would violate the `treeTag` discipline from SH3).

---

## 3. PR breakdown

| PR  | Title                                       | Scope |
|-----|---------------------------------------------|-------|
| AS1 | Assembler skeleton + buffer + relocation type | Public API; per-(arch×format) `Relocation` enum; per-instruction emission interface. |
| AS2 | x86_64 v1 instruction subset encoding        | All instructions in §2.4 x86_64; ModR/M / SIB / REX correct; per-instruction hex roundtrip pin. |
| AS3 | ARM64 v1 instruction subset encoding         | All instructions in §2.4 ARM64; fixed-32-bit encoding correct; per-instruction hex roundtrip pin. |
| AS4 | Relocation emission per (arch × format) matrix | Six combos (2 arch × 3 format). Pin each kind against a known-good encoded image. |
| AS5 | Disassembler round-trip test harness         | x86_64 + ARM64 minimal disassemblers (test-only). Every AS2/AS3 instruction round-trips. |
| AS6 | LIR → bytes pipeline integration             | End-to-end c-subset corpus function: HIR→MIR→LIR→bytes+relocations; linker (LK4) consumes correctly. |

Substrate tier (5-agent review) for AS1 (interface contract) and AS4 (relocation taxonomy). Feature tier for AS2/AS3/AS5/AS6.

---

## 4. Open questions

| # | Question | Default if unanswered |
|---|----------|-----------------------|
| 1 | Hand-written encoding tables vs. JSON-spec-driven? | **Hand-written C++** for v1. Avoids meta-pipeline + JSON-spec authorship overhead. Revisit if a third ISA lands. |
| 2 | Instruction-level peephole optimization at this layer? | **No** — peepholes live in LIR pre-encoding. |
| 3 | Architecture extensions (AVX, NEON-SVE, ARMv8.x features)? | **Out of v1.** Reserve namespace. |
| 4 | x86_64 syntax in disassembler test fixtures: AT&T or Intel? | **Intel** — matches Intel SDM (the encoding-table reference). |
| 5 | Endianness assumption? | **Little** for v1 (both ISAs LE). Big-endian reserved (PowerPC etc. post-v1). |
| 6 | Instruction scheduling at this layer? | **No** — scheduling lives in LIR pre-encoding (or deferred entirely; v1 emits in LIR-given order). |
| 7 | Inline-assembly support (custom-language `__asm__` blocks)? | **Out of v1.** No shipped language uses it. |
| 8 | Position-independent code emission? | **Yes** — required for ELF `.so` / Mach-O dylibs. Default to PIC; `-fno-pic` reserved post-v1. |

---

## 5. Acceptance criteria

- [ ] Every v1-subset instruction (§2.4) assembles to the exact bytes the SDM / ARM ARM document.
- [ ] Per-instruction disassembler round-trip pin passes for every encoded instruction.
- [ ] Golden hex snapshots match objdump / llvm-mc reference output across the v1 subset.
- [ ] Linker consumes our (bytes, relocations) and produces a binary indistinguishable from a system-linker build of the same LIR-equivalent source (validated by running both binaries and diffing stdout/exit code).
- [ ] c-subset corpus' LIR encodes cleanly on x86_64 + ARM64 without unimplemented-instruction failures.
- [ ] Hermetic acceptance: building on a CI runner with NO `as` / `nasm` / `llvm-mc` installed produces correct output.

---

## 6. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Silent encoding bugs (valid bytes, wrong instruction) | High | Critical | Per-instruction hex roundtrip + disassembler oracle is the gate; never merge AS2/AS3 without those tests. |
| ModR/M / SIB / REX prefix interactions on x86_64 are intricate | High | High | Encoding-table rows are exhaustive; review against SDM Vol 2 verbatim. |
| Missing relocations break linkage silently | High | High | AS4 is its own PR with linker-integration pins; never merge AS4 without LK4 alignment. |
| Encoding-table maintenance burden as ISA extensions land | Medium | Medium | Tables are flat; appending rows is cheap. Migration to JSON-driven generation is deferred post-v1 §4.1. |

---

## 7. Sequencing

```
12-mir-lir (ML8) ─► AS1 ─► AS2 (x86_64) ─┐
                           AS3 (ARM64)  ─┴─► AS4 ─► AS5 ─► AS6 ─► 14-linker
```

AS2 / AS3 are parallel after AS1. AS5 (round-trip oracle) gates AS6.
