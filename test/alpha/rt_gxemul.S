/*
 *  Runtime for the Alpha instruction tests, gxemul "testalpha" machine.
 *
 *  The program runs in kernel mode, linked in KSEG. Output goes to the
 *  test machine's console device, one longword store per character.
 */


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
	bsr	$26, cons_base
	stl	$31, 0x10($0)		/*  DEV_CONS_HALT  */
2:	br	2b
	.end	rt_exit

/*  v0 = KSEG address of the console device (0xfffffc0010000000).  */
	.ent	cons_base
cons_base:
	lda	$0, -1024($31)		/*  0xfffffffffffffc00  */
	sll	$0, 32, $0		/*  0xfffffc0000000000  */
	ldah	$0, 0x1000($0)		/*  + 0x10000000  */
	ret	$31, ($26), 1
	.end	cons_base

/*  rt_putc(a0 = character).  Clobbers v0, t0.  */
	.globl	rt_putc
	.ent	rt_putc
rt_putc:
	mov	$26, $1
	bsr	$26, cons_base
	mov	$1, $26
	stl	$16, 0($0)
	ret	$31, ($26), 1
	.end	rt_putc

/*  rt_flush(): nothing to do.  */
	.globl	rt_flush
	.ent	rt_flush
rt_flush:
	ret	$31, ($26), 1
	.end	rt_flush

	.bss
	.align	4
	.space	65536
stack_top:
