# Build instructions for the cachy-selector standalone utility

cachy_selector_sources = [
  'cachy-selector.c',
]

# List the libraries it needs. It's a full GTK app.
cachy_selector_deps = [
  dependency('gtk4'),
  dependency('gtk4-layer-shell-0'),
  dependency('json-glib-1.0')
]

# Tell Meson to build an EXECUTABLE, not a shared library
executable('cachy-selector',
  cachy_selector_sources,
  dependencies: cachy_selector_deps,
  install: true, # This installs it to /usr/local/bin by default
)