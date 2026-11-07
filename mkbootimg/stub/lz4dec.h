/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * lz4dec.h - freestanding LZ4 decoder and CRC-32 for the compressed bounce stub
 * (sboot-bounce-lz4.S/.c). No libc, no globals, no relocations: the same source is built for
 * aarch64 into the stub and for the host by tests/stub-lz4/test.c.
 */
#ifndef GL_LZ4DEC_H
#define GL_LZ4DEC_H

#include <stddef.h>
#include <stdint.h>

#define GL_LZ4_LEGACY_MAGIC	0x184C2102u	/* "lz4 -l": magic, then { u32le csize; csize bytes } ... */
#define GL_LZ4_LEGACY_BLOCK	(8u << 20)	/* a legacy block decodes to at most 8 MiB */

/* Return values: >= 0 is a length, < 0 is an error. */
#define GL_LZ4_E_INPUT		(-1)	/* ran out of input (truncated sequence) */
#define GL_LZ4_E_OUTPUT		(-2)	/* sequence would write past the output limit */
#define GL_LZ4_E_OFFSET		(-3)	/* match offset 0 or before the start of the block */
#define GL_LZ4_E_FRAME		(-4)	/* not a legacy frame / bad block size */
#define GL_LZ4_E_SIZE		(-5)	/* frame decodes to a different length than expected */

/*
 * Decode one raw LZ4 block src[0..srclen) into dst[0..dstmax). Reads only inside the input, writes
 * only inside dst[0..dstmax), never reads dst before dst[0] (offsets are checked against the block
 * start). Returns the number of bytes produced.
 */
long gl_lz4_block(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstmax);

/*
 * Decode a legacy frame src[0..srclen) into dst[0..dstlen); the frame must decode to exactly dstlen
 * bytes and be consumed to its last byte. Returns dstlen or an error. *where (if not NULL) gets the
 * byte offset in src of the block that failed.
 */
long gl_lz4_legacy(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen, size_t *where);

/* zlib-compatible CRC-32 (IEEE 802.3, reflected, init/xorout 0xFFFFFFFF); table[256] is scratch. */
uint32_t gl_crc32(const uint8_t *p, size_t n, uint32_t *table);

#endif
