// WebGL2 renderer for frames published by runtime/glframe.c.
// Frame layout (wasm32): GlFrame { seq, nverts, ncalls, verts*, calls*, vcap, ccap }
// GlCall { list, isp, tsp, tcw, first, count, tex } (7 words)
// GlTex  { tcw, tsp_size, w, h, version, rgba* }     (6 words)
// Vertex: x, y, z(=1/w), u, v, rgba, offset rgba    (13 floats)
'use strict';

const VS = `#version 300 es
in vec3 a_pos; in vec2 a_uv; in vec4 a_col; in vec4 a_ofs;
out vec2 v_uv; out vec4 v_col; out vec4 v_ofs;
void main() {
	float z = max(a_pos.z, 1e-6);           // PVR z is 1/w, bigger is nearer
	float w = 1.0 / z;
	float depth = 1.0 / (1.0 + z);          // 0..1, smaller is nearer
	gl_Position = vec4((a_pos.x / 320.0 - 1.0) * w, (1.0 - a_pos.y / 240.0) * w, depth * w, w);
	v_uv = a_uv; v_col = a_col; v_ofs = a_ofs;
}`;

const FS = `#version 300 es
precision highp float;
in vec2 v_uv; in vec4 v_col; in vec4 v_ofs;
uniform sampler2D u_tex;
uniform int u_textured, u_offset, u_shade, u_useAlpha, u_ignoreTexAlpha;
uniform float u_ptRef;
out vec4 o;
void main() {
	vec4 c = v_col;
	if (u_useAlpha == 0) c.a = 1.0;
	vec4 r = c;
	if (u_textured == 1) {
		vec4 t = texture(u_tex, v_uv);
		if (u_ignoreTexAlpha == 1) t.a = 1.0;
		if (u_shade == 0) r = t;
		else if (u_shade == 1) r = vec4(t.rgb * c.rgb, t.a);
		else if (u_shade == 2) r = vec4(mix(c.rgb, t.rgb, t.a), c.a);
		else r = t * c;
	}
	if (u_offset == 1) r.rgb += v_ofs.rgb;
	r = clamp(r, 0.0, 1.0);
	if (u_ptRef >= 0.0 && r.a < u_ptRef) discard;
	o = r;
}`;

function compile(gl, type, src) {
	const s = gl.createShader(type);
	gl.shaderSource(s, src);
	gl.compileShader(s);
	if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
	return s;
}

export class PvrRenderer {
	constructor(canvas, Module) {
		this.M = Module;
		const gl = canvas.getContext('webgl2', { antialias: false, depth: true, preserveDrawingBuffer: false });
		if (!gl) throw new Error('WebGL2 is not available');
		this.gl = gl;
		const p = gl.createProgram();
		gl.attachShader(p, compile(gl, gl.VERTEX_SHADER, VS));
		gl.attachShader(p, compile(gl, gl.FRAGMENT_SHADER, FS));
		gl.linkProgram(p);
		if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
		this.prog = p;
		this.u = {};
		for (const n of ['u_tex', 'u_textured', 'u_offset', 'u_shade', 'u_useAlpha', 'u_ignoreTexAlpha', 'u_ptRef'])
			this.u[n] = gl.getUniformLocation(p, n);
		this.vao = gl.createVertexArray();
		this.vbo = gl.createBuffer();
		gl.bindVertexArray(this.vao);
		gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
		const stride = 13 * 4;
		const attr = (name, size, off) => {
			const loc = gl.getAttribLocation(p, name);
			gl.enableVertexAttribArray(loc);
			gl.vertexAttribPointer(loc, size, gl.FLOAT, false, stride, off * 4);
		};
		attr('a_pos', 3, 0); attr('a_uv', 2, 3); attr('a_col', 4, 5); attr('a_ofs', 4, 9);
		this.textures = [];   // slot -> { tex, version }
		this.lastSeq = -1;
		this.frames = 0;
	}

	texture(slot) {
		const gl = this.gl, M = this.M;
		const ptr = M._web_tex(slot) >> 2;
		const H = M.HEAPU32;
		const w = H[ptr + 2], h = H[ptr + 3], version = H[ptr + 4], rgba = H[ptr + 5];
		let t = this.textures[slot];
		if (!t) { t = this.textures[slot] = { tex: gl.createTexture(), version: -1 }; }
		gl.bindTexture(gl.TEXTURE_2D, t.tex);
		if (t.version !== version) {
			const pixels = M.HEAPU8.slice(rgba, rgba + w * h * 4);   // copy out of shared memory
			gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
			t.version = version;
		}
		return t.tex;
	}

	blendFactor(i, isSrc) {
		const gl = this.gl;
		switch (i) {
		case 0: return gl.ZERO;
		case 1: return gl.ONE;
		case 2: return isSrc ? gl.DST_COLOR : gl.SRC_COLOR;
		case 3: return isSrc ? gl.ONE_MINUS_DST_COLOR : gl.ONE_MINUS_SRC_COLOR;
		case 4: return gl.SRC_ALPHA;
		case 5: return gl.ONE_MINUS_SRC_ALPHA;
		case 6: return gl.DST_ALPHA;
		default: return gl.ONE_MINUS_DST_ALPHA;
		}
	}

	depthFunc(mode) {
		// PVR compares z = 1/w (bigger is nearer); our depth shrinks as z grows, so mirror the test
		const gl = this.gl;
		return [gl.NEVER, gl.GREATER, gl.EQUAL, gl.GEQUAL, gl.LESS, gl.NOTEQUAL, gl.LEQUAL, gl.ALWAYS][mode];
	}

	// Draw the latest frame if it changed; returns true when something was drawn.
	draw() {
		const gl = this.gl, M = this.M;
		const f = M._web_frame() >> 2;
		const H = M.HEAPU32;
		const seq = H[f];
		if (seq === this.lastSeq || seq === 0) return false;
		this.lastSeq = seq;
		const nverts = H[f + 1], ncalls = H[f + 2], verts = H[f + 3], calls = H[f + 4] >> 2;
		gl.viewport(0, 0, 640, 480);
		gl.clearColor(0, 0, 0, 1);
		gl.clearDepth(1);
		gl.depthMask(true);
		gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
		if (!nverts) return true;
		gl.useProgram(this.prog);
		gl.bindVertexArray(this.vao);
		gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
		gl.bufferData(gl.ARRAY_BUFFER, M.HEAPF32.slice(verts >> 2, (verts >> 2) + nverts * 13), gl.STREAM_DRAW);
		gl.enable(gl.DEPTH_TEST);
		gl.uniform1i(this.u.u_tex, 0);
		gl.activeTexture(gl.TEXTURE0);
		const ptRef = (M._web_reg(0x005F811C) & 0xFF) / 255;
		for (const pass of [0, 4, 2]) {            // opaque, punch-through, translucent
			for (let i = 0; i < ncalls; i++) {
				const c = calls + i * 7;
				if (H[c] !== pass) continue;
				const isp = H[c + 1], tsp = H[c + 2], first = H[c + 4], count = H[c + 5];
				const tex = M.HEAP32[c + 6];
				const textured = isp & 1, offset = (isp >> 1) & 1;
				if (pass === 2) {
					gl.enable(gl.BLEND);
					gl.blendFunc(this.blendFactor((tsp >>> 29) & 7, true), this.blendFactor((tsp >>> 26) & 7, false));
					gl.depthFunc(gl.LEQUAL);
					gl.depthMask(false);
				} else {
					gl.disable(gl.BLEND);
					gl.depthFunc(this.depthFunc((isp >>> 29) & 7));
					gl.depthMask(!((isp >>> 26) & 1));
				}
				gl.uniform1i(this.u.u_textured, textured && tex >= 0 ? 1 : 0);
				gl.uniform1i(this.u.u_offset, offset);
				gl.uniform1i(this.u.u_shade, (tsp >>> 6) & 3);
				gl.uniform1i(this.u.u_useAlpha, (tsp >>> 20) & 1);
				gl.uniform1i(this.u.u_ignoreTexAlpha, (tsp >>> 19) & 1);
				gl.uniform1f(this.u.u_ptRef, pass === 4 ? ptRef : -1);
				if (textured && tex >= 0) {
					this.texture(tex);
					const filter = (tsp >>> 13) & 3 ? gl.LINEAR : gl.NEAREST;
					gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
					gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
					const wrap = (clamp, flip) => clamp ? gl.CLAMP_TO_EDGE : flip ? gl.MIRRORED_REPEAT : gl.REPEAT;
					gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, wrap((tsp >>> 16) & 1, (tsp >>> 18) & 1));
					gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, wrap((tsp >>> 15) & 1, (tsp >>> 17) & 1));
				}
				gl.drawArrays(gl.TRIANGLES, first, count);
			}
		}
		gl.depthMask(true);
		this.frames++;
		return true;
	}
}
