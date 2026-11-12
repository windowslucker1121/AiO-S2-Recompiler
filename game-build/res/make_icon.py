# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes game.ico: the neutral icon of every game exe (exe file, window, taskbar, the Windows volume mixer). A rounded square with a
play triangle, drawn here (no game or vendor art). Needs Pillow: tools\\.venv\\Scripts\\python game-build\\res\\make_icon.py"""
import os
from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
BG = (38, 92, 196, 255)
FG = (244, 247, 252, 255)


def draw(n):
    s = 8   # supersampling
    big = n * s
    im = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pad = big * 0.06
    d.rounded_rectangle([pad, pad, big - pad, big - pad], radius=big * 0.22, fill=BG)
    # play triangle, centred on its centroid
    h = big * 0.46
    w = h * 0.866
    cx, cy = big * 0.5 + w / 6, big * 0.5
    d.polygon([(cx - w * 2 / 3, cy - h / 2), (cx - w * 2 / 3, cy + h / 2), (cx + w / 3, cy)], fill=FG)
    return im.resize((n, n), Image.LANCZOS)


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'game.ico')
    imgs = [draw(n) for n in SIZES]
    imgs[-1].save(out, format='ICO', sizes=[(n, n) for n in SIZES], append_images=imgs[:-1])
    print(out, os.path.getsize(out))


if __name__ == '__main__':
    main()
