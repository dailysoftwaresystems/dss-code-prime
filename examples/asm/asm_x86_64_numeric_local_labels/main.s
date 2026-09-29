# GNU as's NUMERIC LOCAL LABELS (D-ASM-LABELS-INSIDE-A-TEMPLATE-AND-NUMERIC-LOCAL-LABELS-REFUSED).
# `N:` may be defined any number of times; `Nb` names the nearest `N:` BEFORE
# the reference and `Nf` the nearest one AFTER it (GNU as, "Local Symbol
# Names"). Every shape below is load-bearing — a wrong resolution changes the
# exit code:
#   * a loop on `1:` / `1b`                                   (eax 0 -> 6)
#   * `jmp 9f` written AFTER one `9:`, which must reach the NEXT `9:` and never
#     go back to the earlier one, and `jl 9b` looping on the NEAREST `9:`
#   * label 0 reached through `0f` and `0b`, beside the binary literal
#     `0b10101` (21): `0b`/`0f` are label 0, `0b101…` stays a number
# gas 2.42 and clang 18.1.3 both run this to 42 (measured 2026-09-23).
	.text
	.globl	main
	.type	main, @function
main:
	xorl	%eax, %eax
	movl	$3, %ecx
1:	addl	$2, %eax
	subl	$1, %ecx
	jnz	1b
9:	addl	$1, %eax
	cmpl	$7, %eax
	jne	8f
	jmp	9f
9:	addl	$7, %eax
	cmpl	$21, %eax
	jl	9b
	jmp	0f
0:	addl	$0b10101, %eax
	cmpl	$42, %eax
	jg	8f
	jl	0b
	ret
8:	movl	$1, %eax
	ret
