gnome = import('gnome')
cc = meson.get_compiler('c') # <--- Get the compiler object

# Dependencies
gtk_dep = dependency('gtk4')
adw_dep = dependency('libadwaita-1')

# FIX: Use find_library instead of dependency for sensors
sensors_dep = cc.find_library('sensors', required: true) 
m_dep = cc.find_library('m', required : false)

# Compile Resources
thinkfan_resources = gnome.compile_resources('thinkfan-resources',
  'src/resources/thinkfan-c.gresource.xml',
  source_dir: 'src/resources'
)

# Source files
thinkfan_sources = files(
  'src/plugin.c',  
  'src/window.c',
  'src/backend.c'
)

# Shared Module (Plugin) configuration
shared_module('thinkfan', thinkfan_sources, thinkfan_resources,
  dependencies: [gtk_dep, adw_dep, sensors_dep, m_dep],
  install: true,
  install_dir: get_option('libdir') / 'aurora-shell/widgets',
  name_prefix: '' 
)