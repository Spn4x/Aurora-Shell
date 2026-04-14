cargo = find_program('cargo', required: true)

# Build the Rust daemon
custom_target(
  'auroralauncherd',
  build_by_default: true,
  input: ['Cargo.toml', 'src/main.rs'],
  output: 'auroralauncherd',
  command: [
    'sh', '-c',
    '@0@ build --manifest-path @1@/Cargo.toml --release && cp @1@/target/release/auroralauncherd @2@'
    .format(cargo.full_path(), meson.current_source_dir(), '@OUTPUT@')
  ],
  install: true,
  install_dir: get_option('bindir')
)