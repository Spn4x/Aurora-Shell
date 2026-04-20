# widgets/topbar/meson.build

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : false)

# List of sources for the topbar library.
sources = [
  'topbar.c',
  'modules/workspaces.c',
  'modules/sysinfo.c',
  'modules/audio.c',
  'modules/audio_media.c',
  'modules/zen.c',
  'modules/clock.c',
  'modules/popover_anim.c',
  'modules/systray.c', 
]

dependencies = [
  dependency('gtk4'),
  dependency('gtk4-layer-shell-0'),
  dependency('json-glib-1.0'),
  dependency('libadwaita-1'),
  m_dep 
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