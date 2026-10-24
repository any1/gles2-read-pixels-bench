#!/usr/bin/env python3
#
# Turn the benchmark json files in reports/ into REPORT.md, with the bar
# charts in assets/ and snippets.md appended.
#
#   scripts/md-report.py

import argparse
import json
import os
import re
import sys
from xml.sax.saxutils import escape

# Reference palette: chart surface, text tokens and categorical slots 1-4.
THEMES = {
    'light': {
        'surface': '#fcfcfb',
        'border': '#e4e3de',
        'text': '#0b0b0b',
        'text2': '#52514e',
        'axis': '#c3c2b7',
        'bar': '#2a78d6',
        'cpu': '#eb6834',
        'gpu': '#1baf7a',
        'eff': '#eda100',
    },
    'dark': {
        'surface': '#1a1a19',
        'border': '#353533',
        'text': '#f0efec',
        'text2': '#c3c2b7',
        'axis': '#52514e',
        'bar': '#3987e5',
        'cpu': '#d95926',
        'gpu': '#199e70',
        'eff': '#c98500',
    },
}

MONO = 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace'
SANS = 'system-ui, -apple-system, Segoe UI, Helvetica, Arial, sans-serif'

WIDTH = 960
PAD = 16
GAP = 8
FONT = 14
SMALL = 11
PITCH = 34
THIN_PITCH = 16
GROUP_GAP = 12
BAR_H = 22
THIN_H = 6
RADIUS = 4


def log(msg):
    print(f'md-report: {msg}', file=sys.stderr)


def load_pct(r, key):
    load = r.get(key)
    if isinstance(load, dict) and 'load_pct' in load:
        return float(load['load_pct'])
    return None


def efficiency(r):
    cpu = load_pct(r, 'cpu')
    gpu = load_pct(r, 'gpu')
    if cpu is None or gpu is None or cpu + gpu <= 0:
        return None
    return float(r.get('fps', 0.0)) / (cpu + gpu)


def meta(doc):
    cfg = doc.get('config', {})
    gl = doc.get('gl', {})
    crop = cfg.get('crop', {})
    items = []
    if crop and 'width' in cfg:
        items.append(('crop', f"{crop['width']}x{crop['height']}"
                      f"+{crop['x']}+{crop['y']}"))
    if 'width' in cfg:
        items.append(('frame', f"{cfg['width']}x{cfg['height']}"))
    if 'duration_s' in cfg:
        items.append(('duration', f"{cfg['duration_s']:g} s"))
    if 'warmup_frames' in cfg:
        items.append(('warmup', f"{cfg['warmup_frames']} frames"))
    if 'format' in cfg:
        items.append(('format', cfg['format']))
    if cfg.get('encode'):
        items.append(('mjpeg', f"q{cfg.get('jpeg_quality', '?')}"))
    for key in ('renderer', 'version'):
        if key in gl:
            items.append((key, gl[key]))
    return items


def text_w(text, size, mono):
    return len(text) * size * (0.62 if mono else 0.6)


def bar_path(x0, cy, w, h):
    # rounded at the data end, square at the baseline
    y0 = cy - h / 2
    r = min(RADIUS, w / 2, h / 2)
    x1 = x0 + w
    return (f'M{x0:.2f},{y0:.2f}H{x1 - r:.2f}'
            f'A{r:.2f},{r:.2f} 0 0 1 {x1:.2f},{y0 + r:.2f}'
            f'V{y0 + h - r:.2f}'
            f'A{r:.2f},{r:.2f} 0 0 1 {x1 - r:.2f},{y0 + h:.2f}'
            f'H{x0:.2f}Z')


def by_fps(results):
    return sorted(results, key=lambda r: float(r.get('fps', 0)),
                  reverse=True)


def render_svg(results, theme,
               title='fps, CPU and GPU load per test'):
    results = by_fps(results)
    fps_max = max((float(r.get('fps', 0)) for r in results),
                  default=0) or 1.0
    eff_max = max((e for e in map(efficiency, results) if e is not None),
                  default=0) or 1.0

    # (label, value, scale, value text, colour, thick) per bar, per test
    groups = []
    for r in results:
        fps = float(r.get('fps', 0))
        bars = [(r.get('test', '?'), fps, fps_max, f'{fps:,.1f}', 'bar',
                 True)]
        for key, label in (('cpu', 'CPU'), ('gpu', 'GPU')):
            v = load_pct(r, key)
            if v is not None:
                bars.append((label, v, 100.0, f'{v:.1f}%', key, False))
        e = efficiency(r)
        if e is not None:
            bars.append(('fps / load', e, eff_max, f'{e:.2f} fps/%', 'eff',
                         False))
        groups.append(bars)

    label_w = max(text_w(b[0], FONT if b[5] else SMALL, b[5])
                  for bars in groups for b in bars)
    value_w = max(text_w(b[3], FONT if b[5] else SMALL, False)
                  for bars in groups for b in bars)

    plot_x = PAD + label_w + GAP
    plot_w = WIDTH - plot_x - GAP - value_w - PAD

    body_h = (sum(PITCH + THIN_PITCH * (len(bars) - 1) for bars in groups)
              + GROUP_GAP * (len(groups) - 1))
    height = PAD + body_h + PAD

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" '
           f'height="{height:.0f}" viewBox="0 0 {WIDTH} {height:.0f}" '
           f'role="img" aria-label="{escape(title)}">',
           f'<title>{escape(title)}</title>',
           f'<rect x="0.5" y="0.5" width="{WIDTH - 1}" '
           f'height="{height - 1:.0f}" rx="6" fill="{theme["surface"]}" '
           f'stroke="{theme["border"]}"/>',
           f'<line x1="{plot_x:.2f}" y1="{PAD}" x2="{plot_x:.2f}" '
           f'y2="{PAD + body_h:.2f}" stroke="{theme["axis"]}"/>']

    x0 = plot_x + 1
    y = PAD
    for bars in groups:
        for label, v, vmax, text, colour, thick in bars:
            pitch, h, size = ((PITCH, BAR_H, FONT) if thick
                              else (THIN_PITCH, THIN_H, SMALL))
            ink = theme['text'] if thick else theme['text2']
            family = MONO if thick else SANS
            cy = y + pitch / 2
            y += pitch

            out.append(f'<text x="{plot_x - GAP:.2f}" y="{cy:.2f}" '
                       f'font-family="{family}" font-size="{size}" '
                       f'fill="{ink}" text-anchor="end" '
                       f'dominant-baseline="central">{escape(label)}</text>')

            bw = plot_w * min(v, vmax) / vmax
            if bw >= 0.5:
                out.append(f'<path d="{bar_path(x0, cy, bw, h)}" '
                           f'fill="{theme[colour]}"/>')
            out.append(f'<text x="{x0 + bw + GAP:.2f}" y="{cy:.2f}" '
                       f'font-family="{SANS}" font-size="{size}" '
                       f'fill="{theme["text2"]}" '
                       f'font-variant-numeric="tabular-nums" '
                       f'dominant-baseline="central">{escape(text)}</text>')
        y += GROUP_GAP

    out.append('</svg>')
    return '\n'.join(out) + '\n'


def md_escape(text):
    return re.sub(r'([\\`*_\[\]<>|])', r'\\\1', str(text))


def render_section(name, doc, light, dark):
    meta_md = '\n'.join(f'- **{k}**: {md_escape(v)}' for k, v in meta(doc))
    alt = md_escape(f'{name}: bar chart of fps, CPU and GPU load per test')

    return '\n'.join([
        f'## {md_escape(name)}', '', meta_md, '',
        '<picture>',
        f'  <source media="(prefers-color-scheme: dark)" srcset="{dark}">',
        f'  <img alt="{alt}" src="{light}">',
        '</picture>', ''])


def main():
    ap = argparse.ArgumentParser(
        description='Make a single markdown report with svg charts from '
                    'all the benchmark json in a directory.')
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument('dir', nargs='?', default=os.path.join(root, 'reports'),
                    help='directory of benchmark json (default: %(default)s)')
    ap.add_argument('-o', '--output', default=os.path.join(root, 'REPORT.md'),
                    help='markdown file (default: %(default)s)')
    ap.add_argument('-t', '--title', default='glReadPixels benchmark',
                    help='report heading (default: %(default)s)')
    args = ap.parse_args()

    docs = []
    for fname in sorted(os.listdir(args.dir)):
        if not fname.endswith('.json'):
            continue
        path = os.path.join(args.dir, fname)
        try:
            with open(path, 'rb') as f:
                doc = json.load(f)
        except (OSError, ValueError) as e:
            log(f'skipping {path}: {e}')
            continue
        if not doc.get('results'):
            log(f'skipping {path}: no results')
            continue
        docs.append((fname[:-len('.json')], doc))

    if not docs:
        log(f'no benchmark json in {args.dir}')
        return 1

    md_dir = os.path.dirname(os.path.abspath(args.output))
    assets = os.path.join(root, 'assets')
    os.makedirs(assets, exist_ok=True)

    def link(path):
        return os.path.relpath(path, md_dir).replace(os.sep, '/')

    out = [f'# {md_escape(args.title)}', '']
    written = set()
    for name, doc in docs:
        links = {}
        for mode, theme in THEMES.items():
            fname = f'{name}-{mode}.svg'
            path = os.path.join(assets, fname)
            with open(path, 'w') as f:
                f.write(render_svg(doc['results'], theme))
            log(f'wrote {path}')
            written.add(fname)
            links[mode] = link(path)
        out.append(render_section(name, doc, links['light'], links['dark']))

    for fname in sorted(os.listdir(assets)):
        if fname.endswith('.svg') and fname not in written:
            os.remove(os.path.join(assets, fname))
            log(f'deleted {os.path.join(assets, fname)}')

    try:
        with open(os.path.join(root, 'snippets.md')) as f:
            out.append(f.read())
    except OSError as e:
        log(f'no snippets: {e}')

    with open(args.output, 'w') as f:
        f.write('\n'.join(out))
    log(f'wrote {args.output}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
