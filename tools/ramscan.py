"""Compare a Flycast main-RAM dump against the cart's program ROM.

    python3 -I tools/ramscan.py ~/hotd2/roms/hotd2.zip ~/hotd2/dumps/ram_3000.bin
"""
import sys
import zipfile

RAM_BASE = 0x0C000000
IC22_LOAD = 0x0C020000
RTS_NOP = bytes.fromhex("0b000900")


def matching_runs(ram, rom, base, gran=0x1000):
    runs, cur = [], None
    for i in range(0, len(rom), gran):
        same = ram[base + i:base + i + gran] == rom[i:i + gran]
        if same and cur is None:
            cur = i
        if not same and cur is not None:
            runs.append((cur, i))
            cur = None
    if cur is not None:
        runs.append((cur, len(rom)))
    return runs


def main(zip_path, ram_path):
    ic22 = zipfile.ZipFile(zip_path).read("epr-21585.ic22")
    ram = open(ram_path, "rb").read()
    off = IC22_LOAD - RAM_BASE
    for a, b in matching_runs(ram, ic22, off):
        print(f"ic22[{a:06x}:{b:06x}] == RAM {IC22_LOAD + a:08x}-{IC22_LOAD + b:08x}")
    print("rts;nop density per 256KB:")
    for o in range(0, 0x800000, 0x40000):
        n = ram[o:o + 0x40000].count(RTS_NOP)
        print(f"  {RAM_BASE + o:08x} {n:5d} {'#' * (n // 40)}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
