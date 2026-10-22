gtest_dep = dependency('gtest', main: false, required: false)
gmock_dep = dependency('gmock', required: false)

if not gtest_dep.found() or not gmock_dep.found()
    cmake = import('cmake')
    opt_var = cmake.subproject_options()
    opt_var.add_cmake_defines({'CMAKE_CXX_FLAGS': '-Wno-pedantic'})
    opt_var.set_override_option('warning_level', '3')
    opt_var.set_override_option('werror', 'true')

    gtest_proj = cmake.subproject('gtest', options: opt_var, required: false)
    if gtest_proj.found()
        gtest_dep = declare_dependency(
            dependencies: [
                dependency('threads'),
                gtest_proj.dependency('gtest'),
            ],
        )
        gmock_dep = gtest_proj.dependency('gmock')
    else
        gtest_proj = subproject('gtest')
        gtest_dep = gtest_proj.get_variable('gtest_dep')
        gmock_dep = gtest_proj.get_variable('gmock_dep')
    endif
endif

test_deps = [
    gtest_dep,
    gmock_dep,
    dependency('boost'),
    dependency('phosphor-logging'),
    dependency('sdbusplus'),
    dependency('systemd'),
]

# Generate a test-specific copy of the dropbear source with writable temp paths
# so the unit tests do not require access to /etc or /etc/systemd at runtime.
dropbear_test_src = configure_file(
    input: '../src/dropbear_srvcfgmgr.cpp',
    output: 'dropbear_srvcfgmgr_test.cpp',
    command: [
        'sed',
        '-e', 's|/etc/default/dropbear|/tmp/test-dropbear-default|g',
        '-e', 's|/etc/systemd/system/dropbear.socket.d/|/tmp/test-dropbear-dropin/|g',
        '@INPUT@',
    ],
    capture: true,
)

# srvcfg_manager.cpp is excluded: it references externs defined in main.cpp
production_sources = [
    '../src/utils.cpp',
    dropbear_test_src,
]

test_cpp_args = boost_args + ['-DTEST_ENABLED']

# Single binary with all tests so production sources are linked once and the
# coverage report reflects every module via per-test gtest filtering.
test_exe = executable(
    'test_srvcfg_manager',
    [
        'test_main.cpp',
        'test_manager.cpp',
        'test_manager_serialize.cpp',
        'test_password.cpp',
    ] + production_sources,
    include_directories: include_directories('.', '..', '../inc'),
    dependencies: test_deps,
    cpp_args: test_cpp_args,
)

test('unit_test_srvcfg_manager', test_exe)
