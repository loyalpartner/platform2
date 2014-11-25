{
  'target_defaults': {
    'variables': {
      'deps': [
        'dbus-1',
        'openssl',
        'libchrome-<(libbase_ver)',
        'libchromeos-<(libbase_ver)',
      ],
    },
  },
  'targets': [
    {
      'target_name': 'privetd_common',
      'type': 'static_library',
      'sources': [
        'constants.cc',
        'cloud_delegate.cc',
        'daemon_state.cc',
        'device_delegate.cc',
        'privet_handler.cc',
        'security_delegate.cc',
        'wifi_bootstrap_manager.cc',
      ],
      'actions': [
        {
          # Import D-Bus bindings from buffet.
          'action_name': 'generate-buffet-proxies',
          'variables': {
            'dbus_service_config': '../buffet/dbus_bindings/dbus-service-config.json',
            'proxy_output_file': 'include/buffet/dbus-proxies.h'
          },
          'sources': [
            '../buffet/dbus_bindings/org.chromium.Buffet.Command.xml',
            '../buffet/dbus_bindings/org.chromium.Buffet.Manager.xml',
          ],
          'includes': ['../common-mk/generate-dbus-proxies.gypi'],
        },
        {
          # Import D-Bus bindings from peerd.
          'action_name': 'generate-peerd-proxies',
          'variables': {
            'dbus_service_config': '../peerd/dbus_bindings/dbus-service-config.json',
            'proxy_output_file': 'include/peerd/dbus-proxies.h'
          },
          'sources': [
            '../peerd/dbus_bindings/org.chromium.peerd.Manager.xml',
            '../peerd/dbus_bindings/org.chromium.peerd.Peer.xml',
            '../peerd/dbus_bindings/org.chromium.peerd.Service.xml',
          ],
          'includes': ['../common-mk/generate-dbus-proxies.gypi'],
        },
      ],
      'includes': ['../common-mk/deps.gypi'],
    },
    {
      'target_name': 'privetd',
      'type': 'executable',
      'dependencies': [
        'privetd_common',
      ],
      'variables': {
        'exported_deps': [
          'libwebserv-<(libbase_ver)',
        ],
        'deps': ['<@(exported_deps)'],
      },
      'sources': [
        'main.cc',
      ],
    },
  ],
  'conditions': [
    ['USE_test == 1', {
      'targets': [
        {
          'target_name': 'privetd_testrunner',
          'type': 'executable',
          'dependencies': [
            'privetd_common',
          ],
          'variables': {
            'deps': [
              'libchrome-test-<(libbase_ver)',
            ],
          },
          'includes': ['../common-mk/common_test.gypi'],
          'sources': [
            'privetd_testrunner.cc',
            'privet_handler_unittest.cc',
            'security_delegate_unittest.cc',
          ],
        },
      ],
    }],
  ],
}
