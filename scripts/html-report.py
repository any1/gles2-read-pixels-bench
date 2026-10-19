#!/usr/bin/env python3
#
# Turn the benchmark's json into a self-contained html report: a bar chart
# of the fps figures, with the CPU and GPU load and fps per load below each,
# followed by a code snippet for every test that ran.
#
#   ./build/gles2-read-pixels-bench | scripts/html-report.py -o report.html
#
# The json is read from the file given as argument, or from stdin.

import argparse
import html
import json
import re
import sys

# Each capture method as the parts the benchmark builds it from. The
# snippet for a test is put together from these, with the staging
# allocation inside the frame loop for the -realloc variants.
METHODS = {
    'gpu-staging': {
        'decl': 'GLuint tex, fbo;',
        'alloc': '''\
glGenTextures(1, &tex);
glBindTexture(GL_TEXTURE_2D, tex);
glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
	     GL_UNSIGNED_BYTE, NULL);
glGenFramebuffers(1, &fbo);
glBindFramebuffer(GL_FRAMEBUFFER, fbo);
glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		       GL_TEXTURE_2D, tex, 0);''',
        'capture': '''\
glBindFramebuffer(GL_FRAMEBUFFER, render_fbo);
glBindTexture(GL_TEXTURE_2D, tex);
glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, x, y, w, h);
glBindFramebuffer(GL_FRAMEBUFFER, fbo);
glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);''',
        'free': '''\
glDeleteFramebuffers(1, &fbo);
glDeleteTextures(1, &tex);''',
    },
    'cpu-staging': {
        'decl': 'unsigned char *staging;\nint row;',
        'alloc': 'staging = malloc(fb_w * fb_h * 4);',
        'capture': '''\
glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, staging);

for (row = 0; row < h; row++)
	memcpy(dst + row * w * 4,
	       staging + ((y + row) * fb_w + x) * 4, w * 4);''',
        'free': 'free(staging);',
    },
    'per-row': {
        'decl': 'int row;',
        'capture': '''\
for (row = 0; row < h; row++)
	glReadPixels(x, y + row, w, 1, GL_RGBA, GL_UNSIGNED_BYTE,
		     dst + row * w * 4);''',
    },
    'direct': {
        'extensions': ['GL_NV_pack_subimage'],
        'dst_size': 'fb_w * fb_h * 4',
        'setup': '''\
glPixelStorei(GL_PACK_ROW_LENGTH, fb_w);
glPixelStorei(GL_PACK_SKIP_PIXELS, x);
glPixelStorei(GL_PACK_SKIP_ROWS, y);''',
        'capture': 'glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);',
        'teardown': '''\
glPixelStorei(GL_PACK_ROW_LENGTH, 0);
glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
glPixelStorei(GL_PACK_SKIP_ROWS, 0);''',
    },
    'pbo': {
        'extensions': [
            'GL_NV_pixel_buffer_object',
            'GL_EXT_map_buffer_range',
            'GL_OES_mapbuffer',
        ],
        'decl': 'GLuint pbo;\nvoid *map;',
        'alloc': '''\
glGenBuffers(1, &pbo);
glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, pbo);
glBufferData(GL_PIXEL_PACK_BUFFER_NV, w * h * 4, NULL, GL_DYNAMIC_DRAW);
glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, 0);''',
        'capture': '''\
glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, pbo);
glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

map = glMapBufferRangeEXT(GL_PIXEL_PACK_BUFFER_NV, 0, w * h * 4,
			  GL_MAP_READ_BIT_EXT);
memcpy(dst, map, w * h * 4);
glUnmapBufferOES(GL_PIXEL_PACK_BUFFER_NV);
glBindBuffer(GL_PIXEL_PACK_BUFFER_NV, 0);''',
        'free': 'glDeleteBuffers(1, &pbo);',
    },
}

C_TOKENS = re.compile(r'''
    (?P<comment>/\*.*?\*/|//[^\n]*)
  | (?P<string>"(?:\\.|[^"\\])*")
  | (?P<const>\bGL_[A-Z0-9_]+\b|\bNULL\b)
  | (?P<keyword>\b(?:for|while|if|else|return|sizeof)\b)
  | (?P<type>\b(?:GLuint|GLint|GLenum|void|int|unsigned|char|size_t)\b)
  | (?P<func>\b[A-Za-z_]\w*(?=\())
  | (?P<number>\b\d+\b)
''', re.VERBOSE | re.DOTALL)


def indent(code, depth=1):
    return '\n'.join('\t' * depth + line if line else line
                     for line in code.split('\n'))


def snippet(method, realloc):
    m = METHODS[method]
    alloc = m.get('alloc')
    free = m.get('free')

    out = ['unsigned char *dst;']
    if 'decl' in m:
        out += [m['decl']]
    out += ['', f"dst = malloc({m.get('dst_size', 'w * h * 4')});", '']
    if 'setup' in m:
        out += [m['setup'], '']
    if alloc and not realloc:
        out += [alloc, '']

    body = []
    if alloc and realloc:
        body += [alloc, '']
    body += [m['capture']]
    if free and realloc:
        body += ['', free]

    out += ['while (elapsed() < duration) {',
            '\tdraw_triangle(render_fbo);',
            '',
            indent('\n'.join(body)),
            '',
            '\tframes++;',
            '}',
            '']

    if free and not realloc:
        out += [free, '']
    if 'teardown' in m:
        out += [m['teardown'], '']
    out += ['free(dst);']
    return '\n'.join(out)


def highlight(code):
    out = []
    pos = 0
    for tok in C_TOKENS.finditer(code):
        out.append(html.escape(code[pos:tok.start()]))
        out.append(f'<span class="{tok.lastgroup}">'
                   f'{html.escape(tok.group())}</span>')
        pos = tok.end()
    out.append(html.escape(code[pos:]))
    return ''.join(out)


def meta(doc):
    cfg = doc.get('config', {})
    gl = doc.get('gl', {})
    crop = cfg.get('crop', {})
    items = []
    if crop and 'width' in cfg:
        items.append(('crop', f"{crop['width']}x{crop['height']}"
                      f"+{crop['x']}+{crop['y']}"))
        items.append(('frame', f"{cfg['width']}x{cfg['height']}"))
    if 'duration_s' in cfg:
        items.append(('duration', f"{cfg['duration_s']:g} s"))
    if 'warmup_frames' in cfg:
        items.append(('warmup', str(cfg['warmup_frames'])))
    if 'format' in cfg:
        items.append(('format', cfg['format']))
    if cfg.get('encode'):
        items.append(('mjpeg', f"q{cfg.get('jpeg_quality', '?')}"))
    for key in ('renderer', 'version'):
        if key in gl:
            items.append((key, gl[key]))
    return items


def test_id(name):
    return 'test-' + re.sub(r'[^a-z0-9-]', '-', name.lower())


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


def bar_row(cls, label, value, vmax, text, tip):
    pct = 100.0 * min(value, vmax) / vmax
    return f'''\
  <span class="row {cls}">
    <span class="label">{html.escape(label)}</span>
    <span class="track">
      <span class="bar" style="width: {pct:.3f}%"></span>
      <span class="value">{html.escape(text)}</span>
      <span class="tip">{html.escape(tip)}</span>
    </span>
  </span>'''


def render_chart(results):
    fps_max = max((float(r.get('fps', 0)) for r in results),
                  default=0) or 1.0
    eff_max = max((e for e in map(efficiency, results) if e is not None),
                  default=0) or 1.0

    groups = []
    for r in results:
        name = r.get('test', '?')
        fps = float(r.get('fps', 0))
        rows = [bar_row('fps', name, fps, fps_max, f'{fps:,.1f}',
                        f"{r.get('frames', '?')} frames / "
                        f"{float(r.get('elapsed_s', 0)):.3f} s")]

        cpu = load_pct(r, 'cpu')
        if cpu is not None:
            rows.append(bar_row('cpu', 'CPU', cpu, 100.0, f'{cpu:.1f}%',
                                f"{float(r['cpu'].get('time_s', 0)):.3f} s "
                                f"CPU time, {r['cpu'].get('cpus', '?')} CPUs"))

        gpu = load_pct(r, 'gpu')
        if gpu is not None:
            engines = r['gpu'].get('engines', {})
            rows.append(bar_row('gpu', 'GPU', gpu, 100.0, f'{gpu:.1f}%',
                                ' · '.join(f'{k} {float(v):.1f}%'
                                           for k, v in engines.items())))

        eff = efficiency(r)
        if eff is not None:
            rows.append(bar_row('eff', 'fps / load', eff, eff_max,
                                f'{eff:.2f} fps/%',
                                f'{fps:,.1f} fps / '
                                f'({cpu:.1f}% + {gpu:.1f}%)'))

        cls = 'group load' if len(rows) > 1 else 'group'
        groups.append(f'<a class="{cls}" href="#{test_id(name)}">\n'
                      + '\n'.join(rows) + '\n</a>')
    return '\n'.join(groups)


def render_snippets(results):
    out = []
    for r in results:
        name = r.get('test', '?')
        method = r.get('method', name)
        if method not in METHODS:
            continue
        realloc = bool(r.get('staging_realloc'))
        exts = ''.join(f'<code class="ext">{html.escape(e)}</code>'
                       for e in METHODS[method].get('extensions', []))
        out.append(f'''\
<section id="{test_id(name)}">
  <h2>{html.escape(name)} <span class="fps">{float(r.get('fps', 0)):,.1f} fps</span></h2>
  {f'<div class="exts">{exts}</div>' if exts else ''}
  <pre><code>{highlight(snippet(method, realloc))}</code></pre>
</section>''')
    return '\n'.join(out)


CSS = '''\
:root {
  color-scheme: light;
  --surface: #fcfcfb;
  --surface-2: #f3f2ef;
  --border: #e4e3de;
  --text: #0b0b0b;
  --text-2: #52514e;
  --text-3: #7b7a74;
  --bar: #2a78d6;
  --bar-hover: #1d5fae;
  --cpu: #eb6834;
  --gpu: #1baf7a;
  --eff: #eda100;
  --syn-comment: #7b7a74;
  --syn-string: #0f7a52;
  --syn-const: #a24a12;
  --syn-keyword: #6250d6;
  --syn-type: #6250d6;
  --syn-func: #1d5fae;
  --syn-number: #a24a12;
}
@media (prefers-color-scheme: dark) {
  :root {
    color-scheme: dark;
    --surface: #1a1a19;
    --surface-2: #232322;
    --border: #353533;
    --text: #f0efec;
    --text-2: #c3c2b7;
    --text-3: #8f8e86;
    --bar: #3987e5;
    --bar-hover: #6aa6ee;
    --cpu: #d95926;
    --gpu: #199e70;
    --eff: #c98500;
    --syn-comment: #8f8e86;
    --syn-string: #5cc79c;
    --syn-const: #f0a46b;
    --syn-keyword: #a99ff0;
    --syn-type: #a99ff0;
    --syn-func: #7fb3f0;
    --syn-number: #f0a46b;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0;
  padding: 32px 16px 64px;
  background: var(--surface);
  color: var(--text);
  font: 15px/1.5 system-ui, sans-serif;
}
main { max-width: 1400px; margin: 0 auto; }
h1 { font-size: 22px; font-weight: 600; margin: 0 0 8px; }
dl.meta {
  display: flex; flex-wrap: wrap; gap: 4px 20px;
  margin: 0 0 28px; font-size: 13px; color: var(--text-2);
}
dl.meta div { display: flex; gap: 6px; }
dl.meta dt { color: var(--text-3); }
dl.meta dd { margin: 0; }
.chart {
  display: grid; grid-template-columns: max-content 1fr;
  column-gap: 12px; margin-bottom: 48px;
}
.group, .row {
  display: grid; grid-template-columns: subgrid; grid-column: 1 / -1;
  align-items: center; color: inherit; text-decoration: none;
}
.group.load + .group .row.fps .track { height: 46px; padding-top: 12px; }
.group.load + .group .row.fps .label { padding-top: 12px; }
.label { text-align: right; font-size: 14px; color: var(--text); }
.track {
  position: relative; display: flex; align-items: center;
  height: 34px; border-left: 1px solid var(--border);
  padding-right: 80px;
}
.bar {
  display: block; height: 22px; background: var(--bar);
  border-radius: 0 4px 4px 0; min-width: 0;
  transition: background 0.1s;
}
.value {
  margin-left: 8px; font-size: 13px; color: var(--text-2);
  font-variant-numeric: tabular-nums; white-space: nowrap;
}
.row:not(.fps) .label { font-size: 11px; color: var(--text-2); }
.row:not(.fps) .track { height: 16px; }
.row:not(.fps) .bar { height: 6px; border-radius: 0 3px 3px 0; }
.row:not(.fps) .value { font-size: 11px; }
.row.cpu .bar { background: var(--cpu); }
.row.gpu .bar { background: var(--gpu); }
.row.eff .bar { background: var(--eff); }
.tip {
  position: absolute; left: 8px; top: -26px; z-index: 1;
  padding: 2px 8px; border-radius: 4px; font-size: 12px;
  background: var(--text); color: var(--surface);
  white-space: nowrap; pointer-events: none; opacity: 0;
  transition: opacity 0.1s;
}
.group:hover .row.fps .bar, .group:focus-visible .row.fps .bar {
  background: var(--bar-hover);
}
.row:hover .tip, .group:focus-visible .row.fps .tip { opacity: 1; }
.snippets {
  display: grid; gap: 32px 24px;
  grid-template-columns: repeat(auto-fill, minmax(min(100%, 620px), 1fr));
}
section { min-width: 0; scroll-margin-top: 16px; }
section:target pre { border-color: var(--bar); }
h2 {
  font: 600 15px/1.4 ui-monospace, monospace; margin: 0 0 8px;
  display: flex; justify-content: space-between; gap: 12px;
}
h2 .fps {
  font-weight: 400; color: var(--text-2);
  font-variant-numeric: tabular-nums;
}
.exts { display: flex; flex-wrap: wrap; gap: 6px; margin-bottom: 8px; }
.ext {
  font: 12px ui-monospace, monospace; color: var(--text-2);
  border: 1px solid var(--border); border-radius: 4px; padding: 1px 6px;
}
pre {
  margin: 0; padding: 14px 16px; overflow-x: auto;
  background: var(--surface-2); border: 1px solid var(--border);
  border-radius: 6px; tab-size: 8;
  font: 13px/1.5 ui-monospace, SFMono-Regular, Menlo, monospace;
}
.comment { color: var(--syn-comment); font-style: italic; }
.string { color: var(--syn-string); }
.const { color: var(--syn-const); }
.keyword { color: var(--syn-keyword); font-weight: 600; }
.type { color: var(--syn-type); }
.func { color: var(--syn-func); }
.number { color: var(--syn-number); }
@media (max-width: 560px) {
  .chart { column-gap: 8px; }
  .label { font-size: 12px; }
  .track { padding-right: 72px; }
}
'''


def render(doc):
    results = doc.get('results', [])
    meta_html = ''.join(
        f'<div><dt>{html.escape(k)}</dt><dd>{html.escape(str(v))}</dd></div>'
        for k, v in meta(doc))

    return f'''\
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>glReadPixels benchmark</title>
<style>
{CSS}</style>
</head>
<body>
<main>
<h1>glReadPixels fps</h1>
<dl class="meta">{meta_html}</dl>
<div class="chart">
{render_chart(results)}
</div>
<div class="snippets">
{render_snippets(results)}
</div>
</main>
</body>
</html>
'''


def main():
    ap = argparse.ArgumentParser(
        description='Make an html report from the benchmark json.')
    ap.add_argument('input', nargs='?', type=argparse.FileType('rb'),
                    default=sys.stdin.buffer,
                    help='benchmark json (default: stdin)')
    ap.add_argument('-o', '--output', type=argparse.FileType('w'),
                    default=sys.stdout, help='html file (default: stdout)')
    args = ap.parse_args()

    try:
        doc = json.load(args.input)
    except ValueError as e:
        print(f'html-report: could not parse json: {e}', file=sys.stderr)
        return 1

    for r in doc.get('results', []):
        if r.get('method') not in METHODS:
            print(f"html-report: no snippet for method '{r.get('method')}'",
                  file=sys.stderr)

    args.output.write(render(doc))
    return 0


if __name__ == '__main__':
    sys.exit(main())
