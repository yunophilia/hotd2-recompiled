// Minimal ZIP reader: central directory + stored/deflate entries, using the
// browser's DecompressionStream for deflate. Enough for MAME-style ROM sets.
'use strict';

export async function readZip(bytes) {
	const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	// end of central directory: scan back from the end for its signature
	let eocd = -1;
	for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 65557); i--) {
		if (dv.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
	}
	if (eocd < 0) throw new Error('not a zip file');
	const count = dv.getUint16(eocd + 10, true);
	let p = dv.getUint32(eocd + 16, true);
	const entries = new Map();
	for (let n = 0; n < count; n++) {
		if (dv.getUint32(p, true) !== 0x02014b50) throw new Error('bad central directory');
		const method = dv.getUint16(p + 10, true);
		const csize = dv.getUint32(p + 20, true);
		const usize = dv.getUint32(p + 24, true);
		const nameLen = dv.getUint16(p + 28, true);
		const extraLen = dv.getUint16(p + 30, true);
		const commentLen = dv.getUint16(p + 32, true);
		const local = dv.getUint32(p + 42, true);
		const name = new TextDecoder().decode(bytes.subarray(p + 46, p + 46 + nameLen));
		entries.set(name.toLowerCase(), { method, csize, usize, local });
		p += 46 + nameLen + extraLen + commentLen;
	}
	return {
		names: [...entries.keys()],
		async read(name) {
			const e = entries.get(name.toLowerCase());
			if (!e) throw new Error(`missing ${name} in zip`);
			const lp = e.local;
			const start = lp + 30 + dv.getUint16(lp + 26, true) + dv.getUint16(lp + 28, true);
			const data = bytes.subarray(start, start + e.csize);
			if (e.method === 0) return data;
			if (e.method !== 8) throw new Error(`unsupported compression in ${name}`);
			const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
			const out = new Uint8Array(await new Response(stream).arrayBuffer());
			if (out.length !== e.usize) throw new Error(`size mismatch in ${name}`);
			return out;
		},
	};
}
