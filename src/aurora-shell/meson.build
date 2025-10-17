# This is the build script for the main aurora-shell executable.
# It is called from the root meson.build via subdir().

aurora_shell_sources = [
  'main.c',  # <-- THE FIX: Path is relative to this file.
]

aurora_shell_deps = [
  dependency('gtk4'),
  dependency('libadwaita-1'),
  dependency('gtk4-layer-shell-0'),
  dependency('dl'),
  dependency('json-glib-1.0'),
]

executable('aurora-shell',
  aurora_shell_sources,
  dependencies: aurora_shell_deps,
  install: true,
  c_args: '-rdynamic', # Important for dlopen
)