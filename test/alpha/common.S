/*
 *  Common code for the Alpha instruction tests: print the result array.
 *
 *  print_results(a0 = array of quadwords, a1 = number of quadwords)
 *  prints one 16-digit hex number per line.
 */

	.set	noreorder
	.set	noat
	.text

	.globl	print_results
	.ent	print_results
print_results:
	lda	$30, -32($30)
	stq	$26, 0($30)
	stq	$9, 8($30)
	stq	$10, 16($30)
	stq	$11, 24($30)
	mov	$16, $9
	mov	$17, $10
3:	beq	$10, 9f
	ldq	$11, 0($9)
	lda	$12, 60($31)		/*  shift count  */
4:	srl	$11, $12, $16
	and	$16, 15, $16
	cmpult	$16, 10, $1
	addq	$16, 0x30, $16
	bne	$1, 5f
	addq	$16, 0x27, $16		/*  'a' - '0' - 10  */
5:	bsr	$26, rt_putc
	subq	$12, 4, $12
	bge	$12, 4b
	lda	$16, 10($31)
	bsr	$26, rt_putc
	lda	$9, 8($9)
	subq	$10, 1, $10
	br	3b
9:	ldq	$26, 0($30)
	ldq	$9, 8($30)
	ldq	$10, 16($30)
	ldq	$11, 24($30)
	lda	$30, 32($30)
	ret	$31, ($26), 1
	.end	print_results
