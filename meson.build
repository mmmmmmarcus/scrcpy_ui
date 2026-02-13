project('scrcpy', 'c',
        version: '3.3.4',
        meson_version: '>= 0.49',
        default_options: [
            'c_std=c11',
            'warning_level=2',
            'b_ndebug=if-release',
        ])

add_project_arguments('-Wmissing-prototypes', language: 'c')

if host_machine.system() == 'darwin'
    add_languages('objc')
endif

if get_option('compile_app')
    subdir('app')
endif

if get_option('compile_server')
    subdir('server')
endif

if host_machine.system() == 'darwin' and get_option('compile_app')
    custom_target('scrcpyui-bundle',
                  input: [scrcpy_app_target],
                  output: 'ScrcpyUI.app.stamp',
                  command: [find_program('release/package_macos_app.sh'),
                            meson.current_build_dir(),
                            meson.current_build_dir(),
                            '@OUTPUT@'],
                  build_by_default: true,
                  build_always_stale: true,
                  install: false)
endif
