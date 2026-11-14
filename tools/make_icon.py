"""App icon (src/win/app.ico): a paw under two LiDAR arcs on a dark glass tile.

Plain Python, no dependencies: every layer is a signed distance field, anti-aliased by its
distance in pixels. Colors are the app's theme tokens (bg, accent, lidar).

  python3 tools/make_icon.py                 write src/win/app.ico
  python3 tools/make_icon.py --preview x.png also write a contact sheet of every size (enlarged)
"""

import math
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "src", "win", "app.ico")
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]  # 256 is stored as PNG, the rest as 32-bit DIB


def rgb(h):
    return ((h >> 16) & 255) / 255, ((h >> 8) & 255) / 255, (h & 255) / 255


BG_TOP, BG_BOTTOM = rgb(0x242B35), rgb(0x0A0C10)
ACCENT_TOP, ACCENT = rgb(0xFF8A5C), rgb(0xFF6B35)
LIDAR, LIDAR_HI = rgb(0x19C6D1), rgb(0x4FE3EC)
WHITE = (1.0, 1.0, 1.0)


# --- distance fields (unit square, y down) ---------------------------------------------------

def sd_round_rect(x, y, half, r):
    qx, qy = abs(x - 0.5) - half + r, abs(y - 0.5) - half + r
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


def sd_ellipse(x, y, cx, cy, rx, ry, rot):
    # Rotated ellipse, iq's first-order approximation (exact on the boundary, good enough for AA)
    c, s = math.cos(rot), math.sin(rot)
    px, py = (x - cx) * c + (y - cy) * s, -(x - cx) * s + (y - cy) * c
    k0 = math.hypot(px / rx, py / ry)
    k1 = math.hypot(px / (rx * rx), py / (ry * ry))
    return k0 * (k0 - 1) / k1 if k1 > 0 else -min(rx, ry)


def sd_arc(x, y, cx, cy, radius, half_span, width):
    # Arc opening upward, round caps
    px, py = x - cx, cy - y
    ang = math.atan2(px, py)
    if abs(ang) <= half_span:
        return abs(math.hypot(px, py) - radius) - width / 2
    ex = math.copysign(radius * math.sin(half_span), px)
    ey = radius * math.cos(half_span)
    return math.hypot(px - ex, py - ey) - width / 2


def coverage(d, px):
    return min(max(0.5 - d / px, 0.0), 1.0)


# --- the design --------------------------------------------------------------------------------

MARGIN = 0.035
TILE_HALF, TILE_R = 0.5 - MARGIN, 0.22
CENTER = (0.5, 0.60)
TOES = [(0.322, 0.505, -0.50), (0.425, 0.400, -0.16), (0.575, 0.400, 0.16), (0.678, 0.505, 0.50)]


def paw(x, y, grow):
    d = sd_ellipse(x, y, 0.5, 0.668, 0.158 + grow, 0.126 + grow, 0.0)
    for tx, ty, rot in TOES:
        d = min(d, sd_ellipse(x, y, tx, ty, 0.060 + grow, 0.080 + grow, rot))
    return d


def arcs(size):
    # Small icons keep one thicker arc so it still reads as a scan, not as noise
    if size <= 24:
        return [(0.375, 0.95, 0.075, 1.0)]
    if size <= 40:
        return [(0.335, 0.98, 0.050, 0.95), (0.425, 0.98, 0.042, 0.55)]
    return [(0.335, 1.0, 0.036, 0.95), (0.425, 1.0, 0.034, 0.55)]


def over(dst, col, a):
    if a <= 0:
        return dst
    r, g, b, da = dst
    oa = a + da * (1 - a)
    k = da * (1 - a)
    return ((col[0] * a + r * k) / oa, (col[1] * a + g * k) / oa, (col[2] * a + b * k) / oa, oa)


def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def render(size):
    px = 1.0 / size
    grow = -0.006 if size <= 24 else 0.0  # slimmer toes at small sizes keep the gaps visible
    arc_list = arcs(size)
    edge_w = max(px, 0.012)
    rows = []
    for j in range(size):
        y = (j + 0.5) * px
        row = []
        for i in range(size):
            x = (i + 0.5) * px
            c = (0.0, 0.0, 0.0, 0.0)
            dt = sd_round_rect(x, y, TILE_HALF, TILE_R)
            tile = coverage(dt, px)
            if tile > 0:
                c = over(c, lerp(BG_TOP, BG_BOTTOM, y), tile)
                # cyan glow behind the paw (glass cockpit)
                g = max(0.0, 1 - math.hypot(x - CENTER[0], y - CENTER[1]) / 0.48)
                c = over(c, LIDAR, 0.20 * g * g * tile)
                # glass edge, brighter at the top
                ring = abs(dt + edge_w / 2) - edge_w / 2
                c = over(c, WHITE, (0.08 + 0.12 * (1 - y)) * coverage(ring, px) * tile)
                for radius, span, width, alpha in arc_list:
                    da = sd_arc(x, y, CENTER[0], CENTER[1], radius, span, width)
                    c = over(c, lerp(LIDAR_HI, LIDAR, y), alpha * coverage(da, px) * tile)
                # soft shadow, then the paw
                sh = paw(x, y - 0.014, grow)
                c = over(c, (0.0, 0.0, 0.0), 0.45 * coverage(sh, max(px, 0.03)) * tile)
                dp = paw(x, y, grow)
                c = over(c, lerp(ACCENT_TOP, ACCENT, (y - 0.32) / 0.47), coverage(dp, px))
            row.append(c)
        rows.append(row)
    return rows


# --- encoders ------------------------------------------------------------------------------------

def to_bytes(c):
    return tuple(int(round(min(max(v, 0.0), 1.0) * 255)) for v in c)


def png(rows):
    w, h = len(rows[0]), len(rows)
    raw = b"".join(b"\x00" + bytes(v for p in row for v in to_bytes(p)) for row in rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def dib(rows):
    # BITMAPINFOHEADER (height doubled for the AND mask), BGRA bottom-up, then an all-zero AND mask
    w, h = len(rows[0]), len(rows)
    head = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pix = bytearray()
    for row in reversed(rows):
        for p in row:
            r, g, b, a = to_bytes(p)
            pix += bytes((b, g, r, a))
    mask = bytes(((w + 31) // 32) * 4 * h)
    return head + bytes(pix) + mask


def ico(images):
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    blobs = []
    for size, blob in images:
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
        blobs.append(blob)
    return out + b"".join(blobs)


def contact_sheet(rendered):
    # every size enlarged to 128 px with nearest neighbour, side by side, on the app background
    cell, gap = 128, 8
    w = len(rendered) * (cell + gap) + gap
    h = cell + 2 * gap
    bg = (*rgb(0x12161C), 1.0)
    sheet = [[bg] * w for _ in range(h)]
    for n, (size, rows) in enumerate(rendered):
        x0 = gap + n * (cell + gap)
        for j in range(cell):
            for i in range(cell):
                p = rows[j * size // cell][i * size // cell]
                sheet[gap + j][x0 + i] = over(bg, p[:3], p[3])
    return sheet


def main():
    rendered = [(s, render(s)) for s in SIZES]
    data = ico([(s, png(rows) if s >= 256 else dib(rows)) for s, rows in rendered])
    with open(OUT, "wb") as f:
        f.write(data)
    print(f"{os.path.normpath(OUT)}: {len(data)} bytes, sizes {SIZES}")
    if "--preview" in sys.argv:
        path = sys.argv[sys.argv.index("--preview") + 1]
        with open(path, "wb") as f:
            f.write(png(contact_sheet(rendered)))
        with open(os.path.splitext(path)[0] + "_256.png", "wb") as f:
            f.write(png(rendered[-1][1]))
        print(f"preview: {path}")


if __name__ == "__main__":
    main()
