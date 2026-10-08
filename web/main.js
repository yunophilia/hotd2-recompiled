// Loads the Flycast WebAssembly build, copies the user's ROM set into its
// in-memory filesystem and starts emulation.
'use strict';

const logEl = document.getElementById('log');
const log = (msg) => {
	logEl.textContent += msg + '\n';
	logEl.scrollTop = logEl.scrollHeight;
};

const files = { rom: null, bios: null };
let runtimeReady = false;

window.Module = {
	canvas: document.getElementById('canvas'),
	print: log,
	printErr: log,
	locateFile: (path) => 'flycast/' + path,
	onRuntimeInitialized() {
		runtimeReady = true;
		log('[web] runtime ready');
		updateStart();
	},
	onAbort: (what) => log('[web] abort: ' + what),
};

if (!crossOriginIsolated)
	log('[web] page is not cross-origin isolated: pthreads will not work (serve with web/serve.py)');

const script = document.createElement('script');
script.src = 'flycast/flycast.js';
script.onerror = () => log('[web] flycast/flycast.js missing: build it and copy it into web/flycast/');
document.body.appendChild(script);

function updateStart() {
	document.getElementById('start').disabled = !(runtimeReady && files.rom && files.bios);
}

for (const id of ['rom', 'bios']) {
	document.getElementById(id).addEventListener('change', async (e) => {
		const f = e.target.files[0];
		files[id] = f ? new Uint8Array(await f.arrayBuffer()) : null;
		updateStart();
	});
}

const devBtn = document.getElementById('dev');
if (['localhost', '127.0.0.1'].includes(location.hostname)) {
	devBtn.hidden = false;
	devBtn.addEventListener('click', async () => {
		devBtn.disabled = true;
		const get = async (n) => new Uint8Array(await (await fetch('local-roms/' + n)).arrayBuffer());
		[files.rom, files.bios] = await Promise.all([get('hotd2.zip'), get('hod2bios.zip')]);
		log(`[web] fetched dev ROMs (${files.rom.length} + ${files.bios.length} bytes)`);
		updateStart();
		if (runtimeReady) start();
	});
}

document.getElementById('start').addEventListener('click', start);

function start() {
	const FS = Module.FS;
	FS.mkdirTree('/roms');
	FS.writeFile('/roms/hotd2.zip', files.rom);
	FS.writeFile('/roms/hod2bios.zip', files.bios);
	files.rom = files.bios = null;
	document.getElementById('overlay').hidden = true;
	log('[web] starting flycast');
	Module.callMain(['/roms/hotd2.zip']);
}
