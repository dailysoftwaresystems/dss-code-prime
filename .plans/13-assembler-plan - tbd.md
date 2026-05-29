# In-tree Assembler — Sub-Plan (13)

> Owns instruction encoding for v1 ISAs. Translates [LIR](./12-mir-lir-plan%20-%20ok.md) (post-regalloc, post-frame-materialization, physical-register-only) into raw machine bytes + relocation records consumed by the [in-tree linker](./14-linker-plan%20-%20tbd.md). Per the hermetic-compiler invariant, **no GAS / MASM / llvm-mc invocation** — we own every byte.
>
> **Rev 3 (2026-05-29) — config-driven encoding, mirroring the ML5 cycle 2a pivot.** Per [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) Decision #4's three-bucket rule (canonicalized 2026-05-29): encoding rows are JSON-declared (bucket 1) on the per-target schema's `encoding` facet; the assembler is **shape-keyed dispatch** to one specialized format-encoder per encoding shape (bucket 2 — same pattern as `ChildLower` in the frontend). NO per-arch `.cpp` directories. Rev 1/2's `src/asm/x86_64/` + `src/asm/arm64/` per-arch trees are deleted. Validated by 2-architect parallel pass before this rev landed (see §2.4 honest carve-outs).
>
> **The headline test** (honest version, matches §2.4): adding a new target is **JSON if its encoding-format already has an encoder; else JSON + one new format-encoder function**. ARM64 reuses `fixed32` (the format ARM64 already uses) → pure JSON drop, zero C++. RISC-V 32-bit (RV32) reuses `fixed32` with different bit-field row data → pure JSON drop. A novel VLIW/DSP shape with bundle-encoded instructions → JSON + one new bucket-2 walker (`vliw_bundle.cpp`). Both are bucket-2 by the operative test (shape-keyed dispatch, never identity-keyed) — but the JSON-only case is the cheap one we should size the architecture for.

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
├── format/                 # ONE bucket-2 walker per encoding-shape tag (NOT per arch)
│   ├── x86_variable.cpp    # x86-variable: prefix chain + ModR/M + SIB + imm
│   ├── fixed32.cpp         # fixed32: 32-bit fixed-word + bit-field slice composition (ARM64 + future RV32 + any future 32-bit-fixed ISA — name is shape-keyed, not arch-keyed)
│   └── (vliw_bundle.cpp / variable_long.cpp etc. deferred until a target needs them)
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
- **One format-encoder function per encoding-shape tag** (`x86_variable.cpp`, `fixed32.cpp`) — shape-keyed dispatch (NOT arch-keyed; same discipline as the frontend's `ChildLower` enum). Adding ARM64 = drop arm64.target.json declaring `format: "fixed32"` (no new walker — `fixed32` already exists). Adding RV32 = drop riscv32.target.json also declaring `format: "fixed32"` (no new walker — the bit-field row data differs, the walker doesn't). Adding a genuinely novel encoding shape (VLIW bundles, RISC-V compressed 16-bit) = JSON + one new walker keyed on a new shape tag.

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

### 2.4 Shape-keyed dispatch (bucket-2 done right, architect-validated 2026-05-29)

The "one universal encoder" framing in earlier drafts overstated uniformity. The honest shape per the 2-architect validation pass is **shape-keyed dispatch over a closed encoding-format vocabulary**: each `encoding.format` tag in the JSON maps to one specialized walker. Mirrors the frontend's `ChildLower` discipline (`src/core/types/hir_lowering_config.hpp` — a closed enum of lowering verbs, one walker per verb, JSON declares which verb to use). The walkers ARE specialized — x86's variable-length ModR/M-driven shape and the 32-bit fixed-word bit-field-driven shape are genuinely different algorithms — but they are **shape-keyed (closed vocabulary), not identity-keyed (open per-target)**. New target = JSON if its shape exists, JSON + one walker if a new shape is needed. Same bucket-2 pattern, no Decision #4 dilution.

**Structural rules each format walker must own (unconditional engine conventions, NOT arch branches):**

- **x86_64 REX prefix extension bits** (`REX.R` / `REX.B`) — derived from operand `hwEncoding >> 3`, not an arch branch. `x86_variable.cpp` always computes these from the register table.
- **x86_64 SIB byte forced presence** when `ModR/M.rm == 4` — architectural rule of the encoding shape, not arch identity.
- **x86_64 VEX/EVEX prefix field arithmetic** (for SSE/AVX) — the `vvvv` field is the bit-complement of the source register's hwEncoding. Mechanical derivation.
- **`fixed32` `sf` flag** (64-bit operand selector on AArch64 instructions) — derived from the result register's width. Same walker handles RV32-style fixed-word encodings without a branch — the bit-field row data differs, the walker doesn't.

These rules live in the format walker as named functions (`encodeRexPrefix(reg, ...)`, `forcedSibByte(modrm)`, `derivedSizeFlag(resultWidth)`). They are bucket-2 by the operative test: zero identity branches.

**Structured-bytecode targets are NOT in plan 13.** WASM (block-scoped stack VM, structured CF, value stack — not registers) and SPIR-V (typed value stream with result-id minting, shader-bound) consume MIR DIRECTLY (per their plans 18 + 17) via MIR→structured-bytecode walkers — they bypass LIR entirely. The earlier draft's "WASM/SPIR-V are bucket-2 carve-outs in plan 13" framing was wrong: those targets aren't native-ISA byte-encoders at all, they're a different downstream-of-MIR family with their own pivot path. The MIR optimizer (gen-optimizer step 11) runs upstream of all four target classes (native ISAs via LIR, WASM via plan 18, SPIR-V via plan 17, future bytecode VMs).

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
    "format": "fixed32",
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

**Ownership boundary** (cross-referenced from [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) §2.0, pinned so AS↔LK don't diverge at implementation time):

| Schema | Owns | Consumed by |
|---|---|---|
| `*.target.json` `relocations[]` | The **formula+tag**: opaque `uint32_t tag → { isPCRelative, width, addendWidth, ... }` | Assembler emits the tag; linker applies the formula |
| `*.format.json` `relocations[]` | The **format-name → tag** mapping: `"R_X86_64_PC32" → tag 1`, `"IMAGE_REL_AMD64_REL32" → tag 1`, etc. | Linker uses the format name when writing the object file's reloc table |

Same opaque `uint32_t` tag joins both sides. Neither schema duplicates the other's data. AS4 (plan 13) authors the formula+tag table on the target schema; LK6 (plan 14 — reloc-apply integration) authors the format-name→tag mapping on each format schema. Cross-PR review pin: AS4 and LK6 land in the same review window so the integer assignments don't drift.

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

Same shape-keyed dispatch as the encoder: one disassembler-per-shape function (`x86_variable_disasm.cpp`, `fixed32_disasm.cpp`). Reference outputs (objdump / llvm-mc) are TEST ORACLES only — golden hex snapshots live in fixtures; the production pipeline does not invoke objdump.

---

## 3. PR breakdown (rev 3)

| PR  | Title                                              | Scope |
|-----|----------------------------------------------------|-------|
| ~~AS1~~ ✅ **landed 2026-05-29** | Assembler skeleton + `assemble()` API + buffer + opaque `Relocation` tag + `A_*` diagnostic family | **Done.** `src/asm/asm.{hpp,cpp}` ships `assemble(Lir, TargetSchema, lirToMir, reporter) → AssembledModule`. New types: `AssembledModule` (parallel-index + derived `ok()` via `expectedFuncCount` field — matches `LirAllocation::ok()` cycle-3a discipline pioneered ML6/adopted ML7), `AssembledFunction` (carries `SymbolId` so the linker doesn't re-consult `Lir`), `Relocation` (opaque `uint32_t kind`; `offset:uint32_t` width-matched to `SourceMapEntry::byteOffset`), `SourceMapEntry` (`MirInstId` per the architect-2 review). New `TargetEncodingShape` enum (`None`/`X86Variable`/`Fixed32` — closed shape-keyed vocabulary, §2.4 discipline) + optional `TargetOpcodeInfo::encodingShape` facet (JSON `encoding.format`; `format` REQUIRED when `encoding` block present — closes silent-typo gap). New `TargetRelocationInfo` + `relocations[]` + `relocationNameIndex` + `relocationKindIndex` (O(1) linker hot-path lookup). `A_*` family at `0x1xxx` (3 codes: `A_NoEncodingDeclared`, `A_NoEncodingShapeWalker`, `A_LirToMirSizeMismatch`) + plan 00 §0.3 registry update + parse_diagnostic.cpp nibble switch. `validate()` rejects: dup `kind`, zero `kind`, empty `name`, dup `name`. `lirToMir` size-mismatch fail-loud at entry. Format-walker arms (X86Variable / Fixed32) currently fire `A_NoEncodingShapeWalker` — AS2/AS3 plug in. NO per-arch `.cpp` directories. **7-agent review fix-up folded inline**: `ok()` shape-check fix (3-agent convergence: type-design + silent-failure + architect), Relocation.offset width unification, AssembledFunction.symbol field, lirToMir bounds check, encoding-block-without-format fail-loud, formula type-strict, abiModel/regClass fromName helpers for symmetry, encodeInst enum-drift fallback diagnostic, comment cross-plan attribution fix. 22 AS tests + 101/101 ctest. |
| ~~AS2~~ ✅ **cycle 2 landed 2026-05-29 (substrate slice)** | `format/x86_variable.cpp` engine + `encoding` facet authored on x86_64.target.json | **Substrate vertical slice done.** New `src/asm/format/x86_variable.{hpp,cpp}` — REX prefix arithmetic (W/R/B/X bits derived from operand `hwEncoding`'s bit 3 — NO arch identity branches), opcode-byte emission, ModR/M assembly (mod=3 register-direct), little-endian immediate emission. Pure free function; wired into `asm.cpp` dispatch's `TargetEncodingShape::X86Variable` arm. New encoding-facet types in `target_schema.hpp`: `TargetEncodingInfo` (replaces cycle-1's `encodingShape` field with structured `{shape, variants[]}`), `TargetEncodingVariant{operandKinds, tmpl, resultSlot, wires}`, `TargetEncodingTemplate{rexW, opcodeBytes, modrmRegExt}`, `TargetEncodingWire{index, slotKind}`, closed enums `OperandKindFilter{Reg, ImmInt}` + `EncodingSlotKind{ModRmReg, ModRmRm, Imm32}`. JSON loader extension parses `variants[]`/`template`/`wires`. `A_NoMatchingEncodingVariant` (0x1004) diagnostic family expansion. x86_64.target.json authored encoding for **3 opcodes**: `ret` (0xC3 no-operand), `mov reg-reg` (REX.W 0x8B /r), `mov reg-imm32` (REX.W 0xC7 /0 imm32). Binary ops + memory addressing deferred to future cycles (require 2-address legalization + SIB substrate — anchored in plan 12 §3.1 / plan 13 §3 AS-cycle-3+). **7-agent review fold-in (inline) — multi-agent convergences**: (A) walker silent slot-overwrite hole (4-agent: code-reviewer + silent-failure + type-design + simplifier) — `EncodingState` now tracks `wroteModRmReg`/`wroteModRmRm`; second writer fails loud + validate-time rule; (B) `modrmRegExt` + `ModRmReg`-wire silently drops register (3-agent: code-reviewer + silent-failure + type-design) — validate() rejects the combination; (C) guard-vs-wire arity coverage (3-agent: code-reviewer + silent-failure + pr-test) — validate() rejects uncovered guard positions; (D) overlapping variant guards silent first-match-win (2-agent: silent-failure + architect) — validate() rejects same-operandKinds variants; (E) "variant mismatch" test didn't actually exercise kind-mismatch (2-agent: pr-test + comment-analyzer) — rewritten with proper [reg]-vs-[imm32] kind-mismatch fixture. **Single-agent HIGH folded**: `wireSlot` enum-drift fallback; `result != None` requires destination-slot validate rule; `OperandKindFilter::Imm32` → `ImmInt` rename (matches `LirOperandKind::ImmInt`); `TargetEncodingVariant::operands` → `wires` rename; REX-suppression test; multi-byte opcode test; `modrmRegExt=7` test; virtual-reg fail-loud test; docstring cycle-qualifier; validate-rule comment correction; ret JSON $comment reword. **Deferred items ALL closed**: (1) 6-enum `Name`/`FromName` cascade collapsed via new `EnumNameTable<E,N>` substrate template — single source of truth for 8 closed enums (TargetAbiModel/CondCode/ResultRule/RegClass/TerminatorKind/EncodingShape + OperandKindFilter/EncodingSlotKind); (2) `EncodingState::rexX` field declared + threaded through REX-byte assembly (no consumer until SIB lands but substrate is structurally complete); (3) preexisting `enforceRefs` dead-bool tautology removed. 25 AS x86_variable tests + 26 AS substrate tests + 11 validate-rule tests + 102/102 ctest. |
| ~~AS3~~ ✅ **cycle 3 landed 2026-05-29 (substrate slice + binary ops + D-ML7-2.1)** | `format/fixed32.cpp` walker + `encoding` facet authored on arm64.target.json | **Four coupled deliverables in one cycle.** (1) `src/lir/lir_2addr_legalize.{hpp,cpp}` — post-regalloc rewrite pass that inserts implicit `mov result, operands[0]` when `requires2Address=true` and `result != operands[0]`. Target-blind; per-opcode `requires2Address` is the only signal. (2) x86_64 binary ops: `add` (REX.W 0x01 /r), `sub` (REX.W 0x29 /r), `mul` (REX.W 0x0F 0xAF /r — INVERTED operand mapping, dest→modrm.reg) — encoded end-to-end through legalize + assemble pipeline. (3) `src/asm/format/fixed32.{hpp,cpp}` walker — bit-field-slot encoding (5-bit Rd/Rn/Rm fields at bits 0..4 / 5..9 / 16..20) into a 32-bit fixed word, LE byte output. Pure free function; wired into `asm.cpp` `TargetEncodingShape::Fixed32` arm. (4) **D-ML7-2.1 ARM64 target schema** — new `src/dss-config/targets/arm64.target.json`: AAPCS64 calling convention (sp + lr + x0..x30 + xzr + 8 GPR arg regs + callee-saved x19..x28), 3 encoded opcodes (`ret`=0xD65F03C0, `mov` reg-reg via ORR-alias=0xAA0003E0 base + Rd + Rm, `unreachable`=0xD4200000). Schema additions: `TargetEncodingTemplate::fixedWord:uint32_t`, `EncodingSlotKind::Rd/Rn/Rm`, `TargetOpcodeInfo::requires2Address:bool`, `slotShapeFor()` constexpr classifier. validate() rules expanded: shape-vs-slot cross-check, fixed32-specific opcodeBytes/modrmRegExt rejection, result-routing rule G with `requires2Address` destination-bearing-slot exception, schema-level "requires2Address opcodes require `mov` declaration." **7-agent review fold-in**: 5-agent convergence on legalize silent-continue when `mov` missing (A) — pass tracks `expectedFuncCount` + `allFunctionsLegalized`; ok() pattern aligned with `LirCallconvResult` (B); 4-agent convergence on rule G admitting wires to non-destination slots (C) — restricted to ModRmReg/ModRmRm/Rd; 3-agent convergence on fixed32 slot-uniqueness validate (D) — extended to Rd/Rn/Rm; single-agent HIGH (E) legalize hard-diag on non-Reg op[0]; (F) requires2Address rejected on result=none / minOperands=0 / non-Reg operandKinds[0]; (G) `wroteSlot` enum-derived size via `kEncodingSlotKindTable.rows.size()`; (H) `windowFor` enum-drift fallback note; (J) `result_reg` → `result` naming; (K) `enforceRefs` dead comment removed; (N) doc fixes (fixedWord all-zeros sentinel note, fixed32 immediate-rejection clarification, Rm added to slot enumeration). 20 new tests (5 legalize + 4 x86 binary-op pins + 8 arm64 schema/encoder/validate + 3 new requires2Address rejection tests). 105/105 ctest. |
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
| 9 | ~~Migration of WASM/SPIR-V into the assembler~~ | **Resolved 2026-05-29: NOT in plan 13.** WASM (typed value stack, structured-CF, module sections) and SPIR-V (typed value stream, shader-bound, result-id minting) are **MIR-downstream targets bypassing LIR entirely** (per [`18-wasm-plan`](./18-wasm-plan%20-%20tbd.md) + [`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md)). The assembler tier (plan 13) handles native ISAs only — instruction encoding from a register-allocated LIR. WASM/SPIR-V never produce LIR. |

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
