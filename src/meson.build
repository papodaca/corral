config_data = configuration_data ()
config_data.set ('VERSION', app_version)
config_data.set (
  'GHOSTTY_RESOURCES_DIR',
  get_option ('prefix') / get_option ('datadir') / 'ghostty',
)
config_vala = configure_file (
  input: 'config.vala.in',
  output: 'config.vala',
  configuration: config_data,
)

ghostty_src = meson.project_source_root () / 'third_party' / 'ghostty'
ghostty_prefix = meson.project_build_root () / 'libghostty-prefix'
build_libghostty = find_program (meson.project_source_root () / 'scripts' / 'build-libghostty.sh')
ghostty_patch = meson.project_source_root () / 'patches' / 'ghostty-gtk-embed.patch'

libghostty_built = custom_target (
  'libghostty',
  output: ['libghostty-built.h', 'libghostty.so'],
  command: [
    build_libghostty,
    ghostty_src,
    ghostty_prefix,
    '@OUTPUT0@',
    '@OUTPUT1@',
  ],
  depend_files: [
    ghostty_patch,
    meson.project_source_root () / 'scripts' / 'build-libghostty.sh',
    ghostty_src / 'include' / 'ghostty.h',
    ghostty_src / 'src' / 'apprt' / 'embedded.zig',
    ghostty_src / 'src' / 'renderer' / 'OpenGL.zig',
    ghostty_src / 'src' / 'build' / 'SharedDeps.zig',
  ],
  console: true,
  install: true,
  install_dir: [false, get_option ('libdir')],
)

ghostty_inc = include_directories ('../third_party/ghostty/include')

libghostty_dep = declare_dependency (
  include_directories: ghostty_inc,
  link_args: [
    '-L' + ghostty_prefix / 'lib',
    '-lghostty',
  ],
  sources: libghostty_built[0],
)

corral_resources = gnome.compile_resources (
  'corral-resources',
  meson.project_source_root () / 'data' / 'corral.gresource.xml',
  c_name: 'corral',
  source_dir: meson.project_source_root (),
)

settings_source = files ('settings.vala')

epoxy_lib = meson.get_compiler ('c').find_library ('epoxy')

app_sources = files (
  'main.vala',
  'application.vala',
  'window.vala',
  'settings.vala',
  'preferences.vala',
  'ghostty-widget.c',
)

executable (
  'corral',
  app_sources,
  config_vala,
  corral_resources,
  dependencies: [
    gtk_dep,
    adw_dep,
    gio_dep,
    epoxy_lib,
    libghostty_dep,
  ],
  include_directories: [ghostty_inc, include_directories ('.')],
  vala_args: [
    '--target-glib=2.78',
    '--vapidir', meson.current_source_dir (),
    '--pkg', 'ghostty-widget',
  ],
  install_rpath: '$ORIGIN/../' + get_option ('libdir'),
  build_rpath: ghostty_prefix / 'lib',
  install: true,
)

ghostty_spike = executable (
  'ghostty-spike',
  files ('ghostty-spike.c', 'ghostty-widget.c'),
  dependencies: [
    gtk_dep,
    epoxy_lib,
    libghostty_dep,
  ],
  include_directories: [ghostty_inc, include_directories ('.')],
  install_rpath: '$ORIGIN/../' + get_option ('libdir'),
  build_rpath: ghostty_prefix / 'lib',
  install: false,
)
