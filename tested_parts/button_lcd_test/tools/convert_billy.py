"""Convert billy.jpg to a flash-resident RGB565 image using macOS sips."""

from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    project_dir = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory() as temp_dir:
        bitmap_path = Path(temp_dir) / "billy.bmp"
        subprocess.run(
            [
                "sips", "--resampleHeightWidthMax", "240",
                "-s", "format", "bmp", str(project_dir / "billy.jpg"),
                "--out", str(bitmap_path),
            ],
            check=True,
        )
        bitmap = bitmap_path.read_bytes()

    if bitmap[:2] != b"BM":
        raise ValueError("Expected a BMP image")
    offset = struct.unpack_from("<I", bitmap, 10)[0]
    header_size, width, signed_height, planes, bits, compression = (
        struct.unpack_from("<IiiHHI", bitmap, 14)
    )
    height = abs(signed_height)
    if (header_size < 40 or planes != 1 or bits != 24 or compression != 0
            or not 0 < width <= 240 or not 0 < height <= 240):
        raise ValueError("Expected an uncompressed 24-bit BMP up to 240x240")
    stride = (width * 3 + 3) & ~3
    if len(bitmap) < offset + stride * height:
        raise ValueError("Truncated BMP pixels")

    pixels = []
    for y in range(height):
        source_y = y if signed_height < 0 else height - 1 - y
        for x in range(width):
            index = offset + source_y * stride + x * 3
            blue, green, red = bitmap[index:index + 3]
            pixels.append(
                ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
            )

    lines = [
        "// Generated from billy.jpg by tools/convert_billy.py.",
        "#pragma once", "", "#include <Arduino.h>", "",
        "namespace billy_image", "{",
        f"constexpr int16_t width = {width};",
        f"constexpr int16_t height = {height};",
        "const uint16_t pixels[] PROGMEM =", "{",
    ]
    for index in range(0, len(pixels), 8):
        row = ", ".join(f"0x{value:04X}" for value in pixels[index:index + 8])
        lines.append(f"    {row},")
    lines.extend([
        "};",
        "static_assert(sizeof(pixels) / sizeof(pixels[0]) == width * height,",
        '              "Image pixel count must match its dimensions");',
        "}", "",
    ])
    (project_dir / "include" / "billy_image.h").write_text("\n".join(lines))
    print(f"Generated {width}x{height} RGB565 image ({len(pixels) * 2} bytes)")


if __name__ == "__main__":
    main()
