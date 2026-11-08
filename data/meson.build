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

if get_option('install_shell_integration')
  install_subdir(
    'shell-integration',
    install_dir: get_option('datadir') / 'ghostty',
    exclude_files: ['README.md'],
  )
endif

# Not under share/ghostty/themes: on Arch that directory belongs to ghostty.
install_subdir(
  'ghostty-themes',
  install_dir: get_option('datadir') / 'corral',
)

# The child's TERMINFO is the terminfo directory beside share/ghostty.
if get_option('install_terminfo')
  tic = find_program('tic')
  custom_target(
    'terminfo',
    input: 'terminfo' / 'ghostty.terminfo',
    output: 'terminfo',
    command: [tic, '-x', '-o', '@OUTPUT@', '@INPUT@'],
    build_by_default: true,
    install: true,
    install_dir: get_option('datadir'),
  )
endif

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
gschemas = custom_target(
  'gschemas.compiled',
  input: gschema_file,
  output: 'gschemas.compiled',
  command: [
    glib_compile_schemas,
    meson.current_build_dir (),
  ],
  build_by_default: true,
)
