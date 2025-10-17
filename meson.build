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

# Install the templates directory with world-readable permissions
install_subdir(
  'templates',
  install_dir: get_option('datadir') / 'aurora-shell',
  install_mode: 'rw-r--r--'
)

# Install the default config.json with world-readable permissions
install_data(
  'config.json',
  install_dir: get_option('datadir') / 'aurora-shell',
  install_mode: 'rw-r--r--'
)

# --- THIS IS THE CORRECT FIX ---
# Install the scripts to /usr/local/bin and make them executable ('rwxr-xr-x')
install_data(
  ['themer/wallpaper.sh', 'themer/lighten_color.py'],
  install_dir: get_option('bindir'),
  install_mode: 'rwxr-xr-x'
)
# -------------------------------