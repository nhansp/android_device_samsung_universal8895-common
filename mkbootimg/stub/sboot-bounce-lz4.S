/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Compressed bounce stub: S-Boot loads it as the "kernel", with an "lz4 -l" frame of the arm64
 * Image right behind it. The stub decompresses the Image to DEST, verifies it and branches there
 * with the state S-Boot handed over (x0-x3, MMU and caches off, SCTLR/TCR/TTBR0/MAIR/VBAR/DAIF/SP
 * restored, D-cache cleaned and invalidated to PoC, I-cache invalidated); only x4-x30 differ.
 *
 * Entry: EL1, MMU and caches off, x0 = DTB, x3 = L = 0x80080000 + n * 0x4000 (n in 0..63).
 * The MMU is turned on with an identity map while decoding: with it off every access is
 * Device-nGnRnE and a byte-wise decode of ~41 MB would take tens of seconds.
 *
 * Memory map:
 *   L - 0x800 ..            S-Boot image buffer (header page, this stub and the frame, ramdisk,
 *                           DTBH), at most the partition size: ends below SCRATCH
 *   0x83E00000..0x83E20000  SCRATCH: L1 and L2 tables, saved state, stack, status record
 *   0x84000000..            DEST: decompressed Image; DEST + image_size must end below INITRD_PA
 *   0x89000000..0x8a000000  ramdisk
 *   0x8a000000..0x8a100000  DTB copy
 *   0x8d000000              ATAG list
 *
 * A failed check records a status block at STATUS and parks the CPU in wfe, never branching into
 * DEST; a private VBAR records ESR/FAR/ELR in the same block.
 */
#include "sboot-bounce-lz4.h"

	.macro	movimm reg, val		// 32-bit constant, position independent
	movz	\reg, #(((\val) >> 16) & 0xffff), lsl #16
	movk	\reg, #((\val) & 0xffff)
	.endm

	.section .text.entry, "ax"
	.globl	_start
_start:
	b	entry

	.balign	16
/* Header (offset 0x10): comp_len, orig_len and crc32 are patched at image build; unpatched, comp_len
 * is 0 and the stub refuses to run */
	.globl	stub_hdr
stub_hdr:
	.ascii	"GLLZ4STB"
	.quad	DEST			// 0x18
	.quad	0			// 0x20 comp_len: bytes of the lz4 frame following the stub
	.quad	0			// 0x28 orig_len: exact length of the decompressed Image
	.word	0			// 0x30 crc32 of the decompressed Image (zlib CRC-32)
	.word	0			// 0x34 reserved, zero

	.globl	entry
entry:
	movimm	x4, SCRATCH
	mrs	x5, CurrentEL
	cmp	x5, #4			// EL1 only
	b.ne	1f
	add	x5, x4, #(CTX - SCRATCH)	// saved state (aligned 8-byte stores: MMU is off)
	stp	x0, x1, [x5, #CTX_X0]
	stp	x2, x3, [x5, #CTX_X2]
	mrs	x6, sctlr_el1
	mrs	x7, ttbr0_el1
	stp	x6, x7, [x5, #CTX_SCTLR]
	mrs	x6, tcr_el1
	mrs	x7, mair_el1
	stp	x6, x7, [x5, #CTX_TCR]
	mrs	x6, vbar_el1
	mrs	x7, daif
	stp	x6, x7, [x5, #CTX_VBAR]
	mov	x6, sp
	str	x6, [x5, #CTX_SP]
	msr	daifset, #0xf
	add	x6, x4, #(STACK_TOP - SCRATCH)
	mov	sp, x6
	adr	x0, _start
	bl	stub_c			// returns only if DEST holds a verified Image
	b	tail
1:	add	x6, x4, #(STACK_TOP - SCRATCH)
	mov	sp, x6
	mov	x0, #1			// code 1: not EL1
	bl	stub_fail
	b	.

/* Restore the entered state from registers only: MMU and caches are on, DEST is dirty in the D-cache */
tail:
	movimm	x4, SCRATCH
	add	x5, x4, #(CTX - SCRATCH)
	ldp	x0, x1, [x5, #CTX_X0]
	ldp	x2, x3, [x5, #CTX_X2]
	ldp	x9, x10, [x5, #CTX_SCTLR]	// original SCTLR (M=C=I=0), TTBR0
	ldp	x11, x12, [x5, #CTX_TCR]	// TCR, MAIR
	ldp	x13, x14, [x5, #CTX_VBAR]	// VBAR, DAIF
	ldr	x15, [x5, #CTX_SP]
	ldp	x16, x17, [x5, #CTX_DEST]	// dest, orig_len
	ldp	x19, x20, [x5, #CTX_SRC]	// L, stub_len + comp_len
	mrs	x21, ctr_el0
	ubfx	x21, x21, #16, #4		// DminLine, log2(words)
	mov	x22, #4
	lsl	x22, x22, x21			// D-cache line size in bytes
	sub	x23, x22, #1

	// clean+invalidate to PoC by VA: DEST image, the whole scratch area, the stub+frame
	mov	x24, x16
	add	x25, x16, x17
	bic	x24, x24, x23
2:	dc	civac, x24
	add	x24, x24, x22
	cmp	x24, x25
	b.lo	2b
	mov	x24, x4
	add	x25, x4, #SCRATCH_SIZE
	bic	x24, x24, x23
3:	dc	civac, x24
	add	x24, x24, x22
	cmp	x24, x25
	b.lo	3b
	mov	x24, x19
	add	x25, x19, x20
	bic	x24, x24, x23
4:	dc	civac, x24
	add	x24, x24, x22
	cmp	x24, x25
	b.lo	4b
	dsb	sy

	msr	sctlr_el1, x9			// M=C=I=0 again (value S-Boot left)
	isb
	ic	iallu				// no stale instructions for DEST (and none for the stub)
	tlbi	vmalle1
	dsb	sy
	isb
	msr	ttbr0_el1, x10
	msr	tcr_el1, x11
	msr	mair_el1, x12
	msr	vbar_el1, x13
	msr	daif, x14
	mov	sp, x15
	isb
	br	x16

/* Exception vectors (VBAR_EL1 while the stub runs): record the slot, ESR, FAR and ELR and park */
	.section .vectors, "ax"
	.balign	2048
	.macro	vec idx
	.balign	128
	mov	x0, #\idx
	b	vec_common
	.endm
vectors:
	vec 0
	vec 1
	vec 2
	vec 3
	vec 4
	vec 5
	vec 6
	vec 7
	vec 8
	vec 9
	vec 10
	vec 11
	vec 12
	vec 13
	vec 14
	vec 15

	.section .text, "ax"
vec_common:
	movimm	x4, STATUS
	mrs	x5, esr_el1
	mrs	x6, far_el1
	mrs	x7, elr_el1
	mov	x8, #8
	str	x8, [x4, #8]		// code 8: exception
	stp	x5, x6, [x4, #16]
	stp	x7, x0, [x4, #32]
	ldr	x5, =0x315453345A4C4C47	// "GLLZ4ST1" little-endian: struct status.magic
	str	x5, [x4]
	dc	civac, x4
	dsb	sy
5:	wfe
	b	5b
	.ltorg
