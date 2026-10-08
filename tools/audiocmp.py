"""Compare loudness over time of two 44.1 kHz stereo 16-bit recordings
(WAV with a 44-byte header, or headerless raw PCM).

    python3 -I tools/audiocmp.py ours.wav flycast.pcm [window_seconds]
"""
import math
import struct
import sys

RATE = 44100


def load(path):
    d = open(path, "rb").read()
    if d[:4] == b"RIFF":
        d = d[44:]
    n = len(d) // 4
    return struct.unpack(f"<{n * 2}h", d[:n * 4])


def windows(s, sec):
    step = int(RATE * sec) * 2
    out = []
    for k in range(0, len(s), step):
        blk = s[k:k + step]
        if not blk:
            break
        rms = math.sqrt(sum(x * x for x in blk) / len(blk))
        out.append((rms, max(abs(x) for x in blk)))
    return out


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    sec = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
    wa, wb = windows(a, sec), windows(b, sec)
    print(f"{'t':>5}  {'ours rms':>9} {'peak':>6}   {'flycast rms':>11} {'peak':>6}")
    for i in range(max(len(wa), len(wb))):
        ra = wa[i] if i < len(wa) else (0, 0)
        rb = wb[i] if i < len(wb) else (0, 0)
        print(f"{i * sec:5.0f}  {ra[0]:9.1f} {ra[1]:6d}   {rb[0]:11.1f} {rb[1]:6d}")


if __name__ == "__main__":
    main()
