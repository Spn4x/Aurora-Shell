gtk_dep = dependency('gtk4')
adwaita_dep = dependency('libadwaita-1')
json_glib_dep = dependency('json-glib-1.0')

cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required : true)

mpris_sources = [
  'src/main.c',
  'src/mpris.c',
  'src/lyrics.c',
  'src/utils.c',
]

shared_library('mpris_player', mpris_sources,
  c_args : ['-pthread', '-D_GNU_SOURCE'],
  dependencies : [
    gtk_dep,
    adwaita_dep,
    json_glib_dep,
    math_dep
  ],
  install : true,
  install_dir: get_option('libdir') / 'aurora-shell' / 'widgets',
  name_prefix: ''
)