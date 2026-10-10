/* PVR2 display-list decoding, texture decoding and a reference software
 * rasteriser. See pvr.h. Written from the PVR2 parameter format: 32-byte
 * parameters, some 64-byte; PCW in the first word of each. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pvr.h"

#define W 640
#define H 480

/* ---------------- parameter decoding ---------------- */

static float f32(const u8 *p, int word) { float f; memcpy(&f, p + 4 * word, 4); return f; }
static u32 u32w(const u8 *p, int word) { u32 v; memcpy(&v, p + 4 * word, 4); return v; }

static void packed(u32 argb, float out[4])
{
	out[0] = ((argb >> 16) & 0xFF) / 255.0f;
	out[1] = ((argb >> 8) & 0xFF) / 255.0f;
	out[2] = (argb & 0xFF) / 255.0f;
	out[3] = (argb >> 24) / 255.0f;
}

static void floats_argb(const u8 *p, int word, float out[4])
{
	out[3] = f32(p, word); out[0] = f32(p, word + 1); out[1] = f32(p, word + 2); out[2] = f32(p, word + 3);
}

/* 16-bit UV pair: u in the high half, v in the low half, each the top of a float */
static void uv16(u32 w, float *u, float *v)
{
	u32 hu = w & 0xFFFF0000u, hv = w << 16;
	memcpy(u, &hu, 4); memcpy(v, &hv, 4);
}

static void scale_col(const float face[4], float k, float out[4])
{
	for (int i = 0; i < 3; i++) out[i] = face[i] * k;
	out[3] = face[3];
}

typedef struct {
	PvrState st;
	int vtype;           /* vertex parameter type 0..14, 15 = sprite, -1 = modifier volume */
	int vsize;           /* bytes */
	float face[4], face_ofs[4];
	u32 sprite_base, sprite_ofs;
	PvrVert strip[3];
	int nstrip, parity;
} DecodeState;

static int vertex_type(u32 pcw)
{
	int tex = (pcw >> 3) & 1, col = (pcw >> 4) & 3, vol = (pcw >> 6) & 1, uv = pcw & 1;
	if (!tex) {
		if (!vol) return col == 0 ? 0 : col == 1 ? 1 : 2;
		return col == 0 ? 9 : 10;
	}
	if (!vol) {
		if (col == 0) return uv ? 4 : 3;
		if (col == 1) return uv ? 6 : 5;
		return uv ? 8 : 7;
	}
	if (col == 0) return uv ? 12 : 11;
	return uv ? 14 : 13;
}

static int vertex_bytes(int t) { return (t == 5 || t == 6 || t >= 11) ? 64 : 32; }

static void emit_tri(DecodeState *d, PvrTriFn emit, void *user, int *count)
{
	PvrVert *s = d->strip;
	/* keep strip winding consistent: odd triangles swap two vertices */
	if (d->parity) emit(user, &d->st, &s[1], &s[0], &s[2]);
	else emit(user, &d->st, &s[0], &s[1], &s[2]);
	(*count)++;
}

static void decode_vertex(DecodeState *d, const u8 *p, PvrVert *v)
{
	memset(v, 0, sizeof *v);
	v->x = f32(p, 1); v->y = f32(p, 2); v->z = f32(p, 3);
	v->col[3] = 1;
	switch (d->vtype) {
	case 0: packed(u32w(p, 6), v->col); break;
	case 1: floats_argb(p, 4, v->col); break;
	case 2: scale_col(d->face, f32(p, 6), v->col); break;
	case 3: v->u = f32(p, 4); v->v = f32(p, 5); packed(u32w(p, 6), v->col); packed(u32w(p, 7), v->ofs); break;
	case 4: uv16(u32w(p, 4), &v->u, &v->v); packed(u32w(p, 6), v->col); packed(u32w(p, 7), v->ofs); break;
	case 5: v->u = f32(p, 4); v->v = f32(p, 5); floats_argb(p, 8, v->col); floats_argb(p, 12, v->ofs); break;
	case 6: uv16(u32w(p, 4), &v->u, &v->v); floats_argb(p, 8, v->col); floats_argb(p, 12, v->ofs); break;
	case 7: v->u = f32(p, 4); v->v = f32(p, 5); scale_col(d->face, f32(p, 6), v->col); scale_col(d->face_ofs, f32(p, 7), v->ofs); break;
	case 8: uv16(u32w(p, 4), &v->u, &v->v); scale_col(d->face, f32(p, 6), v->col); scale_col(d->face_ofs, f32(p, 7), v->ofs); break;
	/* two-volume types: use volume 0 */
	case 9: packed(u32w(p, 4), v->col); break;
	case 10: scale_col(d->face, f32(p, 4), v->col); break;
	case 11: v->u = f32(p, 4); v->v = f32(p, 5); packed(u32w(p, 6), v->col); packed(u32w(p, 7), v->ofs); break;
	case 12: uv16(u32w(p, 4), &v->u, &v->v); packed(u32w(p, 6), v->col); packed(u32w(p, 7), v->ofs); break;
	case 13: v->u = f32(p, 4); v->v = f32(p, 5); scale_col(d->face, f32(p, 6), v->col); scale_col(d->face_ofs, f32(p, 7), v->ofs); break;
	case 14: uv16(u32w(p, 4), &v->u, &v->v); scale_col(d->face, f32(p, 6), v->col); scale_col(d->face_ofs, f32(p, 7), v->ofs); break;
	}
}

static void decode_sprite(DecodeState *d, const u8 *p, PvrTriFn emit, void *user, int *count)
{
	/* A, B, C full xyz; D x/y with z on the A-B-C plane; UVs for A, B, C */
	PvrVert q[4];
	float ax = f32(p, 1), ay = f32(p, 2), az = f32(p, 3);
	float bx = f32(p, 4), by = f32(p, 5), bz = f32(p, 6);
	float cx = f32(p, 7), cy = f32(p, 8), cz = f32(p, 9);
	float dx = f32(p, 10), dy = f32(p, 11);
	/* plane through A, B, C for D's depth */
	float e1x = bx - ax, e1y = by - ay, e1z = bz - az, e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
	float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
	float dz = nz != 0 ? az - (nx * (dx - ax) + ny * (dy - ay)) / nz : az;
	float xs[4] = { ax, bx, cx, dx }, ys[4] = { ay, by, cy, dy }, zs[4] = { az, bz, cz, dz };
	float us[4], vs[4];
	uv16(u32w(p, 13), &us[0], &vs[0]);
	uv16(u32w(p, 14), &us[1], &vs[1]);
	uv16(u32w(p, 15), &us[2], &vs[2]);
	us[3] = us[0] + us[2] - us[1];
	vs[3] = vs[0] + vs[2] - vs[1];
	for (int i = 0; i < 4; i++) {
		memset(&q[i], 0, sizeof q[i]);
		q[i].x = xs[i]; q[i].y = ys[i]; q[i].z = zs[i];
		q[i].u = us[i]; q[i].v = vs[i];
		packed(d->sprite_base, q[i].col);
		packed(d->sprite_ofs, q[i].ofs);
	}
	emit(user, &d->st, &q[0], &q[1], &q[3]);
	emit(user, &d->st, &q[1], &q[2], &q[3]);
	*count += 2;
}

int pvr_decode(const u8 *list, u32 len, PvrTriFn emit, void *user)
{
	DecodeState d;
	memset(&d, 0, sizeof d);
	int list_type = -1, count = 0;
	for (u32 off = 0; off + 32 <= len; ) {
		const u8 *p = list + off;
		u32 pcw = u32w(p, 0);
		u32 para = pcw >> 29;
		u32 step = 32;
		switch (para) {
		case 0: /* end of list */
			list_type = -1;
			d.nstrip = 0;
			break;
		case 4: /* polygon or modifier volume header */
		case 5: /* sprite header */
			if (list_type < 0)
				list_type = (pcw >> 24) & 7;
			d.nstrip = 0;
			if (list_type == 1 || list_type == 3) { /* modifier volumes: not drawn */
				d.vtype = -1;
				d.vsize = 64;
				break;
			}
			d.st.list = list_type;
			d.st.isp = u32w(p, 1);
			d.st.tsp = u32w(p, 2);
			d.st.tcw = u32w(p, 3);
			d.st.textured = (pcw >> 3) & 1;
			d.st.offset = (pcw >> 2) & 1;
			d.st.gouraud = (pcw >> 1) & 1;
			if (para == 5) {
				d.vtype = 15;
				d.vsize = 64;
				d.sprite_base = u32w(p, 4);
				d.sprite_ofs = u32w(p, 5);
				break;
			}
			d.vtype = vertex_type(pcw);
			d.vsize = vertex_bytes(d.vtype);
			{
				int col = (pcw >> 4) & 3, vol = (pcw >> 6) & 1, tex = (pcw >> 3) & 1, ofs = (pcw >> 2) & 1;
				for (int i = 0; i < 4; i++) d.face[i] = d.face_ofs[i] = 1;
				if (!vol && col == 2 && tex && ofs) {          /* type 2: 64 bytes, face + offset colours */
					floats_argb(p, 8, d.face);
					floats_argb(p, 12, d.face_ofs);
					step = 64;
				} else if (!vol && col == 2) {                 /* type 1: face colour */
					floats_argb(p, 4, d.face);
				} else if (vol && col == 2) {                  /* type 4: 64 bytes, two face colours */
					floats_argb(p, 8, d.face);
					step = 64;
				}
			}
			break;
		case 7: /* vertex */
			step = d.vsize ? d.vsize : 32;
			if (off + step > len || d.vtype < 0)
				break;
			if (d.vtype == 15) {
				decode_sprite(&d, p, emit, user, &count);
				break;
			}
			{
				PvrVert v;
				decode_vertex(&d, p, &v);
				if (d.nstrip < 2) {
					d.strip[d.nstrip++] = v;
					d.parity = 0;
				} else {
					if (d.nstrip == 3) {
						d.strip[0] = d.strip[1];
						d.strip[1] = d.strip[2];
						d.parity ^= 1;
					}
					d.strip[2] = v;
					d.nstrip = 3;
					emit_tri(&d, emit, user, &count);
				}
				if (pcw & (1u << 28)) /* end of strip */
					d.nstrip = 0;
			}
			break;
		default: /* user clip, object list set */
			break;
		}
		off += step;
	}
	return count;
}

/* ---------------- textures ---------------- */

static inline u32 twiddle(u32 x, u32 y)
{
	/* PVR twiddled order: bits interleaved, y in the lowest bit */
	u32 r = 0;
	for (int b = 0; b < 11; b++)
		r |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
	return r;
}

static inline u32 twiddle_rect(u32 x, u32 y, u32 w, u32 h)
{
	/* rectangular twiddled textures are a row/column of square twiddled tiles */
	u32 m = w < h ? w : h;
	u32 tile = (w > h) ? x / m : y / m;
	return tile * m * m + twiddle(x % m, y % m);
}

static void conv16(u16 c, int fmt, u8 o[4])
{
	switch (fmt) {
	case 0: case 7: /* ARGB1555 */
		o[0] = (u8)(((c >> 10) & 31) * 255 / 31); o[1] = (u8)(((c >> 5) & 31) * 255 / 31);
		o[2] = (u8)((c & 31) * 255 / 31); o[3] = (c & 0x8000) ? 255 : 0; break;
	case 1: /* RGB565 */
		o[0] = (u8)(((c >> 11) & 31) * 255 / 31); o[1] = (u8)(((c >> 5) & 63) * 255 / 63);
		o[2] = (u8)((c & 31) * 255 / 31); o[3] = 255; break;
	case 2: /* ARGB4444 */
		o[0] = (u8)(((c >> 8) & 15) * 17); o[1] = (u8)(((c >> 4) & 15) * 17);
		o[2] = (u8)((c & 15) * 17); o[3] = (u8)((c >> 12) * 17); break;
	default: /* bump map etc.: grey */
		o[0] = o[1] = o[2] = (u8)(c >> 8); o[3] = 255; break;
	}
}

static void palette(u32 index, u8 o[4])
{
	u32 c = pvr_reg(0x005F9000u + 4 * (index & 1023));
	switch (pvr_reg(0x005F8108u) & 3) {
	case 0: conv16((u16)c, 0, o); break;
	case 1: conv16((u16)c, 1, o); break;
	case 2: conv16((u16)c, 2, o); break;
	default: o[0] = (u8)(c >> 16); o[1] = (u8)(c >> 8); o[2] = (u8)c; o[3] = (u8)(c >> 24); break;
	}
}

static u32 mip_texels(u32 size)          /* offset of the size x size level, in texels */
{
	u32 off = 3, s = 1;
	while (s < size) { off += s * s; s <<= 1; }
	return off;
}

static u32 vq_mip_bytes(u32 size)        /* offset of the level after the codebook, in index bytes */
{
	if (size <= 1) return 0;
	u32 off = 1, s = 2;
	while (s < size) { off += (s / 2) * (s / 2); s <<= 1; }
	return off;
}

static inline u8 vb(u32 a) { return vram[a & 0xFFFFFF]; }
static inline u16 vh(u32 a) { return (u16)(vb(a) | vb(a + 1) << 8); }

static void yuv422(u32 base, u32 x, u32 y, u32 w, int twiddled, u8 o[4])
{
	/* pairs of pixels share U/V: Y0 U Y1 V as 16-bit words */
	u32 x0 = x & ~1u;
	u32 a0 = twiddled ? base + 2 * twiddle(x0, y) : base + 2 * (y * w + x0);
	u32 a1 = twiddled ? base + 2 * twiddle(x0 + 1, y) : a0 + 2;
	u16 p0 = vh(a0), p1 = vh(a1);
	int U = p0 & 0xFF, Y0 = p0 >> 8, V = p1 & 0xFF, Y1 = p1 >> 8;
	int Y = (x & 1) ? Y1 : Y0;
	int r = Y + (int)(1.375f * (V - 128)), g = Y - (int)(0.6875f * (V - 128) + 0.34375f * (U - 128)),
	    b = Y + (int)(1.71875f * (U - 128));
	o[0] = (u8)(r < 0 ? 0 : r > 255 ? 255 : r);
	o[1] = (u8)(g < 0 ? 0 : g > 255 ? 255 : g);
	o[2] = (u8)(b < 0 ? 0 : b > 255 ? 255 : b);
	o[3] = 255;
}

void pvr_texel(u32 tsp, u32 tcw, int x, int y, u8 o[4])
{
	u32 w = 8u << ((tsp >> 3) & 7), h = 8u << (tsp & 7);
	int mip = (tcw >> 31) & 1, vq = (tcw >> 30) & 1, fmt = (tcw >> 27) & 7;
	int scan = (tcw >> 26) & 1, stride = (tcw >> 25) & 1;
	u32 base = (tcw & 0x1FFFFF) << 3;
	if (mip) h = w;
	if (scan && stride && !vq)
		w = (pvr_reg(0x005F80E4u) & 31) * 32;
	if (fmt == 5 || fmt == 6) {           /* palettised, always twiddled */
		u32 sel = (tcw >> 21) & 63;
		if (mip) base += mip_texels(w) * (fmt == 5 ? 1 : 2) / 2;
		u32 t = twiddle_rect((u32)x, (u32)y, w, h);
		if (fmt == 6) palette((sel >> 4) * 256 + vb(base + t), o);
		else palette(sel * 16 + ((vb(base + t / 2) >> ((t & 1) * 4)) & 15), o);
		return;
	}
	if (vq) {
		u32 idx_base = base + 2048 + (mip ? vq_mip_bytes(w) : 0);
		u32 code = vb(idx_base + twiddle_rect((u32)x / 2, (u32)y / 2, w / 2, h / 2));
		u32 sub = twiddle((u32)x & 1, (u32)y & 1);
		u16 c = vh(base + code * 8 + sub * 2);
		if (fmt == 3) { o[0] = o[1] = o[2] = (u8)(c >> 8); o[3] = 255; return; }
		conv16(c, fmt, o);
		return;
	}
	if (mip) base += mip_texels(w) * 2;
	if (fmt == 3) { yuv422(base, (u32)x, (u32)y, w, !scan, o); return; }
	u32 a = scan ? base + 2 * ((u32)y * w + (u32)x) : base + 2 * twiddle_rect((u32)x, (u32)y, w, h);
	conv16(vh(a), fmt, o);
}

/* ---------------- software rasteriser ---------------- */

typedef struct {
	u8 *rgba;
	float *zbuf;
	int pass;  /* list type drawn in this pass */
} Raster;

static int wrap_coord(int c, int size, int clamp, int flip)
{
	if (clamp) return c < 0 ? 0 : c >= size ? size - 1 : c;
	if (flip) {
		int period = 2 * size;
		c %= period; if (c < 0) c += period;
		return c < size ? c : period - 1 - c;
	}
	c %= size; if (c < 0) c += size;
	return c;
}

static float blend_factor(int instr, const float src[4], const float dst[4], int ch, int is_src)
{
	const float *other = is_src ? dst : src;
	switch (instr) {
	case 0: return 0;
	case 1: return 1;
	case 2: return other[ch];
	case 3: return 1 - other[ch];
	case 4: return src[3];
	case 5: return 1 - src[3];
	case 6: return dst[3];
	default: return 1 - dst[3];
	}
}

static void shade(const PvrState *st, const float col[4], const float ofs[4], float u, float v, float out[4])
{
	u32 tsp = st->tsp;
	float c[4] = { col[0], col[1], col[2], (tsp >> 20) & 1 ? col[3] : 1 };
	if (!st->textured) {
		memcpy(out, c, sizeof c);
	} else {
		int w = 8 << ((tsp >> 3) & 7), h = 8 << (tsp & 7);
		if ((st->tcw >> 31) & 1) h = w;
		int cu = (tsp >> 16) & 1, cv = (tsp >> 15) & 1, fu = (tsp >> 18) & 1, fv = (tsp >> 17) & 1;
		float t[4];
		if ((tsp >> 13) & 3) {
			/* bilinear: blend the four texels around the sample point */
			float fx = u * w - 0.5f, fy = v * h - 0.5f;
			int ix = (int)floorf(fx), iy = (int)floorf(fy);
			float ax = fx - ix, ay = fy - iy;
			float acc[4] = { 0, 0, 0, 0 };
			for (int k = 0; k < 4; k++) {
				int dx = k & 1, dy = k >> 1;
				u8 t8[4];
				pvr_texel(tsp, st->tcw, wrap_coord(ix + dx, w, cu, fu), wrap_coord(iy + dy, h, cv, fv), t8);
				float wt = (dx ? ax : 1 - ax) * (dy ? ay : 1 - ay);
				for (int i = 0; i < 4; i++) acc[i] += t8[i] * wt;
			}
			for (int i = 0; i < 4; i++) t[i] = acc[i] / 255.f;
		} else {
			u8 t8[4];
			pvr_texel(tsp, st->tcw, wrap_coord((int)floorf(u * w), w, cu, fu), wrap_coord((int)floorf(v * h), h, cv, fv), t8);
			for (int i = 0; i < 4; i++) t[i] = t8[i] / 255.f;
		}
		if ((tsp >> 19) & 1) t[3] = 1;
		switch ((tsp >> 6) & 3) {
		case 0: out[0] = t[0]; out[1] = t[1]; out[2] = t[2]; out[3] = t[3]; break;
		case 1: for (int i = 0; i < 3; i++) out[i] = t[i] * c[i]; out[3] = t[3]; break;
		case 2: for (int i = 0; i < 3; i++) out[i] = t[i] * t[3] + c[i] * (1 - t[3]); out[3] = c[3]; break;
		default: for (int i = 0; i < 4; i++) out[i] = t[i] * c[i]; break;
		}
	}
	if (st->offset)
		for (int i = 0; i < 3; i++) out[i] += ofs[i];
	for (int i = 0; i < 4; i++) out[i] = out[i] < 0 ? 0 : out[i] > 1 ? 1 : out[i];
}

static int depth_pass(int mode, float z, float zb)
{
	switch (mode) {
	case 0: return 0;
	case 1: return z < zb;
	case 2: return z == zb;
	case 3: return z <= zb;
	case 4: return z > zb;
	case 5: return z != zb;
	case 6: return z >= zb;
	default: return 1;
	}
}

static void raster_tri(void *user, const PvrState *st, const PvrVert *a, const PvrVert *b, const PvrVert *c)
{
	Raster *r = user;
	if (st->list != r->pass)
		return;
	float minx = fminf(a->x, fminf(b->x, c->x)), maxx = fmaxf(a->x, fmaxf(b->x, c->x));
	float miny = fminf(a->y, fminf(b->y, c->y)), maxy = fmaxf(a->y, fmaxf(b->y, c->y));
	int x0 = (int)fmaxf(0, floorf(minx)), x1 = (int)fminf(W - 1, ceilf(maxx));
	int y0 = (int)fmaxf(0, floorf(miny)), y1 = (int)fminf(H - 1, ceilf(maxy));
	float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
	if (area == 0 || x0 > x1 || y0 > y1)
		return;
	int depth_mode = (st->isp >> 29) & 7;
	int zwrite = !((st->isp >> 26) & 1);
	int src_i = (st->tsp >> 29) & 7, dst_i = (st->tsp >> 26) & 7;
	float pt_ref = (pvr_reg(0x005F811Cu) & 0xFF) / 255.f;
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			float px = x + 0.5f, py = y + 0.5f;
			float w0 = ((b->x - px) * (c->y - py) - (c->x - px) * (b->y - py)) / area;
			float w1 = ((c->x - px) * (a->y - py) - (a->x - px) * (c->y - py)) / area;
			float w2 = 1 - w0 - w1;
			if (w0 < 0 || w1 < 0 || w2 < 0)
				continue;
			float z = w0 * a->z + w1 * b->z + w2 * c->z;
			float *zb = &r->zbuf[y * W + x];
			if (!depth_pass(r->pass == 2 ? 6 : depth_mode, z, *zb))
				continue;
			/* perspective-correct interpolation with z = 1/w */
			float iz = z != 0 ? 1 / z : 0;
			float u = (w0 * a->u * a->z + w1 * b->u * b->z + w2 * c->u * c->z) * iz;
			float v = (w0 * a->v * a->z + w1 * b->v * b->z + w2 * c->v * c->z) * iz;
			float col[4], ofs[4], s[4];
			const PvrVert *flat = c;
			for (int i = 0; i < 4; i++) {
				col[i] = st->gouraud ? w0 * a->col[i] + w1 * b->col[i] + w2 * c->col[i] : flat->col[i];
				ofs[i] = st->gouraud ? w0 * a->ofs[i] + w1 * b->ofs[i] + w2 * c->ofs[i] : flat->ofs[i];
			}
			shade(st, col, ofs, u, v, s);
			u8 *px8 = &r->rgba[(y * W + x) * 4];
			if (r->pass == 4 && s[3] < pt_ref)
				continue;
			if (r->pass == 2) {
				float d[4] = { px8[0] / 255.f, px8[1] / 255.f, px8[2] / 255.f, px8[3] / 255.f };
				for (int i = 0; i < 4; i++) {
					float o = s[i] * blend_factor(src_i, s, d, i, 1) + d[i] * blend_factor(dst_i, s, d, i, 0);
					px8[i] = (u8)(o < 0 ? 0 : o > 1 ? 255 : o * 255);
				}
			} else {
				for (int i = 0; i < 4; i++) px8[i] = (u8)(s[i] * 255);
				if (zwrite) *zb = z;
			}
		}
	}
}

/* 32-bit word from VRAM through the 32-bit path (two interleaved 8 MB banks) */
static u32 vram32(u32 o)
{
	o &= 0xFFFFFC;
	u32 lin = ((o & 0x7FFFFC) << 1) | (((o >> 23) & 1) << 2);
	return (u32)vb(lin) | (u32)vb(lin + 1) << 8 | (u32)vb(lin + 2) << 16 | (u32)vb(lin + 3) << 24;
}

/* The background polygon lives in VRAM at PARAM_BASE + tag address (ISP_BACKGND_T):
 * ISP, TSP, TCW, then three vertices of x, y, z, [uv], colour, [offset colour]. */
static void draw_background(Raster *r)
{
	u32 tag = pvr_reg(0x005F808Cu);
	u32 base = (pvr_reg(0x005F8020u) & 0xF00000) + (((tag >> 3) & 0x1FFFFF) * 4);
	u32 skip = (tag >> 24) & 7;
	PvrState st;
	memset(&st, 0, sizeof st);
	st.isp = vram32(base);
	st.tsp = vram32(base + 4);
	st.tcw = vram32(base + 8);
	st.textured = (st.isp >> 25) & 1;
	st.offset = (st.isp >> 24) & 1;
	st.gouraud = (st.isp >> 23) & 1;
	int uv16f = (st.isp >> 22) & 1;
	PvrVert v[4];
	u32 a = base + 12;
	for (int k = 0; k < 3; k++) {
		u32 w[8];
		for (u32 i = 0; i < 3 + skip && i < 8; i++) w[i] = vram32(a + 4 * i);
		memset(&v[k], 0, sizeof v[k]);
		memcpy(&v[k].x, &w[0], 4); memcpy(&v[k].y, &w[1], 4); memcpy(&v[k].z, &w[2], 4);
		int i = 3;
		if (st.textured) {
			if (uv16f) uv16(w[i++], &v[k].u, &v[k].v);
			else { memcpy(&v[k].u, &w[i], 4); memcpy(&v[k].v, &w[i + 1], 4); i += 2; }
		}
		packed(w[i++], v[k].col);
		if (st.offset) packed(w[i], v[k].ofs);
		a += 4 * (3 + skip);
	}
	/* fourth corner completes the parallelogram */
	v[3] = v[2];
	v[3].x = v[0].x + v[2].x - v[1].x; v[3].y = v[0].y + v[2].y - v[1].y;
	v[3].u = v[0].u + v[2].u - v[1].u; v[3].v = v[0].v + v[2].v - v[1].v;
	float depth;
	u32 d = pvr_reg(0x005F8088u);
	memcpy(&depth, &d, 4);
	for (int k = 0; k < 4; k++) v[k].z = depth;
	st.list = 0;
	st.isp = (st.isp & ~(7u << 29)) | (7u << 29);   /* always pass */
	int saved = r->pass;
	r->pass = 0;
	raster_tri(r, &st, &v[0], &v[1], &v[2]);
	raster_tri(r, &st, &v[0], &v[2], &v[3]);
	r->pass = saved;
}

void pvr_render_soft(const u8 *list, u32 len, u8 *rgba)
{
	static float zbuf[W * H];
	Raster r = { rgba, zbuf, 0 };
	for (int i = 0; i < W * H; i++) {
		zbuf[i] = 0;
		rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = 0;
		rgba[4 * i + 3] = 255;
	}
	draw_background(&r);
	static const int order[3] = { 0, 4, 2 };  /* opaque, punch-through, translucent */
	for (int k = 0; k < 3; k++) {
		r.pass = order[k];
		pvr_decode(list, len, raster_tri, &r);
	}
}

/* Render-to-texture: when FB_W_SOF1 has bit 24 set the game is drawing into a
 * texture (screen transitions, the shattering-glass effect...). Draw the list
 * in software and pack it into VRAM the way the PVR's framebuffer writer would:
 * FB_W_CTRL picks the 16-bit format, FB_W_LINESTRIDE the row pitch (8-byte
 * units), FB_X_CLIP/FB_Y_CLIP the size. */
void glframe_vram_dirty(u32 off, u32 len);

void pvr_render_rtt(const u8 *list, u32 len)
{
	static u8 rgba[W * H * 4];
	u32 ctrl = pvr_reg(0x005F8048u), sof1 = pvr_reg(0x005F8060u) & 0x00FFFFFFu;
	u32 xclip = pvr_reg(0x005F8068u), yclip = pvr_reg(0x005F806Cu);
	int w = (int)((xclip >> 16) & 0x7FF) + 1, h = (int)((yclip >> 16) & 0x3FF) + 1;
	if (w > W) w = W;
	if (h > H) h = H;
	u32 stride = (pvr_reg(0x005F804Cu) & 0x1FF) * 8;
	if (!stride) stride = (u32)w * 2;
	pvr_render_soft(list, len, rgba);
	int mode = ctrl & 7;
	u32 kbit = (ctrl >> 15) & 1, athresh = (ctrl >> 16) & 0xFF;
	for (int y = 0; y < h; y++) {
		u32 row = sof1 + (u32)y * stride;
		if (row + (u32)w * 2 > sizeof vram) break;
		for (int x = 0; x < w; x++) {
			const u8 *p = &rgba[(y * W + x) * 4];
			u32 r = p[0], g = p[1], b = p[2], a = p[3];
			u16 v;
			switch (mode) {
			case 1:  v = (u16)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); break;                       /* 565 */
			case 2:  v = (u16)((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4); break;     /* 4444 */
			case 3:  v = (u16)((a >= athresh) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3); break; /* 1555 */
			default: v = (u16)(kbit << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3); break;          /* 0555 + K */
			}
			memcpy(&vram[row + (u32)x * 2], &v, 2);
		}
	}
	glframe_vram_dirty(sof1, (u32)h * stride);
}
