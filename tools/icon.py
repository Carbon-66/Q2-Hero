#!/usr/bin/env python3
"""The card's icons, drawn: three gems coming down a fretboard toward their fret rings, under a
stage light, in a rounded square. card/icon.png (52 px) and card/icon-40.png (40 px).
Usage: tools/icon.py"""
import pathlib
from PIL import Image, ImageDraw, ImageFilter

def icon(size):
    S = size * 8  # drawn large, then scaled down smooth
    bg = Image.new('RGB', (S, S))
    px = bg.load()
    for y in range(S):
        t = y / S
        top, bot = (58, 18, 70), (10, 6, 16)
        c = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        for x in range(S):
            px[x, y] = c
    glow = Image.new('RGB', (S, S), (0, 0, 0))
    ImageDraw.Draw(glow).ellipse([S * 0.15, -S * 0.35, S * 0.85, S * 0.35], fill=(255, 120, 40))
    glow = glow.filter(ImageFilter.GaussianBlur(S * 0.12))
    bg = Image.blend(bg, Image.eval(glow, lambda v: v), 0.45)
    d = ImageDraw.Draw(bg)
    # the board in perspective
    cx, top_y, bot_y = S / 2, S * 0.1, S * 1.0
    d.polygon([(cx - S * 0.12, top_y), (cx + S * 0.12, top_y), (cx + S * 0.46, bot_y), (cx - S * 0.46, bot_y)], fill=(32, 28, 46))
    for k in (-1, 1):
        d.line([(cx + k * 0.04 * S, top_y), (cx + k * 0.153 * S, bot_y)], fill=(80, 74, 104), width=S // 90)
    cols = [(34, 221, 85), (255, 51, 68), (255, 204, 26)]
    def lane_x(l, y):
        s = (y - top_y) / (bot_y - top_y)
        return cx + (l - 1) * (0.08 + 0.23 * s) * S
    # fret rings
    ry = S * 0.82
    for l in range(3):
        x, r = lane_x(l, ry), S * 0.105
        d.ellipse([x - r, ry - r * 0.5, x + r, ry + r * 0.5], outline=cols[l], width=S // 28, fill=(20, 18, 28))
    # gems coming down
    for l, y in ((0, 0.6), (1, 0.42), (2, 0.28)):
        y *= S
        s = 0.35 + 0.65 * (y - top_y) / (bot_y - top_y)
        x, r = lane_x(l, y), S * 0.1 * s
        d.ellipse([x - r * 1.05, y - r * 0.5, x + r * 1.05, y + r * 0.62], fill=(8, 8, 8))
        d.ellipse([x - r, y - r * 0.55, x + r, y + r * 0.45], fill=cols[l])
        d.ellipse([x - r * 0.45, y - r * 0.38, x - r * 0.05, y - r * 0.12], fill=(255, 255, 255))
    mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=S * 0.22, fill=255)
    out = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    out.paste(bg, (0, 0), mask)
    return out.resize((size, size), Image.LANCZOS)

root = pathlib.Path(__file__).resolve().parent.parent / 'card'
icon(52).save(root / 'icon.png')
icon(40).save(root / 'icon-40.png')
