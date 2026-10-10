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

// EEPROM (settings) and SRAM (bookkeeping, scores) persist in this browser.
const SAVE_KEY = 'hotd2-web-save-v1';
function toB64(u8) { let s = ''; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode(...u8.subarray(i, i + 0x8000)); return btoa(s); }
function fromB64(b64) { const s = atob(b64); const u8 = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) u8[i] = s.charCodeAt(i); return u8; }
function saveRegions() {
	return [[Module._web_eeprom(), Module._web_eeprom_size(), 'eeprom'], [Module._web_sram(), Module._web_sram_size(), 'sram']];
}
function restoreSave() {
	try {
		const saved = JSON.parse(localStorage.getItem(SAVE_KEY) || 'null');
		if (!saved) return;
		for (const [ptr, size, name] of saveRegions())
			if (saved[name]) Module.HEAPU8.set(fromB64(saved[name]).subarray(0, size), ptr);
		log('restored EEPROM/SRAM from this browser');
	} catch (e) { log('could not restore saves: ' + e.message); }
}
let lastSave = '';
function persistSave() {
	try {
		const out = {};
		for (const [ptr, size, name] of saveRegions()) out[name] = toB64(Module.HEAPU8.slice(ptr, ptr + size));
		const json = JSON.stringify(out);
		if (json !== lastSave) { localStorage.setItem(SAVE_KEY, json); lastSave = json; }
	} catch (e) { /* storage may be unavailable (private mode); the game still runs */ }
}

let audioCtx;
// ?mute: the player runs at zero volume (for testing: see hotd2AudioLevel and hotd2AudioStats)
const MUTE = new URLSearchParams(location.search).has('mute');
// Loudness of the last `ms` of game audio, read straight from the output ring:
// { rms, peak } on a 0..32767 scale. Handy for checking sound with the page muted.
window.hotd2AudioLevel = (ms = 500) => {
	const size = Module._web_audio_size(), write = Module.HEAPU32[Module._web_audio_write() >> 2];
	const ring = new Int16Array(Module.HEAPU8.buffer, Module._web_audio_ring(), size * 2);
	const n = Math.min(size, Math.round(44.1 * ms));
	let sum = 0, peak = 0;
	for (let i = 0; i < n; i++) {
		const k = ((write - n + i + size) % size) * 2;
		for (const s of [ring[k], ring[k + 1]]) { sum += s * s; peak = Math.max(peak, Math.abs(s)); }
	}
	return { rms: Math.round(Math.sqrt(sum / (2 * n))), peak };
};
// Must be called synchronously from a user gesture (click / file pick) so audio may start.
function prepareAudio() {
	if (audioCtx) return;
	try { audioCtx = new AudioContext({ sampleRate: 44100 }); } catch (e) { log('no audio: ' + e.message); }
}

async function startAudio() {
	if (!audioCtx) return;
	try {
		await audioCtx.audioWorklet.addModule('audio-worklet.js');
		const node = new AudioWorkletNode(audioCtx, 'aica-player', { outputChannelCount: [2] });
		node.port.postMessage({
			buffer: Module.HEAPU8.buffer,
			ring: Module._web_audio_ring(),
			write: Module._web_audio_write(),
			size: Module._web_audio_size(),
		});
		node.port.onmessage = (e) => { window.hotd2AudioStats = e.data; };
		if (MUTE) {
			const silent = audioCtx.createGain();
			silent.gain.value = 0;
			node.connect(silent).connect(audioCtx.destination);
		} else {
			node.connect(audioCtx.destination);
		}
		const resume = () => audioCtx.state !== 'running' && audioCtx.resume();
		window.addEventListener('pointerdown', resume);
		window.addEventListener('keydown', resume);
		resume();
	} catch (e) {
		log('audio failed: ' + e.message);
	}
}

async function start(zipBytes) {
	$('overlay').hidden = true;
	if (document.activeElement) document.activeElement.blur();   // so Enter reaches the game, not the button
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
	restoreSave();
	log('starting game thread');
	Module._web_start();
	setInterval(persistSave, 5000);
	window.addEventListener('pagehide', persistSave);
	startAudio();
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
			const per = 1000 / (now - last);   // the page may have been paused for longer than a second
			$('stats').textContent = `${Math.round((f - lastFrames) * per)} game fps, ${Math.round((rn - lastRenders) * per)} renders/s, frame ${f}`;
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
	// The game samples its inputs once per frame: keep a tap held for a few frames.
	let startDown = 0;
	window.addEventListener('keydown', (e) => {
		if (e.repeat) return;
		if (e.key === '5') set16(IN.coins, get16(IN.coins) + 2);   // one game's worth: the game needs 2 credits to start
		if (e.key === '1' || e.key === 'Enter') { set16(IN.buttons, get16(IN.buttons) | START); startDown = performance.now(); }
		if (e.key === '9') Module.HEAPU8[base + IN.test] = 1;
	});
	window.addEventListener('keyup', (e) => {
		if (e.key === '1' || e.key === 'Enter')
			setTimeout(() => set16(IN.buttons, get16(IN.buttons) & ~START), Math.max(0, 100 - (performance.now() - startDown)));
		if (e.key === '9') Module.HEAPU8[base + IN.test] = 0;
	});
	void u8;
}

$('rom').addEventListener('change', async (e) => {
	const f = e.target.files[0];
	prepareAudio();
	if (f) start(new Uint8Array(await f.arrayBuffer())).catch((err) => log('error: ' + err.message));
});
if (['localhost', '127.0.0.1'].includes(location.hostname)) {
	$('dev').hidden = false;
	$('dev').addEventListener('click', async () => {
		prepareAudio();
		const r = await fetch('../local-roms/hotd2.zip');
		start(new Uint8Array(await r.arrayBuffer())).catch((err) => log('error: ' + err.message));
	});
}
