# LIR unit tests (ML5).

dss_add_test(NAME lir/test_lir           SOURCES test_lir.cpp)
dss_add_test(NAME lir/test_lir_liveness  SOURCES test_lir_liveness.cpp)
dss_add_test(NAME lir/test_lir_callconv  SOURCES test_lir_callconv.cpp)
# Cycle 10k: mutate_target_schema.hpp consumes nlohmann_json for the
# in-memory JSON-strip negative pin (D-CSUBSET-LOCAL-INT-CODEGEN-NEGATIVE-PIN).
target_link_libraries(dss_lir_test_lir_callconv PRIVATE nlohmann_json::nlohmann_json)
dss_add_test(NAME lir/test_lir_regalloc  SOURCES test_lir_regalloc.cpp)
# D-OPT-REGALLOC-EXCLUSION-BUFFER closure (2026-06-11): the >8-ordinal
# exclusion-union growth pins mutate idiv_op/shl implicitRegisters via
# mutate_target_schema.hpp.
target_link_libraries(dss_lir_test_lir_regalloc PRIVATE nlohmann_json::nlohmann_json)
dss_add_test(NAME lir/test_lir_rewrite   SOURCES test_lir_rewrite.cpp)
# D-LIR-PER-INST-REG-CONSTRAINTS: the module side-structure
# carry-across (both pools, all four rebuild passes) and the verifier rules
# that make a forgotten carry loud instead of a silent miscompile.
dss_add_test(NAME lir/test_lir_pass_util SOURCES test_lir_pass_util.cpp)
dss_add_test(NAME lir/test_lir_verifier  SOURCES test_lir_verifier.cpp)
# D-LIR-EARLYCLOBBER-FLAG-UNSETTABLE-AFTER-EMISSION: `"=&r"` on an
# allocator-chosen register. Drives the SHIPPED asm dialects through the
# shared template engine so the flag is stamped on an instruction this
# tier did not emit — the exact shape `mir_to_lir`'s expansion produces.
dss_add_test(NAME lir/test_lir_earlyclobber SOURCES test_lir_earlyclobber.cpp)
# D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE: the two-address tie is an operand
# INDEX. Re-aims x86_64 `add`'s tie via mutate_target_schema.hpp, which
# needs nlohmann_json.
dss_add_test(NAME lir/test_lir_two_address_tie SOURCES test_lir_two_address_tie.cpp)
target_link_libraries(dss_lir_test_lir_two_address_tie PRIVATE nlohmann_json::nlohmann_json)
# D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE (the asm half): a `"+r"` operand's two
# halves bind to ONE register. Drives the REAL c → HIR → MIR → LIR
# lowering for the positive claim and its untied control, and hand-builds the
# two descriptor shapes that must still REFUSE.
dss_add_test(NAME lir/test_lir_asm_tied_operand SOURCES test_lir_asm_tied_operand.cpp)
# D-LIR-ASM-GOTO-REFUSAL-NAMES-A-CONFIG-GAP-THAT-NO-LONGER-EXISTS (P20): the
# `asm goto` label edge, the fall-through edge, result piece 0 and the two
# refusals that must NOT become acceptance. Drives the REAL c → HIR →
# MIR → LIR lowering, because the property is a relation between what the front
# end minted (the spellings) and what this tier bound (the blocks).
dss_add_test(NAME lir/test_lir_asm_goto_labels SOURCES test_lir_asm_goto_labels.cpp)
# D-ASM-DUPLICATE-SYMBOLIC-NAME-BINDS-THE-WRONG-OPERAND (P20): the binding
# builder's collision refusal, for BOTH row sets (operands and `asm goto`
# labels). Hand-builds the descriptors, because the C front end now refuses the
# shape and this guard exists for the producers that never run that pass.
dss_add_test(NAME lir/test_lir_asm_binding_collision
             SOURCES test_lir_asm_binding_collision.cpp)
# D-LIR-PER-INSTRUCTION-OUTPUTS-NOT-ENFORCED-SUBSET-OF-CLOBBERED: a
# register-pinned `"=a"` output must enter the per-instruction constraint's
# CLOBBER set and so reach `effectiveForbiddenOrdinals`, because the allocator
# omits outputs from its forbidden set on exactly that invariant. Drives the
# REAL c → HIR → MIR → LIR lowering — a hand-built constraint would satisfy
# the invariant because the test wrote it that way.
dss_add_test(NAME lir/test_lir_asm_pinned_output_clobber
             SOURCES test_lir_asm_pinned_output_clobber.cpp)
dss_add_test(NAME lir/test_lir_text      SOURCES test_lir_text.cpp)
# D-OPT-JCC-FALLTHROUGH: Rule 1b's ONE legal asymmetry — a branch whose
# trailing fallthrough operand `lir_peephole` elided, verified against the
# module's own block LAYOUT. Its fixture (`fallthrough_form_schema.hpp`) builds
# a target that SPELLS the shorter encoding form by mutating the shipped
# document in memory, which needs nlohmann_json.
target_link_libraries(dss_lir_test_lir_text PRIVATE nlohmann_json::nlohmann_json)
dss_add_test(NAME lir/test_mir_to_lir    SOURCES test_mir_to_lir.cpp)
# FC1 (V2-4.X): the role-contract red-on-disable levers strip
# inputRoles/outputRoles off idiv_op via mutate_target_schema.hpp.
target_link_libraries(dss_lir_test_mir_to_lir PRIVATE nlohmann_json::nlohmann_json)
# FC3.5 sweep-c2: the FCmp predicate-mapping truth table + shape pins
# (D-COND-FLOAT-NAN-TRUTHINESS-FCMP). Uses mutate_target_schema.hpp for
# the strip-foeq/strip-fogt capability red-on-disable levers.
dss_add_test(NAME lir/test_fcmp_lowering SOURCES test_fcmp_lowering.cpp)
target_link_libraries(dss_lir_test_fcmp_lowering PRIVATE nlohmann_json::nlohmann_json)

# D-LIR-RETURN-REG-REFUSAL-IS-UNREACHABLE-FROM-THE-TEST-TIER: the result-register
# pool rows are the ONE owner of both the acceptance decision and the sentence
# that advertises it. Reaches `returnRegisterForClass` directly, which is the
# whole point — while it lived in an anonymous namespace no tier could observe
# its refusal, and retyping the accepted set as a literal reddened nothing.
dss_add_test(NAME lir/test_lir_return_pool_projection
             SOURCES test_lir_return_pool_projection.cpp)

# D-LIR-ARG-PASSING-POOL-SELECTION-IS-TWO-WAY-AND-VR-FALLS-INTO-GPR — the ARG
# side of the pool projection above, plus the cursor WALK the row table alone
# was measured not to cover.
dss_add_test(NAME lir/test_lir_arg_cursor_projection
             SOURCES test_lir_arg_cursor_projection.cpp)

# D-TARGET-ALIASED-VIEWS-BOTH-ALLOCATABLE-DOUBLE-COUNT-ONE-FILE +
# D-TARGET-ALLOCATABLE-POOL-LIST-SET-HAS-NO-OWNER: the TRIPWIRE for
# D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS. Asserts the trigger is
# still false, states the d/v aliasing as an ABI fact rather than deriving it,
# and drives the load-time refusal through mutate_target_schema.hpp.
dss_add_test(NAME lir/test_lir_aliased_view_allocability
             SOURCES test_lir_aliased_view_allocability.cpp)
target_link_libraries(dss_lir_test_lir_aliased_view_allocability
                      PRIVATE nlohmann_json::nlohmann_json)

# D-LIR-FRAME-SLOT-STRIDE-ENUMERATES-CLASSES-INSTEAD-OF-DERIVING: the frame's
# uniform slot stride must cover the widest class that OCCUPIES a slot, not the
# two classes an inline `max(GPR, FPR)` happened to name.
dss_add_test(NAME lir/test_lir_frame_slot_stride
             SOURCES test_lir_frame_slot_stride.cpp)

# D-LIR-CALLEE-SAVED-AND-SPILL-STORES-DEFAULT-TO-WIDTH-64-BELOW-THE-SLOT: the
# SIBLING of the row above — the stride sized the slot correctly and the store
# then wrote 8 bytes into it, because `emitFrameStore`'s width DEFAULTED to 64
# for every class on every target. Drives the real c -> ... -> assemble
# pipeline on both shipped targets and closes on the emitted ENCODING.
dss_add_test(NAME lir/test_lir_frame_access_width
             SOURCES test_lir_frame_access_width.cpp)

# D-LIR-ASM-OPERAND-MOVE-IS-CLASS-BLIND + D-TARGET-NO-CROSS-CLASS-MOVE-VERB: the
# inline-asm operand materialisation used ONE module-wide integer `mov` for
# every operand whatever class it bound — a live silent miscompile on both
# shipped targets. Drives the REAL c -> HIR -> MIR -> LIR lowering.
dss_add_test(NAME lir/test_lir_asm_operand_move_class
             SOURCES test_lir_asm_operand_move_class.cpp)

# D-ASM-MEMORY-CONSTRAINT-OUTPUT-FORM-NOT-REALIZED: a memory-form constraint in
# the OUTPUT section (`"=m"`, `"+m"`, `"=&m"`) is carried as the object's
# ADDRESS and mints no result piece. Drives the REAL c -> HIR -> MIR ->
# LIR lowering on BOTH shipped targets, because the property is a relation
# between the source's output SECTION and the descriptor's result-piece list.
dss_add_test(NAME lir/test_lir_asm_memory_output
             SOURCES test_lir_asm_memory_output.cpp)

# D-CODEGEN-APPLE-ARM64-STACK-ARGS-NOT-NATURALLY-PACKED: Apple's arm64 ABI packs
# NAMED stacked scalars at their own alignment and size, while keeping 8-byte
# slots for named aggregates and for varargs — three axes that were MEASURED to
# differ, which is why the CC declares an object rather than a boolean. The pins
# type the measured offsets out as literals and fetch the CC by name from the
# shipped target, so deleting the declaration reddens the Apple arms and leaves
# the AAPCS64 control green.
dss_add_test(NAME lir/test_lir_apple_stack_arg_packing
             SOURCES test_lir_apple_stack_arg_packing.cpp)

# D-CSUBSET-LONG-DOUBLE-STACK-ARG-ALIGNMENT: the stacked-argument ALIGNMENT rule
# (the sibling of the packing pin above — packing says how many bytes a datum
# owns, this says where its slot may begin).
dss_add_test(NAME lir/test_lir_long_double_stack_arg_alignment
             SOURCES test_lir_long_double_stack_arg_alignment.cpp)

# D-LIR-OUTGOING-ARG-CURSOR-SPLIT-BETWEEN-TWO-PASSES-COLLIDES: ONE pass owns
# every outgoing-argument byte offset. `lowerWideCallArgs` states each stacked
# aggregate's offset on its carrier and stamps the Call; callconv reads those
# offsets and refuses to place anything of its own. The pins drive the REAL
# pipeline (lowerWideCallArgs included -- the defect lives between that pass and
# callconv, so a fixture that skips it cannot see this row) and assert that no
# outgoing-argument byte is written twice.
dss_add_test(NAME lir/test_lir_outgoing_arg_cursor
             SOURCES test_lir_outgoing_arg_cursor.cpp)

# OPT8 (plan 22) — the LIR peephole's redundant-copy rule. The load-bearing
# pins are the NEGATIVES: `zext`/`sext`/`trunc`/`not`/`neg` whose result
# register equals their only operand register are live computation, and on
# x86-64 the first three encode to bytes a disassembler prints as `mov`. The
# rule tests OPCODE IDENTITY against `registerClassOps[].move`, never a
# mnemonic or a byte pattern; deleting the wrong one is a wrong answer that
# every structural check in the pipeline still accepts.
# Its 128-bit-width pins mutate the shipped x86_64 document in memory via
# mutate_target_schema.hpp, which needs nlohmann_json.
dss_add_test(NAME lir/test_lir_peephole
             SOURCES test_lir_peephole.cpp)
target_link_libraries(dss_lir_test_lir_peephole PRIVATE nlohmann_json::nlohmann_json)

# D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED: `&&label` as a LINK-TIME
# address constant. Pins what this tier owes the static-data path — the block
# symbol published with a byte offset for a block NO instruction names, one symbol
# per block when the body addresses it too, and no register materialized for an
# address that only ever reaches data. Drives the REAL c -> HIR -> MIR -> LIR
# lowering, because the property is a relation between what HIR->MIR minted and
# what this tier bound.
dss_add_test(NAME lir/test_lir_label_address_static_table
             SOURCES test_lir_label_address_static_table.cpp)

# D-LIR-2ADDR-IGNORES-EMIT-TERMINATOR-FAILURE: a refusal that crashes is not a
# refusal. FIVE passes rebuild a module through the one shared terminator
# dispatch, and a refusal from it leaves the open block unterminated — which
# `LirBuilder` used to answer with `std::abort()`. Pins all five callers'
# unwind AND the builder poison that makes the sixth caller's omission
# survivable, so the guarantee does not rest on five independent `return`s.
dss_add_test(NAME lir/test_lir_rebuild_unwind
             SOURCES test_lir_rebuild_unwind.cpp)

# D-CODEGEN-APPLE-ARM64-X29-USED-AS-GENERAL-SCRATCH-AGAINST-ITS-RESERVED-ROLE:
# WHEN the frame pointer leaves the allocatable pool is a DECLARED per-convention
# fact (`framePointerReservation`), and these pins assert on the MATERIALIZED
# instruction stream — the emitted code — because the pre-fix program ran fine
# and no ordinary run could have noticed. Consumes mutate_target_schema.hpp for
# the REMOVE-direction mutant, hence nlohmann_json.
dss_add_test(NAME lir/test_lir_frame_pointer_reservation
             SOURCES test_lir_frame_pointer_reservation.cpp)
target_link_libraries(dss_lir_test_lir_frame_pointer_reservation
                      PRIVATE nlohmann_json::nlohmann_json)

# D-LIR-TEXT-PARSE-UNSEALED-BLOCK-ABORT: a malformed instruction in a NON-FINAL
# block used to kill the process. Panic-mode recovery reports the bad
# instruction and keeps driving `LirBuilder`, so a block whose terminator was
# skipped reached the next `beginBlock` unsealed and hit its *"current block has
# no terminator"* fatal. Every pre-existing malformed-input test placed the bad
# instruction in the LAST block, the one arrangement `finalize`'s `errors_`
# short-circuit keeps away from the abort — so the suite could not see it. Its
# own target because the regression mode is a DEAD RUNNER, not a failed
# assertion: keep it away from the 100-odd unrelated pins in test_lir_text.
dss_add_test(NAME lir/test_lir_text_unsealed_block
             SOURCES test_lir_text_unsealed_block.cpp)

# D-CSUBSET-ALIGNAS-OVERALIGNED-STACK-LOCAL: a local whose alignment exceeds the
# convention's `stackAlignment` cannot be placed by CHOOSING an offset — the
# post-prologue stack pointer is congruent to 0 modulo that and no finer. It is
# placed by reserving spare bytes and rounding the address at run time. These pins
# are over the two published derivations that decide the reservation, and over the
# SUFFICIENCY PROPERTY (rounding any raw slot address must land inside its own
# reservation) rather than over a table of offsets — a table would be right at 32 and
# silent at 64. Nothing here drives the unit-tier front end — a sufficiency property
# is pinned harder by calling the derivation directly than by inspecting one lowered
# program, and the file's own banner carries the reason (the row it used to cite,
# D-LIR-TEST-FRONT-END-LOWERS-A-MANY-ARG-CALL-TO-NOTHING-SO-PINS-MEASURE-ZERO, closed
# in P49 and no longer states a live hazard). The end-to-end evidence is the runtime
# corpus (`examples/c/alignas_local_over32`, `alignas_local_cacheline64`,
# `alignas_member_over32_local`).
dss_add_test(NAME lir/test_lir_overaligned_local_frame
             SOURCES test_lir_overaligned_local_frame.cpp)

# D-LIR-TEST-FRONT-END-LOWERS-A-MANY-ARG-CALL-TO-NOTHING-SO-PINS-MEASURE-ZERO:
# the pin over `lowered_lir_fixture.hpp` ITSELF. `lowerCToLir` used to return a
# refused lowering looking exactly like an accepted one, so every pin driving it
# could be asserting over a mutilated module. Its arms run the fixture INSIDE
# `EXPECT_NONFATAL_FAILURE` — an instrument that reports is worth nothing until
# it has been seen to report — and carry the false-red controls (an empty body,
# and a body whose one statement is a bare `return`) that keep the refusal off
# correct C. Its own target because it is the only file here that asserts the
# harness fails rather than that the compiler succeeds.
dss_add_test(NAME lir/test_lir_fixture_vacuity_guard
             SOURCES test_lir_fixture_vacuity_guard.cpp)

# D-LIR-COPY-COALESCING-ASKS-THE-REGISTERS-WIDTH-NOT-THE-VALUES: the copy
# admission test compared the copy's stated width against the widest register
# its CLASS declares, which no FP copy on any shipped target could ever satisfy
# (the class move carries the width-default flags, `xmm`/`v` are 16 bytes) — so
# the whole FP file was vetoed and the identity copies the allocator left behind
# reached the emitted stream, `lir_peephole` R1 being unable to prove them
# no-ops for the same reason. The pins are the PREMISE (the file really is wider
# than the copies), the POSITIVE through the real lowering on BOTH shipped
# targets, and the two-sided NEGATIVE over the published value-width bound —
# hand-built, because whether an unproved copy is dropped must not depend on
# which register the linear scan happened to pick.
dss_add_test(NAME lir/test_lir_copy_coalescing_value_width
             SOURCES test_lir_copy_coalescing_value_width.cpp)

# D-LIR-PEEPHOLE-CALLCONV-IDENTITY-COPY-CLAIM-HAS-NO-INSTRUMENT. The peephole's
# placement argument rests on "callconv mints exactly ZERO identity copies", and
# the evidence it carried was a NET across three passes because no dump stage
# existed between the peephole and callconv. This pins the claim on the pass it
# names — the census read on BOTH SIDES OF CALLCONV ALONE — plus the one-owner
# identity between the census and rule R1, and the fixpoint that makes any later
# residue a refusal rather than a miss.
dss_add_test(NAME lir/test_lir_identity_copy_stage_attribution
             SOURCES test_lir_identity_copy_stage_attribution.cpp)
