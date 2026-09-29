// CODE AFTER A TERMINATOR, WITH NO LABEL BETWEEN THEM
// (D-ASM-INSTRUCTION-AFTER-A-TERMINATOR-REFUSED), and a conditional branch's
// fall-through laid out where the text puts it
// (D-ASM-CONDITIONAL-BRANCH-FALLTHROUGH-LAID-OUT-AT-THE-FUNCTION-END), on the
// second dialect. Each dead line below changes the exit code if it runs:
//   * `add #100` after `b 1f`                        (exit 142)
//   * `add #1000` after the first `ret`              (never reached: ret)
//   * `nop` + `add #7` after the LAST `ret`, ending `main`
// `b.ne 2f` falls into `add #2`, the next line: taken, the exit is 1.
// aarch64-linux-gnu-as 2.42 and clang 18.1.3 both run this to 42 under qemu
// (measured 2026-09-23).
	.text
	.globl	main
	.type	main, %function
main:
	mov	w0, #40
	b	1f
	add	w0, w0, #100
1:	cmp	w0, #40
	b.ne	2f
	add	w0, w0, #2
	ret
	add	w0, w0, #1000
2:	mov	w0, #1
	ret
	nop
	add	w0, w0, #7
