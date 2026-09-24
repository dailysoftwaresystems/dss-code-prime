# Assembler unit tests (plan 13 AS1).

dss_add_test(NAME asm/test_asm_substrate     SOURCES test_asm_substrate.cpp)
dss_add_test(NAME asm/test_asm_x86_variable  SOURCES test_asm_x86_variable.cpp)
dss_add_test(NAME asm/test_asm_x86_binaryops SOURCES test_asm_x86_binaryops.cpp)
dss_add_test(NAME asm/test_asm_x86_sse       SOURCES test_asm_x86_sse.cpp)
# FC2 Part A⇄B integration: source-level cast → SSE bytes.
dss_add_test(NAME asm/test_asm_cast_integration SOURCES test_asm_cast_integration.cpp)
dss_add_test(NAME asm/test_asm_x86_relocs    SOURCES test_asm_x86_relocs.cpp)
dss_add_test(NAME asm/test_asm_arm64         SOURCES test_asm_arm64.cpp)
dss_add_test(NAME asm/test_asm_arm64_binops  SOURCES test_asm_arm64_binops.cpp)
dss_add_test(NAME asm/test_lir_2addr_legalize SOURCES test_lir_2addr_legalize.cpp)
dss_add_test(NAME asm/test_asm_roundtrip      SOURCES test_asm_roundtrip.cpp)
dss_add_test(NAME asm/test_asm_pipeline       SOURCES test_asm_pipeline.cpp)
dss_add_test(NAME asm/test_lk10_entry_slice_a SOURCES test_lk10_entry_slice_a.cpp)
# FC3 c2 (D-CSUBSET-32BIT-ALU-FORMS): width-axis byte pins — x86 32-bit
# no-REX.W forms + arm64 W-forms + sext/trunc + loader width validation
# + the strip-a-variant red-on-disable (mutation-substrate consumer).
dss_add_test(NAME asm/test_asm_width_axis     SOURCES test_asm_width_axis.cpp)
# TLS C1 (D-CSUBSET-THREAD-LOCAL): tlsbase / tls-lea byte pins + the
# tls identity + tlsAccess schema pins + loader/validate fail-louds.
dss_add_test(NAME asm/test_asm_x86_tls        SOURCES test_asm_x86_tls.cpp)
# D-ASM-ARM64-NEGATIVE-IMMEDIATE-UNENCODABLE: the MOVN red-on-disable
# pins mutate the shipped arm64 JSON in memory (nlohmann consumer) —
# stripping a variant / a width key and proving the witness disappears.
target_link_libraries(dss_asm_test_asm_arm64
    PRIVATE nlohmann_json::nlohmann_json)

# TLS C2 (D-CSUBSET-THREAD-LOCAL): the arm64 twin — MRS TPIDR_EL0 +
# ADD/ADD hi12/lo12 word pins + the Variant-I identity / tprel-row /
# tlsAccess / TLSLE-native-id schema pins.
dss_add_test(NAME asm/test_asm_arm64_tls      SOURCES test_asm_arm64_tls.cpp)
# D-ASM-ARM64-BITWISE-W-FORMS-UNDECLARED + D-ARM64-C-PATH-32BIT-BITWISE-WIDENS-TO-64:
# byte-exact AND/ORR/EOR W-form and X-form pins (measured against
# aarch64-linux-gnu-as 2.42), the C-path width-32 election pin, and the
# strip-the-W-variant red-on-disable — which mutates the shipped arm64 JSON in
# memory (nlohmann consumer), the `mutate_target_schema.hpp` posture.
dss_add_test(NAME asm/test_asm_arm64_bitwise_w
             SOURCES test_asm_arm64_bitwise_w.cpp)
target_link_libraries(dss_asm_test_asm_arm64_bitwise_w
    PRIVATE nlohmann_json::nlohmann_json)
# D-ASM-ARM64-MOVZ-W-FORM-UNDECLARED: the same treatment for the IMMEDIATE move
# — byte-exact MOVZ W/X word pins (measured against aarch64-linux-gnu-as 2.42),
# the C-path width-32 election pin, and TWO red-on-disable mutants: strip the
# W arm (loud refusal, empty output) and reconstruct the PRE-FIX schema (real
# bytes, the OLD word). Same in-memory `mutate_target_schema.hpp` posture.
# ⚠ ITS MUTANTS ARE CONFIG-LEVEL, so it MUST run through ctest — `dss_add_test`
# sets DSS_CONFIG_ROOT, while a bare .exe walks the cwd and would read whichever
# tree the shell stands in.
dss_add_test(NAME asm/test_asm_arm64_movz_w
             SOURCES test_asm_arm64_movz_w.cpp)
target_link_libraries(dss_asm_test_asm_arm64_movz_w
    PRIVATE nlohmann_json::nlohmann_json)
# Plan 29 P4: the standalone assembly TEXT -> LIR walker. Its fixture builds a
# dialect grammar by mutating the shipped `asm-x86_64-att.lang.json` in memory
# (nlohmann consumer), the same posture `mutate_target_schema.hpp` takes for
# targets -- so every test stays anchored to the real grammar and token table.
dss_add_test(NAME asm/test_asm_text_to_lir    SOURCES test_asm_text_to_lir.cpp)
target_link_libraries(dss_asm_test_asm_text_to_lir
    PRIVATE nlohmann_json::nlohmann_json)

# Plan 29 P5: the EMBEDDED template path (`lowerAsmTemplateToLirRun`) — the
# SECOND caller of the one text->LIR engine. Drives the SHIPPED dialects and
# the SHIPPED targets unmutated, and pins BYTES rather than opcodes, because a
# template that elected the right opcode and the wrong encoding variant is the
# silent-miscompile class this subsystem exists to refuse.
# WARNING: its byte pins are config-level (they read the shipped `.lang.json`
# and `.target.json`), so it MUST run through ctest -- `dss_add_test` sets
# DSS_CONFIG_ROOT, while a bare .exe walks the cwd and would read whichever
# tree the shell stands in.
dss_add_test(NAME asm/test_asm_template_to_lir
             SOURCES test_asm_template_to_lir.cpp)
target_link_libraries(dss_asm_test_asm_template_to_lir
    PRIVATE nlohmann_json::nlohmann_json)

# The SHIPPED dialect documents, UNMUTATED. Its sibling above replaces
# `assembly.directives` with the rows each test needs, which is right for
# testing the ENGINE but leaves every one of those tests green whether or not
# the two shipped documents declare the rows at all. This file is the other
# half: it drives `asm-x86_64-att.lang.json` and `asm-arm64-gas.lang.json` as
# they ship, and it is the host-independent guard beside the
# `*_section_reserve_fill_readback` corpus examples, whose arm64 arm only runs
# where an emulator is installed.
dss_add_test(NAME asm/test_asm_shipped_dialects
             SOURCES test_asm_shipped_dialects.cpp)
target_link_libraries(dss_asm_test_asm_shipped_dialects
    PRIVATE nlohmann_json::nlohmann_json)

# D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED: the `.s` CALL-FRAME producer. Runs
# BOTH shipped dialects UNMUTATED, because the defect this closes existed on
# x86_64 (accepted-and-dropped) and did NOT exist on arm64 (the spellings were
# absent, so `.cfi_startproc` failed loud) — a one-port test would cover half
# the fix. Drives the whole chain lower -> assemble() -> attachAssemblyCfi ->
# buildEhFrame, so the byte offsets are the encoder's MEASURED ones and the
# DWARF opcodes are asserted on every leg, including hosts that can execute
# neither corpus binary.
# ⚠ ITS PINS ARE CONFIG-LEVEL (they read the shipped `.lang.json` and
# `.target.json`), so it MUST run through ctest — `dss_add_test` sets
# DSS_CONFIG_ROOT, while a bare .exe walks the cwd and would read whichever tree
# the shell stands in.
dss_add_test(NAME asm/test_asm_cfi_producer
             SOURCES test_asm_cfi_producer.cpp)
target_link_libraries(dss_asm_test_asm_cfi_producer
    PRIVATE nlohmann_json::nlohmann_json)

# The ONE numeric-local-label resolver (`asm/asm_local_labels.hpp`) that a `.s`
# and an inline-asm template both ask — pinned once, on its own
# (D-ASM-LABELS-INSIDE-A-TEMPLATE-AND-NUMERIC-LOCAL-LABELS-REFUSED).
dss_add_test(NAME asm/test_asm_local_labels
             SOURCES test_asm_local_labels.cpp)

# Numeric labels, template labels and the template text forms through the
# SHIPPED dialects and targets — the refusals, the shipped declarations and the
# loader's refusal of every half-declaration. ⚠ CONFIG-LEVEL (it reads and
# mutates the shipped `.lang.json` / `.target.json`), so it MUST run through
# ctest, which points DSS_CONFIG_ROOT at the run's config snapshot.
dss_add_test(NAME asm/test_asm_numeric_labels_and_forms
             SOURCES test_asm_numeric_labels_and_forms.cpp)
target_link_libraries(dss_asm_test_asm_numeric_labels_and_forms
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_numeric_labels_and_forms
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# arm64's exclusive pair (`ldaxr` / `stlxr`) written in assembly text, and the
# target's `resultEarlyClobber` that refuses the UNPREDICTABLE overlap at every
# tier — the builder's early mark, the text lowering and the encoder backstop.
# ⚠ CONFIG-LEVEL, as above.
dss_add_test(NAME asm/test_asm_arm64_exclusive_pair
             SOURCES test_asm_arm64_exclusive_pair.cpp)
target_link_libraries(dss_asm_test_asm_arm64_exclusive_pair
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_arm64_exclusive_pair
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# The `.s` walker's BLOCK PLAN: every block of a function created in TEXT order,
# the conditional branch's false edge laid out next, and a line after a
# terminator given a block where the text put it
# (D-ASM-CONDITIONAL-BRANCH-FALLTHROUGH-LAID-OUT-AT-THE-FUNCTION-END,
# D-ASM-INSTRUCTION-AFTER-A-TERMINATOR-REFUSED). ⚠ CONFIG-LEVEL, as above; it
# also reads the numeric-local-labels corpus sources.
dss_add_test(NAME asm/test_asm_block_plan
             SOURCES test_asm_block_plan.cpp)
target_link_libraries(dss_asm_test_asm_block_plan
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_block_plan
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# Tests under tests/asm/ share `asm_test_support.hpp` via this
# include path. test_asm_substrate additionally reuses the LIR
# lowering fixture (lowered_lir_fixture.hpp lives in tests/lir/).
foreach(t test_asm_substrate test_asm_x86_variable test_asm_x86_binaryops
          test_asm_x86_sse test_asm_x86_relocs test_asm_arm64
          test_asm_arm64_binops test_lir_2addr_legalize test_asm_roundtrip
          test_asm_pipeline test_asm_width_axis test_asm_x86_tls
          test_asm_arm64_tls test_asm_text_to_lir test_asm_shipped_dialects
          test_asm_arm64_bitwise_w test_asm_arm64_movz_w
          test_asm_template_to_lir test_asm_cfi_producer)
    target_include_directories(dss_asm_${t}
        PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)
endforeach()
target_include_directories(dss_asm_test_asm_substrate
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/lir)
# FC2 Part B: the registerClassOps schema pins mutate the shipped
# x86_64 JSON via mutate_target_schema.hpp (nlohmann consumer).
target_link_libraries(dss_asm_test_asm_substrate
    PRIVATE nlohmann_json::nlohmann_json)
# FC3 c2: the width-axis red-on-disable + loader pins mutate the
# shipped JSON via the same substrate.
target_link_libraries(dss_asm_test_asm_width_axis
    PRIVATE nlohmann_json::nlohmann_json)
# TLS C1: the tlsAccess negative pins mutate the shipped elf-exec
# format JSON in memory (nlohmann consumer).
target_link_libraries(dss_asm_test_asm_x86_tls
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_pipeline
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/lir)

# D-TARGET-X86-64-DECLARES-NO-MEMORY-DESTINATION-ARITHMETIC +
# D-ASM-SUB-NATIVE-OPERAND-UNUSABLE-IN-INLINE-ASM: the byte tier for arithmetic
# whose operand is MEMORY (both directions) and for the `movb`/`movw` sub-native
# moves. Drives the SHIPPED dialect and the SHIPPED target UNMUTATED, and every
# memory-form expectation is GNU as 2.42's own measured output.
# ⚠ ITS RED-ON-DISABLE MUTANTS ARE CONFIG-LEVEL, so it MUST run through ctest —
# `dss_add_test` sets DSS_CONFIG_ROOT, while a bare .exe walks the cwd and would
# read whichever tree the shell stands in.
dss_add_test(NAME asm/test_asm_x86_memory_arithmetic
             SOURCES test_asm_x86_memory_arithmetic.cpp)
target_link_libraries(dss_asm_test_asm_x86_memory_arithmetic
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_x86_memory_arithmetic
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# D-ASM-X86-NO-16BIT-IMMEDIATE-SLOT + D-ASM-X86-WIDTH-EXTENDING-MOVES-UNSPELLABLE
# + D-ASM-X86-CMP-AGAINST-MEMORY-DIRECTION-IS-UNELECTABLE: the byte tier for the
# three shapes gas accepts and DSS used to refuse. Drives the SHIPPED dialect and
# the SHIPPED target UNMUTATED; every expectation is GNU as 2.42's own measured
# output unless the comment beside it records the deliberate difference.
# WARNING ITS RED-ON-DISABLE MUTANTS ARE CONFIG-LEVEL, so it MUST run through
# ctest -- dss_add_test sets DSS_CONFIG_ROOT, while a bare .exe walks the cwd and
# would read whichever tree the shell stands in.
dss_add_test(NAME asm/test_asm_x86_width_and_direction
             SOURCES test_asm_x86_width_and_direction.cpp)
target_link_libraries(dss_asm_test_asm_x86_width_and_direction
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_x86_width_and_direction
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD: the encoder's register-CLASS
# gate — an FPR-class LIR operand must encode through an FPR field and a
# GPR-class one through a GPR field, per the bank each field DECLARES in
# `.target.json`. Drives BOTH shipped targets UNMUTATED for the refusals, plus
# one mutant that re-points a field's declared bank and watches the verdict
# flip (which is what proves the gate reads config rather than a table in the
# encoder).
# WARNING ITS RED-ON-DISABLE MUTANT IS CONFIG-LEVEL, so it MUST run through
# ctest -- dss_add_test sets DSS_CONFIG_ROOT, while a bare .exe walks the cwd
# and would read whichever tree the shell stands in.
dss_add_test(NAME asm/test_asm_register_class_gate
             SOURCES test_asm_register_class_gate.cpp)
target_link_libraries(dss_asm_test_asm_register_class_gate
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_register_class_gate
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# D-ASM-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE: which register VIEW a
# modifier-less `%N` substitutes at. The derivation is declared per target and
# per register class (`asmBareOperandWidths`), because gcc and clang agree with
# each other and DISAGREE across ports — aarch64 names the full `x` register
# for every integer type, x86_64 tracks the operand's own type — so one rule
# fixes one machine by breaking the other. Drives BOTH shipped targets from ONE
# binding width and asserts they must not agree, plus two config-level mutants
# (the declaration REMOVED must refuse; the declaration FLIPPED must emit the
# other port's encoding, which is the control proving the pin is not simply
# failing in both directions).
# WARNING ITS RED-ON-DISABLE MUTANTS ARE CONFIG-LEVEL, so it MUST run through
# ctest -- dss_add_test sets DSS_CONFIG_ROOT, while a bare .exe walks the cwd
# and would read whichever tree the shell stands in.
dss_add_test(NAME asm/test_asm_bare_operand_width
             SOURCES test_asm_bare_operand_width.cpp)
target_link_libraries(dss_asm_test_asm_bare_operand_width
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_bare_operand_width
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# D-ASM-AARCH64-FP-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE (R5 + R8, P50):
# class-scoped width-view modifier letters. The aarch64 letters are scoped
# (b/h/s/d/q => fpr, w/x => gpr; a wrong-class letter refuses BY NAME listing
# the operand class's own alphabet — both references hard-error the FP-on-GPR
# direction, and the operator ruled the GPR-on-FP direction dissolved), the
# x86-64 letters stay width-only (gcc ACCEPTS a GPR letter on an xmm operand
# — the disjunction decides acceptance), and the loader refuses a document
# mixing the two postures. Also carries the cross-tier static_assert that
# every width `AssemblyConfig::kTemplateModifierWidthBits` admits has a LIR
# flag (core cannot include lir_node.hpp; this test includes both).
# WARNING ITS RED-ON-DISABLE MUTANTS ARE CONFIG-LEVEL (shipped dialect JSON
# edited in memory), so it MUST run through ctest -- dss_add_test sets
# DSS_CONFIG_ROOT.
dss_add_test(NAME asm/test_asm_class_scoped_modifiers
             SOURCES test_asm_class_scoped_modifiers.cpp)
target_link_libraries(dss_asm_test_asm_class_scoped_modifiers
    PRIVATE nlohmann_json::nlohmann_json)

# D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME (the x86_64
# half): the SSE/scalar-FP instruction rows that make the `x` constraint's
# `fpr` register class nameable by a template. Before them the target declared
# 103 opcodes, this dialect spelled 36, and ALL EIGHTEEN FP/SSE opcodes were
# unspelled -- so `"x"` could be bound and never used.
# The pins read the MANDATORY PREFIX, because that is the only byte separating
# the `sd`/`ss` pair: one target opcode under two width-keyed guards, so a row
# declared at the wrong width still assembles and still runs. Also pins the
# ucomisd/ucomiss prefix ASYMMETRY (66 vs none, against the F2/F3 pattern every
# arithmetic row follows) and the near-miss set that must REFUSE rather than
# bind to a neighbouring opcode.
# WARNING ITS RED-ON-DISABLE MUTANT IS CONFIG-LEVEL -- it removes a row from the
# SHIPPED dialect document -- so it MUST run through ctest: dss_add_test sets
# DSS_CONFIG_ROOT, while a bare .exe walks the cwd and would read whichever tree
# the shell stands in.
dss_add_test(NAME asm/test_asm_x86_sse_dialect_rows
             SOURCES test_asm_x86_sse_dialect_rows.cpp)
target_link_libraries(dss_asm_test_asm_x86_sse_dialect_rows
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_x86_sse_dialect_rows
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# D-ASM-DIALECTS-DECLARE-A-REGISTER-CLASS-NO-INSTRUCTION-CAN-NAME (the arm64
# half): the scalar-FP instruction rows that make the `w` constraint's `fpr`
# register class nameable by a template. Before them the target declared 87
# opcodes, this dialect spelled 51 rows covering 29, and ALL TWENTY FP/SIMD
# opcodes were unspelled -- so `"w"` could be bound and never used.
# The pins read the WHOLE 32-bit word, because on this ISA the single/double
# pair is ftype[23:22] of ONE word and NO dialect row declares a width: the
# register VIEW in the template is the only thing electing between them, so a
# wrong-ftype variant still assembles and still runs. Declaring these rows is
# what exposed that `fmov`'s width-32 and width-16 arms both carried the
# width-64 word (a byte divergence from gas 2.42 AND clang 18.1.3, corrected in
# the same cycle); `FmovSingleIsNotTheDoubleWord` is that ratchet. Also carries
# the near-miss set that must REFUSE rather than bind to a neighbour -- the
# conversions especially, where a width-less row would silently encode FCVTZS
# Xd,Sn for `fcvtzs w0, s1` -- and a standing predicate that EVERY
# constraint-bindable register class has some instruction row able to name it,
# which is the row's own defect class asked as a question rather than retold.
# WARNING ITS RED-ON-DISABLE MUTANTS ARE CONFIG-LEVEL -- one removes rows from
# the SHIPPED dialect document, the other an encoding variant from the SHIPPED
# target -- so it MUST run through ctest: dss_add_test sets DSS_CONFIG_ROOT,
# while a bare .exe walks the cwd and would read whichever tree the shell
# stands in.
dss_add_test(NAME asm/test_asm_arm64_fp_dialect_rows
             SOURCES test_asm_arm64_fp_dialect_rows.cpp)
target_link_libraries(dss_asm_test_asm_arm64_fp_dialect_rows
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_arm64_fp_dialect_rows
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# The LAST three gaps of the same row, closed in cycle P54 (lane el): the
# register-CLASS election axis (the cross-file `fmov` family and the FP memory
# forms), the DESTINATION-WIDTH axis (the whole conversion family, whose
# measured hazard was `fcvtzs %w0, %s1` silently emitting the X form), and the
# lane-ARRANGEMENT grammar (`cnt v0.8b` / `addv b0, v1.8b`). Every expected word
# is byte-verified against GNU as 2.42 AND clang 18.1.3 separately.
# WARNING ITS RED-ON-DISABLE MUTANTS ARE CONFIG-LEVEL -- three over the SHIPPED
# target (a deleted opcode, deleted `destWidth` declarations, a deleted result
# bank) and one over the SHIPPED dialect (the deleted arrangement table) -- so
# it MUST run through ctest: dss_add_test sets DSS_CONFIG_ROOT, while a bare
# .exe walks the cwd and would read whichever tree the shell stands in. Measured
# again this cycle from the other side: a CLI probe run from the repo root while
# the edits sat in a lane worktree silently answered about the MAIN tree's
# config.
dss_add_test(NAME asm/test_asm_arm64_conversion_dialect_rows
             SOURCES test_asm_arm64_conversion_dialect_rows.cpp)
target_link_libraries(dss_asm_test_asm_arm64_conversion_dialect_rows
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_arm64_conversion_dialect_rows
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# D-ASM-ARM64-GAS-SPELLS-NO-ROUNDING-MODE-OR-VECTOR-MOVE (cycle P54, lane av):
# the REST of the scalar floating-point vocabulary — the eight FP->integer
# conversions that name an explicit ROUNDING MODE, the seven round-to-integral
# forms, the 1-/2-/3-source arithmetic, the signalling compare, and the SIMD
# register move (`mov Vd.16b, Vn.16b`, which is the ORR alias). 28 spellings,
# 72 encodings, every word measured against GNU as 2.42 AND clang 18.1.3
# separately with zero disagreements.
# The pins read the WHOLE 32-bit word, because the rounding mode is `rmode`
# [16:15] and the float width is `ftype` [23:22] of the SAME word: a
# mis-bound mnemonic still assembles, still runs, and returns a plausible
# number — the silent-miscompile class this row exists to refuse. Beside the
# equality pins sit the negative form (no rounding mode may ever emit FCVTZS's
# word), the distinctness of the seven rint modes, the `fmax`/`fmaxnm` split
# that is only visible on NaN, the `Ra` field placement of the FMA addend, and
# the standing boundary in BOTH directions — what shipped, and what stays
# refused because it needs an operand form this dialect does not model or
# because both references refuse it at the default -march.
# WARNING ITS RED-ON-DISABLE MUTANTS ARE CONFIG-LEVEL -- two over the SHIPPED
# dialect (deleted rows) and two over the SHIPPED target (a deleted opcode, a
# deleted wire) -- so it MUST run through ctest: dss_add_test sets
# DSS_CONFIG_ROOT, while a bare .exe walks the cwd and would read whichever
# tree the shell stands in.
dss_add_test(NAME asm/test_asm_arm64_rounding_dialect_rows
             SOURCES test_asm_arm64_rounding_dialect_rows.cpp)
target_link_libraries(dss_asm_test_asm_arm64_rounding_dialect_rows
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_arm64_rounding_dialect_rows
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# D-TARGET-ARM64-HALF-BYTE-QUAD-MEMORY-FORMS-UNDECLARED (cycle P55, lane lq):
# the H, B and Q widths of the four scalar SIMD&FP memory opcodes, plus the
# `q0..q31` ALIAS spellings the Q half needs to be nameable at all. Byte pins at
# two (Rt,Rn) placements each beside the S/D controls, the near-miss immediate
# FENCE (one byte offset encodes FIVE different imm12 fields across the five
# widths — the silent-wrong-answer risk in this family), a REMOVE-direction
# mutant per half with a CONTROL named by name, and the double-count negative
# SYNTHESIZED: a convention naming the alias spelling must be REFUSED AT LOAD.
# WARNING ITS RED-ON-DISABLE MUTANTS ARE CONFIG-LEVEL -- both over the SHIPPED
# target (deleted width arms, deleted alias lists) -- so it MUST run through
# ctest: dss_add_test sets DSS_CONFIG_ROOT, while a bare .exe walks the cwd and
# would read whichever tree the shell stands in.
dss_add_test(NAME asm/test_asm_arm64_memory_dialect_rows
             SOURCES test_asm_arm64_memory_dialect_rows.cpp)
target_link_libraries(dss_asm_test_asm_arm64_memory_dialect_rows
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_arm64_memory_dialect_rows
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# [[D-ASM-ARM64-SP-AND-XZR-SHARE-ENCODING-31-SO-MOV-SP-SILENTLY-BECOMES-ZERO]]
# (cycle P55, lane sp): AArch64 register field 31 names the STACK POINTER in
# some instruction forms and the ZERO REGISTER in others, and `sp`/`xzr` (and
# `wsp`/`wzr`) are two rows of `registers[]` at that one number. Byte pins for
# every SP-naming form both references assemble, at more than one register
# placement each; the ZERO-register spellings pinned by name as CONTROLS (a fix
# that routed everything through the ADD form would pass a positive-only
# fixture and break the zero register instead); the seventeen spellings BOTH
# references REFUSE, which DSS accepted at the P55 base while emitting a
# plausible instruction naming the other register; the load-time rules that make
# a role-less collision unsayable; and five REMOVE-direction mutants, each with
# its controls printed BY NAME.
# WARNING ITS RED-ON-DISABLE MUTANTS ARE CONFIG-LEVEL -- over the SHIPPED target
# AND the SHIPPED dialect -- so it MUST run through ctest: dss_add_test sets
# DSS_CONFIG_ROOT, while a bare .exe walks the cwd and would read whichever tree
# the shell stands in.
dss_add_test(NAME asm/test_asm_arm64_shared_encoding_31
             SOURCES test_asm_arm64_shared_encoding_31.cpp)
target_link_libraries(dss_asm_test_asm_arm64_shared_encoding_31
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_arm64_shared_encoding_31
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# [[D-DIAG-OVERLAP-REFUSAL-CODE-NOT-DISCRIMINATING]] (cycle P65, lane dg): the
# static-data producer's diagnostic CODES, pinned as a PARTITION rather than one
# at a time. Every arm of `lowerMirGlobalsToDataItems` used to pass
# `K_NoMatchingObjectFormat`, so only the message text told the causes apart and
# a consumer that triages or greps BY CODE could not. Three fixtures raise the
# three classes through the arms that really produce them -- a shared-bytes
# initializer (the user edits the source), a `_Complex` literal that is not a
# two-component aggregate (a DSS defect), and an aggregate global against a
# target declaring no `aggregateLayout` (a capability gap) -- and the codes are
# compared to EACH OTHER, so a collapse in either direction reds. The residual
# arm is the named CONTROL: it must still be `K_NoMatchingObjectFormat`, so an
# over-eager re-key of the whole function reds there while the specific pins
# stay green. Nameability and the unsuppressable membership are asserted too:
# an unnameable code is ungreppable, which is the defect itself.
dss_add_test(NAME asm/test_static_data_diagnostic_codes
             SOURCES test_static_data_diagnostic_codes.cpp)

# [[D-CSUBSET-LONG-BRANCH]] (cycle P68 round 2, lane `br`): the branch-relaxation
# fixed point. A SYNTHESIZED oversized function body — ~1 MiB of padding, laid
# out so one conditional is out of AArch64 `B.cond`'s ±1 MiB reach by exactly one
# word and a second is exactly AT the limit until the first one escapes — proves
# that the resolver iterates rather than patching once, and that both escape
# shapes (condition preserved / condition inverted) are emitted correctly. The
# six-word in-reach skeleton is the named CONTROL arm: its bytes are pinned as
# literals, so a relaxation that fires on branches that fit reds there while the
# oversized pin stays green.
# ⓘ It is the slowest test in this directory by construction — reaching a ±1 MiB
# branch edge means emitting ~1 MiB of code, three times.
dss_add_test(NAME asm/test_asm_long_branch
             SOURCES test_asm_long_branch.cpp)

# P68 round 8 (D-ASM-DIALECT-GAPS-A-REFERENCE-ASSEMBLER-ACCEPTS,
# D-ASM-DOTTED-LABEL-AS-A-BRANCH-TARGET-REFUSED): every spelling GNU as and
# clang accept that the shipped dialects refused at the part-3 base — literal
# xmm names, the cross-bank movq/movd, movapd, the test family, setcc, the
# unsuffixed AT&T spellings and gas's unstated-width default with its warning,
# dotted labels as branch targets, the aarch64 W-form add/sub immediates,
# cbz/cbnz, and an operand placeholder as a memory base or index — each through
# the SHIPPED dialect and target to bytes, each widening beside its refusal
# control. Reads the shipped documents, so it must run through ctest.
dss_add_test(NAME asm/test_asm_reference_accepted_spellings
             SOURCES test_asm_reference_accepted_spellings.cpp)
target_link_libraries(dss_asm_test_asm_reference_accepted_spellings
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_reference_accepted_spellings
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# P68 round 9 (D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER): the program
# counter as a memory base and a symbol as a displacement — `leaq msg(%rip)`,
# `movl $5, counter(%rip)`, `msg+4` / `msg-8` — to the reference's bytes and
# relocations through the SHIPPED dialect and target, the refusals (`%rip` as an
# operand, an index beside it, a constant after a symbol outside a
# displacement), and the red-on-disable arms that remove each declared fact.
# Reads the shipped documents and mutates the target, so it must run through
# ctest.
dss_add_test(NAME asm/test_asm_pc_relative_memory
             SOURCES test_asm_pc_relative_memory.cpp)
target_link_libraries(dss_asm_test_asm_pc_relative_memory
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_pc_relative_memory
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)

# P68 round 9 (D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL): a `.s`
# naming, by address, a symbol it does not define. The import is minted with
# its code-vs-data kind PENDING for an address operand, a memory displacement
# and a data slot; a call states code and settles an earlier address; a name
# the file defines is never imported. Reads the shipped documents, so it must
# run through ctest.
dss_add_test(NAME asm/test_asm_undefined_symbol_reference
             SOURCES test_asm_undefined_symbol_reference.cpp)
target_link_libraries(dss_asm_test_asm_undefined_symbol_reference
    PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(dss_asm_test_asm_undefined_symbol_reference
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/asm)
