project('wlcursorfix', 'c')
glib = dependency('glib-2.0')
wayland = dependency('wayland-client', version: '>= 1.21.0')
wayland_cursor = dependency('wayland-cursor', version: '>= 1.21.0')
wlprotocols = dependency('wayland-protocols', version: '>= 1.32')
wlproto_dir = wlprotocols.get_variable('pkgdatadir')
wayland_scanner = find_program('wayland-scanner')
cursor_shape_input = files(join_paths(wlproto_dir, 'staging/cursor-shape/cursor-shape-v1.xml'))
cursor_shape_gen_headers = custom_target('cursor-shape-v1 client header', input: cursor_shape_input, output: 'cursor-shape-v1-client-protocol.h', command: [ wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@' ])
cursor_shape_sources = custom_target('cursor-shape-v1 source', input: cursor_shape_input, output: 'cursor-shape-v1-protocol.c', command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
tablet_input = files(join_paths(wlproto_dir, 'unstable/tablet/tablet-unstable-v2.xml'))
tablet_gen_headers = custom_target('tablet-unstable-v2 client header', input: tablet_input, output: 'tablet-unstable-v2-client-protocol.h', command: [ wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@' ])
tablet_sources = custom_target('tablet-unstable-v2 source', input: tablet_input, output: 'tablet-unstable-v2-protocol.c', command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])

library('gtkcursorshape',
  sources: [
    'wlcursorfix.c',
    cursor_shape_sources,
    cursor_shape_gen_headers,
    tablet_sources,
    tablet_gen_headers,
  ],
  dependencies: [
    glib,
    wayland,
    wayland_cursor,
    wlprotocols,
  ],
  link_args: '-Wl,--unresolved-symbols=ignore-all',
)

