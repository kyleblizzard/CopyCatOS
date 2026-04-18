#!/usr/bin/env python3
# CopyCatOS — by Kyle Blizzard at Blizzard.show

"""Generate pixel-perfect Snow Leopard traffic light button PNGs.

The real Snow Leopard window buttons are 14x14 circles with a subtle gloss
highlight in the upper-left quadrant. Colors here are extracted from actual
Snow Leopard screenshots.

Output goes to ~/.local/share/aqua-widgets/ which is where the WM and
decorator expect to find them at runtime.
"""

import struct
import zlib
import os


def create_traffic_light(filename, fill_r, fill_g, fill_b, border_r, border_g, border_b):
    """Create a 14x14 RGBA PNG of a single traffic light button.

    The button is a filled circle with:
      - A 1px anti-aliased border ring in a darker shade
      - A soft white gloss crescent in the upper-left for that classic Aqua look
      - Fully transparent pixels outside the circle

    Args:
        filename:  Where to write the PNG
        fill_*:    RGB values for the main circle fill (e.g. red, yellow, green)
        border_*:  RGB values for the 1px border ring
    """
    size = 14
    radius = 6.0   # Circle radius in pixels
    cx, cy = 7.0, 7.0  # Center of the 14x14 canvas

    pixels = []
    for y in range(size):
        row = []
        for x in range(size):
            # Distance from this pixel's center to the circle center
            dx = x + 0.5 - cx
            dy = y + 0.5 - cy
            dist = (dx * dx + dy * dy) ** 0.5

            if dist <= radius - 0.5:
                # --- Inside the circle ---
                # Gloss effect: a white highlight shifted toward upper-left.
                # We offset the "gloss center" by (-2, -2) from the circle center
                # so the highlight sits in the top-left quadrant.
                gloss_dx = dx + 2.0
                gloss_dy = dy + 2.0
                gloss_dist = (gloss_dx * gloss_dx + gloss_dy * gloss_dy) ** 0.5
                # Fade the gloss out over ~5px, peak intensity 40%
                gloss = max(0, 1.0 - gloss_dist / 5.0) * 0.4

                # Blend toward white by the gloss amount
                r = min(255, int(fill_r + (255 - fill_r) * gloss))
                g = min(255, int(fill_g + (255 - fill_g) * gloss))
                b = min(255, int(fill_b + (255 - fill_b) * gloss))
                a = 255
                row.extend([r, g, b, a])

            elif dist <= radius + 0.5:
                # --- Anti-aliased border edge ---
                # Alpha ramps from 1.0 (fully inside) to 0.0 (fully outside)
                # over a 1px transition zone, giving a smooth circle edge.
                alpha = max(0, min(1, radius + 0.5 - dist))
                r = border_r
                g = border_g
                b = border_b
                a = int(255 * alpha)
                row.extend([r, g, b, a])

            else:
                # --- Outside the circle: fully transparent ---
                row.extend([0, 0, 0, 0])

        pixels.append(bytes(row))

    # Write a minimal valid PNG by hand (avoids needing Pillow/PIL)
    _write_png(filename, size, size, pixels)


def _write_png(filename, width, height, rows):
    """Write raw RGBA pixel rows to a PNG file.

    This builds the PNG byte-by-byte using the spec:
      - 8-byte signature
      - IHDR chunk (image dimensions, bit depth, color type)
      - IDAT chunk (zlib-compressed pixel data with filter bytes)
      - IEND chunk (end marker)

    Each row is prefixed with a 0x00 filter byte (filter type "None"),
    meaning the pixel values are stored as-is with no delta encoding.
    """
    def chunk(ctype, data):
        """Build a single PNG chunk: length + type + data + CRC32."""
        c = ctype + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)

    # PNG magic bytes
    sig = b'\x89PNG\r\n\x1a\n'

    # IHDR: width, height, bit-depth=8, color-type=6 (RGBA), compression=0, filter=0, interlace=0
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)

    # Build raw scanlines: each row gets a 0x00 filter byte prefix
    raw = b''
    for row in rows:
        raw += b'\x00' + row

    # Compress all scanlines into one IDAT chunk
    idat = zlib.compress(raw)

    with open(filename, 'wb') as f:
        f.write(sig)
        f.write(chunk(b'IHDR', ihdr))
        f.write(chunk(b'IDAT', idat))
        f.write(chunk(b'IEND', b''))


# ─── Main: generate all three buttons ───

# Output to the aqua-widgets asset directory
out = os.path.expanduser('~/.local/share/aqua-widgets')
os.makedirs(out, exist_ok=True)

# Close button — Red (#FF5F57 fill, #E0443E border)
create_traffic_light(
    os.path.join(out, 'sl_close_button.png'),
    fill_r=255, fill_g=95, fill_b=87,
    border_r=224, border_g=68, border_b=62
)

# Minimize button — Yellow (#FFBD2E fill, #DEA123 border)
create_traffic_light(
    os.path.join(out, 'sl_minimize_button.png'),
    fill_r=255, fill_g=189, fill_b=46,
    border_r=222, border_g=161, border_b=35
)

# Zoom button — Green (#28C940 fill, #1AAB29 border)
create_traffic_light(
    os.path.join(out, 'sl_zoom_button.png'),
    fill_r=40, fill_g=201, fill_b=64,
    border_r=26, border_g=171, border_b=41
)

# Report what we created
print("Generated 14x14 RGBA traffic light buttons:")
for name in ['sl_close_button.png', 'sl_minimize_button.png', 'sl_zoom_button.png']:
    path = os.path.join(out, name)
    size = os.path.getsize(path)
    print(f"  {name}: {size} bytes")
