// AudioWorklet that plays the AICA output ring straight out of the game's
// shared WebAssembly memory. The game thread writes 44.1 kHz stereo s16 frames
// and bumps a monotonic counter.
//
// The game produces audio at emulated speed, which drifts against the sound
// card's clock and arrives in bursts (one video frame at a time). So instead of
// a fixed read pointer we keep the buffer near a target fill by resampling
// slightly faster or slower (at most ±2%, inaudible), and when it does run dry
// we fade the last sample out instead of dropping to hard silence.
class AicaPlayer extends AudioWorkletProcessor {
	constructor() {
		super();
		this.ready = false;
		this.port.onmessage = (e) => {
			const { buffer, ring, write, size } = e.data;
			this.samples = new Int16Array(buffer, ring, size * 2);
			this.counter = new Uint32Array(buffer, write, 1);
			this.size = size;
			this.pos = Atomics.load(this.counter, 0) - this.target;   // fractional read position
			this.ready = true;
		};
		this.target = 3072;        // frames kept buffered (~70 ms): covers a late video frame or two
		this.fill = this.target;   // smoothed buffer level
		this.lastL = 0; this.lastR = 0;
		this.underruns = 0; this.resyncs = 0; this.dry = false; this.minFill = Infinity;
		this.reported = 0;
	}

	frame(i) {
		const k = ((i % this.size) + this.size) % this.size * 2;
		return [this.samples[k] / 32768, this.samples[k + 1] / 32768];
	}

	process(inputs, outputs) {
		const out = outputs[0];
		const l = out[0], r = out[1] || out[0];
		if (!this.ready) { l.fill(0); r.fill(0); return true; }
		const write = Atomics.load(this.counter, 0);
		let avail = write - this.pos;
		if (avail > this.size / 2 || avail < -this.size / 2) {   // way off (tab was hidden etc.): jump back to the target
			this.pos = write - this.target;
			avail = this.target;
			this.resyncs++;
		}
		this.fill += (avail - this.fill) * 0.02;
		this.minFill = Math.min(this.minFill, avail);
		// proportional rate control around the target fill
		const rate = 1 + Math.max(-0.02, Math.min(0.02, (this.fill - this.target) / this.target * 0.05));
		for (let i = 0; i < l.length; i++) {
			if (write - this.pos >= 2) {
				const i0 = Math.floor(this.pos), t = this.pos - i0;
				const [a0, b0] = this.frame(i0), [a1, b1] = this.frame(i0 + 1);
				this.lastL = l[i] = a0 + (a1 - a0) * t;
				this.lastR = r[i] = b0 + (b1 - b0) * t;
				this.pos += rate;
				this.dry = false;
			} else {
				// ran dry: fade out what was playing; resume when data arrives
				if (!this.dry) { this.dry = true; this.underruns++; }
				this.lastL *= 0.995; this.lastR *= 0.995;
				if (Math.abs(this.lastL) < 1e-4) this.lastL = 0;
				if (Math.abs(this.lastR) < 1e-4) this.lastR = 0;
				l[i] = this.lastL; r[i] = this.lastR;
			}
		}
		if (currentTime - this.reported >= 1) {
			this.reported = currentTime;
			this.port.postMessage({ underruns: this.underruns, resyncs: this.resyncs, fill: Math.round(this.fill), minFill: this.minFill, rate });
			this.minFill = Infinity;
		}
		return true;
	}
}
registerProcessor('aica-player', AicaPlayer);
