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
# P68 round 8 part 4: the inline-asm BUNDLE — one statement is one instruction
# while registers are allocated (`lir/lir_asm_region.hpp`): its slots and roles,
# nothing the allocator writes between a template's lines, the output that
# avoids the statement's clobbers, the capacity refusal where the references
# refuse, the one-block expansion, the verifier, the text codec.
dss_add_test(NAME lir/test_lir_asm_region SOURCES test_lir_asm_region.cpp)
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
# D-C-ATOMIC-COMPOUND-ASSIGNMENT-AND-INCREMENT-ARE-A-LOAD-THEN-A-SEPARATE-STORE:
# the under-aligned `AtomicCas` routing — commit through the atomics runtime's
# generic compare-exchange under the same arbiter as the load, each arm with its
# aligned control.
dss_add_test(NAME lir/test_atomic_cas_runtime_routing
             SOURCES test_atomic_cas_runtime_routing.cpp)
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
# program, and the file's own banner carries the reason (the hazard it used to
# cite — a front end lowering a many-arg call to nothing, so the pins measured
# zero — was fixed in P49). The end-to-end evidence is the runtime
# corpus (`examples/c/alignas_local_over32`, `alignas_local_cacheline64`,
# `alignas_member_over32_local`).
dss_add_test(NAME lir/test_lir_overaligned_local_frame
             SOURCES test_lir_overaligned_local_frame.cpp)

# The pin over `lowered_lir_fixture.hpp` ITSELF. `lowerCToLir` used to return a
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

# D-LIR-COALESCE-ANTI-AFFINITY-QUERY-IS-QUADRATIC. The coalescer's anti-affinity
# veto re-scanned the WHOLE forbidden list per query on a written premise that
# the list is "short"; it is Θ(instructions), so the query was Θ(edges ×
# forbidden) and a large straight-line function spent 63% of its compile there.
# The pin asserts the GROWTH RATIO of the algorithm's own probe COUNTER across a
# doubled input — never a wall clock, which is sized on the machine that wrote
# it — plus a control arm on the coalescer's DECISIONS, which the fix must leave
# byte-identical.
dss_add_test(NAME lir/test_lir_coalesce_anti_affinity_complexity
             SOURCES test_lir_coalesce_anti_affinity_complexity.cpp)

# THE PEEPHOLE/CALLCONV IDENTITY-COPY CLAIM HAD NO INSTRUMENT. The peephole's
# placement argument rests on "callconv mints exactly ZERO identity copies", and
# the evidence it carried was a NET across three passes because no dump stage
# existed between the peephole and callconv. This pins the claim on the pass it
# names — the census read on BOTH SIDES OF CALLCONV ALONE — plus the one-owner
# identity between the census and rule R1, and the fixpoint that makes any later
# residue a refusal rather than a miss.
dss_add_test(NAME lir/test_lir_identity_copy_stage_attribution
             SOURCES test_lir_identity_copy_stage_attribution.cpp)

# D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING: a shared library's own call to
# its own definition must be resolved by the LOADER, not branched to the local
# body — otherwise that image answers one identifier differently from every
# other image in the process, silently (MEASURED: a DSS-built ELF `.so` gave
# rc 4 where the gcc-built control gave rc 6; a DSS-built darwin dylib gave rc 1
# where ld64's gave rc 2). Its own target because the routing is a MATRIX —
# (visibility × binding × the format's declaration) — and the over-routing and
# back-compat directions are as load-bearing as the defect itself: routing a
# `static` callee, or routing anything at all in a main executable, costs every
# call an indirection for a definition no loader can replace.
dss_add_test(NAME lir/test_preemptible_definition_routing
             SOURCES test_preemptible_definition_routing.cpp)

# D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING, the ADDRESS half: the same
# artifact must not materialize its own body's ADDRESS for a name its loader may
# replace — `&w` inside the library and `&w` in the executable would then be two
# different pointers for one identifier, where C 6.2.2p2 gives one identifier one
# function/object across the whole program (MEASURED: gcc 13.3.0 and clang 18.1.3
# both GOT-load `&wk`, `&st`, `&wd` and `&sd` in a `.so` while leaving the
# `static` and hidden siblings in the SAME object a bare `lea`). Its own target
# beside the routing matrix because the SUBJECT differs: a call wants the entry
# the format's dispatch names, an address wants the loader-filled slot's CONTENT,
# and under `direct-plt` those are two different VAs of one import.
dss_add_test(NAME lir/test_preemptible_definition_address
             SOURCES test_preemptible_definition_address.cpp)

# D-LIR-SETCC-DEAD-AFTER-FUSION: the use-count gate that stops `lowerICmp` /
# `lowerFCmp` minting a `cmp -> setcc -> zext` trio whose only would-be reader is
# a CondBr that FUSES the compare and re-derives the flags for itself. Its own
# target because the claim is a MATRIX of two arms that must move in opposite
# ways: the fused-only shape loses all three instructions, and the
# second-consumer shape — same CFG, one extra read — must not change by a byte.
# A rule keyed on the `setcc` mnemonic instead of on the use count passes the
# first arm and silently breaks the second (D-LIR-FUSION-USE-COUNT-GATE), so the
# control arm is as load-bearing as the pin.
dss_add_test(NAME lir/test_lir_fused_compare_dce
             SOURCES test_lir_fused_compare_dce.cpp)

# D-LIR-VREG-ID-BITFIELD-TRUNCATES-SILENTLY-PAST-ITS-WIDTH: `LirReg`'s three
# bit-fields spent 31 of 32 bits, capping one function at 2^24 virtual registers
# and TRUNCATING silently past it — two distinct vregs on one id, which liveness
# keys by and `findAllocationConflict` therefore cannot tell apart. Its own
# target because the claim is about a TYPE's capacity, not about a pass: the
# arms are exact integer identities over `LirReg` itself, and the control arm
# (size, class round-trip, the positional-init seal) must stay green when the id
# field is narrowed, so a red here always means "the id got smaller" and never
# "something else broke".
dss_add_test(NAME lir/test_lir_vreg_id_width
             SOURCES test_lir_vreg_id_width.cpp)

# D-LK10-ENTRY-ARM64-WIDE-IMMEDIATE (the frame-offset arm): a frame access BEYOND
# every form the target declares for it — AArch64's unscaled imm9 and scaled
# imm12 — is emitted as `lea A, [base + offset]` plus the access at `[A + 0]`.
# The arms: every reach boundary read from the encoding (both files, narrowest and
# widest access), x86_64 never far, the spill store `ee` measured refused (through
# x30, which the leaf saves), a far integer reload through its own destination
# (and NO fold — the predicate's exactness), a far SIMD&FP reload through x30, a
# big frame with no far access keeping its frame, stacked params above a
# 40000-byte local, and the refusals that remain — each by name. Byte pins are
# words gas 2.42 and clang 18.1.3 assemble identically. mutate_target_schema.hpp
# needs nlohmann_json.
dss_add_test(NAME lir/test_lir_frame_far_form
             SOURCES test_lir_frame_far_form.cpp)
target_link_libraries(dss_lir_test_lir_frame_far_form PRIVATE nlohmann_json::nlohmann_json)

# D-LIR-ASM-MEMORY-RESIDENT-FLOAT-OPERAND-CARRIED-AS-ITS-ADDRESS: an F80/F128
# value (the LD model keeps it in a memory HOME) bound to an inline-asm REGISTER
# travels by that home — the constraint class's load in, its store out, at the
# value's full width — never as the home's address, which the template used to
# receive (rc=0, the wrong answer). Structural arms over hand-built MIR (the C
# fixture declares no long-double format): the input's register defined by a
# 128-bit load, the output stored at 128 bits, a piece and a `+w` tie, an
# `asm goto` output stored on every edge; each beside a `double` control; a
# value WIDER than the register carried as a register PAIR, half by half (it
# was refused until D-ASM-MULTI-REGISTER-OPERAND-BINDING-NOT-REALIZED landed);
# and the x87 value on an XMM register, which no reference carries, refused by
# name — the two last beside a value of the register's own width.
dss_add_test(NAME lir/test_lir_asm_home_carriage
             SOURCES test_lir_asm_home_carriage.cpp)

# D-MIR-ASM-BY-ADDRESS-OPERAND-BOUND-TO-A-REGISTER-RECEIVES-ITS-ADDRESS and
# D-ASM-MULTI-REGISTER-OPERAND-BINDING-NOT-REALIZED: a struct, union, `_Complex`
# or 128-bit integer bound to an inline-asm REGISTER reaches the template as its
# VALUE — one register, or a register PAIR (`%H0` names the second on aarch64) —
# per the target's `asmValueCarriage` (class x kind x size x direction), never as
# its address. Arms: the MIR entry says "carried" (an output too, with no result
# piece and no tied half); a pair loaded half by half and named in order; the
# x86_64 pair's unnamed high half stored back; a 2-byte struct staged into an XMM
# register through a GPR; no frame-address `lea` inside a statement's constrained
# range (valid C used to be refused at `callconv`); four refusals by name, each
# beside an accepted neighbour; the facet's and the `selects` key's load-time
# contract, and a target declaring no carriage refusing the operand; `movaps`
# keyed at 128 bits. mutate_target_schema.hpp and the dialect edits need
# nlohmann_json.
dss_add_test(NAME lir/test_lir_asm_carried_operand
             SOURCES test_lir_asm_carried_operand.cpp)
target_link_libraries(dss_lir_test_lir_asm_carried_operand PRIVATE nlohmann_json::nlohmann_json)


# D-CODEGEN-APPLE-ARM64-X18-ALLOCATED-AGAINST-THE-PLATFORM-RESERVATION: Apple's
# ARM64 platforms reserve x18 (Apple's ABI document; clang's
# `isX18ReservedByDefault`), and `apple_arm64` now names it in no allocatable
# list. Pins read the EMITTED code of a high-pressure function through regalloc,
# the rewriter and callconv: no x18 under `apple_arm64`, the SAME source under
# `aapcs64` (Linux reserves nothing) reaching x18 as the control, and the
# remove-direction mutant — x18 put back into the Apple list — bringing it
# back. mutate_target_schema.hpp needs nlohmann_json.
dss_add_test(NAME lir/test_lir_apple_platform_register
             SOURCES test_lir_apple_platform_register.cpp)
target_link_libraries(dss_lir_test_lir_apple_platform_register PRIVATE nlohmann_json::nlohmann_json)


# D-C-LOCAL-REGISTER-VARIABLE-ASM-LABEL-IGNORED and
# D-ASM-MATCHING-CONSTRAINT-DIGIT-READ-AS-A-MACHINE-LETTER: a GNU local register
# variable binds the inline-asm operand whose value it is (the control: the same
# label without `register` stays a warn-and-drop); a matching constraint ties
# the input to its output at the input's own width; an unwritten variable is read
# as it stands; a copy into a bound `sp` is the stack-pointer copy; a 16-byte
# variable continues in the register its target declares, and one that
# continues in nothing refuses by name; a leaf binding the link register saves
# it (the emitted frame, beside a control that saves nothing); `svc #imm`; every
# refusal the references share, by code; `continuesIn`'s load-time contract.
# mutate_target_schema.hpp needs nlohmann_json.
dss_add_test(NAME lir/test_lir_asm_register_variable
             SOURCES test_lir_asm_register_variable.cpp)
target_link_libraries(dss_lir_test_lir_asm_register_variable PRIVATE nlohmann_json::nlohmann_json)


# D-LIR-AAPCS64-CALL-MIXING-LONG-DOUBLE-AND-DOUBLE-ARGS-REFUSED: an AArch64 call
# passing a binary128 `long double` AND a `float`/`double` lowers through ONE
# placer — the marshalled F128 keeps its position in the call's operand list as
# the register it was loaded into, so `lir_callconv`'s one `ArgCursors` walk
# gives every later FP argument the next register, and the marshal loads carry
# a constraint naming the registers they write. Arms over hand-built MIR: the
# operand list and the constraint (both orders), the EMITTED call site through
# allocation and callconv (both orders and an interleaved float/int mix); and
# (P68 round 8: D-LIR-AAPCS64-LONG-DOUBLE-ARG-PAST-V7-REFUSED,
# D-LIR-AAPCS64-COMPLEX-LONG-DOUBLE-RETURN-PIECE-REFUSED) a binary128 past v7
# STORED on the stack 16-aligned and 16 wide, the cursor arithmetic for a scalar
# wider than the slot, the callee's 128-bit `arg`, a `_Complex long double`
# result captured from q0 and q1, and a refused piece naming its register.
dss_add_test(NAME lir/test_lir_aapcs64_mixed_fp_call
             SOURCES test_lir_aapcs64_mixed_fp_call.cpp)

# D-LIR-LONG-DOUBLE-LOAD-READ-THE-OBJECT-AFTER-IT-WAS-OVERWRITTEN and
# D-LIR-VOLATILE-LONG-DOUBLE-ACCESS-MOVED-ITS-ADDRESS-INTO-A-FLOAT-REGISTER
# (P68 round 8): a memory-resident `long double` (x87 F80 on x86_64, binary128
# F128 on arm64) is a VALUE — over hand-built MIR, a swap never reads an object
# after overwriting it, a volatile read's qualifier conversion defines no
# floating-point register, and a bit-cast to another kind is refused by name.
dss_add_test(NAME lir/test_lir_long_double_value_semantics
             SOURCES test_lir_long_double_value_semantics.cpp)
