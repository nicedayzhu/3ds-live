"""Prepare the HOME Menu icon for a direct copy to the launcher's BGR8 LCD."""
from pathlib import Path
import sys

from PIL import Image

size = 32
background = (0x14, 0x28, 0x2A)
with Image.open(sys.argv[1]) as icon:
    artwork = icon.convert("RGBA").resize((size, size), Image.Resampling.LANCZOS)
tile = Image.new("RGBA", artwork.size, background + (255,))
tile.alpha_composite(artwork)

# The LCD stores columns from bottom to top. Blend and rotate on the build host.
columns = []
for x in range(size):
    column = bytes(channel for y in reversed(range(size)) for channel in tile.getpixel((x, y))[2::-1])
    columns.append("    {" + ",".join(map(str, column)) + "}")

Path(sys.argv[2]).write_text(
    "/* Generated from the launcher icon by make_launcher_logo.py. */\n"
    f"#define LAUNCHER_HEADER_COLOR 0x{bytes(background).hex()}u\n"
    f"enum {{ LAUNCHER_LOGO_SIZE = {size} }};\n"
    "static const unsigned char launcher_logo[LAUNCHER_LOGO_SIZE][3 * LAUNCHER_LOGO_SIZE] = {\n"
    + ",\n".join(columns) + "\n};\n",
    encoding="ascii",
)
