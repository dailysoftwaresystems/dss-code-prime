# THE x86-64 WINDOWS ROUTINES WHOSE REGISTER CONTRACT NO C FUNCTION KEEPS -- DSS's half of what each reference
# toolchain's own static runtime defines for the objects it compiles (MSVC's CRT objects, clang-cl's, libgcc's). A
# foreign object's code calls them BY NAME, with a contract narrower than the calling convention, so they are written
# here instruction by instruction; the rest of the family (the cookie itself, its seeding, the reporters, the
# exception-time cookie checks) keeps the ordinary convention and is C, in `pe_compiler_support.c`, a unit of the same
# `compilerSupport` row.
#
# No routine here writes unwind data: none of them moves the stack pointer around a call, and `___chkstk_ms`, which
# does move it, calls nothing -- the shape of gcc's own (MEASURED 2026-10-28: libgcc.a's _chkstk_ms.o carries no
# .pdata and no .xdata).

	.text

# __chkstk -- the stack probe cl and clang-cl call from the prologue of a function whose frame exceeds a page, with
# the frame's size in %rax. Every page from the caller's stack pointer down to the lowest byte the frame will own is
# touched in order, so the guard page below the committed stack moves one page at a time; the stack pointer itself
# is not moved (the caller subtracts %rax after the call). Microsoft's documented contract: nothing but %r10, %r11
# and the flags changes. A size past the address space probes down to address zero, which faults, as an
# exhausted stack must.
	.globl	__chkstk
	.type	__chkstk, @function
__chkstk:
	leaq	8(%rsp), %r10
	movq	%r10, %r11
	subq	%rax, %r11
	jae	1f
	xorq	%r11, %r11
1:	andq	$-4096, %r11
	andq	$-4096, %r10
2:	cmpq	%r11, %r10
	je	3f
	subq	$4096, %r10
	orq	$0, (%r10)
	jmp	2b
3:	ret

# ___chkstk_ms -- the same probe as gcc calls it (mingw-w64 objects), with the size in %rax. gcc's contract is
# wider: EVERY register is preserved (only the flags change), so the two scratch registers are saved below the
# return address and restored before it.
	.globl	___chkstk_ms
	.type	___chkstk_ms, @function
___chkstk_ms:
	subq	$16, %rsp
	movq	%r10, (%rsp)
	movq	%r11, 8(%rsp)
	leaq	24(%rsp), %r10
	movq	%r10, %r11
	subq	%rax, %r11
	jae	1f
	xorq	%r11, %r11
1:	andq	$-4096, %r11
	andq	$-4096, %r10
2:	cmpq	%r11, %r10
	je	3f
	subq	$4096, %r10
	orq	$0, (%r10)
	jmp	2b
3:	movq	(%rsp), %r10
	movq	8(%rsp), %r11
	addq	$16, %rsp
	ret

# __security_check_cookie -- called by a /GS function's epilogue with the frame's cookie (already combined with the
# frame's stack pointer) in %rcx, AFTER the function's return value is in %rax or %xmm0. cl treats the call as one of
# its own convention, keeping %rax live across it, so the check changes NOTHING but the flags on the path that
# returns -- the compare reads the cookie in place, touching only what MSVC's own routine touches (%rcx, which it
# restores on its failure path, as the review of P69 fold H5 READ vcruntime's amd64 routine). A cookie that does not match is
# the report, which never returns.
	.globl	__security_check_cookie
	.type	__security_check_cookie, @function
__security_check_cookie:
	cmpq	__security_cookie(%rip), %rcx
	jne	1f
	ret
1:	jmp	__report_gsfailure
