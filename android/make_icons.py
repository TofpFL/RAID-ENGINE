#!/usr/bin/env python3
"""Draws the launcher icons in android/res (needs Pillow).

The emblem is the Hand, the same figure the game draws for its health icon:
four fingers, a palm and an open blue eye, inside a golden ring.
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
DENSITIES = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}
SS = 4  # supersampling

GOLD = (255, 214, 120, 255)
BONE = (236, 232, 224, 255)
BLUE = (80, 150, 255, 255)


def background(px):
    """Dark violet sky with soft golden rays rising from below."""
    img = Image.new("RGBA", (px, px))
    d = ImageDraw.Draw(img)
    cx, cy = px / 2, px * 0.42
    rmax = math.hypot(px, px) * 0.6
    for i in range(64, 0, -1):
        t = i / 64
        c = tuple(int(a + (b - a) * (1 - t)) for a, b in zip((14, 8, 28), (78, 40, 110)))
        r = rmax * t
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=c + (255,))
    rays = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    rd = ImageDraw.Draw(rays)
    ox, oy = px / 2, px * 1.15
    for k in range(9):
        a = math.radians(-90 + (k - 4) * 13)
        w = math.radians(3.2)
        far = px * 2
        pts = [(ox, oy), (ox + math.cos(a - w) * far, oy + math.sin(a - w) * far),
               (ox + math.cos(a + w) * far, oy + math.sin(a + w) * far)]
        rd.polygon(pts, fill=(255, 205, 120, 34))
    rays = rays.filter(ImageFilter.GaussianBlur(px / 90))
    return Image.alpha_composite(img, rays)


def emblem(px, scale):
    """The Hand in its ring, centred, `scale` = canvas units per game unit."""
    img = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    c = px / 2

    def P(x, y):
        return (c + x * scale, c + y * scale)

    glow = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    r = 52 * scale
    gd.ellipse((c - r, c - r, c + r, c + r), fill=(255, 200, 100, 120))
    img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(14 * scale)))
    d = ImageDraw.Draw(img)

    r = 46 * scale
    d.ellipse((c - r, c - r, c + r, c + r), fill=(18, 12, 32, 245))
    w = 5 * scale
    d.ellipse((c - r, c - r, c + r, c + r), outline=GOLD, width=max(1, int(w)))

    def capsule(x0, y0, x1, y1, rad, col):
        d.line([P(x0, y0), P(x1, y1)], fill=col, width=int(rad * 2 * scale))
        for x, y in ((x0, y0), (x1, y1)):
            px_, py_ = P(x, y)
            d.ellipse((px_ - rad * scale, py_ - rad * scale, px_ + rad * scale, py_ + rad * scale), fill=col)

    def ell(x, y, rx, ry, col):
        px_, py_ = P(x, y)
        d.ellipse((px_ - rx * scale, py_ - ry * scale, px_ + rx * scale, py_ + ry * scale), fill=col)

    for i in range(4):
        bx = -21 + i * 14
        capsule(bx, -8, bx * 1.15, -30 - (8 if i in (1, 2) else 0), 6.5, BONE)
    ell(0, 10, 30, 28, BONE)
    ell(0, 10, 13, 8, (255, 255, 255, 255))
    ell(0, 10, 5, 5, BLUE)
    ell(-1.6, 8.4, 1.6, 1.6, (235, 245, 255, 255))
    return img


def rounded(img, radius):
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, img.size[0] - 1, img.size[1] - 1), radius=radius, fill=255)
    out = Image.new("RGBA", img.size, (0, 0, 0, 0))
    out.paste(img, (0, 0), mask)
    return out


def save(img, size, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.resize((size, size), Image.LANCZOS).save(path, optimize=True)


def main():
    for name, k in DENSITIES.items():
        folder = os.path.join(HERE, "res", "mipmap-" + name)
        # Adaptive icon layers (108dp, the emblem inside the 66dp safe zone).
        big = int(108 * k) * SS
        save(background(big), int(108 * k), os.path.join(folder, "ic_launcher_background.png"))
        save(emblem(big, big / 108 * 0.62), int(108 * k), os.path.join(folder, "ic_launcher_foreground.png"))
        # Legacy icon (48dp) for launchers before Android 8.
        small = int(48 * k) * SS
        legacy = Image.alpha_composite(background(small), emblem(small, small / 108 * 0.95))
        save(rounded(legacy, small * 0.18), int(48 * k), os.path.join(folder, "ic_launcher.png"))
    # 512px icon for stores and READMEs.
    full = Image.alpha_composite(background(512 * SS), emblem(512 * SS, 512 * SS / 108 * 0.95))
    save(full, 512, os.path.join(HERE, "icon-512.png"))


if __name__ == "__main__":
    main()
