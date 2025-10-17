# widgets/mpris_player/meson.build

# Define all necessary dependencies for the widget
gtk_dep = dependency('gtk4')
adwaita_dep = dependency('libadwaita-1')
json_glib_dep = dependency('json-glib-1.0')
libsoup_dep = dependency('libsoup-3.0')

# Get the C compiler to find the math library (libm)
cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required : true)

# List of all C source files for this widget
mpris_sources = [
  'src/main.c',
  'src/mpris.c',
  'src/lyrics.c',
  'src/utils.c',
]

# This is the corrected build instruction for the shared library plugin.
shared_library('mpris_player', mpris_sources,
  c_args : ['-pthread', '-D_GNU_SOURCE'],
  dependencies : [
    gtk_dep,
    adwaita_dep,
    json_glib_dep,
    libsoup_dep,
    math_dep
  ],
  install : true,
  install_dir: get_option('libdir') / 'aurora-shell' / 'widgets',
  name_prefix: '' # <-- This is the crucial fix
)