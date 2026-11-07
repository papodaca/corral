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
  output: ['libghostty-built.h', 'libghostty.a'],
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
    ghostty_src / 'src' / 'build' / 'GhosttyLib.zig',
    ghostty_src / 'build.zig',
  ],
  console: true,
  install: false,
)

ghostty_inc = include_directories ('../third_party/ghostty/include')

cc = meson.get_compiler ('c')
libm = cc.find_library ('m')
libdl = cc.find_library ('dl')
libutil = cc.find_library ('util')
libbz2 = cc.find_library ('bz2')
libxml2 = cc.find_library ('xml2')
libz = cc.find_library ('z')
libpng = cc.find_library ('png')
libegl = cc.find_library ('EGL')
thread_dep = dependency ('threads')

libghostty_dep = declare_dependency (
  include_directories: ghostty_inc,
  dependencies: [
    libm,
    libdl,
    libutil,
    libbz2,
    libxml2,
    libz,
    libpng,
    libegl,
    thread_dep,
  ],
)

# Meson 1.9 Vala inspects link_whole targets for .vapi files and crashes on
# CustomTargetIndex. Keep the archive off that path; --whole-archive still
# pulls every libghostty object into the final link.
libghostty_link_args = [
  '-Wl,--whole-archive',
  libghostty_built[1].full_path (),
  '-Wl,--no-whole-archive',
]

corral_resources = gnome.compile_resources (
  'corral-resources',
  meson.project_source_root () / 'data' / 'corral.gresource.xml',
  c_name: 'corral',
  source_dir: meson.project_source_root (),
)

settings_source = files ('settings.vala')
config_appearance_source = files ('config-appearance.c')
term_session_source = files ('term-session.c')

harfbuzz_dep = dependency ('harfbuzz')

# Vala passes --pkg for each dependency of a target, and HarfBuzz has no
# vapi, so the C widget builds on its own.
corral_widget = static_library (
  'corral-widget',
  files ('ghostty-widget.c', 'term-sprites.c') + term_session_source + config_appearance_source,
  dependencies: [
    gtk_dep,
    adw_dep,
    gio_dep,
    harfbuzz_dep,
    libghostty_vt_dep,
    libm,
  ],
  c_args: [
    '-DCORRAL_THEMES_DIR="@0@"'.format (get_option ('prefix') / get_option ('datadir') / 'corral' / 'ghostty-themes'),
  ],
)

app_sources = files (
  'main.vala',
  'application.vala',
  'window.vala',
  'settings.vala',
  'preferences.vala',
)

corral_exe = executable (
  'corral',
  app_sources,
  config_vala,
  corral_resources,
  dependencies: [
    gtk_dep,
    adw_dep,
    gio_dep,
  ],
  link_with: corral_widget,
  include_directories: include_directories ('.'),
  vala_args: [
    '--target-glib=2.78',
    '--vapidir', meson.current_source_dir (),
    '--pkg', 'ghostty-widget',
  ],
  install: true,
)

ghostty_spike = executable (
  'ghostty-spike',
  files ('ghostty-spike.c'),
  dependencies: [
    gtk_dep,
  ],
  link_with: corral_widget,
  include_directories: include_directories ('.'),
  install: false,
)
