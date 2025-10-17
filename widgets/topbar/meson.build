# widgets/topbar/meson.build

# List of sources for the topbar library.
sources = [
  'topbar.c',
  'modules/workspaces.c',
  'modules/sysinfo.c',
  'modules/audio.c',
  'modules/zen.c',
]

dependencies = [
  dependency('gtk4'),
  dependency('gtk4-layer-shell-0'),
  dependency('json-glib-1.0')
]

# Build a 'shared_library' for the topbar plugin.
shared_library('topbar',
  sources,
  dependencies: dependencies,
  name_prefix: '',
  # This ensures the plugin gets installed correctly.
  install: true,
  install_dir: get_option('libdir') / 'aurora-shell' / 'widgets'
)