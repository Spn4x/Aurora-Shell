# aurora-shell/meson.build

project('aurora-shell', 'c',
  version: '0.1.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ]
)

# Process the build files in our subdirectories
subdir('src/aurora-shell')
subdir('widgets')
subdir('daemons')

# Install the templates directory
install_subdir(
  'templates',
  install_dir: get_option('datadir') / 'aurora-shell',
  install_mode: 'rw-r--r--'
)

# Install the default config.json
install_data(
  'config.json',
  install_dir: get_option('datadir') / 'aurora-shell',
  install_mode: 'rw-r--r--'
)

# Install the themer helper scripts to the binary directory (e.g., /usr/local/bin)
install_data(
  ['themer/wallpaper.sh', 'themer/lighten_color.py'],
  install_dir: get_option('bindir'),
  install_mode: 'rwxr-xr-x'  # Make them executable
)

# Install the MPRIS resetter daemon script to the binary directory
install_data(
  'daemons/mpris-resetter/mpris-resetter.sh',
  install_dir: get_option('bindir'),
  rename: 'aurora-mpris-resetter', # Rename it for a clean command name
  install_mode: 'rwxr-xr-x'         # Make it executable
)