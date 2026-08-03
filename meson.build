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
  dependency('json-c'),
  dependency('libturbojpeg'),
  cc.find_library('m', required: false),
]

executable('gles2-read-pixels-bench',
  files(
    'src/main.c',
    'src/gl_context.c',
    'src/render.c',
    'src/capture.c',
    'src/mjpeg.c',
    'src/util.c',
  ),
  dependencies: deps,
  install: true)
