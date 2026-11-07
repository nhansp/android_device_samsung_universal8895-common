/* SPDX-License-Identifier: GPL-2.0-only */
/* Flat, position-independent image: entry+header first, vectors 2 KiB aligned, padded to 4 KiB. */
ENTRY(_start)
SECTIONS
{
	. = 0;
	.text : {
		*(.text.entry)
		*(.text .text.*)
		*(.rodata .rodata.*)
		. = ALIGN(2048);
		__vectors = .;
		*(.vectors)
		. = ALIGN(4096);
		__stub_end = .;
	}
	.data : { *(.data .data.* .got .got.* .bss .bss.* COMMON) }
	/DISCARD/ : { *(.comment .note* .eh_frame* .ARM.* .interp .dynsym .dynstr .hash .gnu.hash) }
}
ASSERT(SIZEOF(.data) == 0, "stub must have no data, bss or got: it runs at an unknown load address")
