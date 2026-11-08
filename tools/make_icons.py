#!/usr/bin/env python3
"""Make the app icons from a frame of the game (needs Pillow).

Crops the board out of a 320x240 frame written by joytest (build/shots/
level1.ppm: `./build/joytest JOYSCRIPT LEVEL1_KEYS build/shots`) and
scales it up with whole pixels, onto the lawn, for iOS and Android.

Usage: make_icons.py FRAME.ppm
"""
import json
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ANDROID = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}


def main():
    frame = Image.open(sys.argv[1]).convert("RGB")
    # the board: radius 88 around (160, 120), plus a little lawn; the tove
    # waits at the top of the rim
    board = frame.crop((160 - 96, 120 - 98, 160 + 96, 120 + 94))
    # outside the rim, plain lawn (the HUD panels sit in the corners)
    lawn = frame.getpixel((300, 200))
    px = board.load()
    for y in range(board.height):
        for x in range(board.width):
            dx, dy = x + 0.5 - 96, y + 0.5 - 98
            if dx * dx + dy * dy > 92 * 92 and not (abs(dx) < 8 and dy < 0):
                px[x, y] = lawn
    big = board.resize((1024, 1024), Image.NEAREST)

    ios = os.path.join(ROOT, "ios", "Assets.xcassets", "AppIcon.appiconset")
    os.makedirs(ios, exist_ok=True)
    big.save(os.path.join(ios, "icon-1024.png"))
    with open(os.path.join(ios, "Contents.json"), "w") as f:
        json.dump({"images": [{"filename": "icon-1024.png", "idiom": "universal",
                               "platform": "ios", "size": "1024x1024"}],
                   "info": {"author": "xcode", "version": 1}}, f, indent=2)
    with open(os.path.join(ROOT, "ios", "Assets.xcassets", "Contents.json"), "w") as f:
        json.dump({"info": {"author": "xcode", "version": 1}}, f, indent=2)

    for dpi, size in ANDROID.items():
        d = os.path.join(ROOT, "android", "app", "src", "main", "res", f"mipmap-{dpi}")
        os.makedirs(d, exist_ok=True)
        board.resize((size, size), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"))
    print("icons written")


if __name__ == "__main__":
    main()
