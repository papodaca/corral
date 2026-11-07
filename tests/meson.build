libghostty_vt_version_test = executable(
  'libghostty-vt-version-test',
  files('libghostty-vt-version.c'),
  dependencies: [
    libghostty_vt_dep,
  ],
)

test('libghostty-vt-version', libghostty_vt_version_test)

config_appearance_test = executable(
  'config-appearance-test',
  files('config-appearance-test.c') + config_appearance_source,
  dependencies: [
    gio_dep,
    libghostty_vt_dep,
  ],
  include_directories: include_directories('../src'),
  c_args: [
    '-DG_LOG_DOMAIN="corral"',
    '-DBUNDLED_THEMES_DIR="@0@"'.format(meson.project_source_root() / 'data' / 'ghostty-themes'),
  ],
)

test('config-appearance', config_appearance_test)

herdr_test = executable(
  'herdr-test',
  files('herdr-test.vala'),
  dependencies: [
    gio_dep,
  ],
  vala_args: [
    '--target-glib=2.78',
  ],
)

test('herdr', herdr_test)

settings_test = executable(
  'settings-test',
  files('settings-test.vala') + settings_source,
  dependencies: [
    gio_dep,
  ],
  vala_args: [
    '--target-glib=2.78',
  ],
)

test('settings', settings_test,
  env: {
    'GSETTINGS_SCHEMA_DIR': meson.project_build_root () / 'data',
  },
)
