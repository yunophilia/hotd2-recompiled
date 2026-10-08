/* NAOMI M2 cartridge: ROM PIO reads and G1 DMA into main RAM.
 *
 * The cart image is the flat 0xA800000-byte layout tools/cart.py builds from
 * the user's ROM set. M2 boards start in "4MB mode", where CPU-visible
 * offsets are remapped onto the ROM (see docs/FINDINGS.md).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sh4ctx.h"
#include "cart.h"

static u8 *rom;
static u32 rom_size;
static u32 pio_offset, dma_offset;
static int pio_autoinc;

void cart_set_image(u8 *image, u32 size)
{
	rom = image;
	rom_size = size;
}

int cart_load(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return 0; }
	fseek(f, 0, SEEK_END);
	rom_size = (u32)ftell(f);
	fseek(f, 0, SEEK_SET);
	rom = malloc(rom_size);
	if (!rom || fread(rom, 1, rom_size, f) != rom_size) { fclose(f); return 0; }
	fclose(f);
	return 1;
}

static u32 translate(u32 offset)
{
	if (!(pio_offset & 0x20000000u))  /* 4MB mode */
		offset = (offset & 0x103FFFFFu) | ((offset & 0x07C00000u) << 1);
	return offset & 0x1FFFFFFFu;
}

static u8 rom_byte(u32 offset)
{
	return offset < rom_size ? rom[offset] : 0xFF;
}

int cart_read_reg(u32 p, u32 *v)
{
	switch (p) {
	case 0x005F7000u: *v = (pio_offset >> 16) | (pio_autoinc ? 0x8000u : 0); return 1;
	case 0x005F7004u: *v = pio_offset & 0xFFFF; return 1;
	case 0x005F7008u: {
		u32 o = translate(pio_offset);
		*v = rom_byte(o) | (rom_byte(o + 1) << 8);
		if (pio_autoinc) pio_offset += 2;
		return 1;
	}
	case 0x005F700Cu: *v = dma_offset >> 16; return 1;
	case 0x005F7010u: *v = dma_offset & 0xFFFF; return 1;
	case 0x005F707Cu: *v = 0; return 1;  /* board ID: no security PIC answer needed so far */
	}
	return 0;
}

int cart_write_reg(u32 p, u32 v)
{
	switch (p) {
	case 0x005F7000u:
		pio_autoinc = (v & 0x8000) != 0;
		pio_offset = (pio_offset & 0xFFFF) | ((v << 16) & 0x7FFF0000u);
		return 1;
	case 0x005F7004u: pio_offset = (pio_offset & 0xFFFF0000u) | (v & 0xFFFF); return 1;
	case 0x005F700Cu: dma_offset = (dma_offset & 0xFFFF) | ((v & 0x7FFF) << 16); return 1;
	case 0x005F7010u: dma_offset = (dma_offset & 0xFFFF0000u) | (v & 0xFFFF); return 1;
	}
	return 0;
}

void cart_dma(u32 dst, u32 len)
{
	static FILE *log;
	static int checked;
	if (!checked) { checked = 1; if (getenv("CART_LOG")) log = fopen("cart_dma.log", "w"); }
	if (log) { fprintf(log, "%08X %08X %08X %08X\n", dma_offset, translate(dma_offset), len, dst); fflush(log); }
	for (u32 k = 0; k < len; k++) {
		u32 o = translate(dma_offset + k);
		wr8(dst + k, rom_byte(o));
	}
	dma_offset += len;
}
