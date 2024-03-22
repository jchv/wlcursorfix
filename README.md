# wlcursorfix
This is a small `LD_PRELOAD` shim designed to add support for cursor-shape-v1 to software that does not yet support it. It is designed specifically to address the visual problems caused by applications that exhibit different behavior for scaling or cursor loading. When using a compositor that supports cursor-shape-v1, this shim should fix most applications to have uniform cursor behavior.

Note that this _does not_ improve memory usage or anything like that; it should have a (hopefully negligable) performance hit, but it is specifically designed to address the visual issues, _not_ the additional memory usage caused by having applications load their own cursors. This shim does not prevent applications from loading their own cursors, it just tries to replace `set_cursor` calls with `set_shape` calls.

## How it works
This library only works if the application either uses libwayland-cursor, or GTK4.

In both cases, we need to hook the `wl_registry` listener, so we do this by overriding `wl_proxy_add_listener`. We also need to hook certain Wayland calls, which we do by overriding `wl_proxy_marshal_array_flags` - this is what allows us to mask and override calls. This gives us the primitives we need to grab the cursor-shape-v1 extension *and* intercept `set_cursor` calls to replace them with `set_shape` calls, but from `wl_proxy_marshal_array_flags` we only will know what `wl_buffer` is being set - we need to figure out what cursor shape corresponds to each `wl_buffer` object.

For libwayland-cursor applications, it hooks `wl_cursor_theme_get_cursor` calls. When `wl_cursor_theme_get_cursor` is called, all of the buffers are added to a hash table mapping buffers to shapes. This is all that is needed to support most applications, and this is expected to be relatively stable. We do rely on a couple of libwayland internals, but they are probably not going to break.

GTK4 is a little more complicated. It doesn't use libwayland-cursor, but instead it has its own vendored copy that loads cursors on-demand and handles multiple cursor sizes within a single `wl_cursor_theme`. Thus, for GTK4, we maintain a mapping of buffers to shapes as in the previous method, but when GTK4 is loaded, if a buffer _isn't_ in the map, we grab the current GTK cursor theme off of the `GdkWaylandDisplay`, using the private `_gdk_wayland_display_get_cursor_theme`. Because that symbol is private, we get it by manually traversing the symbol table of the GTK4 binary in memory. To actually use it, we need the `GdkWaylandDisplay`. We get this, however, when the `wl_registry` listener is registered, as `GdkWaylandDisplay` will register the `wl_registry` with the userdata set to the `GdkWaylandDisplay`. So, after we detect `gtk_init`, we wait for the next `wl_registry` listener and save the userdata as the `GdkWaylandDisplay` device. And that's all there is to it: now, when encountering a new `wl_buffer`, we can search the cursor theme and record what shape it corresponds to. (For performance, we also record when a shape is _not_ found.)

The GTK4 code is definitely more fragile, but nonetheless a lot of the details it relies on have not changed in years, so it probably won't break overnight.

## How to use
Note that this is pretty ugly software and it may cause any number of unknown side-effects; if you find any, feel free to report them, although I can't guarantee I can fix them. If you want to give it a shot, it requires:

- meson
- ninja
- pkg-config
- libwayland
- wayland-protocols
- GLib
- A C compiler

To build it, you can do something like:

```
mkdir .build
cd .build
meson setup ..
ninja
```

And then you can try running it with a Wayland application:

```
LD_PRELOAD=$PWD/libgtkcursorshape.so nautilus
```

If you experience problems, you can set `G_MESSAGES_DEBUG=wlcursorfix` to get verbose debug messages; this may help narrow down where things are going wrong.
