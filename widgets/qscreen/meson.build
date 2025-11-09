# aurora-shell/widgets/qscreen/meson.build

# Define the directory where the final .so plugin will be installed.
widget_install_dir = get_option('libdir') / 'aurora-shell' / 'widgets'

# Get the C compiler so we can find the math library.
c_compiler = meson.get_compiler('c')
m_dep = c_compiler.find_library('m', required: true) 

# List the libraries our plugin needs to link against.
dependencies = [
  dependency('gtk4'),
  dependency('json-glib-1.0'),
  m_dep  
]

# List all the .c source files that make up this plugin.
sources = [
  'qscreen.c',
  'utils.c',
  'features/ocr.c'
]

# The command to build the shared library (the .so plugin).
shared_library('qscreen', sources,
  dependencies: dependencies,
  # This tells the compiler where to look for headers like "qscreen.h".
  include_directories: '.',
  # Install the final .so file.
  install: true,
  install_dir: widget_install_dir,
  # This is crucial: it creates "qscreen.so" instead of "libqscreen.so".
  name_prefix: '',
)