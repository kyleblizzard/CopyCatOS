// CopyCatOS — by Kyle Blizzard at Blizzard.show

// dbusmenu_client.h — Client proxy for com.canonical.dbusmenu.
//
// Given a (service, object_path) pair returned by appmenu_bridge_lookup,
// a DbusMenuClient fetches the remote menu layout, subscribes to change
// signals, and maintains a MenuNode tree tagged with is_legacy = true
// so the renderer can tell where the menu came from.
//
// Ownership model: each DbusMenuClient owns exactly one MenuNode root.
// The root is replaced wholesale on LayoutUpdated at parent_id == 0 and
// patched in-place on ItemsPropertiesUpdated. Callers read via
// dbusmenu_client_root(), which hands back a borrowed pointer — valid
// until the next refetch or the client's destruction.
//
// Action dispatch: dbusmenu_client_activate(client, legacy_id) sends
// Event(id, "clicked", null, timestamp) to the remote service. 18-C
// wires this to click/accelerator delivery; 18-B only needs it live so
// mb-dbusmenu-dump can pulse an item end-to-end.
//
// Like appmenu_bridge, glib.h stays inside the .c. The public header is
// plain C — MenuNode + opaque DbusMenuClient handle.

#ifndef CC_MENUBAR_DBUSMENU_CLIENT_H
#define CC_MENUBAR_DBUSMENU_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "menu_model.h"

typedef struct DbusMenuClient DbusMenuClient;

// Fired whenever the client finishes rebuilding the root (initial
// GetLayout reply, LayoutUpdated refetch) or patches properties from
// ItemsPropertiesUpdated. Renderer registers once per client.
typedef void (*DbusMenuChangedFn)(DbusMenuClient *client, void *user_data);

// Connect to `service` at `object_path` on the session bus and kick
// off the initial GetLayout fetch. Returns NULL on hard error; the
// returned client is usable immediately but its root may be empty
// until the first async reply lands.
//
// `on_changed` is invoked on the GLib main context once the tree has
// been built (or rebuilt). Safe to pass NULL if the caller polls
// dbusmenu_client_root() some other way — mb-dbusmenu-dump uses the
// callback, the menubar renderer will too.
DbusMenuClient *dbusmenu_client_new(const char *service,
                                    const char *object_path,
                                    DbusMenuChangedFn on_changed,
                                    void *user_data);

// Borrowed pointer to the current root, or NULL if no layout has
// arrived yet. Do not free. Invalidated on refetch — copy fields out
// if you need to hold them across an iteration of the main loop.
const MenuNode *dbusmenu_client_root(const DbusMenuClient *client);

// Send Event(id, "clicked", null-variant, timestamp) to the remote
// service. Returns immediately; errors are logged. The dbusmenu spec
// expects clients to fire this for keyboard and mouse activation
// alike, so 18-C will reuse it for both paths.
void dbusmenu_client_activate(DbusMenuClient *client, int32_t legacy_id);

// Unsubscribe from signals, drop the proxy, free the root. Safe on NULL.
void dbusmenu_client_free(DbusMenuClient *client);

#endif // CC_MENUBAR_DBUSMENU_CLIENT_H
