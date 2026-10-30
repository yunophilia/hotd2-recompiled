/* TAS runner for the native harness: HOTD2_TAS=script.tas plays player 1 from a script.
 *
 * Scripts only drive the JVS inputs (gun position, trigger, reload, start, coins) and
 * read guest RAM to decide what to do; they never write game memory. See docs/tas.md
 * for the language. Exit codes: 0 success, 1 a `check` failed, 3 an `until` timed out
 * (or the frame limit came first), 2 script error.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "sh4ctx.h"
#include "jvs.h"

#define FPS 60
#define MAXLINES 20000
#define MAXCOND 8

/* --- guest RAM, read only --- */
static int mem_ok(u32 a, int size) { return is_ram(a) && is_ram(a + size - 1); }
static u8 m8(u32 a) { return mem_ok(a, 1) ? ram[a & RAM_MASK] : 0; }
static u16 m16(u32 a) { u16 v = 0; if (mem_ok(a, 2)) memcpy(&v, ram + (a & RAM_MASK), 2); return v; }
static u32 m32(u32 a) { u32 v = 0; if (mem_ok(a, 4)) memcpy(&v, ram + (a & RAM_MASK), 4); return v; }
static float mf(u32 a) { u32 v = m32(a); float f; memcpy(&f, &v, 4); return f; }

/* known addresses (player 1); see docs/tas.md */
static const struct { const char *name; u32 addr; } names[] = {
	{ "lives", 0x0C3D0430 },     /* u8, 3 -> 0 */
	{ "shots", 0x0C3D0454 },     /* u32 shots fired */
	{ "hits", 0x0C3D0458 },      /* u32 shots that hit */
	{ "ammo", 0x0C3D0444 },      /* u32 bullets left (player struct +36) */
	{ "player", 0x0C3D0420 },    /* player 1 struct (256 bytes per player) */
	{ "mode", 0x0C3D07B4 },      /* u32 main game mode */
	{ "submode", 0x0C3D07B8 },
	{ "bosshp", 0x0C3D11FC },    /* f32 boss health fraction */
	{ "gunx", 0x0C3D17CC },      /* s16 gun position, game units 0..255 */
	{ "guny", 0x0C3D17CE },
	{ "aimx", 0x0C3D17D0 },      /* s16 gun offset from the screen centre (ray units) */
	{ "aimy", 0x0C3D17D2 },
	{ "targets", 0x0C14E100 },   /* u32 shootable objects tested last frame */
	{ "focal", 0x0C403438 },     /* f32 projection distance */
};

/* --- script --- */
typedef struct { char *text; const char *file; int line; int match; } Line;
static Line lines[MAXLINES];
static int nlines;

static void die(int code, const char *fmt, ...);

static char *trim(char *s)
{
	while (isspace((unsigned char)*s)) s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	return s;
}

static void load_script(const char *path, int depth)
{
	if (depth > 16) die(2, "include nesting too deep at %s", path);
	FILE *f = fopen(path, "r");
	if (!f) die(2, "cannot open %s", path);
	char buf[1024];
	int ln = 0;
	char *fname = strdup(path);
	while (fgets(buf, sizeof buf, f)) {
		ln++;
		char *h = strchr(buf, '#');
		if (h) *h = 0;
		char *s = trim(buf);
		if (!*s) continue;
		if (!strncmp(s, "include ", 8)) {
			char inc[1024];
			const char *arg = trim(s + 8);
			const char *slash = strrchr(path, '/');
			if (arg[0] != '/' && slash) snprintf(inc, sizeof inc, "%.*s/%s", (int)(slash - path), path, arg);
			else snprintf(inc, sizeof inc, "%s", arg);
			load_script(inc, depth + 1);
			continue;
		}
		if (nlines >= MAXLINES) die(2, "script too long");
		lines[nlines].text = strdup(s);
		lines[nlines].file = fname;
		lines[nlines].line = ln;
		lines[nlines].match = -1;
		nlines++;
	}
	fclose(f);
}

/* --- run state --- */
static int pc;                       /* next line */
static int cur = -1;                 /* line of the running timed command */
static struct { int line, left; } loops[64];
static int nloops;
static u64 now;                      /* current frame */
static FILE *tas_log;
static int checks_ok, checks_bad;

enum { A_NONE, A_WAIT, A_HOLD, A_TAP, A_MASH, A_UNTIL };
static struct {
	int kind;
	long left, total;                /* frames */
	u32 mask;                        /* input held / tapped */
	int on, period;                  /* tap: frames pressed; mash: period */
	char conds[512];                 /* until */
	u32 mash;
} act;

#define IN_FIRE   1u
#define IN_RELOAD 2u
#define IN_START  4u
#define IN_SERVICE 8u
#define IN_TEST  16u

static u32 bg_mask;                  /* `set` */
static int aim_x = 320, aim_y = 240;
static char shot_name[256];

/* auto play */
static int auto_on;
static int auto_fire_phase;
static int auto_reload;              /* frames of reload left */
static u32 auto_last_obj;
static int auto_have;
static float auto_sx, auto_sy;
/* input pixels -> ray offset, fitted from a sweep (docs/tas.md): offset = (px - cx) * k */
static float cal_cx = 320.0f, cal_cy = 240.0f, cal_kx = 300.0f / 320, cal_ky = -220.0f / 240;

static int auto_debug;
static int ignore_after = 2, ignore_frames = 90;
/* objects by task function (object word 0) that are only shot once nearer than `depth`;
 * depth 0: never (innocents: shooting one costs a life) */
static struct { u32 task; float depth; } avoid[32] = { { 0x0C092DE6, 0 } };
static int navoid = 1;

static void logmsg(const char *fmt, ...)
{
	va_list ap;
	char buf[1024];
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	fprintf(tas_log, "[tas %6llu] %s\n", (unsigned long long)now, buf);
	fflush(tas_log);
}

static void summary(void)
{
	logmsg("frames %llu  lives %u  shots %u  hits %u  checks %d passed, %d failed",
	       (unsigned long long)now, m8(0x0C3D0430), m32(0x0C3D0454), m32(0x0C3D0458), checks_ok, checks_bad);
}

static void die(int code, const char *fmt, ...)
{
	va_list ap;
	char buf[1024];
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (!tas_log) tas_log = stderr;
	if (cur >= 0 && cur < nlines) logmsg("%s:%d: %s", lines[cur].file, lines[cur].line, buf);
	else logmsg("%s", buf);
	summary();
	exit(code);
}

/* --- expressions: ADDR | NAME | [EXPR] with +/- offsets --- */
static const char *skipws(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

static int parse_addr(const char **ps, u32 *out)
{
	const char *s = skipws(*ps);
	u32 v;
	if (*s == '[') {
		s++;
		if (!parse_addr(&s, &v)) return 0;
		s = skipws(s);
		if (*s != ']') return 0;
		s++;
		v = m32(v);
	} else if (isalpha((unsigned char)*s) && !(s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
		const char *e = s;
		while (isalnum((unsigned char)*e) || *e == '_') e++;
		size_t n = (size_t)(e - s);
		int found = 0;
		for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
			if (strlen(names[i].name) == n && !strncmp(names[i].name, s, n)) { v = names[i].addr; found = 1; break; }
		if (!found) {
			/* bare hex like 0C3D0430 starts with a digit; names with hex letters (e.g. "ad") are names */
			char *end;
			v = (u32)strtoul(s, &end, 16);
			if (end != e) return 0;
		}
		s = e;
	} else {
		char *end;
		v = (u32)strtoul(s, &end, 16);
		if (end == s) return 0;
		s = end;
	}
	for (;;) {
		const char *t = skipws(s);
		if (*t != '+' && *t != '-') break;
		int neg = *t == '-';
		t = skipws(t + 1);
		char *end;
		u32 off = (u32)strtoul(t, &end, 0);   /* offsets are decimal unless 0x */
		if (end == t) break;
		v = neg ? v - off : v + off;
		s = end;
	}
	*ps = s;
	*out = v;
	return 1;
}

static int read_typed(const char *type, u32 a, double *out)
{
	if (!strcmp(type, "u8")) *out = m8(a);
	else if (!strcmp(type, "s8")) *out = (s8)m8(a);
	else if (!strcmp(type, "u16")) *out = m16(a);
	else if (!strcmp(type, "s16")) *out = (s16)m16(a);
	else if (!strcmp(type, "u32")) *out = m32(a);
	else if (!strcmp(type, "s32")) *out = (s32)m32(a);
	else if (!strcmp(type, "f32")) *out = mf(a);
	else return 0;
	return 1;
}

/* one condition: TYPE EXPR OP VALUE, or `frame OP N` */
static int eval_cond(const char *s, const char **rest, double *lhs_out)
{
	char type[8];
	s = skipws(s);
	int n = 0;
	while (isalnum((unsigned char)s[n]) && n < 7) { type[n] = s[n]; n++; }
	type[n] = 0;
	s += n;
	double lhs;
	if (!strcmp(type, "frame")) lhs = (double)now;
	else {
		u32 a;
		if (!parse_addr(&s, &a)) die(2, "bad address in condition");
		if (!read_typed(type, a, &lhs)) die(2, "bad type '%s'", type);
	}
	s = skipws(s);
	char op[3] = { 0 };
	op[0] = *s++;
	if (*s == '=') op[1] = *s++;
	char *end;
	double rhs = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? (double)strtoul(s, &end, 16) : strtod(s, &end);
	if (end == s) die(2, "bad value in condition");
	*rest = end;
	*lhs_out = lhs;
	if (!strcmp(op, "==")) return lhs == rhs;
	if (!strcmp(op, "!=")) return lhs != rhs;
	if (!strcmp(op, "<")) return lhs < rhs;
	if (!strcmp(op, "<=")) return lhs <= rhs;
	if (!strcmp(op, ">")) return lhs > rhs;
	if (!strcmp(op, ">=")) return lhs >= rhs;
	die(2, "bad operator '%s'", op);
	return 0;
}

/* COND [and COND ...] [or COND ...] (left to right, no precedence beyond that) */
static int eval_conds(const char *s)
{
	double lhs;
	int r = eval_cond(s, &s, &lhs);
	for (;;) {
		s = skipws(s);
		if (!strncmp(s, "and ", 4)) { int b = eval_cond(s + 4, &s, &lhs); r = r && b; }
		else if (!strncmp(s, "or ", 3)) { int b = eval_cond(s + 3, &s, &lhs); r = r || b; }
		else break;
	}
	return r;
}

/* log text with {TYPE EXPR} substitutions */
static void expand(const char *s, char *out, size_t cap)
{
	size_t n = 0;
	while (*s && n + 1 < cap) {
		if (*s == '{') {
			const char *e = strchr(s, '}');
			if (e) {
				char tmp[128], type[8];
				snprintf(tmp, sizeof tmp, "%.*s", (int)(e - s - 1), s + 1);
				const char *t = skipws(tmp);
				int k = 0;
				while (isalnum((unsigned char)t[k]) && k < 7) { type[k] = t[k]; k++; }
				type[k] = 0;
				t += k;
				u32 a;
				double v;
				char val[64];
				if (!strcmp(type, "frame")) snprintf(val, sizeof val, "%llu", (unsigned long long)now);
				else if (parse_addr(&t, &a) && read_typed(type, a, &v)) {
					if (!strcmp(type, "f32")) snprintf(val, sizeof val, "%g", v);
					else if (type[0] == 'u' && v > 0xFFFF) snprintf(val, sizeof val, "0x%08X", (u32)v);
					else snprintf(val, sizeof val, "%.0f", v);
				} else snprintf(val, sizeof val, "?");
				n += (size_t)snprintf(out + n, cap - n, "%s", val);
				if (n >= cap) n = cap - 1;
				s = e + 1;
				continue;
			}
		}
		out[n++] = *s++;
	}
	out[n] = 0;
}

static long parse_time(const char *s, const char **rest)
{
	char *end;
	double v = strtod(s, &end);
	if (end == s) die(2, "bad time '%s'", s);
	long frames;
	if (*end == 'f') { frames = (long)v; end++; }
	else frames = (long)(v * FPS + 0.5);
	if (rest) *rest = end;
	return frames;
}

static u32 parse_input(const char *s, const char **rest)
{
	u32 m = 0;
	s = skipws(s);
	for (;;) {
		const char *e = s;
		while (isalpha((unsigned char)*e)) e++;
		size_t n = (size_t)(e - s);
		if (!n) break;
		if (n == 4 && !strncasecmp(s, "FIRE", 4)) m |= IN_FIRE;
		else if (n == 7 && !strncasecmp(s, "TRIGGER", 7)) m |= IN_FIRE;
		else if (n == 6 && !strncasecmp(s, "RELOAD", 6)) m |= IN_RELOAD;
		else if (n == 5 && !strncasecmp(s, "START", 5)) m |= IN_START;
		else if (n == 7 && !strncasecmp(s, "SERVICE", 7)) m |= IN_SERVICE;
		else if (n == 4 && !strncasecmp(s, "TEST", 4)) m |= IN_TEST;
		else break;
		s = e;
		if (*s != '+') break;
		s++;
	}
	if (!m) die(2, "bad input '%s'", s);
	if (rest) *rest = s;
	return m;
}

/* --- auto aim: shootable spheres from the game's own hit-test data --- */
typedef struct { float sx, sy, depth, r, ax, ay; u32 obj, flags; } Cand;
#define MAXCAND 512
static Cand cands[MAXCAND];
static int ncands;

static void add_sphere(u32 obj, float x, float y, float z, float r, u32 flags)
{
	if (ncands >= MAXCAND || !(z < -1.0f) || !(r > 0)) return;
	float F = mf(0x0C403438);
	if (!(F > 1 && F < 10000)) F = 640.0f;
	Cand *c = &cands[ncands++];
	c->sx = -F * x / z;
	c->sy = -F * y / z;
	c->depth = -z;
	c->r = F * r / -z;
	c->obj = obj;
	c->flags = flags;
}

static void walk_parts(u32 obj, u32 node, int depth)
{
	if (depth > 32 || !mem_ok(node, 28)) return;
	s16 idx = (s16)m16(node + 20);
	u32 part = obj + 0x208 + (u32)(s32)idx * 0x90;
	if (idx >= 0 && idx < 128 && m32(part)) {
		u32 pf = m32(part + 116);
		if (!(pf & 0x10))   /* bit 4 set: tested against a mesh instead */
			add_sphere(obj, mf(part + 104), mf(part + 108), mf(part + 112), mf(part + 120), pf);
	}
	s16 kids = (s16)m16(node + 22);
	for (int k = 0; k < kids && k < 64; k++)
		walk_parts(obj, m32(node + 24 + 4 * k), depth + 1);
}

static void gather(void)
{
	ncands = 0;
	u32 n = m32(0x0C14E100);
	if (n > 256) n = 256;
	for (u32 i = 0; i < n; i++) {
		u32 obj = m32(0x0C146D70 + i * 20);
		if (!mem_ok(obj, 0x2000)) continue;
		u32 fl = m32(obj + 48);
		float near = -1;
		for (int k = 0; k < navoid; k++) if (m32(obj) == avoid[k].task) near = avoid[k].depth;
		if (near == 0) continue;
		if (fl & 0x10) continue;      /* mesh-tested objects (scenery pieces) */
		if (fl & 0x100) continue;     /* hits are not counted (dying) */
		/* single-sphere objects with bit 31 (thrown weapons, props) take one hit; once shot
		 * (bit 3 pending, bits 1-2 by a player) they stay shootable but nothing more happens */
		if ((fl & 0x80000000u) && (fl & 0x0E)) continue;
		int parts = 0;
		if (fl & 0x80) {
			u32 root = m32(0x0CB59DD4 + 4 * (u32)(s32)(s16)m16(obj + 0x1F0));
			if (mem_ok(root, 28) && (s16)m16(root + 22) > 0 && !(fl & 0x8000)) {
				walk_parts(obj, root, 0);   /* the game then tests only the parts */
				parts = 1;
			}
		}
		if (!parts)
			add_sphere(obj, mf(obj + 108), mf(obj + 112), mf(obj + 116), mf(obj + 0x120), fl);
		if (near > 0)
			for (int k = ncands - 1; k >= 0 && cands[k].obj == obj; k--)
				if (cands[k].depth > near) cands[k] = cands[--ncands];
	}
}


/* objects that took shots without the hit counter moving (dead, dying, or not hurt by bullets)
 * get a lower priority for a while */
#define NIGNORE 64
static struct { u32 obj; u64 until; } ignored[NIGNORE];
static u32 strike_obj;
static int strikes;

static int is_ignored(u32 obj)
{
	for (int i = 0; i < NIGNORE; i++)
		if (ignored[i].obj == obj && ignored[i].until > now) return 1;
	return 0;
}

static void ignore(u32 obj, int frames)
{
	int slot = 0;
	for (int i = 0; i < NIGNORE; i++) {
		if (ignored[i].obj == obj || ignored[i].until <= now) { slot = i; break; }
		if (ignored[i].until < ignored[slot].until) slot = i;
	}
	ignored[slot].obj = obj;
	ignored[slot].until = now + (u64)frames;
}

/* watch the game's counters: a counted shot at our target that did not add a hit is a strike */
static void score_shots(void)
{
	static u32 last_shots, last_hits;
	u32 sh = m32(0x0C3D0454), hi = m32(0x0C3D0458);
	if (hi != last_hits) strikes = 0;
	else if (sh != last_shots && auto_have) {
		if (strike_obj != auto_last_obj) { strike_obj = auto_last_obj; strikes = 0; }
		if (++strikes >= ignore_after) { ignore(auto_last_obj, ignore_frames); strikes = 0; }
	}
	last_shots = sh;
	last_hits = hi;
}

/* can the gun reach this sphere? sets the aim point (the centre, or the nearest point on the
 * screen edge while that is still well inside the sphere) */
static int reachable(Cand *c)
{
	float lim_x = (639 - cal_cx) * fabsf(cal_kx) - 2, lim_y = (479 - cal_cy) * fabsf(cal_ky) - 2;
	c->ax = fmaxf(-lim_x, fminf(lim_x, c->sx));
	c->ay = fmaxf(-lim_y, fminf(lim_y, c->sy));
	float dx = c->ax - c->sx, dy = c->ay - c->sy;
	return dx * dx + dy * dy <= 0.5f * c->r * c->r;
}

/* pick a target: the nearest object (by its nearest sphere), keeping the one we are on while it
 * lasts; on that object aim at the topmost sphere (for people, the head) */
static const Cand *pick(void)
{
	u32 obj = 0;
	float bs = 0;
	for (int i = 0; i < ncands; i++) {
		Cand *c = &cands[i];
		if (!reachable(c)) continue;
		/* nearest first; stay on the current target, avoid ones that stopped taking hits */
		float score = c->depth * (c->obj == auto_last_obj ? 0.5f : 1.0f) * (is_ignored(c->obj) ? 4.0f : 1.0f);
		if (!obj || score < bs) { obj = c->obj; bs = score; }
	}
	const Cand *best = NULL;
	for (int i = 0; i < ncands; i++) {
		Cand *c = &cands[i];
		if (c->obj != obj || !reachable(c)) continue;
		if (!best || c->sy > best->sy) best = c;
	}
	return best;
}

static void auto_step(u32 *mask, int *x, int *y)
{
	u32 ammo = m32(0x0C3D0444);
	static u8 last_lives;
	u8 lives = m8(0x0C3D0430);
	if (lives < last_lives) {
		logmsg("auto: life lost (%u left), aiming at %08X %.0f,%.0f", lives, auto_have ? auto_last_obj : 0, auto_sx, auto_sy);
		if (auto_debug && !shot_name[0]) snprintf(shot_name, sizeof shot_name, "life_%llu.ppm", (unsigned long long)now);
	}
	last_lives = lives;
	if (auto_reload > 0) {
		auto_reload--;
		if (auto_reload >= 4) *mask |= IN_RELOAD;
		return;
	}
	score_shots();
	gather();
	const Cand *c = pick();
	if (auto_debug < 0) {
		/* one line per shot the game counted: where the gun was against the target we chose */
		static u32 last_shots, last_hits;
		u32 sh = m32(0x0C3D0454), hi = m32(0x0C3D0458);
		if (sh != last_shots) {
			u32 rec = 0x0C14A538 + 60 * m32(0x0C149548 + 4);   /* nearest hit record of the last test */
			logmsg("shot %u: gun %d,%d  target %08X (task %08X flags %08X) %.0f,%.0f r %.0f  hits %+d  record %08X/%08X flags %08X n %u",
			       sh, (s16)m16(0x0C3D17D0), (s16)m16(0x0C3D17D2), auto_last_obj, m32(auto_last_obj), m32(auto_last_obj + 48), auto_sx, auto_sy, c ? c->r : 0,
			       (int)(hi - last_hits), m32(rec + 36), m32(rec + 40), m32(rec + 44), m32(0x0C14E104));
		}
		last_shots = sh;
		last_hits = hi;
	}
	if (auto_debug && now % auto_debug == 0) {
		logmsg("auto: %d candidates, target %s ammo %u", ncands, c ? "yes" : "no", ammo);
		for (u32 i = 0; i < m32(0x0C14E100) && i < 64; i++) {
			u32 o = m32(0x0C146D70 + i * 20);
			logmsg("  list %u: %08X flags %08X  %08X %08X %08X %08X", i, o, m32(o + 48), m32(0x0C146D70 + i * 20 + 4),
			       m32(0x0C146D70 + i * 20 + 8), m32(0x0C146D70 + i * 20 + 12), m32(0x0C146D70 + i * 20 + 16));
		}
		for (int i = 0; i < ncands; i++)
			logmsg("  obj %08X task %08X model %d flags %08X screen %.0f,%.0f r %.0f depth %.0f", cands[i].obj, m32(cands[i].obj), (s16)m16(cands[i].obj + 0x1F0), cands[i].flags,
			       cands[i].sx, cands[i].sy, cands[i].r, cands[i].depth);
	}
	if (ammo == 0 || (!c && ammo < 6)) {
		auto_reload = 8;
		*mask |= IN_RELOAD;
		auto_reload--;
		return;
	}
	if (!c) { auto_have = 0; auto_fire_phase = 0; return; }
	auto_last_obj = c->obj;
	auto_have = 1;
	auto_sx = c->ax;
	auto_sy = c->ay;
	int px = (int)lrintf(cal_cx + c->ax / cal_kx), py = (int)lrintf(cal_cy + c->ay / cal_ky);
	if (px < 0) px = 0;
	if (px > 639) px = 639;
	if (py < 0) py = 0;
	if (py > 479) py = 479;
	*x = px;
	*y = py;
	/* press for 2 frames, release for 2 */
	if ((auto_fire_phase++ & 3) < 2) *mask |= IN_FIRE;
}

/* --- commands --- */
static int match_block(void)
{
	int stack[64], sp = 0;
	for (int i = 0; i < nlines; i++) {
		if (!strncmp(lines[i].text, "repeat", 6) && (lines[i].text[6] == ' ' || !lines[i].text[6])) {
			if (sp >= 64) return 0;
			stack[sp++] = i;
		} else if (!strcmp(lines[i].text, "end")) {
			if (!sp) { cur = i; die(2, "end without repeat"); }
			int r = stack[--sp];
			lines[r].match = i;
			lines[i].match = r;
		}
	}
	if (sp) { cur = stack[sp - 1]; die(2, "repeat without end"); }
	return 1;
}

static void finish(int code)
{
	if (code == 0 && checks_bad) code = 1;
	logmsg("%s (exit %d)", code == 0 ? "done" : "finished with failures", code);
	summary();
	exit(code);
}

/* run instant commands until a timed one starts; returns 0 at the end of the script */
static int next_command(void)
{
	for (int budget = 0; budget < 100000; budget++) {
		if (pc >= nlines) return 0;
		cur = pc++;
		char *t = lines[cur].text;
		char cmd[32];
		int n = 0;
		while (t[n] && !isspace((unsigned char)t[n]) && n < 31) { cmd[n] = t[n]; n++; }
		cmd[n] = 0;
		const char *a = skipws(t + n);
		memset(&act, 0, sizeof act);
		if (!strcmp(cmd, "wait")) {
			act.kind = A_WAIT; act.left = parse_time(a, NULL);
		} else if (!strcmp(cmd, "hold")) {
			act.kind = A_HOLD; act.left = parse_time(a, &a); act.mask = parse_input(a, NULL);
		} else if (!strcmp(cmd, "tap") || !strcmp(cmd, "fire") || !strcmp(cmd, "start") || !strcmp(cmd, "reload")) {
			const char *r = a;
			if (!strcmp(cmd, "tap")) act.mask = parse_input(a, &r);
			else act.mask = cmd[0] == 'f' ? IN_FIRE : cmd[0] == 's' ? IN_START : IN_RELOAD;
			r = skipws(r);
			act.on = *r ? (int)parse_time(r, NULL) : (act.mask & IN_RELOAD ? 6 : 3);
			act.kind = A_TAP; act.left = act.on * 2;
		} else if (!strcmp(cmd, "mash")) {
			act.mask = parse_input(a, &a);
			a = skipws(a);
			act.left = parse_time(a, &a);
			a = skipws(a);
			act.total = act.left;
			act.period = *a ? (int)parse_time(a, NULL) : 12;
			if (act.period < 2) act.period = 2;
			act.kind = A_MASH;
		} else if (!strcmp(cmd, "until")) {
			act.kind = A_UNTIL;
			act.left = act.total = 60 * FPS;
			snprintf(act.conds, sizeof act.conds, "%s", a);
			char *to = strstr(act.conds, " timeout ");
			char *ms = strstr(act.conds, " mash ");
			if (to) act.left = act.total = parse_time(to + 9, NULL);
			if (ms) act.mash = parse_input(ms + 6, NULL);
			if (to) *to = 0;
			if (ms) *ms = 0;
			if (eval_conds(act.conds)) continue;
		} else if (!strcmp(cmd, "check")) {
			char buf[512];
			snprintf(buf, sizeof buf, "%s", a);
			char *msg = strstr(buf, " msg ");
			if (msg) *msg = 0;
			int ok = eval_conds(buf);
			if (ok) checks_ok++; else checks_bad++;
			logmsg("%s %s%s%s", ok ? "PASS" : "FAIL", buf, msg ? ": " : "", msg ? msg + 5 : "");
			continue;
		} else if (!strcmp(cmd, "coin")) {
			jvs_input.coins[0]++;
			continue;
		} else if (!strcmp(cmd, "aim")) {
			char *e;
			aim_x = (int)strtol(a, &e, 10);
			aim_y = (int)strtol(e, NULL, 10);
			continue;
		} else if (!strcmp(cmd, "set")) {
			bg_mask = *a ? parse_input(a, NULL) : 0;
			continue;
		} else if (!strcmp(cmd, "auto")) {
			if (!strncmp(a, "on", 2)) auto_on = 1;
			else if (!strncmp(a, "off", 3)) { auto_on = 0; auto_reload = 0; }
			else if (!strncmp(a, "debug", 5)) auto_debug = atoi(a + 5);
			else if (!strncmp(a, "avoid", 5)) {
				if (navoid >= 32) die(2, "too many avoid entries");
				char *e;
				avoid[navoid].task = (u32)strtoul(a + 5, &e, 16);
				avoid[navoid++].depth = strtof(e, NULL);
			} else if (!strncmp(a, "ignore", 6)) {
				if (sscanf(a + 6, "%d %d", &ignore_after, &ignore_frames) != 2) die(2, "auto ignore SHOTS FRAMES");
			} else die(2, "auto on|off|debug N|ignore SHOTS FRAMES");
			continue;
		} else if (!strcmp(cmd, "calibrate")) {
			if (sscanf(a, "%f %f %f %f", &cal_cx, &cal_cy, &cal_kx, &cal_ky) != 4) die(2, "calibrate CX CY KX KY");
			continue;
		} else if (!strcmp(cmd, "log")) {
			char buf[1024];
			expand(a, buf, sizeof buf);
			logmsg("%s", buf);
			continue;
		} else if (!strcmp(cmd, "shot")) {
			char name[200];
			expand(a, name, sizeof name);
			snprintf(shot_name, sizeof shot_name, "%s.ppm", name);
			continue;
		} else if (!strcmp(cmd, "dump")) {
			char base[200], name[256];
			expand(a, base, sizeof base);
			snprintf(name, sizeof name, "%s.bin", base);
			FILE *f = fopen(name, "wb");
			if (f) { fwrite(ram, 1, RAM_SIZE, f); fclose(f); }
			logmsg("dump %s", name);
			continue;
		} else if (!strcmp(cmd, "repeat")) {
			if (nloops >= 64) die(2, "repeat nesting too deep");
			int k = *a ? atoi(a) : -1;   /* no count: forever (leave with until/quit) */
			if (k == 0) { pc = lines[cur].match + 1; continue; }
			loops[nloops].line = cur;
			loops[nloops].left = k;
			nloops++;
			continue;
		} else if (!strcmp(cmd, "end")) {
			if (!nloops) die(2, "end without repeat");
			if (loops[nloops - 1].left > 0) loops[nloops - 1].left--;
			if (loops[nloops - 1].left != 0) pc = loops[nloops - 1].line + 1;
			else nloops--;
			continue;
		} else if (!strcmp(cmd, "quit")) {
			finish(*a ? atoi(a) : 0);
		} else {
			die(2, "unknown command '%s'", cmd);
		}
		return 1;
	}
	die(2, "too many commands without waiting (endless loop?)");
	return 0;
}

static int active;

int tas_active(void) { return active; }

const char *tas_shot_name(void)
{
	static char name[256];
	if (!shot_name[0]) return NULL;
	snprintf(name, sizeof name, "%s", shot_name);
	shot_name[0] = 0;
	return name;
}

void tas_frame_limit(void) { die(3, "frame limit reached before the script ended"); }

/* called once per frame (vblank in) */
void tas_frame(u64 frame)
{
	static int loaded;
	if (!loaded) {
		loaded = 1;
		const char *path = getenv("HOTD2_TAS");
		if (!path || !*path) return;
		tas_log = stderr;
		if (getenv("HOTD2_TAS_LOG") && !(tas_log = fopen(getenv("HOTD2_TAS_LOG"), "w"))) tas_log = stderr;
		load_script(path, 0);
		match_block();
		active = 1;
		logmsg("script %s: %d lines", path, nlines);
	}
	if (!active) return;
	now = frame;

	u32 mask = bg_mask;
	int x = aim_x, y = aim_y;
	for (;;) {
		if (act.kind == A_NONE && !next_command()) finish(0);
		if (act.kind == A_UNTIL) {
			if (eval_conds(act.conds)) { act.kind = A_NONE; continue; }
			if (act.left-- <= 0) die(3, "timeout after %.1f s waiting for %s", act.total / (double)FPS, act.conds);
			if (act.mash && ((act.total - act.left) % 12) < 3) mask |= act.mash;
			break;
		}
		if (act.left <= 0) { act.kind = A_NONE; continue; }
		long done = (act.kind == A_TAP ? act.on * 2 : act.total) - act.left;
		if (act.kind == A_HOLD) mask |= act.mask;
		else if (act.kind == A_TAP && act.left > act.on) mask |= act.mask;
		else if (act.kind == A_MASH && (done % act.period) < act.period / 2) mask |= act.mask;
		act.left--;
		break;
	}
	if (auto_on && !(mask & (IN_FIRE | IN_RELOAD))) auto_step(&mask, &x, &y);

	jvs_input.gun_x[0] = (u16)x;
	jvs_input.gun_y[0] = (u16)y;
	jvs_input.offscreen[0] = (mask & IN_RELOAD) != 0;
	u16 b = jvs_input.buttons[0] & ~(JVS_START | JVS_TRIGGER | JVS_SERVICE);
	if (mask & IN_FIRE) b |= JVS_TRIGGER;
	if (mask & IN_START) b |= JVS_START;
	if (mask & IN_SERVICE) b |= JVS_SERVICE;
	jvs_input.buttons[0] = b;
	jvs_input.test = (mask & IN_TEST) != 0;
}
