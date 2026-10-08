/* GPU-ready frames for the browser renderer, see glframe.h. */
#include <stdlib.h>
#include <string.h>
#include "glframe.h"
#include "pvr.h"

GlTex glf_tex[GLF_MAX_TEX];
u32 glf_ntex;

static GlFrame frames[2];
static volatile u32 published = 0;   /* index of the latest complete frame */
static u32 frame_seq;

/* ---- change tracking: one generation counter per 4 KB of VRAM ---- */
#define PAGE_SHIFT 12
static u32 page_gen[0x1000000 >> PAGE_SHIFT];
static u32 gen_counter = 1, palette_gen = 1;
static u32 tex_gen[GLF_MAX_TEX];      /* generation each texture was decoded at */
static u32 tex_pal[GLF_MAX_TEX];
static u32 tex_lo[GLF_MAX_TEX], tex_hi[GLF_MAX_TEX];

void glframe_vram_dirty(u32 off, u32 len)
{
	u32 g = ++gen_counter;
	for (u32 p = (off & 0xFFFFFF) >> PAGE_SHIFT; p <= ((off + len - 1) & 0xFFFFFF) >> PAGE_SHIFT; p++)
		page_gen[p] = g;
}

void glframe_palette_dirty(void) { palette_gen++; }

static u32 range_gen(u32 lo, u32 hi)
{
	u32 g = 0;
	for (u32 p = lo >> PAGE_SHIFT; p <= (hi >> PAGE_SHIFT) && p < (0x1000000 >> PAGE_SHIFT); p++)
		if (page_gen[p] > g) g = page_gen[p];
	return g;
}

/* bytes of VRAM a texture reads, for change tracking */
static void tex_range(u32 tsp, u32 tcw, u32 *lo, u32 *hi)
{
	u32 w = 8u << ((tsp >> 3) & 7), h = 8u << (tsp & 7);
	int mip = (tcw >> 31) & 1, vq = (tcw >> 30) & 1, fmt = (tcw >> 27) & 7;
	if (mip) h = w;
	u32 base = (tcw & 0x1FFFFF) << 3;
	u32 bytes;
	if (vq) bytes = 2048 + w * h / 4 * (mip ? 2 : 1);
	else if (fmt == 5) bytes = w * h / 2 * (mip ? 2 : 1);
	else if (fmt == 6) bytes = w * h * (mip ? 2 : 1);
	else bytes = w * h * 2 * (mip ? 2 : 1);
	*lo = base;
	*hi = base + bytes - 1;
}

static s32 texture_for(u32 tsp, u32 tcw)
{
	u32 size = tsp & 0x3F;
	u32 key_tcw = tcw;
	int fmt = (tcw >> 27) & 7;
	s32 slot = -1;
	for (u32 i = 0; i < glf_ntex; i++)
		if (glf_tex[i].tcw == key_tcw && glf_tex[i].tsp_size == size) { slot = (s32)i; break; }
	if (slot < 0) {
		if (glf_ntex >= GLF_MAX_TEX) return -1;
		slot = (s32)glf_ntex++;
		GlTex *t = &glf_tex[slot];
		t->tcw = key_tcw; t->tsp_size = size;
		t->w = 8u << ((tsp >> 3) & 7);
		t->h = (tcw >> 31) & 1 ? t->w : 8u << (tsp & 7);
		if (((tcw >> 26) & 1) && ((tcw >> 25) & 1) && !((tcw >> 30) & 1))
			t->w = (pvr_reg(0x005F80E4u) & 31) * 32;
		t->rgba = malloc((size_t)t->w * t->h * 4);
		tex_gen[slot] = 0;
		tex_range(tsp, tcw, &tex_lo[slot], &tex_hi[slot]);
	}
	GlTex *t = &glf_tex[slot];
	u32 g = range_gen(tex_lo[slot], tex_hi[slot]);
	int pal = fmt == 5 || fmt == 6;
	if (tex_gen[slot] == 0 || g > tex_gen[slot] || (pal && tex_pal[slot] != palette_gen)) {
		for (u32 y = 0; y < t->h; y++)
			for (u32 x = 0; x < t->w; x++)
				pvr_texel(tsp, tcw, (int)x, (int)y, &t->rgba[(y * t->w + x) * 4]);
		tex_gen[slot] = g ? g : 1;
		tex_pal[slot] = palette_gen;
		t->version++;
	}
	return slot;
}

typedef struct {
	GlFrame *f;
	PvrState cur;
	int have;
} Builder;

static void push_vert(GlFrame *f, const PvrVert *v)
{
	if (f->nverts + 1 > f->vcap) {
		f->vcap = f->vcap ? f->vcap * 2 : 65536;
		f->verts = realloc(f->verts, (size_t)f->vcap * GLF_VERT_FLOATS * sizeof(float));
	}
	float *o = &f->verts[f->nverts++ * GLF_VERT_FLOATS];
	o[0] = v->x; o[1] = v->y; o[2] = v->z; o[3] = v->u; o[4] = v->v;
	memcpy(o + 5, v->col, 4 * sizeof(float));
	memcpy(o + 9, v->ofs, 4 * sizeof(float));
}

static void on_tri(void *user, const PvrState *st, const PvrVert *a, const PvrVert *b, const PvrVert *c)
{
	Builder *bd = user;
	GlFrame *f = bd->f;
	int same = bd->have && f->ncalls && st->list == bd->cur.list && st->isp == bd->cur.isp &&
	           st->tsp == bd->cur.tsp && st->tcw == bd->cur.tcw && st->textured == bd->cur.textured &&
	           st->offset == bd->cur.offset && st->gouraud == bd->cur.gouraud;
	if (!same) {
		if (f->ncalls + 1 > f->ccap) {
			f->ccap = f->ccap ? f->ccap * 2 : 4096;
			f->calls = realloc(f->calls, (size_t)f->ccap * sizeof(GlCall));
		}
		GlCall *cl = &f->calls[f->ncalls++];
		cl->list = (u32)st->list; cl->isp = st->isp; cl->tsp = st->tsp; cl->tcw = st->tcw;
		/* state bits the shader needs but the words don't carry are folded into isp's unused low bits */
		cl->isp = (st->isp & ~7u) | (st->textured ? 1u : 0) | (st->offset ? 2u : 0) | (st->gouraud ? 4u : 0);
		cl->first = f->nverts; cl->count = 0;
		cl->tex = st->textured ? texture_for(st->tsp, st->tcw) : -1;
		bd->cur = *st;
		bd->have = 1;
	}
	push_vert(f, a); push_vert(f, b); push_vert(f, c);
	f->calls[f->ncalls - 1].count += 3;
}

void glframe_build(const u8 *list, u32 len)
{
	u32 back = published ^ 1;
	GlFrame *f = &frames[back];
	f->nverts = 0; f->ncalls = 0;
	Builder bd = { f, { 0 }, 0 };
	pvr_decode(list, len, on_tri, &bd);
	f->seq = ++frame_seq;
	__atomic_store_n(&published, back, __ATOMIC_RELEASE);
}

GlFrame *glframe_latest(void)
{
	return &frames[__atomic_load_n(&published, __ATOMIC_ACQUIRE)];
}
