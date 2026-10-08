"""Assemble the HOTD2 (NAOMI M2 cart) ROM set into a flat cart image.

Layout follows Flycast's naomi_roms.cpp entry for "hotd2". The game data is
not encrypted (no 315-5881 key), so the image is directly readable.

    python -I tools/cart.py C:/RetroBat/roms/naomi/hotd2.zip out/hotd2_cart.bin
"""
import sys
import zipfile

CART_SIZE = 0xA800000
LAYOUT = [
    ("epr-21585.ic22", 0x0000000),
    ("epr-21585.ic22", 0x0200000),  # reload
] + [
    (f"mpr-{21386 + i}.ic{i + 1}{'s' if i + 1 >= 12 else ''}", 0x0800000 * (i + 1))
    for i in range(20)
]


def build(zip_path):
    image = bytearray(b"\xff" * CART_SIZE)
    with zipfile.ZipFile(zip_path) as z:
        for name, offset in LAYOUT:
            data = z.read(name)
            image[offset:offset + len(data)] = data
    return image


def to_4mb_mode(offset):
    """Cart address as seen by the CPU in M2 "4MB mode" (Flycast M2Cartridge::Read)."""
    return (offset & 0x103FFFFF) | ((offset & 0x07C00000) << 1)


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    with open(dst, "wb") as f:
        f.write(build(src))
