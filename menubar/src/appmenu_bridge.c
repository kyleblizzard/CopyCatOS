// CopyCatOS — by Kyle Blizzard at Blizzard.show

// appmenu_bridge.c — DBusMenu / AppMenu.Registrar bridge implementation.
//
// Slice 18-A: own `com.canonical.AppMenu.Registrar` on the session bus,
// accept Register/Unregister/GetMenuForWindow calls, keep a WID table
// slice 18-B can query. No menu-model import, no rendering, no menubar
// visual change — just the bus plumbing.
//
// Integration model: GDBus runs on the default GLib main context, but
// menubar.c has its own select() loop and will not hand control to a
// GMainLoop. So we expose prepare_select / dispatch wrappers that merge
// the context's pollfds into the host loop's fd_set, then check + dispatch
// after select() wakes. This is the same pattern GTK uses when embedded
// under another toolkit's event loop — no tick-poll, no latency ceiling.

#include "appmenu_bridge.h"

#include <gio/gio.h>
#include <glib.h>

#include <stdio.h>
#include <string.h>

// DBus introspection XML for com.canonical.AppMenu.Registrar.
// Signature matches the Canonical appmenu spec GTK3/Qt5/Qt6-appmenu apps
// call. Signals (WindowRegistered, WindowUnregistered) aren't emitted by
// 18-A — only the three methods below are live.
static const char REGISTRAR_XML[] =
    "<node>"
    "  <interface name='com.canonical.AppMenu.Registrar'>"
    "    <method name='RegisterWindow'>"
    "      <arg type='u' name='windowId' direction='in'/>"
    "      <arg type='o' name='menuObjectPath' direction='in'/>"
    "    </method>"
    "    <method name='UnregisterWindow'>"
    "      <arg type='u' name='windowId' direction='in'/>"
    "    </method>"
    "    <method name='GetMenuForWindow'>"
    "      <arg type='u' name='windowId' direction='in'/>"
    "      <arg type='s' name='service' direction='out'/>"
    "      <arg type='o' name='menuObjectPath' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

typedef struct {
    char *service;   // sender bus name, e.g. ":1.42"
    char *path;      // object path the app exported dbusmenu on
} AppMenuEntry;

// ── Module-local state ──────────────────────────────────────────────
// Everything lives here so menubar.c never has to include glib.h.
static GDBusConnection *bus_conn      = NULL;
static GDBusNodeInfo   *node_info     = NULL;
static guint            owner_id      = 0;
static guint            object_reg_id = 0;
static GHashTable      *wid_table     = NULL;   // GUINT_TO_POINTER(wid) → AppMenuEntry*
static bool             name_acquired = false;
// nil_state: the bus connection is live but somebody else owns
// com.canonical.AppMenu.Registrar (kwin on a KDE dev box, for example).
// Lookups unconditionally fail; init still returns true so a KDE machine
// doesn't block menubar startup.
static bool             nil_state     = false;

// Scratch buffer for g_main_context_query results. Grown on demand. GLib
// may report n > alloc; we retry with a bigger buffer in that case.
static GPollFD         *poll_fds        = NULL;
static int              poll_fds_alloc  = 0;
static int              poll_fds_len    = 0;
static gint             glib_max_prio   = 0;

static void entry_free(gpointer data) {
    AppMenuEntry *e = data;
    if (!e) return;
    g_free(e->service);
    g_free(e->path);
    g_free(e);
}

// ── DBus method dispatch ────────────────────────────────────────────
// Three methods, one handler — the spec's surface is small enough that
// splitting per-method would be pure overhead.
static void handle_method_call(GDBusConnection      *conn,
                               const gchar          *sender,
                               const gchar          *object_path,
                               const gchar          *interface_name,
                               const gchar          *method_name,
                               GVariant             *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer              user_data) {
    (void)conn; (void)object_path; (void)interface_name; (void)user_data;

    if (g_strcmp0(method_name, "RegisterWindow") == 0) {
        guint32 wid = 0;
        const gchar *menu_path = NULL;
        g_variant_get(parameters, "(u&o)", &wid, &menu_path);

        AppMenuEntry *entry = g_new0(AppMenuEntry, 1);
        entry->service = g_strdup(sender);
        entry->path    = g_strdup(menu_path);
        g_hash_table_replace(wid_table, GUINT_TO_POINTER(wid), entry);

        fprintf(stderr,
                "[appmenu] RegisterWindow wid=0x%x service=%s path=%s\n",
                wid, sender, menu_path);

        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "UnregisterWindow") == 0) {
        guint32 wid = 0;
        g_variant_get(parameters, "(u)", &wid);
        g_hash_table_remove(wid_table, GUINT_TO_POINTER(wid));
        fprintf(stderr, "[appmenu] UnregisterWindow wid=0x%x\n", wid);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "GetMenuForWindow") == 0) {
        guint32 wid = 0;
        g_variant_get(parameters, "(u)", &wid);
        AppMenuEntry *e =
            g_hash_table_lookup(wid_table, GUINT_TO_POINTER(wid));
        if (!e) {
            g_dbus_method_invocation_return_error(invocation,
                G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                "no menu registered for window 0x%x", wid);
            return;
        }
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(so)", e->service, e->path));
        return;
    }

    g_dbus_method_invocation_return_error(invocation,
        G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
        "unknown method: %s", method_name);
}

static const GDBusInterfaceVTable registrar_vtable = {
    .method_call  = handle_method_call,
    .get_property = NULL,
    .set_property = NULL,
};

// ── Name ownership callbacks ────────────────────────────────────────
static void on_name_acquired(GDBusConnection *conn,
                             const gchar     *name,
                             gpointer         user_data) {
    (void)conn; (void)user_data;
    name_acquired = true;
    nil_state     = false;
    fprintf(stderr, "[appmenu] acquired %s\n", name);
}

static void on_name_lost(GDBusConnection *conn,
                         const gchar     *name,
                         gpointer         user_data) {
    (void)conn; (void)user_data;
    name_acquired = false;
    nil_state     = true;
    fprintf(stderr,
            "[appmenu] %s held by another owner — nil state\n", name);
}

// ── Public API ──────────────────────────────────────────────────────

bool appmenu_bridge_init(MenuBar *mb) {
    (void)mb;

    GError *err = NULL;
    bus_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!bus_conn) {
        fprintf(stderr, "[appmenu] session bus connect failed: %s\n",
                err ? err->message : "(null)");
        if (err) g_error_free(err);
        return false;
    }

    node_info = g_dbus_node_info_new_for_xml(REGISTRAR_XML, &err);
    if (!node_info) {
        fprintf(stderr, "[appmenu] introspection parse failed: %s\n",
                err ? err->message : "(null)");
        if (err) g_error_free(err);
        g_object_unref(bus_conn); bus_conn = NULL;
        return false;
    }

    object_reg_id = g_dbus_connection_register_object(
        bus_conn,
        "/com/canonical/AppMenu/Registrar",
        node_info->interfaces[0],
        &registrar_vtable,
        NULL, NULL, &err);
    if (object_reg_id == 0) {
        fprintf(stderr, "[appmenu] register_object failed: %s\n",
                err ? err->message : "(null)");
        if (err) g_error_free(err);
        g_dbus_node_info_unref(node_info); node_info = NULL;
        g_object_unref(bus_conn); bus_conn = NULL;
        return false;
    }

    wid_table = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                      NULL, entry_free);

    // G_BUS_NAME_OWNER_FLAGS_NONE: don't steal the name, don't allow
    // replacement. If kwin already owns it we lose — on_name_lost fires,
    // nil_state goes true, and the bridge stops pretending to host a
    // registrar. That's the right behaviour on KDE dev machines.
    owner_id = g_bus_own_name_on_connection(
        bus_conn,
        "com.canonical.AppMenu.Registrar",
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_name_acquired, on_name_lost,
        NULL, NULL);

    poll_fds_alloc = 16;
    poll_fds       = g_new0(GPollFD, poll_fds_alloc);

    fprintf(stderr, "[appmenu] bridge initialised\n");
    return true;
}

void appmenu_bridge_prepare_select(fd_set *readfds,
                                   int    *max_fd,
                                   int    *timeout_ms) {
    if (!bus_conn || !readfds || !max_fd || !timeout_ms) return;

    GMainContext *ctx = g_main_context_default();

    // prepare() folds any pending idle sources into the poll plan; the
    // return value is "sources ready now" but g_main_context_check/dispatch
    // handle that path too, so we don't branch on it.
    g_main_context_prepare(ctx, &glib_max_prio);

    gint glib_timeout = -1;
    int n;
    for (;;) {
        n = g_main_context_query(ctx, glib_max_prio,
                                 &glib_timeout,
                                 poll_fds, poll_fds_alloc);
        if (n <= poll_fds_alloc) break;
        poll_fds_alloc = n;
        poll_fds = g_renew(GPollFD, poll_fds, poll_fds_alloc);
    }
    poll_fds_len = n;

    for (int i = 0; i < poll_fds_len; i++) {
        int fd = poll_fds[i].fd;
        if (fd < 0) continue;
        if (poll_fds[i].events & (G_IO_IN | G_IO_HUP | G_IO_ERR)) {
            FD_SET(fd, readfds);
            if (fd > *max_fd) *max_fd = fd;
        }
    }

    if (glib_timeout >= 0 && glib_timeout < *timeout_ms) {
        *timeout_ms = glib_timeout;
    }
}

void appmenu_bridge_dispatch(fd_set *readfds) {
    if (!bus_conn) return;

    GMainContext *ctx = g_main_context_default();

    for (int i = 0; i < poll_fds_len; i++) {
        int fd = poll_fds[i].fd;
        poll_fds[i].revents = 0;
        if (fd >= 0 && readfds && FD_ISSET(fd, readfds)) {
            poll_fds[i].revents =
                poll_fds[i].events & (G_IO_IN | G_IO_HUP | G_IO_ERR);
        }
    }

    if (g_main_context_check(ctx, glib_max_prio,
                             poll_fds, poll_fds_len)) {
        g_main_context_dispatch(ctx);
    }
}

bool appmenu_bridge_lookup(uint32_t wid,
                           const char **service,
                           const char **path) {
    if (!wid_table || nil_state) return false;
    AppMenuEntry *e =
        g_hash_table_lookup(wid_table, GUINT_TO_POINTER(wid));
    if (!e) return false;
    if (service) *service = e->service;
    if (path)    *path    = e->path;
    return true;
}

void appmenu_bridge_shutdown(MenuBar *mb) {
    (void)mb;

    if (owner_id != 0) {
        g_bus_unown_name(owner_id);
        owner_id = 0;
    }
    if (object_reg_id != 0 && bus_conn) {
        g_dbus_connection_unregister_object(bus_conn, object_reg_id);
        object_reg_id = 0;
    }
    if (node_info) {
        g_dbus_node_info_unref(node_info);
        node_info = NULL;
    }
    if (bus_conn) {
        g_object_unref(bus_conn);
        bus_conn = NULL;
    }
    if (wid_table) {
        g_hash_table_destroy(wid_table);
        wid_table = NULL;
    }
    g_free(poll_fds);
    poll_fds       = NULL;
    poll_fds_alloc = 0;
    poll_fds_len   = 0;
    name_acquired  = false;
    nil_state      = false;
}
