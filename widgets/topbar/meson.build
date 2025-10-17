

# List of sources for the library.
# - REMOVED: runner.c (no longer needed, the main shell is the runner)
# - REMOVED: All .h header files (they are included, not compiled directly)
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

# --- THE MAIN CHANGE ---
# We now build a 'shared_library' instead of an 'executable'.
shared_library('topbar',
  sources,
  dependencies: dependencies,
  # This prevents Meson from naming the file 'libtopbar.so'
  # and names it 'topbar.so' instead, for consistency with your other widgets.
  name_prefix: '',
  # This ensures the plugin gets installed to the correct directory
  # when you run 'meson install'. You can adjust the path as needed.
  install: true,
  install_dir: get_option('prefix') / get_option('libdir') / 'aurora-shell/widgets'
)