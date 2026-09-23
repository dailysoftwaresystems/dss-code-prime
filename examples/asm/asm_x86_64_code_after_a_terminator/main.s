# CODE AFTER A TERMINATOR, WITH NO LABEL BETWEEN THEM
# (D-ASM-INSTRUCTION-AFTER-A-TERMINATOR-REFUSED), and a conditional branch's
# fall-through laid out where the text puts it
# (D-ASM-CONDITIONAL-BRANCH-FALLTHROUGH-LAID-OUT-AT-THE-FUNCTION-END).
# The assembler emits every line where it stands; nothing branches to a line
# that follows `jmp` or `ret` without a label, so it never runs. Each dead line
# below changes the exit code if it does:
#   * `addl $100` after `jmp 1f`                     (exit 142)
#   * `addl $1000` after the first `ret`             (never reached: ret)
#   * `nop` + `addl $7` after the LAST `ret`, ending `main`
# `jne 2f` falls into `addl $2`, the next line: taken, the exit is 1.
# gas 2.42 and clang 18.1.3 both run this to 42 (measured 2026-09-23).
	.text
	.globl	main
	.type	main, @function
main:
	movl	$40, %eax
	jmp	1f
	addl	$100, %eax
1:	cmpl	$40, %eax
	jne	2f
	addl	$2, %eax
	ret
	addl	$1000, %eax
2:	movl	$1, %eax
	ret
	nop
	addl	$7, %eax
