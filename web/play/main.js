// Browser front end for the recompiled HOTD2: loads the user's ROM set,
// starts the game thread and draws its frames with WebGL2.
'use strict';
import { readZip } from './zip.js';
import { PvrRenderer } from './gl.js';

const $ = (id) => document.getElementById(id);
const log = (m) => { $('log').textContent += m + '\n'; $('log').scrollTop = 1e9; };

// Cart layout, as in tools/cart.py: ic22 twice at the start, then 20 x 8 MB mask ROMs.
const CART_SIZE = 0xA800000;
const PROGRAM = 'epr-21585.ic22';
function maskRoms() {
	const out = [];
	for (let i = 0; i < 20; i++)
		out.push([`mpr-${21386 + i}.ic${i + 1}${i + 1 >= 12 ? 's' : ''}`, 0x800000 * (i + 1)]);
	return out;
}

let Module;
const moduleReady = new Promise((resolve) => {
	window.Module = Module = {
		print: log,
		printErr: log,
		locateFile: (p) => '../game/' + p,
		onRuntimeInitialized: resolve,
		onAbort: (w) => log('abort: ' + w),
	};
});
const s = document.createElement('script');
s.src = '../game/hotd2.js';
s.onerror = () => log('../game/hotd2.js is missing: run runtime/build-web.sh first');
document.body.appendChild(s);
if (!crossOriginIsolated) log('page is not cross-origin isolated; serve it with web/serve.py');

async function start(zipBytes) {
	$('overlay').hidden = true;
	log('reading ROM set...');
	const zip = await readZip(zipBytes);
	await moduleReady;
	const program = await zip.read(PROGRAM);
	const cartPtr = Module._web_alloc(CART_SIZE);
	const progPtr = Module._web_alloc(program.length);
	Module.HEAPU8.fill(0xFF, cartPtr, cartPtr + CART_SIZE);
	Module.HEAPU8.set(program, cartPtr);
	Module.HEAPU8.set(program, cartPtr + 0x200000);
	Module.HEAPU8.set(program, progPtr);
	for (const [name, off] of maskRoms()) {
		const data = await zip.read(name);
		Module.HEAPU8.set(data, cartPtr + off);
	}
	Module._web_set_program(progPtr, program.length);
	Module._web_set_cart(cartPtr, CART_SIZE);
	log('starting game thread');
	Module._web_start();
	run();
}

function run() {
	const canvas = $('screen');
	const r = new PvrRenderer(canvas, Module);
	setupInput(canvas);
	let last = performance.now(), lastFrames = 0, lastRenders = 0;
	const tick = (now) => {
		r.draw();
		if (now - last > 1000) {
			const f = Module._web_frames(), rn = Module._web_renders();
			$('stats').textContent = `${f - lastFrames} game fps, ${rn - lastRenders} renders/s, frame ${f}`;
			last = now; lastFrames = f; lastRenders = rn;
		}
		requestAnimationFrame(tick);
	};
	requestAnimationFrame(tick);
}

// JvsInput { u8 test; u16 buttons[2]; u16 gun_x[2], gun_y[2]; u8 offscreen[2]; u16 coins[2]; }
const IN = { test: 0, buttons: 2, gunX: 6, gunY: 10, offscreen: 14, coins: 16 };
const START = 0x8000, TRIGGER = 0x0200;
function setupInput(canvas) {
	const base = Module._web_input();
	const u8 = (o) => Module.HEAPU8;
	const set16 = (o, v) => { Module.HEAPU8[base + o] = v & 0xFF; Module.HEAPU8[base + o + 1] = v >> 8; };
	const get16 = (o) => Module.HEAPU8[base + o] | (Module.HEAPU8[base + o + 1] << 8);
	const aim = (e) => {
		const r = canvas.getBoundingClientRect();
		const x = Math.round((e.clientX - r.left) / r.width * 639), y = Math.round((e.clientY - r.top) / r.height * 479);
		const inside = x >= 0 && x < 640 && y >= 0 && y < 480;
		set16(IN.gunX, Math.max(0, Math.min(639, x)));
		set16(IN.gunY, Math.max(0, Math.min(479, y)));
		if (!(e.buttons & 2)) Module.HEAPU8[base + IN.offscreen] = inside ? 0 : 1;
	};
	canvas.addEventListener('pointermove', aim);
	canvas.addEventListener('pointerdown', (e) => {
		aim(e);
		if (e.button === 2) Module.HEAPU8[base + IN.offscreen] = 1;     // right button: aim off-screen to reload
		else set16(IN.buttons, get16(IN.buttons) | TRIGGER);
		canvas.setPointerCapture(e.pointerId);
	});
	canvas.addEventListener('pointerup', (e) => {
		if (e.button === 2) Module.HEAPU8[base + IN.offscreen] = 0;
		else set16(IN.buttons, get16(IN.buttons) & ~TRIGGER);
	});
	canvas.addEventListener('contextmenu', (e) => e.preventDefault());
	window.addEventListener('keydown', (e) => {
		if (e.repeat) return;
		if (e.key === '5') set16(IN.coins, get16(IN.coins) + 1);
		if (e.key === '1' || e.key === 'Enter') set16(IN.buttons, get16(IN.buttons) | START);
		if (e.key === '9') Module.HEAPU8[base + IN.test] = 1;
	});
	window.addEventListener('keyup', (e) => {
		if (e.key === '1' || e.key === 'Enter') set16(IN.buttons, get16(IN.buttons) & ~START);
		if (e.key === '9') Module.HEAPU8[base + IN.test] = 0;
	});
	void u8;
}

$('rom').addEventListener('change', async (e) => {
	const f = e.target.files[0];
	if (f) start(new Uint8Array(await f.arrayBuffer())).catch((err) => log('error: ' + err.message));
});
if (['localhost', '127.0.0.1'].includes(location.hostname)) {
	$('dev').hidden = false;
	$('dev').addEventListener('click', async () => {
		const r = await fetch('../local-roms/hotd2.zip');
		start(new Uint8Array(await r.arrayBuffer())).catch((err) => log('error: ' + err.message));
	});
}
