"""JPEG frames for mock_robot.py (camera test pattern and LiDAR-style map).

Drawing is plain Python on an RGB bytearray (Canvas). Encoding backends:
  pillow - Pillow from this interpreter or from tools/.venv: every frame is drawn live.
  sips   - macOS /usr/bin/sips converts a BMP to JPEG. One process per image is slow, so the camera
           frames are generated once and cached in tools/mock_assets/; the map (1 fps) is re-converted
           only when it changes.
  None   - no encoder: the video endpoints answer 503, everything else still works.
"""

import glob
import io
import math
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE_DIR = os.path.join(HERE, "mock_assets")
VENV_DIR = os.path.join(HERE, ".venv")
JPEG_QUALITY = 80

CAM_W, CAM_H = 320, 240
CACHED_CAMERA_FRAMES = 20  # sips backend: the camera loops over this many pre-made frames
CACHE_TAG = "v1"           # bump when the drawing changes so old cached JPEGs are not reused

WHITE, BLACK, GREY = (255, 255, 255), (0, 0, 0), (128, 128, 128)
RED, BLUE = (230, 20, 20), (40, 80, 255)

# 5x7 glyphs, only what the overlays need
FONT = {
    "0": (".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."),
    "1": ("..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."),
    "2": (".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"),
    "3": ("#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."),
    "4": ("...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."),
    "5": ("#####", "#....", "####.", "....#", "....#", "#...#", ".###."),
    "6": ("..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."),
    "7": ("#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."),
    "8": (".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."),
    "9": (".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."),
    "M": ("#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"),
    "O": (".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."),
    "C": (".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."),
    "K": ("#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"),
}


class Canvas:
    """RGB image as a bytearray, row-major, top-left origin."""

    def __init__(self, w, h, rgb=BLACK):
        self.w, self.h = w, h
        self.buf = bytearray(bytes(rgb) * (w * h))

    def copy(self):
        c = Canvas.__new__(Canvas)
        c.w, c.h, c.buf = self.w, self.h, bytearray(self.buf)
        return c

    def rect(self, x0, y0, x1, y1, rgb):
        """Fill [x0, x1) x [y0, y1), clipped."""
        x0, x1 = max(0, int(x0)), min(self.w, int(x1))
        y0, y1 = max(0, int(y0)), min(self.h, int(y1))
        if x0 >= x1 or y0 >= y1:
            return
        row = bytes(rgb) * (x1 - x0)
        for y in range(y0, y1):
            o = (y * self.w + x0) * 3
            self.buf[o:o + len(row)] = row

    def disc(self, cx, cy, r, rgb):
        cx, cy = int(round(cx)), int(round(cy))
        for dy in range(-r, r + 1):
            dx = int(math.sqrt(r * r - dy * dy))
            self.rect(cx - dx, cy + dy, cx + dx + 1, cy + dy + 1, rgb)

    def line(self, a, b, rgb, width=1):
        (x0, y0), (x1, y1) = a, b
        steps = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        half = width // 2
        for i in range(steps + 1):
            t = i / steps
            x = int(round(x0 + (x1 - x0) * t)) - half
            y = int(round(y0 + (y1 - y0) * t)) - half
            self.rect(x, y, x + width, y + width, rgb)

    def text(self, x, y, s, rgb, scale=1):
        for ch in s.upper():
            for gy, row in enumerate(FONT.get(ch, ())):
                for gx, bit in enumerate(row):
                    if bit == "#":
                        self.rect(x + gx * scale, y + gy * scale, x + (gx + 1) * scale, y + (gy + 1) * scale, rgb)
            x += 6 * scale


# ── drawings ──

BAR_COLORS = [(235, 235, 235), (235, 235, 0), (0, 235, 235), (0, 235, 0),
              (235, 0, 235), (235, 0, 0), (0, 0, 235), (30, 30, 30)]


def draw_camera(n, w=CAM_W, h=CAM_H):
    """Test pattern for frame n: scrolling colour bars, a bouncing square, 'MOCK <n>' label."""
    shift = (n * 4) % w
    row = b"".join(bytes(BAR_COLORS[((x + shift) * 8 // w) % 8]) for x in range(w))
    c = Canvas(w, h)
    c.buf[:] = row * h
    period = 30
    k = n % (2 * period)
    p = k if k < period else 2 * period - k
    sx = 10 + p * (w - 60) // period
    sy = 40 + int((h - 100) * (0.5 + 0.5 * math.sin(n / 7.0)))
    c.rect(sx, sy, sx + 40, sy + 40, BLACK)
    c.rect(sx + 10, sy + 10, sx + 30, sy + 30, WHITE)
    c.rect(0, 0, w, 26, BLACK)
    c.text(6, 6, "MOCK %d" % n, WHITE, scale=2)
    return c


class MapDrawer:
    """Top-down map image: grey unknown, white free, black walls, red robot + arrow, blue trail.

    Pixel mapping is the one documented for /api/map:
      u = (x - left_m) * px_per_m ;  v = height_px - (y - bottom_m) * px_per_m
    """

    def __init__(self, w, h, left_m, bottom_m, px_per_m, room, boxes, wall_m=0.1):
        self.w, self.h, self.left, self.bottom, self.ppm = w, h, left_m, bottom_m, px_per_m
        base = Canvas(w, h, GREY)
        x0, y0, x1, y1 = room
        self._fill(base, x0 - wall_m, y0 - wall_m, x1 + wall_m, y1 + wall_m, BLACK)
        self._fill(base, x0, y0, x1, y1, WHITE)
        for bx0, by0, bx1, by1 in boxes:  # LiDAR sees the outline; the inside stays unknown
            self._fill(base, bx0, by0, bx1, by1, BLACK)
            self._fill(base, bx0 + wall_m, by0 + wall_m, bx1 - wall_m, by1 - wall_m, GREY)
        self.base = base

    def to_px(self, x, y):
        return (x - self.left) * self.ppm, self.h - (y - self.bottom) * self.ppm

    def _fill(self, c, x0, y0, x1, y1, rgb):
        u0, v1 = self.to_px(x0, y0)
        u1, v0 = self.to_px(x1, y1)
        c.rect(round(u0), round(v0), round(u1), round(v1), rgb)

    def draw(self, pose, trail):
        c = self.base.copy()
        pts = [self.to_px(x, y) for x, y in trail]
        for a, b in zip(pts, pts[1:]):
            c.line(a, b, BLUE, 2)
        x, y, yaw = pose
        u, v = self.to_px(x, y)
        c.disc(u, v, 6, RED)
        # image v grows downwards while map +y is up, hence the minus on sin
        tip = (u + 16 * math.cos(yaw), v - 16 * math.sin(yaw))
        c.line((u, v), tip, RED, 3)
        for side in (2.6, -2.6):  # arrow head, about +-150 degrees from the heading
            c.line(tip, (tip[0] + 6 * math.cos(yaw + side), tip[1] - 6 * math.sin(yaw + side)), RED, 2)
        return c


# ── encoders ──

def load_pillow():
    """PIL.Image from this interpreter, else from tools/.venv when its Python version matches."""
    try:
        from PIL import Image
        return Image
    except Exception:
        pass
    site = os.path.join(VENV_DIR, "lib", "python%d.%d" % sys.version_info[:2], "site-packages")
    for path in glob.glob(site):
        sys.path.append(path)
        try:
            from PIL import Image
            return Image
        except Exception:  # e.g. a venv built on another OS
            sys.path.remove(path)
    return None


class PillowJpeg:
    name = "pillow"
    live = True

    def __init__(self, image_module):
        self.Image = image_module

    def encode(self, canvas, tag=None):
        img = self.Image.frombytes("RGB", (canvas.w, canvas.h), bytes(canvas.buf))
        out = io.BytesIO()
        img.save(out, "JPEG", quality=JPEG_QUALITY)
        return out.getvalue()


def bmp_bytes(canvas):
    """24-bit bottom-up BMP."""
    w, h = canvas.w, canvas.h
    bgr = bytearray(canvas.buf)
    bgr[0::3], bgr[2::3] = canvas.buf[2::3], canvas.buf[0::3]
    stride = w * 3
    pad = b"\0" * ((-stride) % 4)
    data = b"".join(bytes(bgr[y * stride:(y + 1) * stride]) + pad for y in range(h - 1, -1, -1))
    header = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    return header + info + data


class SipsJpeg:
    name = "sips"
    live = False

    def __init__(self, sips_path, workdir=CACHE_DIR):
        self.sips, self.workdir = sips_path, workdir

    def encode(self, canvas, tag):
        """Writes <tag>.jpg in the work dir (kept, it doubles as the cache) and returns its bytes."""
        os.makedirs(self.workdir, exist_ok=True)
        bmp = os.path.join(self.workdir, tag + ".bmp")
        jpg = os.path.join(self.workdir, tag + ".jpg")
        with open(bmp, "wb") as fp:
            fp.write(bmp_bytes(canvas))
        try:
            subprocess.run([self.sips, "-s", "format", "jpeg", "-s", "formatOptions", str(JPEG_QUALITY),
                            bmp, "--out", jpg], check=True, capture_output=True, timeout=20)
        finally:
            os.remove(bmp)
        with open(jpg, "rb") as fp:
            return fp.read()


def make_encoder(mode="auto"):
    """mode: auto | pillow | sips | none. Returns an encoder or None."""
    if mode in ("auto", "pillow"):
        image = load_pillow()
        if image is not None:
            return PillowJpeg(image)
        if mode == "pillow":
            raise RuntimeError("Pillow not found (python3 -m venv tools/.venv && tools/.venv/bin/pip install pillow)")
    if mode in ("auto", "sips"):
        sips = shutil.which("sips")
        if sips:
            return SipsJpeg(sips)
        if mode == "sips":
            raise RuntimeError("sips not found (macOS only)")
    return None


class CameraFrames:
    """Camera JPEG for frame n: drawn live with Pillow, or looped from cached files with sips."""

    def __init__(self, encoder):
        self.encoder = encoder
        self.cached = None
        if encoder is not None and not encoder.live:
            self.cached = [self._cached(i) for i in range(CACHED_CAMERA_FRAMES)]

    def _cached(self, i):
        tag = "camera_%s_%dx%d_%02d" % (CACHE_TAG, CAM_W, CAM_H, i)
        path = os.path.join(CACHE_DIR, tag + ".jpg")
        if os.path.isfile(path):
            with open(path, "rb") as fp:
                return fp.read()
        return self.encoder.encode(draw_camera(i), tag)

    def frame(self, n):
        if self.cached:
            return self.cached[n % len(self.cached)]
        return self.encoder.encode(draw_camera(n), "camera")
