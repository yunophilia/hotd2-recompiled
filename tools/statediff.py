"""Find game state variables that progress in Flycast but freeze in the harness.

The two runs do not reach the same point at the same frame, so the dumps are
paired by game progress, not frame number. A candidate is a small-integer BSS
word that is equal in the "early" pair, keeps changing across Flycast's later
dumps and stays constant across ours.

    python3 -I tools/statediff.py --early OURS.bin FLY.bin --ours A.bin B.bin --fly C.bin D.bin
"""
import argparse
import struct

BASE = 0x0C000000
BSS = (0x0C145E10, 0x0C9D8704)


def words(path):
    d = open(path, "rb").read()
    lo, hi = BSS[0] - BASE, BSS[1] - BASE
    return struct.unpack_from(f"<{(hi - lo) // 4}I", d, lo & ~3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--early", nargs=2, required=True)
    ap.add_argument("--ours", nargs="+", required=True)
    ap.add_argument("--fly", nargs="+", required=True)
    ap.add_argument("--max", type=lambda s: int(s, 0), default=0x10000)
    a = ap.parse_args()
    eo, ef = words(a.early[0]), words(a.early[1])
    ours = [words(p) for p in a.ours]
    fly = [words(p) for p in a.fly]
    hits = []
    for i in range(len(eo)):
        if eo[i] != ef[i]:
            continue
        o_vals = [w[i] for w in ours]
        f_vals = [w[i] for w in fly]
        if any(v > a.max for v in o_vals + f_vals):
            continue
        if len(set(o_vals)) == 1 and len(set(f_vals)) > 1:
            hits.append((BSS[0] + 4 * i, eo[i], o_vals, f_vals))
    print(f"{len(hits)} candidates (addr: early | ours later | flycast later)")
    for addr, e, o, f in hits[:80]:
        print(f"  {addr:08x}: {e:#x} | {[hex(x) for x in o]} | {[hex(x) for x in f]}")


if __name__ == "__main__":
    main()
