#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the CardputerZero theme with original raster art and Lucide icons."""

import argparse
import json
import math
from pathlib import Path
import shutil
import struct
import tempfile
import zlib


INK = (42, 62, 79, 255)
TEAL = (43, 154, 145, 255)
SOFT = (127, 159, 177, 255)
WHITE = (241, 248, 251, 255)
GOLD = (246, 190, 69, 255)


class Canvas:
    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.pixels = bytearray(width * height * 4)

    def rect(self, x, y, width, height, color):
        x1, x2 = max(0, x), min(self.width, x + width)
        if x1 >= x2:
            return
        row = bytes(color) * (x2 - x1)
        for line in range(max(0, y), min(self.height, y + height)):
            offset = (line * self.width + x1) * 4
            self.pixels[offset:offset + len(row)] = row

    def circle(self, x, y, radius, color):
        for delta in range(-radius, radius + 1):
            half_width = math.isqrt(radius * radius - delta * delta)
            self.rect(x - half_width, y + delta, 2 * half_width + 1, 1, color)

    def paste_frame(self, image, x):
        size = image.width * 4
        for line in range(image.height):
            offset = (line * self.width + x) * 4
            self.pixels[offset:offset + size] = image.pixels[line * size:(line + 1) * size]

    def write(self, path):
        def chunk(kind, data):
            return (struct.pack('>I', len(data)) + kind + data
                    + struct.pack('>I', zlib.crc32(kind + data)))
        row = self.width * 4
        pixels = b''.join(b'\0' + self.pixels[i:i + row] for i in range(0, len(self.pixels), row))
        header = struct.pack('>IIBBBBB', self.width, self.height, 8, 6, 0, 0, 0)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header)
                         + chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))


def raster(width, height, motif, light=True):
    canvas = Canvas(width, height)
    x, y, size = width // 2, height // 2, min(width, height)
    unit = max(1, size // 24)
    color = INK if light else WHITE
    if motif in ('card', 'keyboard', 'warning', 'play'):
        canvas.rect(x - 10 * unit, y - 7 * unit, 20 * unit, 14 * unit, SOFT)
        canvas.rect(x - 9 * unit, y - 6 * unit, 18 * unit, 12 * unit, INK)
        if motif == 'keyboard':
            for row in range(3):
                for column in range(5):
                    canvas.rect(x + (3 * column - 7) * unit, y + (3 * row - 4) * unit,
                                2 * unit, 2 * unit, WHITE)
        elif motif == 'warning':
            canvas.rect(x - unit, y - 4 * unit, 2 * unit, 5 * unit, GOLD)
            canvas.rect(x - unit, y + 2 * unit, 2 * unit, 2 * unit, GOLD)
        else:
            for step in range(8 * unit):
                half_height = max(1, (8 * unit - step) // 2)
                canvas.rect(x - 3 * unit + step, y - half_height, 1, 2 * half_height, TEAL)
    elif motif in ('avatar', 'portrait'):
        canvas.circle(x, y, 10 * unit, SOFT)
        canvas.circle(x, y - 3 * unit, 3 * unit, WHITE)
        canvas.circle(x, y + 6 * unit, 5 * unit, TEAL)
    elif motif in ('eye', 'quotation', 'history', 'notice'):
        canvas.circle(x, y, 9 * unit, color)
        canvas.circle(x, y, 7 * unit, (0, 0, 0, 0))
        if motif == 'eye':
            canvas.circle(x, y, 3 * unit, color)
        elif motif == 'history':
            canvas.rect(x - unit, y - 5 * unit, 2 * unit, 6 * unit, color)
            canvas.rect(x, y - unit, 4 * unit, 2 * unit, color)
        else:
            canvas.rect(x - unit, y - 4 * unit, 2 * unit, 5 * unit, color)
            canvas.rect(x - unit, y + 2 * unit, 2 * unit, 2 * unit, color)
    elif motif.startswith('wifi-'):
        strength = int(motif[-1])
        for i in range(4):
            canvas.rect(x + (4 * i - 7) * unit, y + (4 - 3 * i) * unit,
                        3 * unit, (2 + 3 * i) * unit, color if i <= strength else SOFT)
    elif motif == 'battery':
        canvas.rect(x - 9 * unit, y - 5 * unit, 17 * unit, 10 * unit, color)
        canvas.rect(x + 8 * unit, y - 2 * unit, 3 * unit, 4 * unit, color)
        canvas.rect(x - 7 * unit, y - 3 * unit, 13 * unit, 6 * unit, (0, 0, 0, 0))
    elif motif == 'ethernet':
        canvas.rect(x - 8 * unit, y - 7 * unit, 16 * unit, 14 * unit, color)
        canvas.rect(x - 6 * unit, y - 5 * unit, 12 * unit, 9 * unit, (0, 0, 0, 0))
        for i in range(4):
            canvas.rect(x + (3 * i - 5) * unit, y + 3 * unit, unit, 4 * unit, color)
    elif motif == 'cursor':
        for line in range(16 * unit):
            canvas.rect(5 * unit, 4 * unit + line, max(unit, line // 2), 1, WHITE)
    else:
        raise ValueError(motif)
    return canvas


def coin_strip(width, height, count):
    frames = 24
    frame_width = width // frames
    strip = Canvas(width, height)
    for frame in range(frames):
        image = Canvas(frame_width, height)
        bounce = round(math.sin(math.tau * frame / frames) * height / 16)
        radius = frame_width // (3 if count == 1 else 4)
        for coin in range(count):
            x = frame_width // 2 + (2 * coin - count + 1) * radius // 2
            y = height // 2 + bounce - coin * radius
            image.circle(x, y, radius, INK)
            image.circle(x, y, radius - 3, GOLD)
            image.circle(x, y, radius - 9, INK)
            image.circle(x, y, radius - 12, GOLD)
            stroke = max(2, radius // 12)
            image.rect(x - stroke, y - radius // 3, 2 * stroke, 2 * radius // 3, INK)
        strip.paste_frame(image, frame * frame_width)
    return strip


RASTER = {
    'icon/icon.png': (256, 256, 'play'),
    'icon/logo.png': (160, 40, 'play'),
    'icon/bilibili_video.png': (256, 189, 'play'),
    'pictures/22_close.png': (233, 210, 'keyboard'),
    'pictures/22_open.png': (232, 210, 'portrait'),
    'pictures/33_close.png': (216, 212, 'keyboard'),
    'pictures/33_open.png': (216, 212, 'portrait'),
    'pictures/danmaku_highlight.png': (360, 120, 'notice'),
    'pictures/danmaku_ohh.png': (360, 120, 'notice'),
    'pictures/default_avatar.png': (157, 155, 'avatar'),
    'pictures/empty.png': (560, 316, 'card'),
    'pictures/hint_game_1.png': (1566, 689, 'keyboard'),
    'pictures/hint_game_2.png': (1556, 668, 'keyboard'),
    'pictures/hint_hbmenu.png': (1548, 659, 'keyboard'),
    'pictures/hint_wiliwili.png': (1548, 659, 'keyboard'),
    'pictures/icon-eye-16_16.png': (16, 16, 'eye'),
    'pictures/icon-eye.png': (42, 42, 'eye'),
    'pictures/icon-history.png': (32, 32, 'history'),
    'pictures/icon-quotation.png': (32, 32, 'quotation'),
    'pictures/net_error.png': (560, 316, 'warning'),
    'pictures/playlistbg.png': (190, 120, 'card'),
    'pictures/room-block.png': (800, 414, 'warning'),
    'pictures/sorry.png': (420, 224, 'warning'),
    'pictures/video-card-bg.png': (568, 354, 'card'),
    'img/sys/cursor.png': (36, 56, 'cursor'),
}


def icon_color(name):
    if 'active' in name or 'activate' in name or name.endswith('-on.svg'):
        return '#4dd5c0'
    if name.startswith('bpx-svg-sprite-') or name.endswith('-dark.svg'):
        return '#f1f8fb'
    return '#5b7085'


def generate(upstream, target, art):
    for dirname in ('font', 'i18n', 'material', 'opencc', 'xml'):
        shutil.copytree(upstream / dirname, target / dirname, ignore=shutil.ignore_patterns(
            'keymap_ps.ttf', 'keymap_xbox.ttf', 'FREEWARE.txt', '.gitignore'))
    assets = json.loads((art / 'icons.json').read_text(encoding='utf-8'))
    (target / 'svg').mkdir()
    for name, icon in assets['aliases'].items():
        text = assets['icons'][icon].replace('stroke="currentColor"', f'stroke="{icon_color(name)}"')
        (target / 'svg' / name).write_text(text, encoding='utf-8')
    for name, specification in RASTER.items():
        raster(*specification).write(target / name)
    for mascot, count in (('22', 1), ('33', 2)):
        for suffix, width, height in (('', 4488, 300), ('-small', 3366, 225)):
            coin_strip(width, height, count).write(target / f'pictures/{mascot}-coin-ani{suffix}.png')
    for theme in ('light', 'dark'):
        for strength in range(4):
            raster(176, 176, f'wifi-{strength}', theme == 'light').write(
                target / f'img/sys/wifi_{strength}_{theme}.png')
        raster(44, 44, 'battery', theme == 'light').write(target / f'img/sys/battery_back_{theme}.png')
        raster(176, 176, 'ethernet', theme == 'light').write(target / f'img/sys/ethernet_{theme}.png')
    for notice in ('LUCIDE-LICENSE', 'FEATHER-LICENSE', 'NOTO-EMOJI-OFL', 'NOTICE.md'):
        shutil.copyfile(art / notice, target / notice)


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('upstream', type=Path)
    cli.add_argument('output', type=Path)
    args = cli.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.output.parent) as work:
        target = Path(work) / 'resources'
        target.mkdir()
        generate(args.upstream, target, Path(__file__).resolve().parent)
        if args.output.is_dir() and not args.output.is_symlink():
            shutil.rmtree(args.output)
        target.rename(args.output)


if __name__ == '__main__':
    main()
