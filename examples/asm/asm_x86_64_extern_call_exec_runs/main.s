	# `call putchar` from hand-written assembly, bound by the PLATFORM
	# (D-ASM-EXTERN-CALL-CANNOT-BIND-A-LIBRARY). This example REPLACES
	# `asm_x86_64_extern_call_exec_unbound_error`, which pinned the refusal —
	# the row it pinned is closed, so the pin is superseded rather than re-cut.
	#
	# ★ WHY `putchar` IS THE FIXTURE SYMBOL. `stdio.json` declares it for EVERY
	# object format as an ordinary library row — no `synthesize` recipe, no
	# per-target `linkName` — so ONE source text is realizable on both legs and
	# the only thing that varies is which image the platform names. A symbol
	# with a synthesize recipe would be REFUSED here on purpose: the `encode`
	# tier emits no bodies, so binding pe `printf` would link clean and then
	# die at LOAD with 0xC0000139.
	#
	# ★★ BOTH ARGUMENT REGISTERS ARE SET, DELIBERATELY, AND IT IS NOT
	# REDUNDANCY. Win64 passes the first integer argument in %ecx; SysV passes
	# it in %edi. A `.s` runs BELOW the calling-convention pass — nothing
	# rewrites arguments for it — so setting both is what lets one file assert
	# the same stdout on two ABIs. That is the point of the example: the
	# BINDING is per-platform, the SOURCE is not.
	#
	# The 40-byte frame is Win64's shadow space (32) plus alignment. SysV does
	# not need it and does not mind it.
	.text

	.globl	main
	.type	main, @function
main:
	subq	$40, %rsp
	movl	$42, %ecx
	movl	$42, %edi
	call	putchar
	movl	$0, %eax
	addq	$40, %rsp
	ret
