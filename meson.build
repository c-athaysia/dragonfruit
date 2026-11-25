project('dragonfruit', ['c'],
  version : '0.1.2',
)

subdir('protocols')

is_nixos = false
os_release = run_command('cat', '/etc/os-release', check: false)
if os_release.returncode() == 0
    if os_release.stdout().contains('ID=nixos')
        is_nixos = true
    endif
endif

prefix = get_option('prefix')
sysconfdir = get_option('sysconfdir')

# If sysconfdir starts with prefix, strip the prefix.
if sysconfdir.startswith(prefix) and not is_nixos
  sysconfdir = sysconfdir.substring(prefix.length())
  # Ensure sysconfdir is an absolute path.
  if not sysconfdir.startswith('/')
    sysconfdir = '/' + sysconfdir
  endif
endif

cc = meson.get_compiler('c')
libm = cc.find_library('m')
libdrm = dependency('libdrm')
xcb = dependency('xcb', required : get_option('xwayland'))
xlibs = dependency('xcb-icccm', required : get_option('xwayland'))
xcb_randr = dependency('xcb-randr', required : get_option('xwayland'))
wayland_server_dep = dependency('wayland-server',version: '>=1.23.1')
wlroots_dep = dependency('wlroots-0.20',version: '>=0.20.0')
xkbcommon_dep = dependency('xkbcommon')
libinput_dep = dependency('libinput',version: '>=1.27.1')
libwayland_client_dep = dependency('wayland-client')
pcre2_dep = dependency('libpcre2-8')
libscenefx_dep = dependency('scenefx-0.5',version: '>=0.5.0')
pixman_dep = dependency('pixman-1')
cjson_dep = dependency('libcjson')
pangocairo_dep = dependency('pangocairo')
# needed directly - draw/shader_bg.c does raw gles2 calls into the fbo scenefx gives back
gles2_dep = dependency('glesv2')
egl_dep = dependency('egl')
gbm_dep = dependency('gbm')

# stock background shaders - installed so canvas_shader=dots works without a path
# $DRAGONFRUIT_SHADER_DIR overrides for a build tree
shader_dir = join_paths(get_option('prefix'), get_option('datadir'), 'dragonfruit', 'shaders')
install_data(
  ['assets/shaders/grain.glsl', 'assets/shaders/dots.glsl', 'assets/shaders/plus.glsl'],
  install_dir : shader_dir,
)
add_project_arguments('-DSHADER_DIR="' + shader_dir + '"', language : 'c')

# version info
git = find_program('git', required : false)
is_git_repo = false

# check if current directory is a git repo
if git.found()
  git_status = run_command(git, 'rev-parse', '--is-inside-work-tree', check : false)
  if git_status.returncode() == 0 and git_status.stdout().strip() == 'true'
    is_git_repo = true
  endif
endif

if is_git_repo
  # if current directory is a git repo, get commit hash and latest tag
  commit_hash = run_command(git, 'rev-parse', '--short', 'HEAD', check : false).stdout().strip()
  latest_tag = meson.project_version()
  version_with_hash = '@0@(@1@)'.format(latest_tag, commit_hash)
else
  # if not a git repo, use project version and "release" string
  commit_hash = 'release'
  latest_tag = meson.project_version()
  version_with_hash = '@0@(@1@)'.format(latest_tag, commit_hash)
endif

# define compilation args
c_args = [
  '-g',
  '-O2',
  '-U_FORTIFY_SOURCE',
  '-Wno-unused-function',
  '-Wno-unused-parameter',
  '-Wno-unused-result',
  '-DWLR_USE_UNSTABLE',
  '-D_POSIX_C_SOURCE=200809L',
  '-DVERSION="@0@"'.format(version_with_hash),
  '-DSYSCONFDIR="@0@"'.format('/etc'),
]

if libscenefx_dep.found()
  c_args += '-DMANGO_SCENEFX'
endif

# add debug args only when debug option is enabled
if get_option('asan')
  c_args += [
    '-fsanitize=address',
    '-fno-omit-frame-pointer',
    '-fno-optimize-sibling-calls'
  ]
endif

if xcb.found() and xlibs.found()
  c_args += '-DXWAYLAND'
endif

# define link args
link_args = []
if get_option('asan')
  link_args += '-fsanitize=address'
endif

# grabs every .c under src/ so new files just work at meson setup
# mmsg/mmsg.c is outside src/ on purpose - its own binary with its own main
# directory keeps the mango name and only the binary is dfmsg so rebases dont fight
find = find_program('find')
dragonfruit_sources = files(run_command(find, 'src', '-name', '*.c', check: true).stdout().split())

dragonfruit_inc = include_directories('.', 'include')

executable('dragonfruit',
  dragonfruit_sources,

  wayland_sources,
  include_directories : dragonfruit_inc,
  dependencies : [
    libm,
    libdrm,
    xcb,
    xlibs,
    xcb_randr,
    libscenefx_dep,
    wayland_server_dep,
    wlroots_dep,
    xkbcommon_dep,
    libinput_dep,
    libwayland_client_dep,
    pcre2_dep,
    pixman_dep,
    cjson_dep,
    pangocairo_dep,
    gles2_dep,
    egl_dep,
    gbm_dep,
    dependency('threads'),
  ],
  install : true,
  c_args : c_args,
  link_args : link_args,
)

# build dfmsg

wayland_scanner_client_header = generator(
  wayland_scanner,
  output: '@BASENAME@-protocol.h',
  arguments: ['client-header', '@INPUT@', '@OUTPUT@']
)

wayland_scanner_private_code = generator(
  wayland_scanner,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@']
)

# generator for the dfmsg target
executable('dfmsg',
  'mmsg/mmsg.c',
  'src/config/error_store.c',
  'src/common/log.c',
  'src/common/util.c',

  wayland_sources,

  include_directories : dragonfruit_inc,

  dependencies: [
    libwayland_client_dep,
    wlroots_dep,
    pcre2_dep,
  ],

  install: true,

  c_args: [
    '-g',
    '-O2',
    '-U_FORTIFY_SOURCE',
    '-Wno-unused-function',
    '-Wno-unused-parameter',
    '-DWLR_USE_UNSTABLE',
    '-D_POSIX_C_SOURCE=200809L',
  ],
)

# build mangonag (layer-shell nag/notification client)

mangonag_proto_h = custom_target('mangonag-layer-shell-client-proto',
  input : 'protocols/wlr-layer-shell-unstable-v1.xml',
  output : 'wlr-layer-shell-unstable-v1-client-protocol.h',
  command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

mangonag_proto_c = custom_target('mangonag-layer-shell-private-code',
  input : 'protocols/wlr-layer-shell-unstable-v1.xml',
  output : 'wlr-layer-shell-unstable-v1-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

mangonag_xdg_proto_h = custom_target('mangonag-xdg-shell-client-proto',
  input : wl_protocol_dir + '/stable/xdg-shell/xdg-shell.xml',
  output : 'xdg-shell-client-protocol.h',
  command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

mangonag_xdg_proto_c = custom_target('mangonag-xdg-shell-private-code',
  input : wl_protocol_dir + '/stable/xdg-shell/xdg-shell.xml',
  output : 'xdg-shell-protocol.c',
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

executable('mangonag',
  'mangonag/mangonag.c',
  mangonag_proto_h,
  mangonag_proto_c,
  mangonag_xdg_proto_h,
  mangonag_xdg_proto_c,

  include_directories : dragonfruit_inc,

  dependencies: [
    libwayland_client_dep,
    pangocairo_dep,
  ],

  install: true,

  c_args: [
    '-g',
    '-O2',
    '-U_FORTIFY_SOURCE',
    '-Wno-unused-function',
    '-Wno-unused-parameter',
    '-DWLR_USE_UNSTABLE',
    '-D_POSIX_C_SOURCE=200809L',
  ],
)

mandir = get_option('mandir')
desktop_install_dir = join_paths(prefix, 'share/wayland-sessions')
portal_install_dir = join_paths(prefix, 'share/xdg-desktop-portal')
install_data('assets/dragonfruit.desktop', install_dir : desktop_install_dir)
install_data('assets/dragonfruit-portals.conf', install_dir : portal_install_dir)
install_data('assets/config.conf', install_dir : join_paths(sysconfdir, 'dragonfruit'))

# ships dragonfruit-session.target so systemd user services wanting graphical-session.target start with the session
# mango turns the target on itself - see set_activation_env in src/main.c
install_data('assets/dragonfruit-session.target',
  install_dir : join_paths(prefix, 'lib/systemd/user'))

# man page generated at build time so the version matches
dfmsg_man_conf = configuration_data()
dfmsg_man_conf.set('VERSION', meson.project_version())
install_data(
  configure_file(
    input : 'mmsg/dfmsg.1.in',
    output : 'dfmsg.1',
    configuration : dfmsg_man_conf,
  ),
  install_dir : join_paths(mandir, 'man1'),
)

# generate the mangonag man page at build time so the version always
# matches the project version
mangonag_man_conf = configuration_data()
mangonag_man_conf.set('VERSION', meson.project_version())
install_data(
  configure_file(
    input : 'mangonag/mangonag.1.in',
    output : 'mangonag.1',
    configuration : mangonag_man_conf,
  ),
  install_dir : join_paths(mandir, 'man1'),
)
