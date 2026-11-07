/* SPDX-License-Identifier: GPL-2.0-only */
/* sboot-bounce-lz4 - constants shared by the .S, the .c and (as comments) lib/stub.py. */
#ifndef GL_SBOOT_BOUNCE_LZ4_H
#define GL_SBOOT_BOUNCE_LZ4_H

#define DEST		0x84000000	/* decompressed Image lands here: same address as sboot-bounce.S */
#define INITRD_PA	0x89000000	/* S-Boot copies the ramdisk here: DEST + image_size must stay below */
#define SCRATCH		0x83E00000	/* 128 KiB scratch, see the memory map in sboot-bounce-lz4.S */
#define SCRATCH_SIZE	0x20000
#define PT_L1		(SCRATCH + 0x0000)	/* level-1 table, 512 x 8 B */
#define PT_L2		(SCRATCH + 0x1000)	/* level-2 table covering 0x80000000..0xBFFFFFFF in 2 MiB blocks */
#define CTX		(SCRATCH + 0x2000)	/* saved entry state, see CTX_* */
#define STACK_TOP	(SCRATCH + 0x1E000)	/* stack grows down to SCRATCH + 0x3000 */
#define STATUS		(SCRATCH + 0x1F000)	/* post-mortem record, see struct status in the .c */
#define MAP_BASE	0x80000000		/* first byte of the 1 GiB the level-2 table maps */

/* offsets in the saved-state block at CTX */
#define CTX_X0		0
#define CTX_X1		8
#define CTX_X2		16
#define CTX_X3		24
#define CTX_SCTLR	32
#define CTX_TTBR0	40
#define CTX_TCR		48
#define CTX_MAIR	56
#define CTX_VBAR	64
#define CTX_DAIF	72
#define CTX_SP		80
#define CTX_DEST	88	/* filled by stub_c on success */
#define CTX_DESTLEN	96
#define CTX_SRC		104	/* load address L: the stub and the compressed frame live at L.. */
#define CTX_SRCLEN	112	/* stub_len + comp_len */

#endif
