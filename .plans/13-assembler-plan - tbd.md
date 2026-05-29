# In-tree Assembler — Sub-Plan (13)

> Owns instruction encoding for v1 ISAs. Translates [LIR](./12-mir-lir-plan%20-%20ok.md) (post-regalloc, post-frame-materialization, physical-register-only) into raw machine bytes + relocation records consumed by the [in-tree linker](./14-linker-plan%20-%20tbd.md). Per the hermetic-compiler invariant, **no GAS / MASM / llvm-mc invocation** — we own every byte.
>
> **Rev 3 (2026-05-29) — config-driven encoding, mirroring the ML5 cycle 2a pivot.** Per [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) Decision #4's three-bucket rule (canonicalized 2026-05-29): encoding rows are JSON-declared (bucket 1) on the per-target schema's `encoding` facet; the assembler is one engine reading them (bucket 2). NO per-arch `.cpp` directories. Rev 1/2's `src/asm/x86_64/` + `src/asm/arm64/` per-arch trees are deleted. Validated by 2-architect parallel pass before this rev landed (see §2.4 honest carve-outs).

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **planned (rev 3).** v1 production-critical. |
| Predecessors  | ✅ [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20ok.md) (LIR shape — ML1–ML8 closed 2026-05-29; `.dsslir` round-trip lossless; remaining ML7 cycle 2 = ARM64-stackPointer + ABI goldens, only blocking if AS1 targets ARM64 first). |
| Successors    | ⏳ [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) consumes (bytes, relocations) per section. ⏳ [`15-debug-info-plan`](./15-debug-info-plan%20-%20tbd.md) consumes (byte-offset → MirInstId) maps. |
| Scope         | **Bounded.** AS1–AS6. v1 ISAs: x86_64 + ARM64. WASM bytecode + SPIR-V words live in their own plans (18 / 17) per architect §2.4 carve-out — those formats are genuine bucket-2 boundary cases. |

---

## 1. Motivation

Hermetic per [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1.1 — no shelling to GAS / MASM / llvm-mc. Why its own plan (vs. folding into the linker): **encoding correctness** (bits → ISA-legal) and **layout correctness** (sections + relocations + load) are independently testable concerns. Encoding bugs are silent and disastrous — a wrong REX prefix produces a different valid instruction. Pin them with per-instruction hex roundtrips.

---

## 2. Design

### 2.1 Files (rev 3 — config-driven)

```
src/asm/
├── asm.hpp / .cpp          # Public Assembler API + assemble() entrypoint
├── asm_buffer.hpp / .cpp   # Section bytes + reloc list
├── format/                 # ONE bucket-2 function per encoding-format tag (NOT per arch)
│   ├── x86_variable.cpp    # x86-variable: prefix chain + ModR/M + SIB + imm
│   ├── arm32_fixed.cpp     # arm32-fixed: 32-bit fixed word + bit-field slice composition
│   └── (riscv32_fixed.cpp / wasm_leb.cpp / spirv_word.cpp deferred per §2.4)
├── disasm.hpp / .cpp       # Round-trip oracle (test-only) — same format-dispatched shape
└── tests/
    └── (per-instruction hex roundtrip pins; format-blind golden harness)

src/dss-config/targets/x86_64.target.json
                            # Existing target schema + new `encoding` facet on each opcode row
                            # + new top-level `relocations` array (taxonomy shared with linker)

src/dss-config/targets/arm64.target.json
                            # ARM64 target (drop-in JSON; substrate is target-blind)
```

Rev 1/2's `src/asm/x86_64/` + `src/asm/arm64/` per-arch trees are DELETED — collapsed into:
- **One JSON facet per target** (`*.target.json` gains an `encoding` sub-object on every opcode row + a top-level `relocations` array)
- **One format-encoder function per encoding format** (`x86_variable.cpp`, `arm32_fixed.cpp`) — the function is selected by the row's `format` field, NOT by arch name. Adding ARM64 = drop the JSON file (x86 doesn't change); adding RISC-V = JSON file + new `riscv32_fixed.cpp` format function (existing format functions untouched).

### 2.2 Input contract

```cpp
[[nodiscard]] AssembledModule
assemble(Lir const&                       lir,
         TargetSchema const&              schema,    // declares encoding rows + reloc taxonomy
         std::span<MirInstId const>       lirToMir,  // for the source-position chain
         DiagnosticReporter&              reporter);
```

Single universal entrypoint. **No per-arch overload.** Same target-blind shape ML5 cycle 2a established (`assemble(Lir, TargetSchema, ...)` mirrors `lowerToLir(Mir, TargetSchema, ...)`).

LIR inputs are guaranteed:
- All operands are physical registers, immediates, or memory operands (base+offset+index+scale).
- Stack frame is materialized; spill/reload slots resolved to memory operands.
- Calling convention is lowered — no `call` operand is symbolic except for cross-translation-unit references (those produce a relocation).

### 2.3 Output contract

```cpp
struct SourceMapEntry {
    std::uint32_t byteOffset;
    MirInstId     mirInst;   // composed via lirToMir at assemble time
};                            // (per-architect-2 review — avoids cross-arena LirInstId
                              //  identity drift between pre-ML7 and post-ML7 Lir modules)

struct AssembledFunction {
    std::vector<std::uint8_t>   bytes;
    std::vector<Relocation>     relocations;
    std::vector<SourceMapEntry> sourceMap;
};

struct AssembledModule {
    std::vector<AssembledFunction> functions;  // parallel-index with lir.funcAt(i)
    [[nodiscard]] bool ok() const noexcept;    // derived — never stored (cycle-3a discipline)
    [[nodiscard]] AssembledFunction const* forFuncByIndex(std::uint32_t) const noexcept;
};

struct Relocation {
    std::uint64_t offset;          // Within the section bytes
    SymbolId      target;          // What symbol this references
    std::uint32_t kind;            // Opaque tag — meaning declared in TargetSchema's relocations[]
                                   // (NOT a C++ enum — bucket-1 vocabulary; cycle 2a discipline)
    std::int64_t  addend;          // ABI-specific
};
```

The `Relocation::kind` is an **opaque `uint32_t` tag** whose meaning is declared in `TargetSchema::relocations[]` (the bucket-1 taxonomy facet — see §2.5). The assembler writes the tag; the linker reads the tag via `schema.relocationInfo(kind)`. Neither knows the other format's tags.

### 2.4 Honest bucket-2 carve-outs (architect-validated 2026-05-29)

The "one universal encoder" framing in earlier drafts overstated uniformity. The honest shape per the 2-architect validation pass is **one format-encoder function per encoding-format tag**, each function is data-driven from JSON rows, none of them branches on arch name. This is still bucket-2 (the engine has algorithms parameterized by JSON vocabulary; no identity branches), but the architectural fact deserves naming so we don't drift later.

**Structural rules the format-encoder C++ must own (unconditional engine conventions, NOT arch branches):**

- **x86_64 REX prefix extension bits** (`REX.R` / `REX.B`) — derived from operand `hwEncoding >> 3`, not an arch branch. The `x86_variable.cpp` encoder always computes these from the register table.
- **x86_64 SIB byte forced presence** when `ModR/M.rm == 4` — architectural rule of the encoding format, not arch identity.
- **x86_64 VEX/EVEX prefix field arithmetic** (for SSE/AVX) — the `vvvv` field is the bit-complement of the source register's hwEncoding. Mechanical derivation, not branching.
- **ARM64 `sf` flag** (64-bit operand) — derived from the result register's width, not an arch branch.

These rules live in the format-encoder C++ as named functions (`encodeRexPrefix(reg, ...)`, `forcedSibByte(modrm)`, `derivedSf(resultWidth)`). They are bucket-2 by the operative test: zero identity branches.

**Formats explicitly deferred to their own plans** (genuine bucket-2 boundary cases):

- **WASM bytecode** — `block`/`loop`/`if` are structurally scoped (not address-targeted). The encoder needs a scope stack and `end`-opcode emission at scope boundaries — fundamentally procedural state, not a declarative row template. Owned by [`18-wasm-plan`](./18-wasm-plan%20-%20tbd.md); the WASM encoder is its own bucket-2 algorithm with its own state model.
- **SPIR-V words** — every value-producing instruction has a `<result-id>` that's a sequentially-minted integer, not derivable from the LIR register. Result-id minting + LirReg→spirv-id mapping is global mutable state. Owned by [`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md).

For v1 (x86_64 + ARM64), the `format/x86_variable.cpp` + `format/arm32_fixed.cpp` model is sufficient. WASM + SPIR-V land in their own plans with their own format-encoder cousins consumed by the same top-level `assemble()` dispatch.

### 2.5 Encoding facet on the target schema

Each opcode row in `*.target.json` gains an `encoding` sub-object:

```jsonc
{
  "mnemonic":      "add",
  "result":        "value",
  "terminatorKind": "none",
  // ... existing fields ...
  "encoding": {
    "format": "x86-variable",   // dispatches to format/x86_variable.cpp
    "variants": [
      {
        // Selected at assemble time when LIR operand kinds match this guard.
        "guard": { "operandKinds": ["reg", "reg"] },
        "template": {
          "rex":    { "w": 1 },                    // REX.W forced; R/B computed
          "opcode": ["0x03"]
        },
        "operands": [
          { "index": 0, "slotKind": "modrm.reg" },  // dest reg → ModR/M.reg
          { "index": 1, "slotKind": "modrm.rm"  }   // src reg → ModR/M.rm
        ]
      },
      {
        "guard": { "operandKinds": ["reg", "imm"] },
        "template": {
          "rex":    { "w": 1 },
          "opcode": ["0x81"],
          "modrm":  { "mod": 3, "reg": 0 }          // /0 extension
        },
        "operands": [
          { "index": 0, "slotKind": "modrm.rm" },
          { "index": 1, "slotKind": "imm32"    }
        ]
      }
    ]
  }
}
```

For ARM64 (fixed 32-bit word + bit-field slots):

```jsonc
{
  "mnemonic": "add",
  "encoding": {
    "format": "arm32-fixed",
    "variants": [{
      "guard": { "operandKinds": ["reg", "reg", "imm"] },
      "template": { "fixedWord": "0x11000000", "sf": { "fromResultWidth": true } },
      "operands": [
        { "index": 0, "slotKind": "bitfield", "lo":  0, "hi":  4 },  // Rd
        { "index": 1, "slotKind": "bitfield", "lo":  5, "hi":  9 },  // Rn
        { "index": 2, "slotKind": "bitfield", "lo": 10, "hi": 21 }   // imm12
      ]
    }]
  }
}
```

**EncSlot vocabulary** (the universal slot-kind set the format encoders consume):

| `slotKind` | Meaning |
|---|---|
| `modrm.reg` / `modrm.rm` | Operand `hwEncoding` → ModR/M.reg / .rm field (x86) |
| `bitfield` (`lo`, `hi`) | Operand value/encoding → bits `[hi:lo]` of fixed word (ARM-style) |
| `imm8` / `imm16` / `imm32` / `imm64` | Little-endian immediate append, N bytes |
| `rel32` | Label-relative 4-byte PC-relative fixup (emits a relocation) |
| `mem.base` | Base register's `hwEncoding` → SIB or ModR/M.rm |
| `mem.disp32` | Memory offset → 4-byte displacement |
| `symref` | Symbol reference → relocation record + placeholder bytes |

**Variant selection** — the format encoder linear-scans `variants[]` and picks the first whose `guard.operandKinds` matches the runtime LIR operand-kind vector. Linear scan is O(variants) per emit, trivial at codegen rates. Mnemonic stays stable across variants (the LIR vocabulary doesn't fragment into `add_reg_reg` vs `add_reg_imm`); the assembler picks the right encoding at the byte tier.

### 2.6 Relocation taxonomy facet (shared with linker)

Per architect-2's validation: relocation taxonomy lives as a **facet on `*.target.json`** (NOT a separate file). Same lookup shape as `TargetSchema::opcodeInfo` and `TargetSchema::registerInfo`:

```jsonc
"relocations": [
  { "tag": 0,  "name": "abs64",    "width": 8, "isPCRelative": false, "addendWidth": 8 },
  { "tag": 1,  "name": "pcrel32",  "width": 4, "isPCRelative": true,  "addendWidth": 4 },
  { "tag": 2,  "name": "plt32",    "width": 4, "isPCRelative": true,  "addendWidth": 4 },
  { "tag": 3,  "name": "got_pcrel",...}
]
```

The assembler emits `Relocation{ kind = tag }`; the linker (plan 14) resolves it via `schema.relocationInfo(tag) → { isPCRelative, width, addendWidth, ... }` and applies the formula. **No per-(arch×format) C++ enum** anywhere. Rev 1/2's `src/asm/relocation.hpp` "Per-(arch×format) relocation type enum" is DELETED.

Object-format-specific reloc kinds (e.g., `R_X86_64_PC32` for ELF, `IMAGE_REL_AMD64_REL32` for PE, `X86_64_RELOC_BRANCH` for Mach-O) all map to the SAME bucket-1 formula (PC-relative 32-bit). The format-specific name + integer encoding lives in `*.format.json` (per plan 14's `relocations[]` mapping `format-name → schema-tag`); the assembler only knows the schema tag.

### 2.7 Diagnostics

New `A_*` family at a fresh nibble (parallel to `H_*` HIR, `I_*` MIR, `L_*` LIR, `R_*` regalloc, `O_*` linker). Distinct from `L_*` because the assembler is the next tier after LIR — it consumes a frozen `Lir` and emits bytes, a different artifact class.

- `A_NoEncodingRowForOpcode` — LIR opcode has no `encoding` facet in the target schema (parallel to `L_UnsupportedLoweringForOpcode`)
- `A_NoVariantMatched` — `encoding.variants[]` has no `guard` matching the runtime operand-kind vector
- `A_EncodingInvalid` — operand-shape mismatch with the row's `operands[]` slots
- `A_RelocationTagUnknown` — emitted `kind` tag not in `schema.relocations[]`
- `A_RoundTripMismatch` — disassembler oracle disagrees with encoder (test-only)
- Per-function recovery: emit a target-defined trap byte (`0xCC` for x86_64 INT3; ARM64 `brk #0`) and continue; mark the function `ok=false`. Do NOT abort the entire module — the linker can still report on other functions.

### 2.8 v1 instruction subset

**x86_64** (Intel SDM Vol 2 derived) — declared as `encoding` facets on rows in `x86_64.target.json`:
- Data movement: `MOV` (reg/imm/mem/reg64-imm64/sign+zero ext via MOVSX/MOVZX), `LEA`, `PUSH`, `POP`, `XCHG`
- Arithmetic: `ADD`, `SUB`, `IMUL` (2/3-op forms), `IDIV`, `DIV`, `NEG`, `INC`, `DEC`, `CMP`
- Bitwise: `AND`, `OR`, `XOR`, `NOT`, `SHL`/`SHR`/`SAR`, `ROL`/`ROR`
- Control: `JMP` (rel32/rel8), `Jcc` (16 cond codes), `CALL` (rel32 + indirect), `RET`, `RET imm16`
- Comparison setup: `TEST`, `SETcc`
- Floating-point: SSE2 baseline (`MOVSS`/`MOVSD`/`ADDSS`/`SUBSS`/`MULSS`/`DIVSS` + double variants + `UCOMISS`/`UCOMISD`)
- AVX / AVX-512: **reserved post-v1**

**ARM64** (ARMv8-A ARM derived) — declared as `encoding` facets on rows in `arm64.target.json`:
- Data movement: `MOV` (reg/imm/extended-imm via MOVZ/MOVK/MOVN), `LDR`/`LDRB`/`LDRH`/`LDRSW`/`STR`/`STRB`/`STRH`, `LDP`/`STP` (pair), `LDUR`/`STUR` (unscaled)
- Arithmetic: `ADD`/`SUB` (reg/imm/shifted-reg/extended-reg), `MUL`, `MADD`, `MSUB`, `SDIV`, `UDIV`, `NEG`, `CMP`/`CMN`
- Bitwise: `AND`, `ORR`, `EOR`, `BIC`, `MVN`, `LSL`/`LSR`/`ASR`, `RBIT`, `CLZ`
- Control: `B` (cond + uncond), `BL`, `BR`, `BLR`, `RET`, `CBZ`/`CBNZ`, `TBZ`/`TBNZ`
- Comparison setup: `CSEL`, `CSINC`, `CSET`
- Floating-point: NEON scalar baseline (`FMOV`, `FADD`/`FSUB`/`FMUL`/`FDIV`, `FCMP`)
- SIMD vectors: **reserved post-v1**

Row-count estimate per architect-1's validation: ~40 LIR mnemonic rows on each target × 3–5 variants each = 120–200 effective encoding specifications per target. Variants live INSIDE a single mnemonic row (NOT as separate rows) so the LIR vocabulary stays stable (`"add"` stays one mnemonic; the assembler picks the right encoding variant at byte time). JSON files large but manageable; no row-template macro layer attempted (that would lean toward Turing-completeness — explicitly rejected per the bucket-1 "data, not script" discipline).

### 2.9 Disassembler round-trip oracle

Test-only. After encoding instruction `I` to bytes `B`, disassemble `B` and compare the mnemonic+operands to the original `I`. Catches encoder bugs that produce valid-but-wrong instructions.

Same format-dispatched shape as the encoder: one disassembler-per-format function (`x86_variable_disasm.cpp`, `arm32_fixed_disasm.cpp`). Reference outputs (objdump / llvm-mc) are TEST ORACLES only — golden hex snapshots live in fixtures; the production pipeline does not invoke objdump.

---

## 3. PR breakdown (rev 3)

| PR  | Title                                              | Scope |
|-----|----------------------------------------------------|-------|
| AS1 | Assembler skeleton + `assemble()` API + buffer + opaque `Relocation` tag + `A_*` diagnostic family | Public API per §2.2; `Relocation::kind` as opaque `uint32_t` (schema-declared); `AssembledModule`/`AssembledFunction` shape; SourceMapEntry-via-MirInstId; `A_*` codes at fresh nibble; ONE format-encoder dispatch table keyed on `encoding.format`. NO per-arch `.cpp` directories. |
| AS2 | `format/x86_variable.cpp` engine + `encoding` facet authored on x86_64.target.json | Universal x86-variable encoder: REX/VEX prefix arithmetic (REX.R/B from `hwEncoding>>3`), SIB forced-presence, ModR/M assembly, displacement + imm widths. Encoding rows for §2.8 x86_64 subset. Per-instruction hex roundtrip pin. |
| AS3 | `format/arm32_fixed.cpp` engine + `encoding` facet authored on arm64.target.json | Universal arm32-fixed encoder: fixed-word template, bit-field slot composition, `sf` width derivation. Encoding rows for §2.8 ARM64 subset. Per-instruction hex roundtrip pin. Lands alongside ML7 cycle 2 (ARM64 stackPointer + ABI goldens) per plan 12 §3.1 D-ML7-2.1. |
| AS4 | Relocation taxonomy facet + assembler/linker integration | New `relocations[]` array in `*.target.json` (opaque tag → formula). Object-format-specific reloc name → tag mapping lives in `*.format.json` (per plan 14). Pin each kind against a known-good encoded image. |
| AS5 | Format-dispatched disassembler harness + round-trip test | One `*_disasm.cpp` per format. Every AS2/AS3 instruction round-trips via disasm oracle. |
| AS6 | LIR → bytes pipeline integration                   | End-to-end c-subset corpus: HIR→MIR→LIR→bytes+relocations; linker (LK4) consumes correctly via shared `TargetSchema::relocationInfo`. |

Substrate tier (5-agent review) for AS1 (interface contract + API shape + diag family) and AS4 (relocation taxonomy facet — shared schema with plan 14, validation rules). Feature tier for AS2/AS3/AS5/AS6.

---

## 4. Open questions

| # | Question | Default if unanswered |
|---|----------|-----------------------|
| 1 | ~~Hand-written encoding tables vs. JSON-spec-driven?~~ | **Resolved rev 3 (2026-05-29):** JSON-spec-driven. Encoding rows live in `*.target.json`'s `encoding` facet (bucket-1); one format-encoder function per encoding-format tag (bucket-2). Rejected the "hand-written C++ for v1 / revisit if 3rd ISA lands" answer — the ML5 cycle 2a pivot showed the JSON-driven design is the cleaner shape from cycle 1, not after the second consumer. |
| 2 | Instruction-level peephole optimization at this layer? | **No** — peepholes live in LIR pre-encoding. Same with D-ML7-1.3 push/pop peephole (per plan 12 §3.1) — that's a separate AS1 cycle 1 fold. |
| 3 | Architecture extensions (AVX, NEON-SVE, ARMv8.x features)? | **Out of v1.** Reserve namespace. New extension = new encoding rows in the JSON (existing format encoder consumes unchanged). |
| 4 | x86_64 syntax in disassembler test fixtures: AT&T or Intel? | **Intel** — matches Intel SDM (the encoding-row reference). |
| 5 | Endianness assumption? | **Little** for v1 (both ISAs LE). Big-endian (PowerPC, MIPS-BE) reserved — would need a `byteOrder` field on the encoding format. |
| 6 | Instruction scheduling at this layer? | **No** — scheduling lives in LIR pre-encoding (or deferred entirely; v1 emits in LIR-given order). |
| 7 | Inline-assembly support (custom-language `__asm__` blocks)? | **Out of v1.** No shipped language uses it. |
| 8 | Position-independent code emission? | **Yes** — required for ELF `.so` / Mach-O dylibs. Default to PIC; `-fno-pic` reserved post-v1. |
| 9 | Migration of WASM/SPIR-V into the assembler — same `assemble()` entrypoint or separate? | **Same entrypoint, separate format-encoders** per architect-1's bucket-2 carve-out. WASM's structured-CF and SPIR-V's result-id minting are bucket-2 algorithms with non-trivial state — they belong in `format/wasm_leb.cpp` and `format/spirv_word.cpp`, owned by plans 18 / 17 respectively. The top-level `assemble()` dispatches to them via the encoding-format tag, same as x86/ARM. |

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
| ModR/M / SIB / REX prefix interactions on x86_64 are intricate | High | High | Encoding rows declare the structural fields in JSON; the C++ encoder owns ONE named convention per arithmetic rule (REX.R/B from `hwEncoding>>3`, SIB forced when `ModR/M.rm==4`, etc. — see §2.4 honest carve-outs). Review against SDM Vol 2 verbatim per row. |
| Missing relocations break linkage silently | High | High | Single shared `relocations[]` facet on `*.target.json` consumed by both assembler AND linker — no two-side enum to drift. AS4 is still its own PR with linker-integration pins. |
| ~~Encoding-table maintenance burden as ISA extensions land~~ | Low | Low | **Resolved rev 3:** rows are JSON, appending an AVX/NEON-SVE row is config-only — no engine edit, no recompile. |
| Encoding row schema overstretching (architect-1 carve-out) | Medium | Medium | Bucket-2 boundary cases (WASM structured-CF, SPIR-V result-id minting) are explicit per §2.4 — they get their own format-encoder functions in plans 17/18, not row-encoded in the x86/ARM JSON facet. Document the encoding-format dispatch boundary cleanly so future extensions don't accidentally try to encode bucket-3 behavior as bucket-1 data. |
| Cross-arena `LirInstId` identity drift between pre-ML7 and post-ML7 modules | Medium | High | **SourceMapEntry carries `MirInstId`, not `LirInstId`** (architect-2 validation). Assembler receives `lirToMir` as a parameter and composes through it at encode time — the debug-info chain becomes `byte_offset → MirInstId → HirSourceLoc`, single arena per hop. |

---

## 7. Sequencing

```
12-mir-lir (ML8) ─► AS1 ─► AS2 (x86_64) ─┐
                           AS3 (ARM64)  ─┴─► AS4 ─► AS5 ─► AS6 ─► 14-linker
```

AS2 / AS3 are parallel after AS1. AS5 (round-trip oracle) gates AS6.
