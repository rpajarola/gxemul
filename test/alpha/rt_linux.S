/*
 *  Runtime for the Alpha instruction tests, Linux user mode (used to run
 *  the tests under "qemu-alpha -cpu ev4-alpha-cpu" for reference output).
 *
 *  Characters are collected in a buffer and written at the end. Byte
 *  stores are done with ldq_u/insbl/mskbl/stq_u (EV4 has no stb).
 */

	.equ	SYS_exit, 1
	.equ	SYS_write, 4

	.set	noreorder
	.set	noat
	.text

	.globl	_start
	.ent	_start
_start:
	br	$29, 1f
1:	ldgp	$29, 0($29)
	lda	$30, stack_top
	bsr	$26, test_main
	/*  Fall through to rt_exit.  */
	.end	_start

	.globl	rt_exit
	.ent	rt_exit
rt_exit:
	bsr	$26, rt_flush
	mov	0, $16
	lda	$0, SYS_exit($31)
	call_pal 0x83			/*  callsys  */
2:	br	2b
	.end	rt_exit

/*  rt_putc(a0 = character).  Clobbers t0..t4.  */
	.globl	rt_putc
	.ent	rt_putc
rt_putc:
	lda	$1, outpos
	ldq	$2, 0($1)
	lda	$3, outbuf
	addq	$3, $2, $3		/*  t2 = byte address  */
	ldq_u	$4, 0($3)
	mskbl	$4, $3, $4
	insbl	$16, $3, $5
	bis	$4, $5, $4
	stq_u	$4, 0($3)
	addq	$2, 1, $2
	stq	$2, 0($1)
	ret	$31, ($26), 1
	.end	rt_putc

/*  rt_flush(): write the buffer to stdout.  */
	.globl	rt_flush
	.ent	rt_flush
rt_flush:
	lda	$1, outpos
	ldq	$18, 0($1)
	lda	$17, outbuf
	mov	1, $16
	lda	$0, SYS_write($31)
	call_pal 0x83
	lda	$1, outpos
	stq	$31, 0($1)
	ret	$31, ($26), 1
	.end	rt_flush

	.data
	.align	3
outpos:	.quad	0

	.bss
	.align	4
outbuf:	.space	1048576
	.space	65536
stack_top:
