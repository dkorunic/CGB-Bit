"""Turns tools/demo's raw RGB565 frames into demo.gif: 2x nearest-neighbour, each frame with its own
best 256-colour palette (exact when a frame has no more colours than that)."""
import sys
from PIL import Image

frames_path, out_path = sys.argv[1:3]
W = H = 128
SCALE = 2
data = open(frames_path, 'rb').read()
assert data and len(data) % (W * H * 2) == 0, 'bad demo output'

frames = []
for i in range(0, len(data), W * H * 2):
    px = memoryview(data)[i:i + W * H * 2].cast('H')  # little-endian RGB565, as the PC writes it
    rgb = bytearray()
    for c in px:
        r, g, b = c >> 11, c >> 5 & 63, c & 31
        rgb += bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
    img = Image.frombytes('RGB', (W, H), bytes(rgb)).quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    frames.append(img.resize((W * SCALE, H * SCALE), Image.NEAREST))

frames[0].save(out_path, save_all=True, append_images=frames[1:], duration=50, loop=0, optimize=False)
print(f'{out_path}: {len(frames)} frames')
