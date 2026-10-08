/* GPU-ready frames for the browser renderer.
 *
 * At STARTRENDER the game thread turns the display list into a flat vertex
 * array, a list of draw calls and decoded RGBA textures. The main thread reads
 * the published frame and draws it with WebGL2 (web/gl.js).
 */
#pragma once
#include "sh4ctx.h"

#define GLF_VERT_FLOATS 13   /* x, y, z, u, v, r, g, b, a, or, og, ob, oa */

typedef struct GlCall {
	u32 list, isp, tsp, tcw;
	u32 first, count;        /* vertices (triangles: count multiple of 3) */
	s32 tex;                 /* texture slot or -1 */
} GlCall;

typedef struct GlTex {
	u32 tcw, tsp_size;       /* identity */
	u32 w, h;
	u32 version;             /* bumped whenever pixels change */
	u8 *rgba;
} GlTex;

typedef struct GlFrame {
	u32 seq;                 /* frame number, written last */
	u32 nverts, ncalls;
	float *verts;
	GlCall *calls;
	u32 vcap, ccap;
} GlFrame;

#define GLF_MAX_TEX 4096
extern GlTex glf_tex[GLF_MAX_TEX];
extern u32 glf_ntex;

/* Build the frame for a display list into the back buffer and publish it. */
void glframe_build(const u8 *list, u32 len);
/* The most recently published frame (main thread). */
GlFrame *glframe_latest(void);

/* VRAM / palette change tracking used by the texture cache */
void glframe_vram_dirty(u32 linear_offset, u32 len);
void glframe_palette_dirty(void);
