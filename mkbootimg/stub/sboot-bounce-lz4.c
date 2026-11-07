// SPDX-License-Identifier: GPL-2.0-only
/*
 * sboot-bounce-lz4.c - C half of the compressed bounce stub; the memory map and the register
 * contract are documented in sboot-bounce-lz4.S. Freestanding, position independent, no data.
 * stub_c() runs with MMU off until mmu_on(), on the stack the .S set up in SCRATCH.
 */
#include "lz4dec.h"
#include "sboot-bounce-lz4.h"

typedef uint64_t u64;
typedef uint32_t u32;

struct hdr {			/* stub_hdr in the .S, patched by gl-pack (lib/stub.py) */
	char magic[8];
	u64 dest;
	u64 comp_len;
	u64 orig_len;
	u32 crc32;
	u32 reserved;
};

struct status {			/* post-mortem record at STATUS; survives a forced reset in DRAM, best effort */
	u64 magic;		/* "GLLZ4ST1" */
	u64 code;		/* see FAIL_* */
	u64 a, b;		/* code-specific details */
	u64 c, d;
};

enum {
	FAIL_EL = 1,		/* not running at EL1 */
	FAIL_HDR = 2,		/* header unpatched, wrong dest, or sizes out of range */
	FAIL_RANGE = 3,		/* load address / frame end outside the memory map */
	FAIL_DECODE = 4,	/* decoder error: a = error, b = byte offset of the failing block */
	FAIL_CRC = 5,		/* a = computed, b = expected */
	FAIL_IMAGE = 6,		/* decoded data is not an arm64 Image that fits below the ramdisk */
	FAIL_EXC = 8,		/* exception (written by the vector code) */
};

extern const struct hdr stub_hdr;
extern char _start[], __stub_end[], __vectors[];

#define STATUS_MAGIC 0x315453345A4C4C47ull	/* "GLLZ4ST1" */

static inline void dc_civac(const void *p)
{
	asm volatile("dc civac, %0" :: "r"(p) : "memory");
}

void stub_fail(u64 code, u64 a, u64 b);	/* never returns; also callable with the MMU off */

void stub_fail(u64 code, u64 a, u64 b)
{
	volatile struct status *s = (volatile struct status *)STATUS;

	s->code = code;
	s->a = a;
	s->b = b;
	s->magic = STATUS_MAGIC;
	dc_civac((const void *)s);
	asm volatile("dsb sy" ::: "memory");
	for (;;)
		asm volatile("wfe");
}

/*
 * Identity map: L1[2] (VA 0x80000000..0xBFFFFFFF) -> L2; L2[i] = 2 MiB Normal WB block at
 * MAP_BASE + i*2MiB for MAP_BASE .. map_end. Everything else is unmapped. Volatile 8-byte stores:
 * the MMU is still off, so the accesses are Device and must be aligned and must not become memset.
 */
static void build_tables(u64 map_end)
{
	volatile u64 *l1 = (volatile u64 *)PT_L1, *l2 = (volatile u64 *)PT_L2;
	u64 i, n = (map_end - MAP_BASE) >> 21;

	for (i = 0; i < 512; i++) {
		l1[i] = 0;
		l2[i] = 0;
	}
	l1[(MAP_BASE >> 30) & 0x1ff] = PT_L2 | 3;
	for (i = 0; i < n; i++)		/* AF | SH inner | AttrIdx 1 | block */
		l2[i] = (MAP_BASE + (i << 21)) | 0x705;
}

static void mmu_on(void)
{
	u64 mmfr0, ips, tcr, sctlr;

	asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
	ips = mmfr0 & 7;
	if (ips > 5)
		ips = 5;
	/* T0SZ 25 (39-bit VA, start at level 1), WB-WA inner+outer, inner shareable, 4K granule,
	 * TTBR1 walks disabled */
	tcr = 25 | (1ull << 8) | (1ull << 10) | (3ull << 12) | (25ull << 16) | (1ull << 23) |
	      (2ull << 30) | (ips << 32);
	asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
	sctlr &= ~((1ull << 1) | (1ull << 19) | (1ull << 24) | (1ull << 25));	/* A, WXN, E0E, EE off */
	sctlr |= (1ull << 0) | (1ull << 2) | (1ull << 12);			/* M, C, I */
	asm volatile(
		"msr mair_el1, %[mair]\n"	/* attr0 Device-nGnRnE, attr1 Normal WB RW-allocate */
		"msr tcr_el1, %[tcr]\n"
		"msr ttbr0_el1, %[ttbr]\n"
		"isb\n"
		"ic iallu\n"
		"tlbi vmalle1\n"
		"dsb sy\n"
		"isb\n"
		"msr sctlr_el1, %[sctlr]\n"
		"isb\n"
		:: [mair] "r"(0xff00ull), [tcr] "r"(tcr), [ttbr] "r"((u64)PT_L1), [sctlr] "r"(sctlr)
		: "memory");
}

void stub_c(u64 base)
{
	volatile struct hdr *h = (volatile struct hdr *)&stub_hdr;
	u64 stub_len = (u64)(__stub_end - _start);
	u64 comp_len = h->comp_len, orig_len = h->orig_len;
	const uint8_t *src;
	uint8_t *dst = (uint8_t *)DEST;
	u64 map_end, src_end, image_size, crc, want;
	volatile u64 *ctx = (volatile u64 *)CTX;
	uint32_t *table;
	size_t where = 0;
	long r;

	asm volatile("msr vbar_el1, %0\n isb" :: "r"((u64)__vectors) : "memory");

	if (h->magic[0] != 'G' || h->magic[7] != 'B' || h->dest != DEST || comp_len < 8 || orig_len < 64 ||
	    orig_len > INITRD_PA - DEST || comp_len > (u64)GL_LZ4_LEGACY_BLOCK * 64)
		stub_fail(FAIL_HDR, comp_len, orig_len);
	/* the frame and the stub lie between 0x80000000 and SCRATCH: S-Boot's buffer cannot be elsewhere */
	src_end = base + stub_len + comp_len;
	if (base < MAP_BASE || (base & 0xfff) || src_end > SCRATCH || src_end < base)
		stub_fail(FAIL_RANGE, base, src_end);
	src = (const uint8_t *)(base + stub_len);

	map_end = (DEST + orig_len + 0x1fffff) & ~0x1fffffull;
	build_tables(map_end);
	mmu_on();

	r = gl_lz4_legacy(src, comp_len, dst, orig_len, &where);
	if (r != (long)orig_len)
		stub_fail(FAIL_DECODE, (u64)r, where);

	table = (uint32_t *)(STACK_TOP - 0x1000);	/* 1 KiB scratch table, below the stack top */
	crc = gl_crc32(dst, orig_len, table);
	want = h->crc32;
	if (crc != want)
		stub_fail(FAIL_CRC, crc, want);

	/* Image header (arch/arm64/kernel/head.S): magic "ARM\x64" at 0x38, image_size at 0x10 */
	image_size = *(const u64 *)(dst + 16);
	if (*(const u32 *)(dst + 56) != 0x644d5241u || image_size < orig_len || image_size > INITRD_PA - DEST)
		stub_fail(FAIL_IMAGE, *(const u32 *)(dst + 56), image_size);

	ctx[CTX_DEST / 8] = DEST;
	ctx[CTX_DESTLEN / 8] = orig_len;
	ctx[CTX_SRC / 8] = base;
	ctx[CTX_SRCLEN / 8] = stub_len + comp_len;
}
