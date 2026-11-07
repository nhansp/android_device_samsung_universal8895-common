/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sboot-bounce-lz4.S - compressed variant of sboot-bounce.S (gl-pack --bounce-lz4).
 *
 * S-Boot loads this stub as the "kernel" exactly like the plain bounce stub; behind it (stub_len
 * bytes later) sits an "lz4 -l -12" frame of the arm64 Image. The stub decompresses it to DEST
 * (0x84000000, the same destination as sboot-bounce.S), verifies it, and branches to DEST with the
 * same register and system state the plain stub leaves: x0-x3 as S-Boot passed them (x0 = DTB), MMU and
 * caches off, SCTLR/TCR/TTBR0/MAIR/VBAR/DAIF/SP as on entry, D-cache cleaned+invalidated to PoC over
 * everything the stub touched, I-cache invalidated. Only x4-x30 differ.
 *
 * ENTRY STATE (spec/L0_boot.md 3.1 steps 10-11, SB = factory sboot.bin): EL1, MMU and caches off
 * (SB:0xc900a874 -> SB:0xc903e7a8 clear SCTLR_EL1 C/I/M after set/way clean+invalidate), x0 = DTB,
 * x1 = 0x80000000, x2 = L - 0x80000000, x3 = L (blr x3), L = 0x80080000 + n*0x4000, n in 0..63.
 *
 * WHY THE MMU IS TURNED ON: with SCTLR_EL1.M = 0 every data access is Device-nGnRnE: strictly ordered,
 * uncached and alignment-checked. A byte-wise LZ4 decoder over 41 MB of Device memory would take tens
 * of seconds. The stub therefore builds a 2-level identity map (below), turns on M, C and I, decodes
 * and checksums at full speed, cleans+invalidates by VA to PoC, and turns everything off again.
 *
 * MEMORY MAP (every address with its source). Nothing the stub writes overlaps anything it reads
 * later, anything S-Boot placed, or anything the kernel needs:
 *
 *   0x80001000..0x80002000  S-Boot "kaslr" breadcrumb (SB:0xc900abd8-0xc900ac20); mapped read-only
 *                           in effect (the stub never writes below SCRATCH)
 *   L - 0x800 ..            S-Boot image buffer: header page at L-0x800, then kernel (this stub + the
 *                           frame), ramdisk, DTBH, all read from the partition in one piece
 *                           (SB:0xc900affc-b000 "ldr x0,[x19,#0x1e0]; sub x0,x0,#0x800"). Its size is
 *                           the packed image <= the partition: RECOVERY 48,234,496 B, BOOT 41,943,040 B
 *                           (bootimg.PART_BYTES). Worst end = (0x80080000 + 63*0x4000) - 0x800
 *                           + 48,234,496 = 0x8017B800 + 0x2DF8000 = 0x82F73800 < SCRATCH.
 *                           gl-pack refuses images that do not fit the partition, so this bound holds.
 *   0x83E00000..0x83E20000  SCRATCH (128 KiB): +0x0000 L1 table, +0x1000 L2 table, +0x2000 saved
 *                           state, +0x3000..+0x1E000 stack, +0x1F000 status record. Above the worst
 *                           image end (0x82F73800), below DEST. Nothing in the spec's S-Boot flow
 *                           (3.1 steps 1-12) uses 0x83xxxxxx: the ATAG list is at 0x8d000000, the DTB
 *                           at 0x8a000000 (1 MiB buffer), the ramdisk at 0x89000000.
 *   0x84000000..            DEST: decompressed Image, orig_len bytes (exact Image length), orig_len
 *                           <= 0x89000000 - DEST. The kernel's whole footprint, DEST + image_size
 *                           (Image header offset 16), must also end <= INITRD_PA; the stub checks it
 *                           from the decoded header before it jumps, and gl-pack checks it when packing.
 *   0x89000000..0x8a000000  ramdisk (SB:0xc900b00c "mov x0,#0x89000000"), at most 16 MiB
 *   0x8a000000..0x8a100000  DTB copy (SB:0xc90052ec), x0 on entry
 *   0x8d000000              ATAG list (SB:0xc90052e4-0xc90052f8)
 *
 * Writes: SCRATCH..+128K and DEST..DEST+orig_len. Reads: the stub and the frame at L.., which end
 * below SCRATCH (checked at run time as well: src_end <= SCRATCH). The decoder cannot read past the
 * frame (every input read is bounds-checked, lz4dec.c) nor write past orig_len.
 *
 * FAILURE: any failed check records a status block at STATUS (magic, code, details) and parks the CPU
 * in a wfe loop, never branching into DEST. Exceptions are caught by a private VBAR (below), which
 * records ESR/FAR/ELR in the same block.
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
/* Header (offset 0x10). gl-pack patches comp_len, orig_len, crc32; unpatched, comp_len == 0 and the
 * stub refuses to run. dest is a constant the stub verifies against DEST. */
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
	cmp	x5, #4			// EL1 only: S-Boot hands over at EL1 (3.1 step 11)
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

/*
 * tail: leave in registers only. At this point MMU, D- and I-cache are on, DEST holds the Image
 * (dirty in the D-cache) and stub_c has filled CTX_DEST/DESTLEN/SRC/SRCLEN.
 */
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

/*
 * Exception vectors (2 KiB aligned, VBAR_EL1 while the stub runs). Every entry records which of the
 * 16 slots fired and parks. Slot index in x0, then the common handler writes
 * STATUS{magic, code 8, ESR, FAR, ELR, slot}.
 */
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
