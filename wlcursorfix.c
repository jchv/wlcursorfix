// wlcursorfix, a temporary shim to bring support for cursor-shape-v1 to
// programs that don't yet support it.
//
// Copyright 2024 John Chadwick <john@jchw.io>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#define G_LOG_USE_STRUCTURED
#ifndef G_LOG_DOMAIN
#define G_LOG_DOMAIN "wlcursorfix"
#endif

#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <elf.h>
#include <glib.h>
#include <link.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <cursor-shape-v1-client-protocol.h>
#include <tablet-unstable-v2-client-protocol.h>
#include <wayland-cursor.h>

static unsigned int
gdk_wayland_display_cursor_buffer_shape(void *display,
                                        struct wl_buffer *buffer);
//
// Internal structures
//

// libwayland doesn't provide a way to get the wl_display from a wl_proxy
// object, but there are several locations where being able to do this greatly
// improves the code, so we pry into the implementation. This is liable to break
// at any time, though it might not. Dunno.
struct wl_object {
  const struct wl_interface *interface;
  const void *implementation;
  uint32_t id;
};
struct wl_proxy {
  struct wl_object object;
  struct wl_display *display;
};

// GTK4 uses a vendored version of libwayland-cursor that loads cursors
// on-demand and uses one wl_cursor_theme for all sizes of a cursor theme.
// Therefore for GTK4, our hook to try to catch cursors as they load will not
// work. Instead, we try to capture the GdkWaylandDisplay at gtk_init. Then, we
// can use that to fetch the GTK wl_cursor_theme. Whenever we encounter an
// unknown cursor, we can walk through the current wl_cursor_theme to see if we
// can find it.
struct gtk_wl_cursor_theme {
  unsigned int cursor_count;
  struct gtk_wl_cursor **cursors;
};
struct gtk_wl_cursor {
  unsigned int image_count;
  struct gtk_cursor_image **images;
  char *name;
};
struct gtk_cursor_image {
  struct wl_cursor_image image;
  struct gtk_wl_cursor_theme *theme;
  struct wl_buffer *buffer;
};

// TODO: Should verify that these mappings are accurate.
// TODO: add more legacy Xcursor mappings?
const static struct {
  const char *name;
  const unsigned int shape;
} cursor_shape_list[] = {
    {"default", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
    {"left_ptr", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
    {"help", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP},
    {"context-menu", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU},
    {"pointer", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
    {"progress", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS},
    {"wait", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT},
    {"cell", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL},
    {"crosshair", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR},
    {"text", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT},
    {"xterm", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT},
    {"vertical-text", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT},
    {"alias", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS},
    {"copy", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY},
    {"move", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE},
    {"no-drop", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP},
    {"dnd-ask", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY},
    {"not-allowed", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED},
    {"grab", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB},
    {"grabbing", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING},
    {"all-scroll", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL},
    {"col-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE},
    {"row-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE},
    {"n-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE},
    {"e-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE},
    {"s-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE},
    {"w-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE},
    {"ne-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE},
    {"nw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE},
    {"se-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE},
    {"sw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE},
    {"ew-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE},
    {"ns-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE},
    {"nesw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE},
    {"nwse-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE},
    {"zoom-in", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN},
    {"zoom-out", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT},
};

static mtx_t mutex;

// Map of cursor name -> shape
static GHashTable *cursor_shape_map;
// Map of wl_buffer -> shape
static GHashTable *buffer_shape_map;
// Map of wl_display -> wp_cursor_shape_manager_v1
static GHashTable *display_cursor_shape_manager_map;
// Map of wl_proxy -> wp_cursor_shape_device_v1
static GHashTable *object_cursor_shape_device_map;
// Boolean that is set to true inside gtk_init
static atomic_bool in_gtk_init = false;
// Captured GdkWaylandDisplay from gtk_init call
static void *_Atomic gdk_wayland_display;

// Initialize global structures
static void __attribute__((constructor)) init(void) {
  static atomic_flag initialized = ATOMIC_FLAG_INIT;
  if (atomic_flag_test_and_set(&initialized)) {
    return;
  }
  mtx_init(&mutex, mtx_plain);
  cursor_shape_map = g_hash_table_new(g_str_hash, g_str_equal);
  for (int i = 0; i < sizeof(cursor_shape_list) / sizeof(*cursor_shape_list);
       ++i) {
    g_hash_table_insert(cursor_shape_map, (gpointer)cursor_shape_list[i].name,
                        GUINT_TO_POINTER(cursor_shape_list[i].shape));
  }
  buffer_shape_map = g_hash_table_new(g_direct_hash, g_direct_equal);
  display_cursor_shape_manager_map =
      g_hash_table_new(g_direct_hash, g_direct_equal);
  object_cursor_shape_device_map =
      g_hash_table_new(g_direct_hash, g_direct_equal);
  gdk_wayland_display = NULL;
  g_debug("wlcursorfix initialized");
}

// Register wl_cursor buffers for the corresponding shape to name
static void register_wl_cursor_buffers(const char *name,
                                       struct wl_cursor *cursor) {
  gpointer orig_key, value;
  if (!g_hash_table_lookup_extended(cursor_shape_map, name, &orig_key,
                                    &value)) {
    g_debug("no cursor image for name %s", name);
    return;
  }
  g_debug("register cursor shape %d", GPOINTER_TO_UINT(value));
  mtx_lock(&mutex);
  for (int i = 0; i < cursor->image_count; i++) {
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(cursor->images[i]);
    g_debug("registered buffer %p as %s", buffer, name);
    g_hash_table_insert(buffer_shape_map, buffer, value);
  }
  mtx_unlock(&mutex);
}

// Look up shape that corresponds to buffer
static unsigned int lookup_buffer_shape(struct wl_buffer *buffer) {
  mtx_lock(&mutex);
  unsigned int shape =
      GPOINTER_TO_UINT(g_hash_table_lookup(buffer_shape_map, buffer));
  mtx_unlock(&mutex);
  if (!shape && gdk_wayland_display) {
    // GTK4: Try searching the current GTK cursor theme
    shape =
        gdk_wayland_display_cursor_buffer_shape(gdk_wayland_display, buffer);
  }
  return shape;
}

// Register global shape manager for display
static void register_display_shape_manager(
    struct wl_display *display,
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager) {
  mtx_lock(&mutex);
  if (g_hash_table_lookup(display_cursor_shape_manager_map,
                          (gpointer)display) != NULL) {
    mtx_unlock(&mutex);
    return;
  }
  g_hash_table_insert(display_cursor_shape_manager_map, (gpointer)display,
                      (gpointer)cursor_shape_manager);
  mtx_unlock(&mutex);
}

static struct wp_cursor_shape_device_v1 *
get_cursor_shape_device(struct wl_proxy *object, bool tablet_tool) {
  struct wp_cursor_shape_device_v1 *cursor_shape_device = NULL;
  struct wp_cursor_shape_manager_v1 *cursor_shape_manager = NULL;

  // Lock to search hash tables
  mtx_lock(&mutex);
  {
    // Try to get the cursor shape device if we already acquired it
    cursor_shape_device =
        g_hash_table_lookup(object_cursor_shape_device_map, object);
    if (cursor_shape_device != NULL) {
      mtx_unlock(&mutex);
      return cursor_shape_device;
    }

    // If not, we need to get the cursor shape manager for the display
    cursor_shape_manager = g_hash_table_lookup(display_cursor_shape_manager_map,
                                               (gpointer)object->display);
  }
  mtx_unlock(&mutex);

  // If there is no cursor shape manager for the display, exit
  if (!cursor_shape_manager) {
    return NULL;
  }

  // Get the cursor shape manager for the device
  if (tablet_tool) {
    cursor_shape_device = wp_cursor_shape_manager_v1_get_tablet_tool_v2(
        cursor_shape_manager, (struct zwp_tablet_tool_v2 *)object);
  } else {
    cursor_shape_device = wp_cursor_shape_manager_v1_get_pointer(
        cursor_shape_manager, (struct wl_pointer *)object);
  }
  if (!cursor_shape_device) {
    return NULL;
  }

  // Lock to write to hash tables
  mtx_lock(&mutex);
  {
    // Another thread may have raced us: if we lost the race, we should destroy
    // ours and return theirs. We do this because holding the lock while calling
    // other Wayland functions may be dangerous.
    struct wp_cursor_shape_device_v1 *other_cursor_shape_device =
        g_hash_table_lookup(object_cursor_shape_device_map, object);
    if (other_cursor_shape_device != NULL) {
      mtx_unlock(&mutex);
      wp_cursor_shape_device_v1_destroy(cursor_shape_device);
      return other_cursor_shape_device;
    }
    // We win the race, insert it.
    g_hash_table_insert(object_cursor_shape_device_map, object,
                        cursor_shape_device);
  }
  mtx_unlock(&mutex);
  return cursor_shape_device;
}

//
// Wayland registry hook
//

typedef struct {
  void *data;
  struct wl_registry_listener *implementation;
} registry_hook_data;

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t id, const char *interface,
                                   uint32_t version) {
  registry_hook_data *hook_data = (registry_hook_data *)data;
  hook_data->implementation->global(hook_data->data, registry, id, interface,
                                    version);
  if (strcmp(interface, "wp_cursor_shape_manager_v1") == 0) {
    struct wl_display *display = ((struct wl_proxy *)registry)->display;
    g_debug("acquired wp_cursor_shape_manager_v1");
    register_display_shape_manager(
        display,
        wl_registry_bind(registry, id, &wp_cursor_shape_manager_v1_interface,
                         MIN(version, 1)));
  }

  return;
}

static void registry_handle_global_remove(void *data,
                                          struct wl_registry *registry,
                                          uint32_t id) {
  registry_hook_data *hook_data = (registry_hook_data *)data;
  hook_data->implementation->global_remove(hook_data->data, registry, id);
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove,
};

//
// Wayland Hooks
//

int wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void),
                          void *data) {
  static int (*next)(struct wl_proxy *proxy, void (**implementation)(void),
                     void *data);
  if (!next) {
    next = dlsym(RTLD_NEXT, "wl_proxy_add_listener");
  }
  if (strcmp(wl_proxy_get_class(proxy), "wl_registry") == 0) {
    g_debug("installing listener proxy for wl_registry");
    registry_hook_data *hook_data = malloc(sizeof(registry_hook_data));
    hook_data->data = data;
    hook_data->implementation = (struct wl_registry_listener *)implementation;
    implementation = (void (**)(void)) & registry_listener;
    data = (void *)hook_data;
    if (in_gtk_init) {
      in_gtk_init = false;
      gdk_wayland_display = hook_data->data;
      g_debug("captured GdkWaylandDisplay: %p", gdk_wayland_display);
    }
  }
  return next(proxy, implementation, data);
}

struct wl_cursor *wl_cursor_theme_get_cursor(struct wl_cursor_theme *theme,
                                             const char *name) {
  static struct wl_cursor *(*next)(struct wl_cursor_theme *, const char *name);
  if (!next) {
    next = dlsym(RTLD_NEXT, "wl_cursor_theme_get_cursor");
  }
  struct wl_cursor *cursor = next(theme, name);
  if (cursor) {
    register_wl_cursor_buffers(name, cursor);
  }
  return cursor;
}

struct wl_proxy *
wl_proxy_marshal_array_flags(struct wl_proxy *proxy, uint32_t opcode,
                             const struct wl_interface *interface,
                             uint32_t version, uint32_t flags,
                             union wl_argument *args) {
  static struct wl_proxy *(*next)(struct wl_proxy *proxy, uint32_t opcode,
                                  const struct wl_interface *interface,
                                  uint32_t version, uint32_t flags,
                                  union wl_argument *args);
  if (!next) {
    next = dlsym(RTLD_NEXT, "wl_proxy_marshal_array_flags");
  }
  static thread_local struct {
    struct wl_proxy *object;
    uint32_t version;
    uint32_t enter_serial;
    struct wl_surface *pointer_surface;
    int32_t x, y;
    bool tablet_tool;
  } deferred_set_cursor_data = {0};
  const char *class = wl_proxy_get_class(proxy);
  const bool is_wl_surface_request = strcmp(class, "wl_surface") == 0;
  const bool is_wl_pointer_request = strcmp(class, "wl_pointer") == 0;
  const bool is_zwp_tablet_tool_v2_request =
      strcmp(class, "zwp_tablet_tool_v2") == 0;
  const bool proxy_is_deferred_pointer_surface =
      proxy != NULL &&
      proxy == (struct wl_proxy *)deferred_set_cursor_data.pointer_surface;

  if (is_wl_surface_request && proxy_is_deferred_pointer_surface) {
    unsigned int shape;
    switch (opcode) {
    case WL_SURFACE_ATTACH:
      shape = lookup_buffer_shape((struct wl_buffer *)args[0].o);
      if (shape == 0) {
        g_debug("no shape found for buffer %p", args[0].o);
        break;
      }
      struct wp_cursor_shape_device_v1 *cursor_shape_device =
          get_cursor_shape_device(deferred_set_cursor_data.object,
                                  deferred_set_cursor_data.tablet_tool);
      g_debug("mapped buffer %p to shape %d for device %p", args[0].o, shape,
              cursor_shape_device);
      wp_cursor_shape_device_v1_set_shape(
          cursor_shape_device, deferred_set_cursor_data.enter_serial, shape);
      return NULL;
    case WL_SURFACE_SET_BUFFER_SCALE:
    case WL_SURFACE_DAMAGE:
      return NULL;
    case WL_SURFACE_COMMIT:
      // Still mask this, but also clear the deferred set_cursor now.
      memset(&deferred_set_cursor_data, 0, sizeof(deferred_set_cursor_data));
      return NULL;
    }
  }
  if (deferred_set_cursor_data.pointer_surface != NULL) {
    // If there is a pending deferred wl_pointer_set_cursor, call it now.
    union wl_argument args[4] = {
        {.u = deferred_set_cursor_data.enter_serial},
        {.o = (struct wl_object *)deferred_set_cursor_data.pointer_surface},
        {.i = deferred_set_cursor_data.x},
        {.i = deferred_set_cursor_data.y},
    };
    g_debug("flush deferred set_cursor operation");
    next(deferred_set_cursor_data.object, WL_POINTER_SET_CURSOR, NULL,
         deferred_set_cursor_data.version, 0, args);
    memset(&deferred_set_cursor_data, 0, sizeof(deferred_set_cursor_data));
  }
  // If the next Wayland call is wl_pointer_set_cursor, defer it.
  if (is_wl_pointer_request && opcode == WL_POINTER_SET_CURSOR) {
    deferred_set_cursor_data.object = proxy;
    deferred_set_cursor_data.version = version;
    deferred_set_cursor_data.enter_serial = args[0].u;
    deferred_set_cursor_data.pointer_surface = (struct wl_surface *)args[1].o;
    deferred_set_cursor_data.x = args[2].i;
    deferred_set_cursor_data.y = args[3].i;
    deferred_set_cursor_data.tablet_tool = false;
    return NULL;
  } else if (is_zwp_tablet_tool_v2_request &&
             opcode == ZWP_TABLET_TOOL_V2_SET_CURSOR) {
    deferred_set_cursor_data.object = proxy;
    deferred_set_cursor_data.version = version;
    deferred_set_cursor_data.enter_serial = args[0].u;
    deferred_set_cursor_data.pointer_surface = (struct wl_surface *)args[1].o;
    deferred_set_cursor_data.x = args[2].i;
    deferred_set_cursor_data.y = args[3].i;
    deferred_set_cursor_data.tablet_tool = true;
    return NULL;
  }
  return next(proxy, opcode, interface, version, flags, args);
}

//
// GTK hooks
//

static atomic_bool have_gtk4 = false;

static struct gtk_wl_cursor_theme *(*_gdk_wayland_display_get_cursor_theme)(
    void *display) = NULL;

static char *gtk_cursor_theme_buffer_name(struct gtk_wl_cursor_theme *theme,
                                          struct wl_buffer *buffer) {
  for (int i = 0; i < theme->cursor_count; i++) {
    struct gtk_wl_cursor *cursor = theme->cursors[i];
    for (int j = 0; j < cursor->image_count; j++) {
      struct gtk_cursor_image *image = cursor->images[j];
      if (image->buffer == buffer) {
        return cursor->name;
      }
    }
  }
  return NULL;
}

static unsigned int
gtk_cursor_theme_buffer_shape(struct gtk_wl_cursor_theme *theme,
                              struct wl_buffer *buffer) {
  unsigned int shape = 0;
  char *name = gtk_cursor_theme_buffer_name(theme, buffer);
  gpointer orig_key, value;
  if (!g_hash_table_lookup_extended(cursor_shape_map, name, &orig_key,
                                    &value)) {
    g_debug("no cursor image for name %s", name);
    shape = 0;
  } else {
    shape = GPOINTER_TO_UINT(value);
  }
  mtx_lock(&mutex);
  g_hash_table_insert(buffer_shape_map, buffer, value);
  mtx_unlock(&mutex);
  if (shape != 0) {
    g_debug("registered buffer %p (%s) as GTK cursor shape %d", buffer, name,
            shape);
  }
  return shape;
}

static unsigned int
gdk_wayland_display_cursor_buffer_shape(void *display,
                                        struct wl_buffer *buffer) {
  struct gtk_wl_cursor_theme *theme =
      _gdk_wayland_display_get_cursor_theme(display);
  return gtk_cursor_theme_buffer_shape(theme, buffer);
}

static void init_gtk_hook() {
  // We need to get a private (STB_LOCAL) symbol from symtab.
  // Warning: This code may cause severe psychic damage to sensible people.
  static atomic_flag initialized = ATOMIC_FLAG_INIT;
  if (atomic_flag_test_and_set(&initialized)) {
    return;
  }
  // TODO: maybe try to find a better symbol that reliably detects only GTK4
  void *gtk_init = dlsym(RTLD_NEXT, "gtk_init");
  if (!gtk_init) {
    g_debug("no resident gtk found");
    return;
  }
  Dl_info gtk_info;
  if (!dladdr(gtk_init, &gtk_info)) {
    g_debug("error resolving gtk_init info");
    return;
  }
  if (strstr(gtk_info.dli_fname, "libgtk-4.so") == NULL) {
    g_debug("detected gtk but not gtk4");
    return;
  }
  ElfW(Ehdr) *header = gtk_info.dli_fbase;
  FILE *f = fopen(gtk_info.dli_fname, "r");
  if (!f) {
    g_debug("couldn't open gtk binary");
    return;
  }
  ElfW(Shdr) *sections = malloc(header->e_shnum * sizeof(ElfW(Shdr)));
  fseek(f, header->e_shoff, SEEK_SET);
  if (fread(sections, header->e_shnum * sizeof(ElfW(Shdr)), 1, f) != 1) {
    g_debug("couldn't load symbol table: %s", strerror(errno));
    return;
  }
  ElfW(Shdr) *shstr = &sections[header->e_shstrndx];
  g_debug("header: %p; shstrndx: %d; shoff: %ld; shstrsize: %ld", header,
          header->e_shstrndx, header->e_shoff, shstr->sh_size);

  char *shstrbuf = malloc(shstr->sh_size);
  g_debug("allocate %ld byte buffer: %p", shstr->sh_size, shstrbuf);
  fseek(f, shstr->sh_offset, SEEK_SET);
  if (fread(shstrbuf, shstr->sh_size, 1, f) != 1) {
    g_debug("couldn't load symbol string table: %s", strerror(errno));
    return;
  }
  char *symtab = NULL;
  int symcount = 0;
  int symentsize = 0;
  char *strtab = NULL;
  g_debug("ELF header: %s", header->e_ident);
  for (int i = 0; i < header->e_shnum; i++) {
    ElfW(Shdr) *section = &sections[i];
    const char *name = shstrbuf + section->sh_name;
    if (!name) {
      continue;
    }
    if (strcmp(name, ".symtab") == 0) {
      symtab = malloc(section->sh_size);
      fseek(f, section->sh_offset, SEEK_SET);
      if (fread(symtab, section->sh_size, 1, f) != 1) {
        g_debug("couldn't load symbol table: %s", strerror(errno));
        return;
      }
      symcount = section->sh_size / section->sh_entsize;
      symentsize = section->sh_entsize;
    } else if (strcmp(name, ".strtab") == 0) {
      strtab = malloc(section->sh_size);
      fseek(f, section->sh_offset, SEEK_SET);
      if (fread(strtab, section->sh_size, 1, f) != 1) {
        g_debug("couldn't load symbol string table: %s", strerror(errno));
        return;
      }
    }
  }
  fclose(f);

  if (!symtab || !strtab) {
    g_debug("could not locate symtab or strtab table");
    return;
  }
  for (int i = 0; i < symcount; i++) {
    ElfW(Sym) *symbol = (void *)((const char *)symtab + symentsize * i);
    const char *name = strtab + symbol->st_name;
    if (!*name) {
      continue;
    }
    void *addr = (void *)((const char *)header + symbol->st_value);
    if (strcmp(name, "_gdk_wayland_display_get_cursor_theme") == 0) {
      _gdk_wayland_display_get_cursor_theme = addr;
    }
  }
  if (!_gdk_wayland_display_get_cursor_theme) {
    g_debug("couldn't resolve _gdk_wayland_display_get_cursor_theme");
    return;
  }
  g_debug("resolved gtk module as %s", gtk_info.dli_fname);
  have_gtk4 = true;
}

// We need to hook GTK4 at gtk_init, but it is compiled with -Bsymbolic, so
// internal calls to gtk_init are not possible to catch. Therefore, applications
// that don't call gtk_init directly first will need special interception.
// g_application_run seems to be a good spot. We still need both because someone
// may call gtk_init earlier.
int g_application_run(void *application, int argc, char **argv) {
  init_gtk_hook();
  static int (*next)(void *, int, char **);
  if (!next) {
    next = dlsym(RTLD_NEXT, "g_application_run");
  }
  if (have_gtk4 && gdk_wayland_display == NULL) {
    in_gtk_init = true;
  }
  int result = next(application, argc, argv);
  in_gtk_init = false;
  return result;
}

void gtk_init(void *a, void *b) {
  init_gtk_hook();
  static void (*next)(void *, void *);
  if (!next) {
    next = dlsym(RTLD_NEXT, "gtk_init");
  }
  if (have_gtk4 && gdk_wayland_display == NULL) {
    in_gtk_init = true;
  }
  next(a, b);
  in_gtk_init = false;
}
