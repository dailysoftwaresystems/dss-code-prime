// `cset` AT BOTH WIDTHS, WITH THE UNSIGNED CONDITIONS, EXECUTING.
//
// Six `cset` results are weighted onto DISTINCT POWERS OF TWO and summed, so
// every one of them is individually load-bearing and any single wrong answer
// produces its own distinct exit code rather than a shared "not 42".
	.text

	.globl	main
	.type	main, %function
main:
	// ── X-form (64-bit). x0 = -1, compared against 1. This is the value
	// where SIGNED and UNSIGNED disagree: as a signed integer -1 < 1, as an
	// unsigned one 0xFFFFFFFFFFFFFFFF > 1. A table that mapped gas's `hi`
	// onto the substrate's `sgt` (instead of `ugt`) inverts x1 and x2 below.
	mov	x0, #1
	neg	x0, x0			// x0 = -1 = 0xFFFFFFFFFFFFFFFF
	cmp	x0, #1
	cset	x1, hi			// UNSIGNED >   -> 1   (substrate ugt)
	cset	x2, gt			// SIGNED   >   -> 0   (substrate sgt)
	cset	x3, lo			// UNSIGNED <   -> 0   (substrate ult)
	cset	x4, hs			// UNSIGNED >=  -> 1   (substrate uge)

	// ── W-form (32-bit). `cset w<n>, cc` is a DIFFERENT INSTRUCTION from
	// `cset x<n>, cc` (0x1A9F07E0 vs 0x9A9F07E0) and until the target
	// declared both widths this line was a LOUD REFUSAL, not a wrong
	// encoding. Its presence here is what keeps the W-form on a running path.
	mov	w5, #1
	mov	w6, #2
	cmp	w5, w6
	cset	w7, lo			// UNSIGNED <   -> 1   (substrate ult)
	cset	w8, hi			// UNSIGNED >   -> 0   (substrate ugt)

	// ── weights: the three EXPECTED-1 results carry 32/8/2 (summing to 42)
	// and the three EXPECTED-0 results carry 1/4/16. Any single condition
	// answering wrongly moves the exit code by its own unique amount.
	mov	x9, #32
	mul	x1, x1, x9		// hi(X)  expect 1 -> 32
	mov	x9, #8
	mul	x4, x4, x9		// hs(X)  expect 1 ->  8
	mov	x9, #2
	mul	x7, x7, x9		// lo(W)  expect 1 ->  2
	mov	x9, #4
	mul	x3, x3, x9		// lo(X)  expect 0 ->  4 if wrongly set
	mov	x9, #16
	mul	x8, x8, x9		// hi(W)  expect 0 -> 16 if wrongly set
					// x2      gt(X)  expect 0 ->  1 if wrongly set
	add	x0, x1, x4
	add	x0, x0, x7
	add	x0, x0, x2
	add	x0, x0, x3
	add	x0, x0, x8
	ret
