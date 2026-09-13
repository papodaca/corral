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
