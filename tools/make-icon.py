"""Pack the Workbench icon. Usage: make-icon.py output.info [preview-prefix]."""

import io
import struct
import sys
import zlib
from pathlib import Path

from PIL import Image

ARTWORK = Path(__file__).resolve().parents[1] / "assets" / "DockAwesome-48.png"
STACK_SIZE, ICON_TYPE, FRAMELESS, TOOL_TYPE = 0x80001009, 0x8000100F, 0x80001010, 0x8000100B
WBTOOL = 3
# The dock keeps running, so Workbench must not wait for it in WBStartup.
TOOL_TYPES = (b"DONOTWAIT",)


def png_bytes(image, attributes=b""):
    buffer = io.BytesIO()
    image.save(buffer, "PNG", optimize=True)
    data = buffer.getvalue()
    if not attributes:
        return data
    payload = b"icOn" + attributes
    chunk = struct.pack(">I", len(attributes)) + payload + struct.pack(">I", zlib.crc32(payload))
    return data[:-12] + chunk + data[-12:]


def main(output, preview_prefix=None):
    with Image.open(ARTWORK) as source:
        if source.size != (48, 48) or source.mode != "RGBA":
            raise SystemExit("Expected a 48x48 RGBA icon.")
        normal = Image.frombytes("RGBA", source.size, source.tobytes())

    red, green, blue, alpha = normal.split()
    dimmed = tuple(channel.point(lambda value: value * 85 // 100) for channel in (red, green, blue))
    selected = Image.merge("RGBA", (*dimmed, alpha))
    attributes = struct.pack(">6I", STACK_SIZE, 32768, ICON_TYPE, WBTOOL, FRAMELESS, 1)
    attributes += b"".join(struct.pack(">I", TOOL_TYPE) + text + b"\0" for text in TOOL_TYPES)
    Path(output).write_bytes(png_bytes(normal, attributes) + png_bytes(selected))
    if preview_prefix:
        normal.save(f"{preview_prefix}-normal.png")
        selected.save(f"{preview_prefix}-selected.png")


if __name__ == "__main__":
    if not 2 <= len(sys.argv) <= 3:
        raise SystemExit("Usage: make-icon.py output.info [preview-prefix]")
    main(*sys.argv[1:])
