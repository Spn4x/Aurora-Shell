cargo = find_program('cargo', required: true)

custom_target(
  'aurorawidgetd',
  build_by_default: true,
  input: ['Cargo.toml', 'src/main.rs'],
  output: 'aurorawidgetd',
  command:[
    'sh', '-c',
    '@0@ build --manifest-path @1@/Cargo.toml --release && cp @1@/target/release/aurorawidgetd @2@'
    .format(cargo.full_path(), meson.current_source_dir(), '@OUTPUT@')
  ],
  install: true,
  install_dir: get_option('bindir')
)