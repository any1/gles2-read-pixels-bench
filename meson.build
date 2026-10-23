project('gles2-read-pixels-bench', 'c',
  version: '1.0.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ])

cc = meson.get_compiler('c')

add_project_arguments('-D_GNU_SOURCE', language: 'c')

deps = [
  dependency('glesv2'),
  dependency('epoxy'),
  dependency('gbm'),
  dependency('json-c'),
  dependency('libturbojpeg', version: '>= 2.0'),
  cc.find_library('m', required: false),
]

common = static_library('common',
  files(
    'src/gl_context.c',
    'src/render.c',
    'src/load.c',
    'src/mjpeg.c',
    'src/report.c',
    'src/util.c',
  ),
  dependencies: deps)

executable('gles2-read-pixels-bench',
  files(
    'src/main.c',
    'src/capture.c',
    'src/tests.c',
  ),
  link_with: common,
  dependencies: deps,
  install: true)

executable('gles2-full-frame-bench',
  files('src/full_frame.c'),
  link_with: common,
  dependencies: deps,
  install: true)
