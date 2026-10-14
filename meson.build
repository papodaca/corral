project(
  'corral',
  ['vala', 'c'],
  version: '0.1.0',
  meson_version: '>= 1.0.0',
  default_options: [
    'warning_level=1',
    'c_std=c11',
  ],
)

gnome = import('gnome')

gtk_dep = dependency('gtk4', version: '>= 4.14')
adw_dep = dependency('libadwaita-1', version: '>= 1.5')
gio_dep = dependency('gio-2.0')

# Ghostty is gitignored. Clone and patch before data/ and src/ look at it.
run_command(
  find_program('scripts/sync-ghostty.sh'),
  check: true,
)

app_version = get_option('package_version')
if app_version == ''
  app_version = run_command(
    find_program('packaging/version.sh'),
    check: true,
  ).stdout().strip()
endif

subdir('data')
subdir('src')
subdir('tests')

dev_env = environment()
dev_env.set('GSETTINGS_SCHEMA_DIR', meson.project_build_root() / 'data')
dev_env.set('GHOSTTY_RESOURCES_DIR', '/usr/share/ghostty')

meson.add_devenv(dev_env)

run_target(
  'run',
  command: [find_program('scripts/dev-run.sh'), corral_exe],
  depends: [gschemas],
  env: dev_env,
)

gnome.post_install(
  glib_compile_schemas: true,
  gtk_update_icon_cache: true,
  update_desktop_database: find_program('update-desktop-database', required: false).found(),
)
