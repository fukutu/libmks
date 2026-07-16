libmks_headers_dir = get_option('includedir') / 'libmks-@0@'.format(api_version)

libmks_include_directories = [
  include_directories('.'),
  include_directories('..'),
]

pixman_cflags = run_command('pkg-config',
                             '--cflags-only-I',
                             'pixman-1',
                             check: true).stdout().strip().split()
pixman_include_directories = []
foreach cflag : pixman_cflags
  pixman_include_directories += include_directories(cflag.substring(2))
endforeach
pixman_headers_dep = declare_dependency(
  include_directories: pixman_include_directories,
)

libmks_sources = [
  'mks-audio-format.c',
  'mks-transport.c',
  'mks-chardev.c',
  'mks-chardev-pty.c',
  'mks-clipboard.c',
  'mks-clipboard-redirector.c',
  'mks-dbus-transport.c',
  'mks-init.c',
  'mks-device.c',
  'mks-display.c',
  'mks-keyboard.c',
  'mks-microphone.c',
  'mks-mouse.c',
  'mks-mapped-paintable.c',
  'mks-paintable.c',
  'mks-screen.c',
  'mks-screen-attributes.c',
  'mks-session.c',
  'mks-speaker.c',
  'mks-touchable.c',
]

libmks_headers = [
  'libmks.h',
  'mks-audio-format.h',
  'mks-transport.h',
  'mks-chardev.h',
  'mks-clipboard.h',
  'mks-clipboard-redirector.h',
  'mks-dbus-transport.h',
  'mks-device.h',
  'mks-display.h',
  'mks-init.h',
  'mks-keyboard.h',
  'mks-microphone.h',
  'mks-mouse.h',
  'mks-screen.h',
  'mks-screen-attributes.h',
  'mks-session.h',
  'mks-speaker.h',
  'mks-touchable.h',
  'mks-types.h',
  'mks-version-macros.h',
]

libmks_enum_headers = [
  'mks-clipboard.h',
  'mks-clipboard-redirector.h',
  'mks-mouse.h',
  'mks-screen.h',
  'mks-keyboard.h',
  'mks-touchable.h',
]

libmks_private_sources = [
  'mks-cairo-framebuffer.c',
  'mks-dmabuf-paintable.c',
  'mks-display-picture.c',
  'mks-dbus-chardev.c',
  'mks-dbus-clipboard.c',
  'mks-dbus-keyboard.c',
  'mks-dbus-microphone.c',
  'mks-dbus-mouse.c',
  'mks-dbus-screen.c',
  'mks-dbus-speaker.c',
  'mks-dbus-touchable.c',
  'mks-css.c',
  'mks-inhibitor.c',
  'mks-read-only-list-model.c',
  'mks-screen-resizer.c',
  'mks-trace.c',
  'mks-util.c',
]

libmks_enums = gnome.mkenums_simple('mks-enums',
        body_prefix: '#include "config.h"',
          decorator: '_MKS_EXTERN',
            sources: libmks_enum_headers,
     install_header: true,
        install_dir: libmks_headers_dir,
  identifier_prefix: 'Mks',
      symbol_prefix: 'mks',
)

# Setup mks-version.h for version checking
version_split = meson.project_version().split('.')
micro_version = version_split[2]
if version_split[2][0] in ['a', 'b', 'r']
  micro_version = '0'
endif
version_conf = configuration_data()
version_conf.set('VERSION', meson.project_version())
version_conf.set('MAJOR_VERSION', version_split[0])
version_conf.set('MINOR_VERSION', version_split[1])
version_conf.set('MICRO_VERSION', micro_version)
mks_version_h = configure_file(
          input: 'mks-version.h.in',
         output: 'mks-version.h',
  configuration: version_conf,
        install: true,
    install_dir: libmks_headers_dir,
)

libmks_qemu = gnome.gdbus_codegen('mks-qemu',
       autocleanup: 'all',
  interface_prefix: 'org.qemu.Display1.',
         namespace: 'MksQemu',
           sources: 'dbus-display1.xml',
    object_manager: true,
        extra_args: ['--extension-path=' + libdex_dep.get_variable('gdbus_codegen_extension')],
)
libmks_private_sources += libmks_qemu

libmks_marshal = gnome.genmarshal('mks-marshal',
            internal: true,
              prefix: '_mks_marshal',
             sources: ['mks-marshal.defs'],
  valist_marshallers: true,
)

libmks_gresources = gnome.compile_resources(
  'mks-resources',
  'mks.gresource.xml',
  c_name: 'mks',
  extra_args: [ '--manual-register', ],
)

libmks_generated_sources = [
  libmks_enums[0],
  libmks_marshal[0],
]

libmks_private_generated_sources = [
  libmks_gresources[0],
]

libmks_generated_headers = [
  mks_version_h,
  libmks_enums[1],
  libmks_marshal[1],
]

libmks_private_generated_headers = [
  libmks_gresources[1],
]

libmks_deps = [
  libdex_dep,
  libgio_dep,
  libgiounix_dep,
  dependency('gstreamer-1.0'),
  dependency('gstreamer-app-1.0'),
  libgtk_dep,
  libvte_dep,
  pixman_headers_dep,

  cc.find_library('m', required: false),
]
if sysprof_dep.found()
  libmks_deps += [sysprof_dep]
endif

libmks = shared_library('mks-' + api_version,
    libmks_generated_headers +
    libmks_generated_sources +
    libmks_private_sources +
    libmks_private_generated_sources +
    libmks_private_generated_headers +
    libmks_sources,
    include_directories: libmks_include_directories,
           dependencies: libmks_deps,
                install: true,
                version: '@0@.0.0'.format(api_version),
        darwin_versions: '@0@.0'.format(api_version),
  gnu_symbol_visibility: 'hidden',
)

libmks_dep = declare_dependency(
              sources: libmks_generated_headers,
            link_with: libmks,
         dependencies: libmks_deps,
  include_directories: [include_directories('.')],
)

install_headers(libmks_headers, subdir: 'libmks-@0@'.format(api_version))

pkg.generate(
  description: 'A shared library for Mouse, Keyboard, and Screen to QEMU',
    libraries: libmks,
         name: 'libmks',
     filebase: 'libmks-' + api_version,
      version: meson.project_version(),
      subdirs: 'libmks-@0@'.format(api_version),
     requires: ['libdex-1', 'gio-2.0', 'gio-unix-2.0', 'gstreamer-1.0', 'gstreamer-app-1.0', 'gtk4', 'vte-2.91-gtk4'],
  install_dir: join_paths(get_option('libdir'), 'pkgconfig')
)

if get_option('introspection').enabled()
  libmks_gir_extra_args = [
     '--c-include=libmks.h',
     '--quiet',
     '-DMKS_COMPILATION',
     '--library=' + libmks.full_path(),
   ]
  libmks_gir = gnome.generate_gir(libmks,
                sources: libmks_sources +
                         libmks_headers +
                         libmks_generated_sources +
                         libmks_generated_headers,
              nsversion: api_version,
              link_with: libmks,
              namespace: 'Mks',
          symbol_prefix: 'mks',
      identifier_prefix: 'Mks',
        export_packages: 'libmks-@0@'.format(api_version),
    include_directories: libmks_include_directories,
               includes: ['Dex-1', 'Gio-2.0', 'Gst-1.0', 'Gtk-4.0', 'Vte-3.91'],
                install: true,
             extra_args: libmks_gir_extra_args,
  )

  if get_option('vapi')
    libmks_vapi = gnome.generate_vapi('libmks-' + api_version,
                   sources: libmks_gir[0],
                  packages: [ 'libdex-1', 'gio-2.0', 'gstreamer-1.0', 'gtk4', 'vte-2.91-gtk4'],
                   install: true,
    )
  endif
endif
