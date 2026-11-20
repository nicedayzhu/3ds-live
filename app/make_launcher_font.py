"""Regenerate the checked-in launcher bitmap atlas when UI strings change.

Pass NotoSansSC[wght].ttf from google/fonts at a85815a42757630ce188fdad368c2dfc444d4773,
ofl/notosanssc/. Ordinary launcher builds use the atlas and require no font tools.
"""
import hashlib
import re
import struct
import sys
from pathlib import Path

from PIL import ImageFont

app = Path(__file__).resolve().parent
font_path = Path(sys.argv[1])
if hashlib.sha256(font_path.read_bytes()).hexdigest() != "a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da":
    raise SystemExit("Use the pinned Noto Sans SC variable font listed above.")
source = "\n".join((app / "source" / name).read_text(encoding="utf-8")
                   for name in ("launcher_text.c", "launcher_ui.c"))
strings = re.findall(r'"([^"\n]*)"', source)
characters = sorted(set("".join(strings)) | {chr(n) for n in range(32, 127)})
# Offsets in the blob are little-endian, relative to the start of launcher_font.
faces = [(13, 500), (16, 500), (32, 700), (21, 650)]
blob = bytearray(len(faces) * 8)
for face, (size, weight) in enumerate(faces):
    font = ImageFont.truetype(str(font_path), size)
    font.set_variation_by_axes([weight])
    while len(blob) % 4:
        blob.append(0)
    table = len(blob)
    struct.pack_into("<II", blob, face * 8, table, len(characters))
    blob.extend(bytes(12 * len(characters)))
    for i, ch in enumerate(characters):
        mask, (left, top) = font.getmask2(ch, mode="L", anchor="ls")
        w, h = mask.size
        alpha = bytes((x + 8) // 17 for x in mask)
        packed = bytes(a | (b << 4) for a, b in zip(alpha[::2], (alpha + b"\0")[1::2]))
        struct.pack_into("<HBBBbbxI", blob, table + 12 * i, ord(ch),
                         round(font.getlength(ch)), w, h, left, top, len(blob))
        blob.extend(packed)
(app / "assets/launcher-font.bin").write_bytes(blob)
