# MIR unit tests (ML1). One executable per file for ctest parallelism +
# per-file failure isolation.

dss_add_test(NAME mir/test_mir_opcode       SOURCES test_mir_opcode.cpp)
dss_add_test(NAME mir/test_mir_node         SOURCES test_mir_node.cpp)
dss_add_test(NAME mir/test_mir_literal_pool SOURCES test_mir_literal_pool.cpp)
dss_add_test(NAME mir/test_mir              SOURCES test_mir.cpp)
dss_add_test(NAME mir/test_mir_verifier     SOURCES test_mir_verifier.cpp)
dss_add_test(NAME mir/test_mir_dom          SOURCES test_mir_dom.cpp)
dss_add_test(NAME mir/test_mir_struct_markers SOURCES test_mir_struct_markers.cpp)
dss_add_test(NAME mir/test_mir_text         SOURCES test_mir_text.cpp)

# Inline-asm P5 (plan 29 §4.4) — the MIR carriage: opcode rows, the ReturnPiece
# payload split, the dedicated-builder refusals, and the `asm goto` edge
# placement. Its own binary because it carries DEATH tests (the structural
# backstops), which gtest runs by forking the whole executable.
dss_add_test(NAME mir/test_mir_inline_asm SOURCES test_mir_inline_asm.cpp)

# ML2 lowering tests.
dss_add_test(NAME mir/test_mir_lowering_c SOURCES test_mir_lowering_c.cpp)

# LK6 cycle 2d — HIR ExternFunction → MIR.externImports thread-through.
dss_add_test(NAME mir/test_mir_lowering_extern SOURCES test_mir_lowering_extern.cpp)

# Cycle 25 Stage B — cross-module MIR merge (mergeCuMirs) tested in isolation.
dss_add_test(NAME mir/test_mir_merge SOURCES test_mir_merge.cpp)

# OPT11 (plan 22 §0.2) — the cross-CU SUMMARY INDEX that replaces the monolithic
# whole-program merge above. TWO binaries, because they test opposite sides of
# one contract and must be able to fail independently: the first pins that MIR
# projects onto a summary correctly, the second pins what the global pass
# decides GIVEN summaries. Deriving the second's inputs from real MIR would hide
# an index bug behind an emitter bug.
dss_add_test(NAME mir/test_mir_summary   SOURCES test_mir_summary.cpp)
dss_add_test(NAME mir/test_summary_index SOURCES test_summary_index.cpp)

# D-OPT11-LAZY-IMPORT-EDGE — the TRANSFORMATION half's missing edge: a per-TU
# optimize that ASKS the index for a body (`SummaryIndex::definitionOf`) instead
# of being confined to the precomputed prefetch plan. A THIRD binary beside the
# two above, on the same argument: it tests what happens when the decision is
# ACTED ON, and a failure here must not be confusable with a bad summary or a
# bad index. Its fixture is a FOUR-module call chain, because a two-module one is
# green for an eager plan and a lazy edge alike.
dss_add_test(NAME mir/test_lazy_import_optimize
             SOURCES test_lazy_import_optimize.cpp)

# D-OPT11-LAZY-IMPORT-EDGE — the `.dss.mir` section's BODY payload: the envelope,
# its refusal rules, and the round trip a fetched body has to survive.
dss_add_test(NAME mir/test_mir_body_codec SOURCES test_mir_body_codec.cpp)

# D-OPT11-LAZY-IMPORT-EDGE — the DETERMINISM bar plan 22 §0.2 names as the arc's
# load-bearing risk: a per-TU parallel optimize with on-demand imports must emit
# a byte-identical artifact run to run and at any pool width. It drives the whole
# driver (`Program::setLtoMode`) because that property is not observable at the
# MIR tier; it lives here because its SUBJECT is the MIR-tier import stage.
dss_add_test(NAME mir/test_thin_lto_artifact_identity
             SOURCES test_thin_lto_artifact_identity.cpp)

# D-MIR-OVERLAP-STRUCT-ZERO-INIT — brace-init of a struct whose members SHARE
# BYTES (a c107 explicit-offset overlay). Hand-built HIR, because explicit offsets
# cannot be spelled in C source — they arrive only from a shipped descriptor.
dss_add_test(NAME mir/test_overlap_struct_zero_init
             SOURCES test_overlap_struct_zero_init.cpp)

# D-FFI-PE-CRT-UCRT-MIGRATION Phase 3 — synthesizeStdioShim's va_list ARM
# SELECTION, pinned at the operand the synthesized call forwards. Separate from
# test_mir_merge (which pins the shim's SHAPE) because the two HomogeneousPointer
# arms produce structurally identical bodies: only the leaf differs, and picking
# the wrong one is a silent wrong-ABI forward on apple_arm64.
dss_add_test(NAME mir/test_synth_stdio_shim_valist SOURCES test_synth_stdio_shim_valist.cpp)

# D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS — the three
# operand/payload-carrying `.dssir` forms whose writer out-ran its reader, plus
# the class guard for the next one. Its own binary because the pre-fix behaviour
# of the `indirectbr` arm was a PROCESS ABORT, which takes every sibling
# assertion in the same executable down with it.
dss_add_test(NAME mir/test_mir_text_operand_forms SOURCES test_mir_text_operand_forms.cpp)

# D-ASM-IMMEDIATE-CONSTRAINT-FORM-NOT-REALIZED
# D-MIR-LVALUE-REFUSAL-RENDERS-A-RAW-ORDINAL-NOT-A-NAME
# ⚠ Those two ids sit on their own lines because an anchor id split across a
# line break is invisible to every grep AND to the registry guard — and this
# comment carried an instance of exactly that until the step-10 audit found it.
# Break BEFORE an id or AFTER it, never inside it.
# The `"i"` IMMEDIATE constraint at the tier that owns
# the two facts nothing below it can recover: the CONSTANT PROOF (gcc refuses a
# non-constant with "impossible constraint in 'asm'", and so must this) and the
# fact that the operand FORM is read from the TARGET rather than from the
# letter's spelling. The last arm re-spells `"i"` in the shipped JSON and demands
# the identical lowering, so a `constraint == "i"` implementation is red there
# and green everywhere else -- which is why it needs the nlohmann consumer.
dss_add_test(NAME mir/test_mir_inline_asm_immediate
             SOURCES test_mir_inline_asm_immediate.cpp)
target_link_libraries(dss_mir_test_mir_inline_asm_immediate
                      PRIVATE nlohmann_json::nlohmann_json)

# D-MIRTEXT-GLOBAL-FLAGS-DROPPED-BY-ROUNDTRIP: a global's binding, visibility,
# constness, thread storage and explicit alignment must SURVIVE emit -> parse.
# `parseGlobal` re-minted all four flags as defaults in every arm and never read
# an alignment at all, so a `static const _Thread_local alignas(64)` global came
# back plain — with `ok == true`. ⚠ The existing round-trip pin cannot see it:
# it asserts `firstEmit == secondEmit`, and a TOTALLY LOSSY round trip satisfies
# that. These cases assert the PARSED MODULE's fields, each as a negative
# against the default it used to collapse to.
dss_add_test(NAME mir/test_mir_text_global_attrs
             SOURCES test_mir_text_global_attrs.cpp)

# P44 lane d row 2 — the `.dssir` twin of D-LIR-TEXT-PARSE-UNSEALED-BLOCK-ABORT.
# `mir_text.cpp`'s `if (errors_) return;` guard before `beginBlock` covers every
# DIAGNOSED malformation; these two shapes set no `errors_` at all.
dss_add_test(NAME mir/test_mir_text_unsealed_block
             SOURCES test_mir_text_unsealed_block.cpp)

# P68 round 8 (lane `ht`, part 1b): EVERY `MirInst` field survives a `.dssir`
# round trip — the writer printed no instruction flag and no secondary payload
# (an alloca's alignment, an atomic access's provable alignment), and read them
# back as the defaults with `ok == true`. A field-by-field comparison per reader
# arm, and a `sizeof(MirInst)` assertion that refuses to compile until a NEW
# field is covered too.
dss_add_test(NAME mir/test_mir_text_instruction_fields
             SOURCES test_mir_text_instruction_fields.cpp)

# P68 round 8 (lane `ht`, part 1b): the `.dssir` reader RETURNS on every input.
# The measured table of shapes that used to abort, hang or read clean; a sweep
# of every truncation and every single-token deletion of a golden module, each
# announced on stderr before it is parsed so an abort or a timeout names it; and
# the cascade pin — one bad block costs its own diagnostic, later blocks are read.
dss_add_test(NAME mir/test_mir_text_reader_never_aborts
             SOURCES test_mir_text_reader_never_aborts.cpp)

# P68 round 8 lane `ht` part 1d: the `.dssir` v2 `types` table — a composite is
# `type <H>`, defined ONCE with every layout channel through the ONE owner both text
# tiers share. The cycle (a linked-list node and a mutual pair round-trip), the
# layout (every channel and the computed layout read back equal), the size (a graph
# mentioned N times is defined once), each refusal by name, the spelling the HIR v5
# table shares, and the `.dss.mir` body a module holding a linked list now encodes.
dss_add_test(NAME mir/test_mir_text_types_table
             SOURCES test_mir_text_types_table.cpp)

# D-CSUBSET-C11-THREADS-MACHO-MTX-PLAIN-RECURSIVE + D-CSUBSET-C11-THREADS-MTX-PLAIN-RECURSIVE
# (P51 lane mx): `mtx_init`'s C11 `type` argument. The macho recipe passed a NULL
# mutexattr, so `mtx_init(&m, mtx_recursive)` yielded a NON-recursive Darwin mutex
# and a same-thread re-lock DEADLOCKED — a hang out of a discarded argument. The
# corpus witness `examples/c/c11_mtx_recursive` proves the behaviour but its macho
# arm runs only on a Mac; this asserts the emitted body of BOTH vehicles from ANY
# host, at the OPERAND level (the kind is traced back to Arg#1 through its whole
# Mul/ZExt/ICmpNe/And chain, with both platform literals pinned by value), so a
# regression reds on the Windows leg. Its win32 half is a deliberate NEGATIVE:
# same-thread re-lock of a plain mutex is UNDEFINED and the three references
# disagree three ways, so pe must keep NOT growing a recursion guard.
dss_add_test(NAME mir/test_synth_threads_mtx_type
             SOURCES test_synth_threads_mtx_type.cpp)

# P55 lane `rc` — "depth costs heap, not host call frames". Its OWN binary
# because a red here is a STACK OVERFLOW, which kills the process and would take
# every sibling test in a shared executable down unattributably with it.
dss_add_test(NAME mir/test_deep_nesting_costs_heap
             SOURCES test_deep_nesting_costs_heap.cpp)

# D-MIR-ACCESSORS-ABORT-ON-WRONG-OPCODE — the `try*` non-fatal twins of the typed
# payload readers, plus the two guards that keep them test-only and keep the twin
# set from falling behind `mir.cpp`. Its OWN binary for the same reason
# test_mir_inline_asm has one: it carries DEATH tests, which gtest runs by forking
# the whole executable — and here they are load-bearing rather than incidental.
# They drive each ABORTING sibling over the very instruction its twin answered
# with nullopt, so the pair is the before/after of one identical call and the
# nullopt arm cannot be green for a reason unrelated to the abort.
dss_add_test(NAME mir/test_mir_try_accessors
             SOURCES test_mir_try_accessors.cpp)

# P68 round 9 lane `lm` — D-C-STATIC-INITIALIZER-POINTER-CAST-CHAIN-REFUSED: a null
# pointer or an integer address reaching a STATIC pointer object through a CHAIN of
# pointer casts (`char *p = NULL;` with the references' `((void *)0)` is two) is
# static data. Pinned at the MIR literal and at the static-data producer on every
# real (target, format) pair, through the driver's own `buildCuMir`/`assembleUnit`.
dss_add_test(NAME mir/test_static_pointer_cast_chain
             SOURCES test_static_pointer_cast_chain.cpp)
