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
dss_add_test(NAME lir/test_lir_text      SOURCES test_lir_text.cpp)
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
