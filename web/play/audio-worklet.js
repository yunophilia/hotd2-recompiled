// AudioWorklet that plays the AICA output ring straight out of the game's
// shared WebAssembly memory. The game thread writes 44.1 kHz stereo s16 frames
// and bumps a monotonic counter; we follow it with a small latency target.
class AicaPlayer extends AudioWorkletProcessor {
	constructor() {
		super();
		this.ready = false;
		this.port.onmessage = (e) => {
			const { buffer, ring, write, size } = e.data;
			this.samples = new Int16Array(buffer, ring, size * 2);
			this.counter = new Uint32Array(buffer, write, 1);
			this.size = size;
			this.read = Atomics.load(this.counter, 0);
			this.ready = true;
		};
		this.latency = 2048;   // frames kept buffered (~46 ms)
	}

	process(inputs, outputs) {
		const out = outputs[0];
		const l = out[0], r = out[1] || out[0];
		if (!this.ready) { l.fill(0); r.fill(0); return true; }
		const write = Atomics.load(this.counter, 0);
		let avail = (write - this.read) >>> 0;
		if (avail > this.size / 2) {               // fell far behind (tab hidden etc.): resync
			this.read = (write - this.latency) >>> 0;
			avail = this.latency;
		}
		for (let i = 0; i < l.length; i++) {
			if (avail > 0) {
				const k = (this.read % this.size) * 2;
				l[i] = this.samples[k] / 32768;
				r[i] = this.samples[k + 1] / 32768;
				this.read = (this.read + 1) >>> 0;
				avail--;
			} else {
				l[i] = r[i] = 0;                   // underrun: silence
			}
		}
		return true;
	}
}
registerProcessor('aica-player', AicaPlayer);
