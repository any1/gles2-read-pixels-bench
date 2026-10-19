#!/usr/bin/env python3
#
# Pass the benchmark's json from stdin through to stdout, then draw a
# horizontal bar chart of the fps figures on the terminal using sixel
# graphics. The chart spans the full terminal width.
#
#   ./build/gles2-read-pixels-bench | scripts/fps-graph.py
#
# The chart is written to the controlling terminal (/dev/tty), so stdout
# stays clean json and can be piped further, e.g. into jq.

import fcntl
import json
import os
import re
import select
import struct
import sys
import termios
import tty

from PIL import Image, ImageDraw, ImageFont

# Reference palette: chart surface, text tokens and categorical slot 1.
THEMES = {
    'light': {
        'surface': (0xfc, 0xfc, 0xfb),
        'text': (0x0b, 0x0b, 0x0b),
        'text2': (0x52, 0x51, 0x4e),
        'axis': (0xc3, 0xc2, 0xb7),
        'bar': (0x2a, 0x78, 0xd6),
    },
    'dark': {
        'surface': (0x1a, 0x1a, 0x19),
        'text': (0xf0, 0xef, 0xec),
        'text2': (0xc3, 0xc2, 0xb7),
        'axis': (0x52, 0x51, 0x4e),
        'bar': (0x39, 0x87, 0xe5),
    },
}


def log(msg):
    print(f'fps-graph: {msg}', file=sys.stderr)


def tty_query(fd, request, timeout=0.5):
    # Send a request followed by a primary device attributes query. Every
    # terminal answers DA1, so its reply marks the end of the response and
    # we don't have to wait out the timeout for unsupported requests.
    old = termios.tcgetattr(fd)
    buf = b''
    try:
        tty.setraw(fd)
        os.write(fd, request + b'\x1b[c')
        while not re.search(rb'\x1b\[\?[0-9;]*c', buf):
            ready, _, _ = select.select([fd], [], [], timeout)
            if not ready:
                break
            buf += os.read(fd, 1024)
    finally:
        termios.tcsetattr(fd, termios.TCSAFLUSH, old)
    return buf


def terminal_info(fd):
    rows, cols, xpix, ypix = struct.unpack(
        'HHHH', fcntl.ioctl(fd, termios.TIOCGWINSZ, b'\0' * 8))

    # CSI 16 t: cell size in pixels, OSC 11: background colour.
    reply = tty_query(fd, b'\x1b[16t\x1b]11;?\x1b\\')

    da = re.search(rb'\x1b\[\?([0-9;]*)c', reply)
    sixel = bool(da) and b'4' in da.group(1).split(b';')

    cell_w = xpix // cols if cols and xpix else 0
    cell_h = ypix // rows if rows and ypix else 0
    m = re.search(rb'\x1b\[6;(\d+);(\d+)t', reply)
    if m and not (cell_w and cell_h):
        cell_h, cell_w = int(m.group(1)), int(m.group(2))
    if not (cell_w and cell_h):
        cell_w, cell_h = 10, 20

    bg = None
    m = re.search(rb'\x1b\]11;rgb:([0-9a-fA-F]+)/([0-9a-fA-F]+)/'
                  rb'([0-9a-fA-F]+)', reply)
    if m:
        bg = tuple(int(c, 16) * 255 // (16 ** len(c) - 1)
                   for c in m.groups())

    return {
        'cols': cols,
        'cell_w': cell_w,
        'cell_h': cell_h,
        'bg': bg,
        'sixel': sixel,
    }


def pick_theme(bg):
    if bg is None:
        return dict(THEMES['dark'])
    luma = 0.2126 * bg[0] + 0.7152 * bg[1] + 0.0722 * bg[2]
    theme = dict(THEMES['light' if luma > 128 else 'dark'])
    theme['surface'] = bg
    return theme


def format_fps(v):
    return f'{v:,.1f}'


def subtitle(doc):
    cfg = doc.get('config', {})
    parts = []
    crop = cfg.get('crop')
    if crop and 'width' in cfg:
        parts.append(f"{crop['width']}x{crop['height']} crop of "
                     f"{cfg['width']}x{cfg['height']}")
    if 'duration_s' in cfg:
        parts.append(f"{cfg['duration_s']:g} s per test")
    if cfg.get('encode'):
        parts.append('with mjpeg encoding')
    renderer = doc.get('gl', {}).get('renderer')
    if renderer:
        parts.append(renderer)
    return ' · '.join(parts)


def render(doc, width, cell_h, theme):
    results = doc.get('results', [])
    names = [r.get('test', '?') for r in results]
    fps = [float(r.get('fps', 0.0)) for r in results]
    values = [format_fps(v) for v in fps]

    s = cell_h / 20
    font = ImageFont.load_default(size=round(cell_h * 0.7))
    title_font = ImageFont.load_default(size=round(cell_h * 0.8))
    pad = round(12 * s)
    gap = round(8 * s)
    pitch = round(cell_h * 1.4)
    bar_h = min(round(cell_h * 0.8), round(24 * s))
    radius = max(1, round(4 * s))

    def text_w(t, f=font):
        return round(f.getlength(t))

    label_w = max(text_w(n) for n in names)
    value_w = max(text_w(v) for v in values)

    plot_x = pad + label_w + gap
    plot_w = width - plot_x - gap - value_w - pad
    if plot_w < 20:
        raise ValueError('terminal too narrow for chart')

    head_h = cell_h * 2
    body_h = pitch * len(results)
    height = head_h + body_h + pad

    # Whole sixel bands, or the terminal pads the last band with colour 0.
    height = -(-height // 6) * 6

    im = Image.new('RGB', (width, height), theme['surface'])
    d = ImageDraw.Draw(im)

    d.text((pad, cell_h // 2), 'Captured frames per second',
           font=title_font, fill=theme['text'], anchor='lm')
    d.text((pad, cell_h + cell_h // 2), subtitle(doc), font=font,
           fill=theme['text2'], anchor='lm')

    top = head_h
    d.line([(plot_x, top), (plot_x, top + body_h - 1)],
           fill=theme['axis'], width=max(1, round(s)))

    vmax = max(fps) if fps and max(fps) > 0 else 1.0
    for i, (name, v, label) in enumerate(zip(names, fps, values)):
        cy = top + i * pitch + pitch // 2
        d.text((plot_x - gap, cy), name, font=font,
               fill=theme['text'], anchor='rm')

        bw = round(plot_w * v / vmax)
        x0 = plot_x + 1
        if bw > 0:
            y0 = cy - bar_h // 2
            y1 = y0 + bar_h - 1
            d.rounded_rectangle([(x0, y0), (x0 + bw - 1, y1)],
                                radius=min(radius, bw // 2),
                                fill=theme['bar'],
                                corners=(False, True, True, False))
        d.text((x0 + bw + gap, cy), label, font=font,
               fill=theme['text2'], anchor='lm')

    return im


def sixel_encode(im):
    im = im.quantize(colors=64, dither=Image.Dither.NONE)
    w, h = im.size
    pal = im.getpalette()
    px = im.tobytes()

    out = [f'\x1bP0;1;0q"1;1;{w};{h}']
    ncolours = len(pal) // 3
    for i in range(ncolours):
        r, g, b = pal[i * 3:i * 3 + 3]
        out.append(f'#{i};2;{r * 100 // 255};{g * 100 // 255};'
                   f'{b * 100 // 255}')

    for y0 in range(0, h, 6):
        rows = [px[y * w:(y + 1) * w] for y in range(y0, min(y0 + 6, h))]
        planes = {}
        for k, row in enumerate(rows):
            bit = 1 << k
            for x, c in enumerate(row):
                plane = planes.get(c)
                if plane is None:
                    plane = planes[c] = bytearray(w)
                plane[x] |= bit

        bands = []
        for c, plane in planes.items():
            band = [f'#{c}']
            x = 0
            while x < w:
                v = plane[x]
                n = 1
                while x + n < w and plane[x + n] == v:
                    n += 1
                ch = chr(63 + v)
                band.append(f'!{n}{ch}' if n > 3 else ch * n)
                x += n
            bands.append(''.join(band))
        out.append('$'.join(bands) + '-')

    out.append('\x1b\\')
    return ''.join(out)


def main():
    data = bytearray()
    stdin = sys.stdin.buffer
    stdout = sys.stdout.buffer
    while True:
        chunk = stdin.read1(65536)
        if not chunk:
            break
        data += chunk
        stdout.write(chunk)
        stdout.flush()

    try:
        doc = json.loads(data)
    except ValueError as e:
        log(f'could not parse json: {e}')
        return 1

    if not doc.get('results'):
        log('no results to plot')
        return 0

    try:
        fd = os.open('/dev/tty', os.O_RDWR | os.O_NOCTTY)
    except OSError as e:
        log(f'no terminal to draw on: {e}')
        return 0

    try:
        info = terminal_info(fd)
        if not info['sixel']:
            log('terminal does not report sixel support')
            return 0

        width = info['cols'] * info['cell_w']
        im = render(doc, width, info['cell_h'], pick_theme(info['bg']))
        os.write(fd, sixel_encode(im).encode('ascii') + b'\n')
    except ValueError as e:
        log(str(e))
    finally:
        os.close(fd)

    return 0


if __name__ == '__main__':
    sys.exit(main())
