"""Regenerate pixel-aligned Windows icons from resources/icons/app-icon.svg.

Requires Python 3 and Pillow. This is a design tool, not a build dependency.
The source deliberately uses only rounded rectangles, with no fonts or filters.
"""

from io import BytesIO
from math import floor
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
ICONS = ROOT / "resources" / "icons"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
SUPERSAMPLE = 8


def render(source, size):
    """Snap each rectangle's bounds before antialiasing its rounded corners."""
    image = Image.new("RGBA", (size * SUPERSAMPLE,) * 2)
    draw = ImageDraw.Draw(image)
    scale = size / 64
    for rect in source.findall("{http://www.w3.org/2000/svg}rect"):
        def attr(name):
            return float(rect.get(name, "0"))

        x, y = (floor(attr(key) * scale + 0.5) for key in ("x", "y"))
        right = floor((attr("x") + attr("width")) * scale + 0.5)
        bottom = floor((attr("y") + attr("height")) * scale + 0.5)
        radius = attr("rx") * scale
        draw.rounded_rectangle(
            (x * SUPERSAMPLE, y * SUPERSAMPLE,
             right * SUPERSAMPLE - 1, bottom * SUPERSAMPLE - 1),
            radius=radius * SUPERSAMPLE,
            fill=rect.get("fill"),
        )
    # Area sampling avoids the halos that sharpening/resampling can introduce.
    return image.resize((size, size), Image.Resampling.BOX)


def dib_payload(image):
    """32-bit BGRA XOR bitmap plus the legacy 1-bit transparency AND mask."""
    size = image.width
    pixels = image.tobytes("raw", "BGRA", 0, -1)
    stride = ((size + 31) // 32) * 4
    mask = bytearray(stride * size)
    for row in range(size):
        for col in range(size):
            if image.getpixel((col, size - row - 1))[3] == 0:
                mask[row * stride + col // 8] |= 0x80 >> (col % 8)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32,
                         0, len(pixels) + len(mask), 0, 0, 0, 0)
    return header + pixels + mask


def write_ico(images):
    payloads = []
    for size, image in images.items():
        if size == 256:
            stream = BytesIO()
            image.save(stream, format="PNG")
            payloads.append(stream.getvalue())
        else:
            payloads.append(dib_payload(image))
    offset = 6 + 16 * len(images)
    entries = []
    for size, payload in zip(images, payloads):
        dimension = size if size < 256 else 0
        entries.append(struct.pack("<BBBBHHII", dimension, dimension, 0, 0,
                                   1, 32, len(payload), offset))
        offset += len(payload)
    (ICONS / "app-icon.ico").write_bytes(
        struct.pack("<HHH", 0, 1, len(images)) + b"".join(entries + payloads)
    )


def preview(images):
    """A design sheet showing actual-size icons on light and dark surfaces."""
    sheet = Image.new("RGB", (880, 368), "#F3F6FC")
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default(size=14)
    draw.text((24, 18), "MSA  /  DATA GENERATOR", fill="#20334B", font=font)
    sheet.paste(images[256], (24, 70), images[256])
    for row, background in enumerate(("#FFFFFF", "#15233A")):
        y = 70 + row * 130
        draw.rounded_rectangle((312, y, 856, y + 114), radius=12, fill=background)
        x = 336
        for size in (16, 20, 24, 32, 40, 48, 64):
            icon = images[size]
            sheet.paste(icon, (x, y + 14 + (64 - size) // 2), icon)
            draw.text((x, y + 88), str(size),
                      fill="#728198" if row == 0 else "#B4C2D9", font=font)
            x += size + 22
    (ROOT / "docs").mkdir(exist_ok=True)
    sheet.save(ROOT / "docs" / "app-icon-preview.png")


def main():
    source = ET.parse(ICONS / "app-icon.svg").getroot()
    images = {size: render(source, size) for size in SIZES}
    for size, image in images.items():
        image.save(ICONS / f"app-icon-{size}.png")
    write_ico(images)
    preview(images)
    print("Generated app-icon.ico and PNGs:", ", ".join(map(str, SIZES)))


if __name__ == "__main__":
    main()
