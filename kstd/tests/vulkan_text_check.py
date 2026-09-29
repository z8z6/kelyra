"""Verify one captured frame from the Kelyra font and Vulkan example."""

from pathlib import Path
import sys


frame = Path(sys.argv[1]).read_bytes()
header_end = frame.find(b"\n255\n") + 5
assert frame[:header_end] == b"P6\n800 600\n255\n"
assert len(frame) == header_end + 800 * 600 * 3
pixels = frame[header_end:]


def pixel(x, y):
    at = (y * 800 + x) * 3
    return pixels[at:at + 3]


background = pixel(0, 0)
assert background == pixel(700, 500)
lit = sum(1 for y in range(90, 220) for x in range(40, 650)
          if pixel(x, y) != background)
assert lit > 3000, lit
print(f"Vulkan text rendered: {lit} lit pixels")
