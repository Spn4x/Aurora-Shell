cc = meson.get_compiler('c')

# Dependencies
gtk4_dep = dependency('gtk4')
adwaita_dep = dependency('libadwaita-1') 
layer_shell_dep = dependency('gtk4-layer-shell-0')
m_dep = cc.find_library('m', required : true)
json_dep = dependency('json-glib-1.0')

# --- RUST INTEGRATION ---
cargo = find_program('cargo', required : true)

rust_lib = custom_target('rust_backend',
  output : 'libbackend_engine.a',
  command :[
    'sh', '-c',
    'cargo build --manifest-path @0@/src/backend/Cargo.toml --release && cp @0@/src/backend/target/release/libbackend_engine.a @1@'
    .format(meson.current_source_dir(), '@OUTPUT@')
  ],
  build_by_default : true
)

rust_dep = declare_dependency(
  sources : rust_lib,
  link_args : ['-lpthread', '-ldl']
)
# ------------------------

surfacedesk_sources =[
  'src/main_gtk.c',
  'src/drawer.c',
  'src/widget_template.c',
  'src/storage.c',
  'src/editor.c', 
  'src/wallpaper_chooser.c',
  'src/theme_manager.c',
  'scenes/registry.c',
  'scenes/clock/main.c',
  'scenes/weather/main.c',
  'scenes/system/main.c',
  'scenes/text/main.c', 
  'scenes/media/main.c',
]

shared_library('surfacedesk',
  surfacedesk_sources,
  dependencies :[gtk4_dep, adwaita_dep, layer_shell_dep, m_dep, json_dep, rust_dep],
  install : true,
  install_dir: get_option('libdir') / 'aurora-shell' / 'widgets',
  name_prefix: ''
)