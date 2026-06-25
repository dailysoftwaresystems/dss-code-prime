# Cluster F F5 — string-index + string/symbol-address global emission (build blueprint)

**Status:** DESIGN VETTED (architect 2026-06-25, cycle 9). User chose "both this cycle". Fork resolved → **Option A (in-place exec patch, exec-only)**; `.rela.data` for a `.o` writer is anchored future (`D-LK-DOTO-RELA-DATA`, DSS emits exec directly so out of scope). Scratch doc — delete on cycle close.

## The two features
1. **String-literal index** `"abc"[i]` — contained. `H0009` at hir_to_mir.cpp:~2408 (lowerLvalueAddressNode) — string-literal (kind 23) Index base unhandled. `char* s="x"; s[i]` already works.
2. **Symbol-address GLOBAL** `char* g="..."`, `int* p=&x` — NEW MECHANISM. Both hit `initFn.valid()` runtime-init deferral at asm.cpp:~746 (`D-LK4-RODATA-PRODUCER-RUNTIME-INIT`). They're LINK-TIME constants → emit 8-byte slot + abs64 symbol-address reloc → linker resolves.

## KEY FINDING — abs64 reloc ALREADY EXISTS (no new reloc type)
x86_64.target.json:1797 (kind 2) · arm64.target.json:1875 (kind 4) · PE IMAGE_REL_AMD64_ADDR64 (nativeId 1) · ELF R_X86_64_64 / R_AARCH64_ABS64 · Mach-O X86_64/ARM64_RELOC_UNSIGNED. Discoverable by FORMULA (`widthBytes==8 && !pcRelative`) — linker.cpp:196-199 already does this scan for cross-CU thunks. So: find by formula (NOT name) = agnostic.

## Mechanism (narrowest design — no MirGlobal POD resize)
- **New `MirSymbolAddrValue {SymbolId symbol; int64 addend;}`** arm in `MirLiteralValue` variant (mir_literal_pool.hpp). A symbol-addr global stores `initLiteralIndex` = pool idx of this arm. POD unchanged (it's in the pool, not MirGlobal).
- **classifyGlobals (hir_to_mir.cpp:~6685)**: `tryClassifyAsSymbolAddr(initNode)` recognizes `AddressOf(Ref(global))` and `Cast(Literal(string),Ptr<Char>)` BEFORE evaluateConstant → sets `pg.symbolAddrInit` (not `pg.runtimeInit`). String case mints the rodata global's SymbolId during classify (mintSyntheticGlobalSymbol is lazy-seeded after collect*, so safe). emitGlobals_ processes the rodata global first, then the pointer global.
- **lowerMirGlobalsToDataItems (asm.cpp:~815)**: new `MirSymbolAddrValue` dispatch arm → `bytes.assign(8,0)` + one `Relocation{offset 0, target sym, kind abs64-by-formula, addend}`. Needs `TargetSchema const&` threaded into the signature (currently absent) + `findAbsolutePtrRelocKind(TargetSchema)` helper.
- **Data-item reloc applier**: extract PE's inline patch loop (pe.cpp:977-1043) → shared `applyDataItemRelocations(...)` in exec_data_section.hpp. ELF/Mach-O currently REJECT data-item relocs (`D-LK1-ELF-RODATA-DATAITEM-RELOC`, exec_data_section.hpp:116-125) → lift for abs64 + call the applier. In-place patch (exec image has resolved VAs), no `.rela.*`.
- **PE .data gate (THE likely-bug spot)**: PE's data-item patch loop is gated on `rdataOffsetByIndex` (`.rdata` only, pe.cpp:984-992). A `char* g` lives in `.data` → must extend with `dataOffsetByIndex` + the `.reloc` (baseRelocSiteRvas) ASLR entry for .data items too.
- **String-index (hir_to_mir.cpp:~2134)**: `HirKind::Literal` arm in lowerLvalueAddressNode → `materializeStringLiteralGlobal(HirNodeId)` helper (factor lines 916-931, the existing Cast-arm rodata materialization) → GlobalAddr. Reused by BOTH features.

## Staged build (each stage independently testable)
1. **String index** ✅ **DONE (commit pending, 404/404)** — materializeStringLiteralGlobal helper (factored from the Cast decay arm — one producer) + HirKind::Literal arm in lowerLvalueAddressNode + `agg_string_index` corpus (exit 215 x86 native + arm64 qemu). Red-on-disable: remove the arm → H0009.
2. **MirSymbolAddrValue variant** + classifyGlobals recognition + emitGlobals_ + mir_text round-trip. (Hits a controlled fail-loud at asm until stage 3.)
3. **Assembler dispatch** — thread TargetSchema into lowerMirGlobalsToDataItems + findAbsolutePtrRelocKind + the MirSymbolAddrValue arm. PE works end-to-end (its data-reloc loop exists, after the .data-gate extension). Test `decl_string_global` on x86 PE.
4. **ELF data-item relocs** — extract applyDataItemRelocations shared helper (PE refactored to use it), lift D-LK1, wire encodeElfStatic + encodeElfExecDynamic. Test arm64 qemu (ELF).
5. **Mach-O data-item relocs** — same pattern in macho.cpp encodeExec __data/__const. (macOS CI leg.)
6. **Anchors + corpus finalize** — anchor char-array form (`char buf[]="hi"`, S000B) as `D-CSUBSET-GLOBAL-CHAR-ARRAY-INIT`; release arms; full ctest.

## PLAN-LOCK before stages 2-6 (the reloc mechanism is the new/risky cross-format part; stage 1 is contained, no heavy lock). Verify: abs64-by-formula agnostic; classifyGlobals ordering; PE .data gate; ELF/Mach-O applier + D-LK1 lift; the corpus runtime-witnesses the reloc (exit code = ground truth, NOT a byte pin).

## Corpora (runtime witnesses — relocs resolve at link, so exit-code is the proof)
- `agg_string_index`: `"hello"[i] + "world"[2]` → exit (101+114)%256=215.
- `decl_string_global`: `static int target=42; static int* p=&target; static const char* msg="hello"; return (*p + msg[1])%256` → exit (42+101)%256=143. Red-on-disable: remove the MirSymbolAddrValue asm arm → K_NoMatchingObjectFormat.
- Both: x86 PE + x86 ELF + arm64 ELF (qemu) + arm64 Mach-O (CI), release arm.
