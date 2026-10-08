/* NAOMI Maple bus + JVS I/O board, high-level emulation. See jvs.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jvs.h"

JvsInput jvs_input;
u8 naomi_eeprom[128];

static int jvs_log = -1;
#define LOG(...) do { if (jvs_log) fprintf(stderr, __VA_ARGS__); } while (0)

/* ---------------- I/O board (node 1) ---------------- */

#define NODES 1
#define BROADCAST 0xFF
static const char board_id[] = "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";

/* Handle one JVS packet body addressed to the board.
 * Writes "E0 00 len status reports..." into out and returns its length, 0 = no reply. */
static u32 board_message(const u8 *in, u32 len, u8 *out)
{
	u32 n = 0;
	if (len == 0)
		return 0;
	if (in[0] == 0xF0)                       /* reset */
		return 0;
	if (in[0] == 0xF1 && (len < 2 || in[1] != 1)) /* set address, not ours */
		return 0;

	out[n++] = 0xE0;
	out[n++] = 0x00;
	u32 len_at = n++;
	out[n++] = 1; /* status: normal */

	switch (in[0]) {
	case 0xF1: out[n++] = 1; break;
	case 0x10:
		out[n++] = 1;
		memcpy(out + n, board_id, sizeof(board_id));
		n += sizeof(board_id);
		break;
	case 0x11: out[n++] = 1; out[n++] = 0x13; break; /* command format 1.3 */
	case 0x12: out[n++] = 1; out[n++] = 0x30; break; /* JVS 3.0 */
	case 0x13: out[n++] = 1; out[n++] = 0x10; break; /* comms 1.0 */
	case 0x14: {
		static const u8 features[] = {
			0x01, 2, 13, 0,   /* switches: 2 players, 13 each */
			0x02, 2, 0, 0,    /* coin slots */
			0x06, 16, 16, 2,  /* screen position: 16-bit X/Y, 2 channels */
			0x12, 6, 0, 0,    /* general purpose outputs */
			0x00,
		};
		out[n++] = 1;
		memcpy(out + n, features, sizeof(features));
		n += sizeof(features);
		break;
	}
	case 0x15: out[n++] = 1; break; /* main board ID: just acknowledge */
	default: {
		/* a sequence of input/output requests */
		u32 i = 0;
		while (i < len) {
			u8 cmd = in[i];
			if (cmd == 0x20) {          /* switches: players, bytes per player */
				out[n++] = 1;
				out[n++] = jvs_input.test ? 0x80 : 0;
				for (u32 p = 0; p < in[i + 1]; p++) {
					u16 b = p < 2 ? jvs_input.buttons[p] : 0;
					if (p < 2 && jvs_input.offscreen[p])
						b |= JVS_RELOAD;
					out[n++] = (u8)(b >> 8);
					if (in[i + 2] > 1) out[n++] = (u8)b;
				}
				i += 3;
			} else if (cmd == 0x21) {   /* coins */
				out[n++] = 1;
				for (u32 s = 0; s < in[i + 1]; s++) {
					u16 c = s < 2 ? jvs_input.coins[s] : 0;
					out[n++] = (u8)((c >> 8) & 0x3F);
					out[n++] = (u8)c;
				}
				i += 2;
			} else if (cmd == 0x22) {   /* analog: none, report zeros */
				out[n++] = 1;
				for (u32 ch = 0; ch < in[i + 1]; ch++) { out[n++] = 0; out[n++] = 0; }
				i += 2;
			} else if (cmd == 0x25) {   /* screen position, 1-based channel */
				u32 p = in[i + 1] - 1;
				u16 x = 0, y = 0;
				if (p < 2 && !jvs_input.offscreen[p]) { x = jvs_input.gun_x[p]; y = jvs_input.gun_y[p]; }
				out[n++] = 1;
				out[n++] = (u8)(x >> 8); out[n++] = (u8)x;
				out[n++] = (u8)(y >> 8); out[n++] = (u8)y;
				i += 2;
			} else if (cmd == 0x30) {   /* decrease coins: slot, amount(2) */
				u32 s = in[i + 1] - 1;
				u16 amount = (u16)(in[i + 2] << 8 | in[i + 3]);
				if (s < 2) jvs_input.coins[s] = jvs_input.coins[s] > amount ? jvs_input.coins[s] - amount : 0;
				out[n++] = 1;
				i += 4;
			} else if (cmd == 0x31) {   /* add payout: slot, amount(2) */
				out[n++] = 1;
				i += 4;
			} else if (cmd == 0x32) {   /* general purpose outputs: byte count, data */
				out[n++] = 1;
				i += 2 + in[i + 1];
			} else if (cmd == 0x33) {   /* analog outputs */
				out[n++] = 1;
				i += 2 + 2 * in[i + 1];
			} else {
				LOG("jvs: unknown request %02x\n", cmd);
				out[n++] = 2;
				break;
			}
		}
		break;
	}
	}
	out[len_at] = (u8)(n - 2);
	return n;
}

/* ---------------- MIE (Maple <-> JVS bridge) ---------------- */

static u8 repeat_req[NODES][256];
static u8 rx_buf[32][300];
static u32 rx_len[32];

static void deliver(u32 node, u32 channel, const u8 *msg, u32 len)
{
	u8 reply[256];
	if (node < 1 || node > NODES)
		return;
	u32 n = board_message(msg, len, reply);
	if (n == 0 || rx_len[channel] + n + 3 > sizeof(rx_buf[0]))
		return;
	u8 *p = rx_buf[channel] + rx_len[channel];
	p[0] = (u8)node; p[1] = 0; p[2] = (u8)n;
	memcpy(p + 3, reply, n);
	rx_len[channel] += n + 3;
}

/* Send msg to node (or all), optionally combined with the node's stored repeat request. */
static void transmit(u32 node, u32 channel, const u8 *msg, u32 len, int with_repeat, int repeat_first)
{
	u8 buf[512];
	if (node == BROADCAST) {
		for (u32 k = 1; k <= NODES; k++)
			deliver(k, channel, msg, len);
		return;
	}
	if (node < 1 || node > NODES)
		return;
	u32 rl = repeat_req[node - 1][0];
	if (with_repeat && rl) {
		if (repeat_first) {
			memcpy(buf, &repeat_req[node - 1][1], rl);
			memcpy(buf + rl, msg, len);
		} else {
			memcpy(buf, msg, len);
			memcpy(buf + len, &repeat_req[node - 1][1], rl);
		}
		len += rl;
	} else {
		memcpy(buf, msg, len);
	}
	deliver(node, channel, buf, len);
}

static inline u8 sense(u32 node) { return node == NODES ? 0x8E : 0x8F; }

typedef struct { u8 *p; u32 n; } Out;
static void o8(Out *o, u8 v) { o->p[o->n++] = v; }
static void o32(Out *o, u32 v) { memcpy(o->p + o->n, &v, 4); o->n += 4; }
static void hdr(Out *o, u32 words) { o8(o, 0x87); o8(o, 0); o8(o, 0x20); o8(o, (u8)words); }

static void receive(Out *o, u32 channel)
{
	u32 len = rx_len[channel];
	if (len == 0) {
		hdr(o, 5);
		o8(o, 0x32); o8(o, 0xFF); o8(o, 0xFF); o8(o, 0xFF);
		o32(o, 0xFFFFFF00u); o32(o, 0); o32(o, 0); o32(o, 0);
		return;
	}
	/* total reply (header included) is padded to a whole number of words */
	u32 words = (len + 23 + 3) / 4;
	u32 start = o->n;
	hdr(o, words);
	o8(o, 0x16); o8(o, 0xFF); o8(o, 0xFF); o8(o, 0xFF);
	o32(o, 0xFFFFFF00u); o32(o, 0); o32(o, 0);
	o8(o, 0); o8(o, (u8)channel); o8(o, sense(rx_buf[channel][0]));
	memcpy(o->p + o->n, rx_buf[channel], len);
	o->n += len;
	while (o->n - start < words * 4)
		o8(o, 0);
	rx_len[channel] = 0;
}

static void ack(Out *o, u8 code, u32 channel, u8 sense_v)
{
	hdr(o, 1); o8(o, code); o8(o, (u8)channel); o8(o, sense_v); o8(o, 0);
}

/* Maple command 0x86 to the MIE; returns reply length in bytes */
static u32 mie_86(const u8 *in, u32 inlen, u8 *outp)
{
	Out o = { outp, 0 };
	if (inlen == 0) { hdr(&o, 0); return o.n; }
	u8 sub = in[0];
	u32 node = 0, len = 0, channel = 0;
	const u8 *msg = NULL;
	if (inlen >= 3) {
		if (sub != 0x13 && inlen >= 8) { node = in[6]; len = in[7]; msg = in + 8; channel = in[5] & 0x1F; }
		else { node = in[1]; len = in[2]; msg = in + 3; }
	}
	LOG("mie 86/%02x node=%u ch=%u len=%u\n", sub, node, channel, len);
	switch (sub) {
	case 0x13: /* store repeat request */
		if (len && node >= 1 && node <= NODES) {
			repeat_req[node - 1][0] = (u8)len;
			memcpy(&repeat_req[node - 1][1], msg, len);
		}
		hdr(&o, 1); o8(&o, sub + 1); o8(&o, 0); o8(&o, (u8)(len + 1)); o8(&o, 0);
		break;
	case 0x15: receive(&o, in[1] & 0x1F); break;
	case 0x17:
		rx_len[channel] = 0;
		transmit(node, channel, msg, len, 0, 0);
		ack(&o, 0x18, channel, 0x8E);
		break;
	case 0x19:
	case 0x21:
		rx_len[channel] = 0;
		transmit(node, channel, msg, len, 1, sub == 0x19);
		ack(&o, 0x18, channel, sense(node));
		break;
	case 0x33:
		receive(&o, channel);
		transmit(node, channel, msg, len, 1, 0);
		ack(&o, 0x18, channel, sense(node));
		break;
	case 0x35:
		receive(&o, channel);
		/* fall through */
	case 0x27: {
		rx_len[channel] = 0;
		u32 count = in[6], k = 7;
		for (u32 c = 0; c < count && k + 1 < inlen; c++) {
			node = in[k]; len = in[k + 1];
			transmit(node, channel, in + k + 2, len, 1, 0);
			k += len + 2;
		}
		ack(&o, 0x26, channel, sense(node));
		break;
	}
	case 0x0B: { /* EEPROM write */
		u32 a = in[1] % sizeof(naomi_eeprom), sz = in[2];
		if (sz > sizeof(naomi_eeprom) - a) sz = sizeof(naomi_eeprom) - a;
		memcpy(naomi_eeprom + a, in + 4, sz);
		hdr(&o, 1);
		memcpy(o.p + o.n, naomi_eeprom, 4); o.n += 4;
		break;
	}
	case 0x03: { /* EEPROM read */
		u32 a = in[1] % sizeof(naomi_eeprom);
		hdr(&o, sizeof(naomi_eeprom) / 4);
		memcpy(o.p + o.n, naomi_eeprom + a, sizeof(naomi_eeprom) - a);
		o.n += sizeof(naomi_eeprom); /* tail beyond the copy is whatever was there */
		break;
	}
	case 0x31: /* DIP switches / video mode: report VGA */
		hdr(&o, 5);
		o8(&o, 0x32); o8(&o, 0xFF); o8(&o, 0xFF); o8(&o, 0xFF);
		o8(&o, 0x00); o8(&o, 0xFF); o8(&o, 0xF9); o8(&o, 0xFF);
		o32(&o, 0); o32(&o, 0); o32(&o, 0);
		break;
	case 0x01:
		hdr(&o, 1); o8(&o, 2); o8(&o, 0); o8(&o, 0); o8(&o, 0);
		break;
	default:
		LOG("mie: unknown 0x86 sub-command %02x\n", sub);
		break;
	}
	return o.n;
}

/* ---------------- Maple DMA ---------------- */

void maple_dma(u32 list)
{
	if (jvs_log < 0)
		jvs_log = getenv("JVS_LOG") != NULL;
	for (int guard = 0; guard < 64; guard++) {
		u32 h1 = rd32(list);
		u32 resp = rd32(list + 4) & 0x1FFFFFE0u;
		int last = (int)(h1 >> 31);
		u32 words = (h1 & 0xFF) + 1;
		u32 op = (h1 >> 8) & 7, port = (h1 >> 16) & 3;
		if (op != 0) {               /* non-transfer pattern (reset, NOP...) */
			list += 4;
			if (last) break;
			continue;
		}
		u32 frame = rd32(list + 8);
		u8 cmd = (u8)frame, dst = (u8)(frame >> 8);
		u8 in[1024];
		u32 inlen = (words - 1) * 4;
		for (u32 k = 0; k < inlen && k < sizeof(in); k++)
			in[k] = rd8(list + 12 + k);
		u8 out[1024];
		u32 outlen = 0;
		if (port == 0 && dst == 0x20 && cmd == 0x86) {
			outlen = mie_86(in, inlen, out);
		} else if (port == 0 && dst == 0x20) {
			/* standard Maple commands to the MIE: header has sender/recipient swapped */
			u8 src = (u8)(frame >> 16), code = 0;
			u32 data_words = 0;
			switch (cmd) {
			case 0x01: code = 0x05; break;             /* device request -> status */
			case 0x02: code = 0x06; break;             /* all status */
			case 0x03: case 0x04: code = 0x07; break;  /* reset / kill -> ack */
			case 0x84: code = 0x85; data_words = 1; break; /* JVS self test */
			default: code = 0xFD; break;               /* unknown command */
			}
			LOG("maple: MIE cmd %02x -> %02x\n", cmd, code);
			u32 h = code | (u32)src << 8 | (u32)dst << 16 | data_words << 24;
			memcpy(out, &h, 4);
			memset(out + 4, 0, data_words * 4);
			outlen = 4 + data_words * 4;
		} else {
			LOG("maple: port %u cmd %02x to %02x -> no device\n", port, cmd, dst);
			u32 none = 0xFFFFFFFFu;
			memcpy(out, &none, 4);
			outlen = 4;
		}
		for (u32 k = 0; k < outlen; k++)
			wr8(0x0C000000u | resp, out[k]);
		list += 8 + words * 4;
		if (last) break;
	}
}
