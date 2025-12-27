cargo = find_program('cargo', required: true)

rust_sources = files(
  'backend/Cargo.toml',
  'backend/src/lib.rs',
  'backend/src/netlib.rs'
)

# 1. Build Rust Backend
rust_backend = custom_target(
  'rust_cc_backend',
  input: rust_sources,
  output: 'libaurora_control_center_backend.a',
  command: [
    'sh', '-c',
    '@0@ build --manifest-path @1@/backend/Cargo.toml --release && cp @1@/backend/target/release/libaurora_control_center_backend.a @2@'
    .format(cargo.full_path(), meson.current_source_dir(), '@OUTPUT@')
  ],
  console: true,
  build_by_default: true
)

# 2. Dependency Object for Rust
# We MUST add 'dl', 'm' (math), and 'pthread' because Tokio/Rust needs them.
cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : true)
dl_dep = cc.find_library('dl', required : true)
thread_dep = dependency('threads')

cc_rust_dep = declare_dependency(
  link_args: meson.current_build_dir() / 'libaurora_control_center_backend.a',
  dependencies: [m_dep, dl_dep, thread_dep],
  sources: rust_backend
)

# 3. Add libnm dependency
libnm_dep = dependency('libnm')

control_center_sources = [
  'src/main.c',
  'src/audio_manager.c',
  'src/bluetooth_manager.c',
  'src/bluetooth_scanner.c',
  'src/brightness_manager.c',
  'src/network_manager.c',
  'src/system_monitor.c',
  'src/utils.c',
  'src/wifi_scanner.c',
  'src/qr.c', 
]

control_center_deps = [
  dependency('gtk4'),
  dependency('libadwaita-1'),
  dependency('gio-2.0'),
  dependency('gio-unix-2.0'),
  dependency('libqrencode'),
  cc_rust_dep,
  libnm_dep
]

shared_library('control-center',
  control_center_sources,
  dependencies: control_center_deps,
  name_prefix: '',
  install: true,
  install_dir: get_option('libdir') / 'aurora-shell' / 'widgets'
)