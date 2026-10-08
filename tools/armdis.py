"""Small ARM (ARMv4, ARM state) disassembler for reading the AICA sound driver.

    python3 -I tools/armdis.py <sound_ram_dump.bin> <start hex> [count]
"""
import struct
import sys

COND = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "", "nv"]
DP = ["and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"]
SHIFT = ["lsl", "lsr", "asr", "ror"]


def reg(n):
    return {13: "sp", 14: "lr", 15: "pc"}.get(n, f"r{n}")


def operand2(i):
    if i & (1 << 25):
        imm, rot = i & 0xFF, ((i >> 8) & 15) * 2
        v = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
        return f"#0x{v:x}"
    rm = reg(i & 15)
    t = (i >> 5) & 3
    if i & 0x10:
        return f"{rm}, {SHIFT[t]} {reg((i >> 8) & 15)}"
    amt = (i >> 7) & 31
    if amt == 0 and t == 0:
        return rm
    if amt == 0 and t == 3:
        return f"{rm}, rrx"
    return f"{rm}, {SHIFT[t]} #{amt or 32}"


def dis(pc, i, mem):
    c = COND[i >> 28]
    top = (i >> 25) & 7
    if (i & 0x0FC000F0) == 0x00000090:
        op = "mla" if i & (1 << 21) else "mul"
        return f"{op}{c} {reg((i >> 16) & 15)}, {reg(i & 15)}, {reg((i >> 8) & 15)}"
    if (i & 0x0FBF0FFF) == 0x010F0000:
        return f"mrs{c} {reg((i >> 12) & 15)}, {'spsr' if i & (1 << 22) else 'cpsr'}"
    if (i & 0x0DB0F000) == 0x0120F000:
        return f"msr{c} {'spsr' if i & (1 << 22) else 'cpsr'}_{'f' if i & (1 << 19) else ''}{'c' if i & (1 << 16) else ''}, {operand2(i)}"
    if top in (0, 1):
        op = DP[(i >> 21) & 15]
        s = "s" if (i >> 20) & 1 and op not in ("tst", "teq", "cmp", "cmn") else ""
        rd, rn = reg((i >> 12) & 15), reg((i >> 16) & 15)
        if op in ("mov", "mvn"):
            return f"{op}{c}{s} {rd}, {operand2(i)}"
        if op in ("tst", "teq", "cmp", "cmn"):
            return f"{op}{c} {rn}, {operand2(i)}"
        return f"{op}{c}{s} {rd}, {rn}, {operand2(i)}"
    if top in (2, 3):
        l = "ldr" if i & (1 << 20) else "str"
        b = "b" if i & (1 << 22) else ""
        rd, rn = reg((i >> 12) & 15), (i >> 16) & 15
        up = "" if i & (1 << 23) else "-"
        if i & (1 << 25):
            off = f"{up}{operand2(i & ~(1 << 25))}"
        else:
            off = f"#{up}0x{i & 0xFFF:x}"
        note = ""
        if rn == 15 and not (i & (1 << 25)):
            a = pc + 8 + ((i & 0xFFF) if i & (1 << 23) else -(i & 0xFFF))
            if 0 <= a < len(mem) - 3:
                note = f"   ; [{a:x}] = 0x{struct.unpack_from('<I', mem, a)[0]:08x}"
        if i & (1 << 24):
            return f"{l}{c}{b} {rd}, [{reg(rn)}, {off}]{'!' if i & (1 << 21) else ''}{note}"
        return f"{l}{c}{b} {rd}, [{reg(rn)}], {off}{note}"
    if top == 4:
        l = "ldm" if i & (1 << 20) else "stm"
        mode = ("d" if not i & (1 << 23) else "i") + ("b" if i & (1 << 24) else "a")
        regs = ", ".join(reg(k) for k in range(16) if i & (1 << k))
        return f"{l}{c}{mode} {reg((i >> 16) & 15)}{'!' if i & (1 << 21) else ''}, {{{regs}}}{'^' if i & (1 << 22) else ''}"
    if top == 5:
        off = ((i & 0xFFFFFF) ^ 0x800000) - 0x800000
        return f"b{'l' if i & (1 << 24) else ''}{c} 0x{pc + 8 + off * 4:x}"
    if top == 7 and i & (1 << 24):
        return f"swi{c} 0x{i & 0xFFFFFF:x}"
    return f".word 0x{i:08x}"


def main():
    mem = open(sys.argv[1], "rb").read()
    start = int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    for k in range(count):
        a = start + 4 * k
        i = struct.unpack_from("<I", mem, a)[0]
        print(f"{a:06x}: {i:08x}  {dis(a, i, mem)}")


if __name__ == "__main__":
    main()
