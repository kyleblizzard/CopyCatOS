// CopyCatOS — by Kyle Blizzard at Blizzard.show

// appmenu_bridge.h — DBusMenu / AppMenu.Registrar bridge for Legacy Mode
// apps (GTK3 / Qt5 / Qt6 with appmenu module).
//
// Native MoonBase apps talk to the menubar through the existing MoonRock
// IPC channel and are untouched by this bridge. Legacy X11 clients with
// the right env vars (GTK: `gtk-shell-shows-menubar=1`; Qt:
// `QT_QPA_PLATFORMTHEME=appmenu-qt5`) export their menu tree on the
// session bus under `com.canonical.AppMenu.Registrar`. We own that
// service, accept RegisterWindow calls, and keep a WID→(service,path)
// table that slice 18-B will consume.
//
// Slice 18-A delivers only the registrar: the name-claim, the object at
// /com/canonical/AppMenu/Registrar, the three methods, and the event-loop
// integration so GDBus work drains without extra latency. No menu
// fetching, no rendering, no menubar visual change yet.
//
// GDBus + GLib only exist inside this translation unit — no other
// menubar module includes glib.h. The select-loop integration is exposed
// through two opaque wrappers so menubar.c doesn't need to know about
// GPollFDs.

#ifndef CC_MENUBAR_APPMENU_BRIDGE_H
#define CC_MENUBAR_APPMENU_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/select.h>

#include "menubar.h"

// Claim `com.canonical.AppMenu.Registrar` on the session bus and start
// handling registrar method calls. Returns true if bridge state was
// set up. Returns true even if another owner already has the name —
// in that case a warning is logged and the bridge enters a nil state
// where lookups always fail; this is by design so a KDE dev machine
// (kwin owns the name) doesn't block menubar startup. Returns false
// only on hard errors (cannot connect to session bus, out of memory).
bool appmenu_bridge_init(MenuBar *mb);

// Fold the GLib main context's file descriptors into an existing
// select fd_set. Must be called each iteration, before select(). Grows
// *max_fd and shortens *timeout_ms as needed so GDBus traffic wakes
// the loop promptly instead of waiting for menubar's 500ms tick.
// No-op if the bridge is in the nil state or not initialised.
void appmenu_bridge_prepare_select(fd_set *readfds,
                                   int *max_fd,
                                   int *timeout_ms);

// Dispatch any GLib sources that became ready during the preceding
// select(). Must be called each iteration, after select() returns.
// No-op if the bridge is in the nil state or not initialised.
void appmenu_bridge_dispatch(fd_set *readfds);

// Look up the dbus (service, object-path) pair an app registered for
// the given X11 window id. Returns true on hit; *service and *path
// point at bridge-owned storage valid until the next Unregister or
// shutdown. Returns false if unknown or if the bridge is nil.
//
// 18-B will call this when the active window changes to decide
// whether to build a legacy menu model for it.
bool appmenu_bridge_lookup(uint32_t wid,
                           const char **service,
                           const char **path);

// Release the name, tear down the registrar, free the WID table.
// Safe to call from a signal-triggered shutdown path.
void appmenu_bridge_shutdown(MenuBar *mb);

#endif // CC_MENUBAR_APPMENU_BRIDGE_H
