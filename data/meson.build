desktop_file = configure_file(
  input: 'dev.corral.Corral.desktop.in',
  output: 'dev.corral.Corral.desktop',
  copy: true,
  install: true,
  install_dir: get_option('datadir') / 'applications',
)

gschema_file = configure_file(
  input: 'dev.corral.Corral.gschema.xml',
  output: 'dev.corral.Corral.gschema.xml',
  copy: true,
  install: true,
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas',
)

install_data(
  'dev.corral.Corral.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo',
)

install_subdir(
  meson.project_source_root () / 'third_party' / 'ghostty' / 'src' / 'shell-integration',
  install_dir: get_option('datadir') / 'ghostty',
  exclude_files: ['README.md'],
)

icon_dir = get_option('datadir') / 'icons' / 'hicolor'
install_data(
  'icons/hicolor/scalable/apps/dev.corral.Corral.svg',
  install_dir: icon_dir / 'scalable' / 'apps',
)
install_data(
  'icons/hicolor/symbolic/apps/dev.corral.Corral-symbolic.svg',
  install_dir: icon_dir / 'symbolic' / 'apps',
)

glib_compile_schemas = find_program('glib-compile-schemas')
custom_target(
  'gschemas.compiled',
  input: gschema_file,
  output: 'gschemas.compiled',
  command: [
    glib_compile_schemas,
    meson.current_build_dir (),
  ],
  build_by_default: true,
)
