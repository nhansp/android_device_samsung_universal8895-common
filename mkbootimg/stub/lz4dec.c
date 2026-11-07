// SPDX-License-Identifier: GPL-2.0-only
/*
 * lz4dec.c - bounds-checked LZ4 block decoder, legacy-frame walker, CRC-32. Freestanding C.
 *
 * Safety rules (tests/stub-lz4/test.c checks them with guard pages and a fuzz loop):
 *  - every read of the input is preceded by a check against the end of the input;
 *  - every write is preceded by a check against the end of the output window;
 *  - the 8-byte "wild" copies are used only when 8 spare bytes exist on both sides of the copy, so
 *    they never touch a byte outside the input or the output window;
 *  - a match may only reference bytes already produced by the same block.
 */
#include "lz4dec.h"

typedef uint64_t u64;

static inline void copy8(uint8_t *d, const uint8_t *s)
{
	u64 v;

	__builtin_memcpy(&v, s, 8);	/* one 8-byte load/store; MMU on, so unaligned is fine */
	__builtin_memcpy(d, &v, 8);
}

/* Read an LZ4 length extension (runs of 255 plus a last byte); returns 0 or an GL_LZ4_E_ error. */
static inline long ext_len(const uint8_t **ipp, const uint8_t *iend, size_t *len, size_t limit)
{
	const uint8_t *ip = *ipp;
	size_t l = *len;
	unsigned int s;

	do {
		if (ip >= iend)
			return GL_LZ4_E_INPUT;
		s = *ip++;
		l += s;
		if (l > limit)		/* limit = bytes of output left: a longer run cannot be valid */
			return GL_LZ4_E_OUTPUT;
	} while (s == 255);
	*ipp = ip;
	*len = l;
	return 0;
}

long gl_lz4_block(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstmax)
{
	const uint8_t *ip = src, *const iend = src + srclen;
	uint8_t *op = dst, *const oend = dst + dstmax;

	for (;;) {
		size_t lit, ml, off;
		unsigned int token;
		long rc;

		if (ip >= iend)
			return GL_LZ4_E_INPUT;
		token = *ip++;
		lit = token >> 4;
		if (lit == 15 && (rc = ext_len(&ip, iend, &lit, (size_t)(oend - op))))
			return rc;
		if (lit > (size_t)(iend - ip))
			return GL_LZ4_E_INPUT;
		if (lit > (size_t)(oend - op))
			return GL_LZ4_E_OUTPUT;
		if ((size_t)(iend - ip) >= lit + 8 && (size_t)(oend - op) >= lit + 8) {
			size_t i;

			for (i = 0; i < lit; i += 8)	/* may copy up to 7 bytes past lit, inside both windows */
				copy8(op + i, ip + i);
		} else {
			size_t i;

			for (i = 0; i < lit; i++)
				op[i] = ip[i];
		}
		ip += lit;
		op += lit;
		if (ip == iend)			/* the last sequence has literals only */
			return op - dst;
		if (iend - ip < 2)
			return GL_LZ4_E_INPUT;
		off = ip[0] | ((size_t)ip[1] << 8);
		ip += 2;
		if (off == 0 || off > (size_t)(op - dst))
			return GL_LZ4_E_OFFSET;
		ml = token & 15;
		if (ml == 15 && (rc = ext_len(&ip, iend, &ml, (size_t)(oend - op))))
			return rc;
		ml += 4;
		if (ml > (size_t)(oend - op))
			return GL_LZ4_E_OUTPUT;
		{
			const uint8_t *m = op - off;
			size_t i = 0;

			if (off >= 8 && (size_t)(oend - op) >= ml + 8) {
				for (; i < ml; i += 8)	/* forward 8-byte steps are exact because off >= 8 */
					copy8(op + i, m + i);
			} else {
				for (; i < ml; i++)	/* off < 8 repeats a pattern: byte order matters */
					op[i] = m[i];
			}
			op += ml;
		}
	}
}

long gl_lz4_legacy(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen, size_t *where)
{
	const uint8_t *ip = src, *const iend = src + srclen;
	uint8_t *op = dst, *const oend = dst + dstlen;
	uint32_t v;

	if (where)
		*where = 0;
	if (srclen < 4)
		return GL_LZ4_E_FRAME;
	v = ip[0] | (ip[1] << 8) | (ip[2] << 16) | ((uint32_t)ip[3] << 24);
	if (v != GL_LZ4_LEGACY_MAGIC)
		return GL_LZ4_E_FRAME;
	ip += 4;
	while (ip < iend) {
		size_t bsz, room;
		long n;

		if (where)
			*where = ip - src;
		if (iend - ip < 4)
			return GL_LZ4_E_FRAME;
		bsz = ip[0] | (ip[1] << 8) | (ip[2] << 16) | ((size_t)ip[3] << 24);
		ip += 4;
		if (bsz == 0 || bsz > (size_t)(iend - ip))
			return GL_LZ4_E_FRAME;
		room = oend - op;
		if (room > GL_LZ4_LEGACY_BLOCK)
			room = GL_LZ4_LEGACY_BLOCK;
		n = gl_lz4_block(ip, bsz, op, room);
		if (n < 0)
			return n;
		ip += bsz;
		op += n;
	}
	if (op != oend)
		return GL_LZ4_E_SIZE;
	return dstlen;
}

uint32_t gl_crc32(const uint8_t *p, size_t n, uint32_t *table)
{
	uint32_t c = 0xFFFFFFFFu;
	unsigned int i, k;

	for (i = 0; i < 256; i++) {
		uint32_t r = i;

		for (k = 0; k < 8; k++)
			r = (r & 1) ? (r >> 1) ^ 0xEDB88320u : r >> 1;
		table[i] = r;
	}
	while (n--)
		c = table[(c ^ *p++) & 0xFF] ^ (c >> 8);
	return ~c;
}
