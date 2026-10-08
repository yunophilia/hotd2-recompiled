/* PVR2 display-list decoding and a reference software rasteriser.
 *
 * pvr_decode() walks a captured Tile Accelerator parameter stream and emits
 * triangles with their render state; pvr_render_soft() rasterises them into
 * a 640x480 RGBA image. The browser renderer will consume the same triangles.
 */
#pragma once
#include "sh4ctx.h"

typedef struct PvrVert {
	float x, y, z;          /* screen x/y, z = 1/w (bigger is nearer) */
	float u, v;
	float col[4];           /* base colour, RGBA 0..1 */
	float ofs[4];           /* offset (specular) colour */
} PvrVert;

typedef struct PvrState {
	int list;               /* 0 opaque, 2 translucent, 4 punch-through */
	u32 isp, tsp, tcw;
	int textured, offset, gouraud;
} PvrState;

typedef void (*PvrTriFn)(void *user, const PvrState *st, const PvrVert *a, const PvrVert *b, const PvrVert *c);

/* Decode a parameter stream; calls emit() for each triangle. Returns triangles emitted. */
int pvr_decode(const u8 *list, u32 len, PvrTriFn emit, void *user);

/* Render a parameter stream into rgba (640*480*4), cleared to the background first. */
void pvr_render_soft(const u8 *list, u32 len, u8 *rgba);

/* Texture texel fetch (RGBA 0..255) for a polygon's TSP/TCW at integer texel coordinates. */
void pvr_texel(u32 tsp, u32 tcw, int x, int y, u8 out[4]);

/* Registers the renderer needs, provided by hle.c */
u32 pvr_reg(u32 phys);
extern u8 vram[0x1000000];
