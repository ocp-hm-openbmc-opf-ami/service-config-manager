project(
    'phosphor-srvcfg-manager',
    'cpp',
    default_options: ['warning_level=3', 'werror=true', 'cpp_std=c++23'],
    license: 'Apache-2.0',
    version: '1.0',
    meson_version: '>=1.1.1',
)

# Wno-psabi reduces the number of "Note:" messages when cross-compiling some STL
# stuff for ARM. See https://stackoverflow.com/questions/48149323/strange-gcc-warning-when-compiling-qt-project
# Basically, gcc 6 and gcc 7 are not ABI compatible, but since the whole OpenBMC
# project uses the same compiler, we can safely ignmore these info notes.
add_project_arguments('-Wno-psabi', language: 'cpp')
tests_opt = get_option('tests')
if tests_opt.enabled()
  add_project_arguments('-DTEST_ENABLED', language: 'cpp')
  add_project_arguments('-Wno-template-body', language: 'cpp')
  add_project_arguments('-Wno-overloaded-virtual', language: 'cpp')
  sdbusplus_dep = subproject('sdbusplus').get_variable('sdbusplus_dep')
  meson.override_dependency('sdbusplus', sdbusplus_dep)
  phosphor_dbus_interfaces_dep = subproject('phosphor-dbus-interfaces-ami').get_variable('phosphor_dbus_interfaces_dep')
  phosphor_logging_dep = subproject('phosphor-logging').get_variable('phosphor_logging_dep')
else
   sdbusplus_dep = dependency('sdbusplus')
   phosphor_dbus_interfaces_dep = dependency('phosphor-dbus-interfaces')
   phosphor_logging_dep = dependency('phosphor-logging')
endif

boost_args = [
    '-DBOOST_ALL_NO_LIB',
    '-DBOOST_ASIO_DISABLE_THREADS',
    '-DBOOST_COROUTINES_NO_DEPRECATION_WARNING',
    '-DBOOST_ERROR_CODE_HEADER_ONLY',
    '-DBOOST_NO_RTTI',
    '-DBOOST_NO_TYPEID',
    '-DBOOST_SYSTEM_NO_DEPRECATED',
]

deps = [
    dependency('boost', modules: ['coroutine', 'context']),
    sdbusplus_dep,
    phosphor_dbus_interfaces_dep ,
    phosphor_logging_dep,
    dependency('libsystemd'),
]

if (get_option('usb-code-update').allowed())
    add_project_arguments('-DUSB_CODE_UPDATE', language: 'cpp')
endif

if (get_option('persist-settings-to-file').allowed())
    add_project_arguments('-DPERSIST_SETTINGS', language: 'cpp')
    deps += [dependency('nlohmann_json', include_type: 'system')]
endif

executable(
    'phosphor-srvcfg-manager',
    'src/main.cpp',
    'src/srvcfg_manager.cpp',
    'src/utils.cpp',
    'src/dropbear_srvcfgmgr.cpp',
    implicit_include_directories: false,
    include_directories: ['inc'],
    dependencies: deps,
    cpp_args: boost_args,
    install: true,
    install_dir: get_option('bindir'),
)

systemd = dependency('systemd')
systemd_system_unit_dir = systemd.get_variable(
    'systemd_system_unit_dir',
    pkgconfig_define: ['prefix', get_option('prefix')],
)

tests_opt = get_option('tests')
if tests_opt.enabled()
    subdir('tests')
endif

fs = import('fs')
fs.copyfile(
    'srvcfg-manager.service',
    install: true,
    install_dir: systemd_system_unit_dir,
)
