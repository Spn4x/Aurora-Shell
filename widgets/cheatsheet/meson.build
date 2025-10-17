# widgets/cheatsheet/meson.build

# Define where the compiled .so file should be installed
# This will be /usr/local/lib/aurora-shell/widgets/
widget_install_dir = get_option('libdir') / 'aurora-shell' / 'widgets'

# Define where the data file (keys.conf) should be installed
# This will be /usr/local/share/aurora-shell/
data_install_dir = get_option('datadir') / 'aurora-shell'

# The dependencies needed to compile cheatsheet.c
dependencies = [
  dependency('gtk4'),
    dependency('json-glib-1.0')
]

# 1. Compile cheatsheet.c into a shared library plugin
shared_library('cheatsheet', 'cheatsheet.c',
  dependencies: dependencies,
  install: true,
  install_dir: widget_install_dir,
  name_prefix: '', # Creates cheatsheet.so instead of libcheatsheet.so
)

# 2. Install the configuration file for this widget
install_data('keys.conf',
  install_dir: data_install_dir,
  install_mode: 'rw-r--r--'
)