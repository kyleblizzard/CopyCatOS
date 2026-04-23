// CopyCatOS — by Kyle Blizzard at Blizzard.show

// dbusmenu_client.c — Implementation of the com.canonical.dbusmenu proxy.
//
// See the header for the contract. Notes on the wire:
//
//   GetLayout(i parentId, i recursionDepth, as propertyNames)
//     → (u revision, (i id, a{sv} props, av children))
//   Each child in the recursive `av` is a variant wrapping another
//   (ia{sv}av), so tree walking is a recursive unpack of variants.
//
//   LayoutUpdated(u revision, i parentId)      — refetch the subtree
//   ItemsPropertiesUpdated(a(ia{sv}) changed,  — patch specific ids
//                          a(ias)   removed)
//   Event(i id, s event_id, v data, u timestamp)  — click/activate
//
// Properties we understand:
//   label (s), type (s)='standard'|'separator',
//   enabled (b), visible (b),
//   toggle-type (s)=''|'checkmark'|'radio', toggle-state (i),
//   children-display (s)='submenu' marks a branch,
//   shortcut (aas) — each inner as is a modifier chord like
//     ["Control","S"]; we only keep the first chord for display.
//
// Memory: one GCancellable per client guards every async call, so
// dbusmenu_client_free can tear down without pending callbacks firing
// on a freed struct.

#include "dbusmenu_client.h"

#include <gio/gio.h>
#include <glib.h>

#include <stdio.h>
#include <string.h>

struct DbusMenuClient {
    GDBusProxy        *proxy;            // com.canonical.dbusmenu proxy
    GCancellable      *cancel;           // guards every async call
    MenuNode          *root;             // current tree; replaced wholesale
    DbusMenuChangedFn  on_changed;
    void              *user_data;
    gulong             signal_handler;
    bool               fetch_in_flight;  // drop LayoutUpdated storms to one
    bool               refetch_queued;   // stash arrivals during a fetch
};

// ── Property → MenuNode translation ─────────────────────────────────
// DBusMenu properties arrive as a{sv}; we pick out the ones the
// renderer cares about and fold them onto the node. Unknown keys are
// ignored — some toolkits add vendor-prefixed extras.

static void apply_prop(MenuNode *node, const char *key, GVariant *val) {
    if (!key || !val) return;

    if (g_strcmp0(key, "label") == 0 &&
        g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
        const char *s = g_variant_get_string(val, NULL);
        free(node->label);
        node->label = menu_strip_mnemonic(s);
        return;
    }

    if (g_strcmp0(key, "type") == 0 &&
        g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
        const char *s = g_variant_get_string(val, NULL);
        node->type = (g_strcmp0(s, "separator") == 0)
                         ? MENU_ITEM_SEPARATOR
                         : MENU_ITEM_STANDARD;
        return;
    }

    if (g_strcmp0(key, "enabled") == 0 &&
        g_variant_is_of_type(val, G_VARIANT_TYPE_BOOLEAN)) {
        node->enabled = g_variant_get_boolean(val);
        return;
    }

    if (g_strcmp0(key, "visible") == 0 &&
        g_variant_is_of_type(val, G_VARIANT_TYPE_BOOLEAN)) {
        node->visible = g_variant_get_boolean(val);
        return;
    }

    if (g_strcmp0(key, "toggle-type") == 0 &&
        g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
        const char *s = g_variant_get_string(val, NULL);
        if      (g_strcmp0(s, "checkmark") == 0) node->toggle = MENU_TOGGLE_CHECKMARK;
        else if (g_strcmp0(s, "radio")     == 0) node->toggle = MENU_TOGGLE_RADIO;
        else                                     node->toggle = MENU_TOGGLE_NONE;
        return;
    }

    if (g_strcmp0(key, "toggle-state") == 0 &&
        g_variant_is_of_type(val, G_VARIANT_TYPE_INT32)) {
        node->toggle_state = g_variant_get_int32(val);
        return;
    }

    if (g_strcmp0(key, "children-display") == 0 &&
        g_variant_is_of_type(val, G_VARIANT_TYPE_STRING)) {
        const char *s = g_variant_get_string(val, NULL);
        if (g_strcmp0(s, "submenu") == 0) node->is_submenu = true;
        return;
    }

    if (g_strcmp0(key, "shortcut") == 0 &&
        g_variant_is_of_type(val, G_VARIANT_TYPE("aas"))) {
        // aas: array of chord; chord is array of modifier/key strings.
        // We join the first chord with "+". Second+ chords (ambiguous
        // accelerators) are dropped — Aqua only shows one per item.
        GVariantIter it;
        g_variant_iter_init(&it, val);
        GVariant *chord = g_variant_iter_next_value(&it);
        if (chord) {
            GString *buf = g_string_new(NULL);
            GVariantIter cit;
            g_variant_iter_init(&cit, chord);
            const gchar *tok;
            bool first = true;
            while (g_variant_iter_next(&cit, "&s", &tok)) {
                if (!first) g_string_append_c(buf, '+');
                g_string_append(buf, tok);
                first = false;
            }
            // menu_model.c uses plain libc free(); match with strdup.
            // Never hand g_strdup'd memory to menu_node_free — GLib
            // doesn't guarantee g_malloc/free interop per the spec.
            free(node->shortcut);
            node->shortcut = strdup(buf->str);
            g_string_free(buf, TRUE);
            g_variant_unref(chord);
        }
        return;
    }
}

static void apply_props_dict(MenuNode *node, GVariant *props) {
    if (!props) return;
    GVariantIter it;
    const gchar *key;
    GVariant    *val;
    g_variant_iter_init(&it, props);
    while (g_variant_iter_next(&it, "{&sv}", &key, &val)) {
        apply_prop(node, key, val);
        g_variant_unref(val);
    }
}

// ── Recursive tree build from a GetLayout reply ─────────────────────
// The payload for each node is `(ia{sv}av)`. Root is passed in
// un-variant-wrapped; child items are variants that must be unpacked
// one level first.

static MenuNode *build_from_item(GVariant *item) {
    if (!item) return NULL;

    gint32    id       = 0;
    GVariant *props    = NULL;
    GVariant *children = NULL;
    g_variant_get(item, "(i@a{sv}@av)", &id, &props, &children);

    MenuNode *n   = menu_node_new_item(NULL);
    n->is_legacy  = true;
    n->action_kind = MENU_ACTION_LEGACY;
    n->legacy_id  = id;

    apply_props_dict(n, props);

    // DBusMenu id 0 is the synthetic root — it has no label, no action,
    // just children. Drop its legacy-action tagging so the renderer
    // doesn't try to "click the root."
    if (id == 0) {
        n->action_kind = MENU_ACTION_NONE;
    }

    GVariantIter cit;
    g_variant_iter_init(&cit, children);
    GVariant *child_v;
    while ((child_v = g_variant_iter_next_value(&cit))) {
        // Each element of av is a variant wrapping the item tuple.
        GVariant *inner = g_variant_get_variant(child_v);
        MenuNode *c = build_from_item(inner);
        if (c) menu_node_add_child(n, c);
        g_variant_unref(inner);
        g_variant_unref(child_v);
    }

    g_variant_unref(props);
    g_variant_unref(children);
    return n;
}

// ── GetLayout async round-trip ──────────────────────────────────────

static void trigger_refetch(DbusMenuClient *client);

static void on_get_layout_reply(GObject *src, GAsyncResult *res, gpointer ud) {
    DbusMenuClient *client = ud;
    GError *err = NULL;
    GVariant *reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);

    // Cancellation = client freed; stop touching it.
    if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        return;
    }

    client->fetch_in_flight = false;

    if (!reply) {
        fprintf(stderr, "[dbusmenu] GetLayout failed: %s\n",
                err ? err->message : "(null)");
        if (err) g_error_free(err);
        return;
    }

    // (u(ia{sv}av))
    guint32   revision = 0;
    GVariant *layout   = NULL;
    g_variant_get(reply, "(u@(ia{sv}av))", &revision, &layout);
    (void)revision;

    MenuNode *new_root = build_from_item(layout);
    g_variant_unref(layout);
    g_variant_unref(reply);

    menu_node_free(client->root);
    client->root = new_root;

    if (client->on_changed) client->on_changed(client, client->user_data);

    // A LayoutUpdated arrived mid-flight — fetch again so we're fresh.
    if (client->refetch_queued) {
        client->refetch_queued = false;
        trigger_refetch(client);
    }
}

static void trigger_refetch(DbusMenuClient *client) {
    if (!client || !client->proxy) return;
    if (client->fetch_in_flight) {
        client->refetch_queued = true;
        return;
    }
    client->fetch_in_flight = true;

    // GetLayout(parentId=0, depth=-1 meaning "everything",
    //           propertyNames=[] meaning "every known property")
    GVariant *args = g_variant_new("(iias)", 0, -1, NULL);
    g_dbus_proxy_call(client->proxy,
                      "GetLayout",
                      args,
                      G_DBUS_CALL_FLAGS_NONE,
                      -1,
                      client->cancel,
                      on_get_layout_reply,
                      client);
}

// ── Signal fan-in ───────────────────────────────────────────────────
// One handler for every dbusmenu signal; branch on the name. Keeps
// signal subscription to a single g_signal_connect.

static MenuNode *find_by_id(MenuNode *root, int32_t id) {
    if (!root) return NULL;
    if (root->action_kind == MENU_ACTION_LEGACY && root->legacy_id == id) {
        return root;
    }
    for (int i = 0; i < root->n_children; i++) {
        MenuNode *hit = find_by_id(root->children[i], id);
        if (hit) return hit;
    }
    return NULL;
}

static void apply_items_properties_updated(DbusMenuClient *client,
                                           GVariant       *params) {
    // params: (a(ia{sv}) updated, a(ias) removed)
    GVariant *updated = NULL;
    GVariant *removed = NULL;
    g_variant_get(params, "(@a(ia{sv})@a(ias))", &updated, &removed);

    GVariantIter it;
    g_variant_iter_init(&it, updated);
    gint32    id;
    GVariant *props;
    while (g_variant_iter_next(&it, "(i@a{sv})", &id, &props)) {
        MenuNode *n = find_by_id(client->root, id);
        if (n) apply_props_dict(n, props);
        g_variant_unref(props);
    }

    // For removed-property lists we reset the known ones to defaults —
    // the sender is telling us those properties no longer apply.
    g_variant_iter_init(&it, removed);
    GVariant *keys_v;
    while (g_variant_iter_next(&it, "(i@as)", &id, &keys_v)) {
        MenuNode *n = find_by_id(client->root, id);
        if (n) {
            GVariantIter kit;
            g_variant_iter_init(&kit, keys_v);
            const gchar *k;
            while (g_variant_iter_next(&kit, "&s", &k)) {
                if      (g_strcmp0(k, "label") == 0)    { free(n->label);    n->label    = NULL; }
                else if (g_strcmp0(k, "shortcut") == 0) { free(n->shortcut); n->shortcut = NULL; }
                else if (g_strcmp0(k, "enabled") == 0)  { n->enabled = true;  }
                else if (g_strcmp0(k, "visible") == 0)  { n->visible = true;  }
                else if (g_strcmp0(k, "toggle-type") == 0) n->toggle = MENU_TOGGLE_NONE;
                else if (g_strcmp0(k, "toggle-state") == 0) n->toggle_state = 0;
            }
        }
        g_variant_unref(keys_v);
    }

    g_variant_unref(updated);
    g_variant_unref(removed);

    if (client->on_changed) client->on_changed(client, client->user_data);
}

static void on_dbusmenu_signal(GDBusProxy  *proxy,
                               const gchar *sender_name,
                               const gchar *signal_name,
                               GVariant    *params,
                               gpointer     user_data) {
    (void)proxy; (void)sender_name;
    DbusMenuClient *client = user_data;

    if (g_strcmp0(signal_name, "LayoutUpdated") == 0) {
        // Payload is (u revision, i parentId). We cheat and always
        // refetch from the root — menus are small, correctness beats
        // partial-subtree bookkeeping for slice 18-B.
        trigger_refetch(client);
        return;
    }

    if (g_strcmp0(signal_name, "ItemsPropertiesUpdated") == 0) {
        apply_items_properties_updated(client, params);
        return;
    }

    // ItemActivationRequested and a couple of others are informational —
    // the renderer will care eventually, slice 18-B ignores them.
}

// ── Proxy creation ──────────────────────────────────────────────────

static void on_proxy_ready(GObject *src, GAsyncResult *res, gpointer ud) {
    (void)src;
    DbusMenuClient *client = ud;
    GError *err = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, &err);

    if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        if (proxy) g_object_unref(proxy);
        return;
    }

    if (!proxy) {
        fprintf(stderr, "[dbusmenu] proxy creation failed: %s\n",
                err ? err->message : "(null)");
        if (err) g_error_free(err);
        return;
    }

    client->proxy          = proxy;
    client->signal_handler = g_signal_connect(proxy, "g-signal",
                                              G_CALLBACK(on_dbusmenu_signal),
                                              client);

    trigger_refetch(client);
}

DbusMenuClient *dbusmenu_client_new(const char *service,
                                    const char *object_path,
                                    DbusMenuChangedFn on_changed,
                                    void *user_data) {
    if (!service || !object_path) return NULL;

    DbusMenuClient *client = g_new0(DbusMenuClient, 1);
    client->cancel     = g_cancellable_new();
    client->on_changed = on_changed;
    client->user_data  = user_data;

    g_dbus_proxy_new_for_bus(
        G_BUS_TYPE_SESSION,
        // GET_INVALIDATED_PROPERTIES is default; we don't use property
        // caching here — DBusMenu uses custom signals, not the standard
        // org.freedesktop.DBus.Properties.PropertiesChanged.
        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
        NULL,
        service,
        object_path,
        "com.canonical.dbusmenu",
        client->cancel,
        on_proxy_ready,
        client);

    return client;
}

const MenuNode *dbusmenu_client_root(const DbusMenuClient *client) {
    return client ? client->root : NULL;
}

void dbusmenu_client_activate(DbusMenuClient *client, int32_t legacy_id) {
    if (!client || !client->proxy) return;

    // Event(id, event_id, data, timestamp). `data` is a variant — the
    // dbusmenu spec insists it be wrapped even when empty. Use an
    // empty string as the no-op payload (every live implementation
    // accepts this; some reject the void type).
    guint32 ts = (guint32)(g_get_monotonic_time() / G_TIME_SPAN_MILLISECOND);
    GVariant *payload = g_variant_new_string("");
    GVariant *args = g_variant_new("(isvu)", legacy_id, "clicked",
                                   payload, ts);
    g_dbus_proxy_call(client->proxy,
                      "Event",
                      args,
                      G_DBUS_CALL_FLAGS_NO_AUTO_START,
                      -1,
                      client->cancel,
                      NULL, NULL);
}

void dbusmenu_client_free(DbusMenuClient *client) {
    if (!client) return;

    if (client->cancel) {
        g_cancellable_cancel(client->cancel);
        g_object_unref(client->cancel);
    }
    if (client->proxy) {
        if (client->signal_handler) {
            g_signal_handler_disconnect(client->proxy, client->signal_handler);
        }
        g_object_unref(client->proxy);
    }
    menu_node_free(client->root);
    g_free(client);
}
