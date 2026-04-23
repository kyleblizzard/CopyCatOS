#!/usr/bin/env python3
# CopyCatOS — by Kyle Blizzard at Blizzard.show
#
# legacy-mock-app.py — All-in-one DBusMenu test vehicle:
#   - Creates a visible X window (WM_CLASS "MockApp")
#   - Exports a menu tree on show.blizzard.DbusMenuMock /DbusMenuMock
#   - Registers that window's wid with com.canonical.AppMenu.Registrar
#   - Raises the window so the menubar sees it as _NET_ACTIVE_WINDOW
#
# This is 18-D end-to-end validation scaffolding that runs without
# depending on third-party bridge packages (appmenu-gtk-module /
# appmenu-qt5) which aren't in Nobara's default repos. A real GIMP /
# Dolphin / Inkscape test follows once those bridges land.
#
# Run:
#   python3 legacy-mock-app.py
# then click in the global menu bar at the top of the screen.
# Ctrl-C (or kill) to quit.

import os, sys, signal
from Xlib import X, display, Xatom
from Xlib.protocol import event
from gi.repository import Gio, GLib

BUS_NAME = "show.blizzard.DbusMenuMock"
OBJECT_PATH = "/DbusMenuMock"
REGISTRAR_SVC = "com.canonical.AppMenu.Registrar"
REGISTRAR_PATH = "/com/canonical/AppMenu/Registrar"
REGISTRAR_IFACE = "com.canonical.AppMenu.Registrar"

# ── Menu tree ────────────────────────────────────────────────────────
# Flat list of (id, parent_id, label, enabled, children_display, toggle,
#  toggle_state, shortcut_chord_or_None)
ITEMS = [
    (1,  0, "_File",      True,  "submenu", None,  0, None),
    (10, 1, "_New",        True,  "",        None,  0, ["Control", "N"]),
    (11, 1, "_Open...",    True,  "",        None,  0, ["Control", "O"]),
    (12, 1, "",            True,  "",        None,  0, None),   # separator
    (13, 1, "_Save",       True,  "",        None,  0, ["Control", "S"]),
    (14, 1, "Save _As...", True,  "",        None,  0, ["Control", "Shift", "S"]),
    (15, 1, "",            True,  "",        None,  0, None),   # separator
    (16, 1, "_Quit",       True,  "",        None,  0, ["Control", "Q"]),

    (2,  0, "_Edit",      True,  "submenu", None,  0, None),
    (20, 2, "_Undo",       True,  "",        None,  0, ["Control", "Z"]),
    (21, 2, "_Redo",       True,  "",        None,  0, ["Control", "Shift", "Z"]),
    (22, 2, "",            True,  "",        None,  0, None),
    (23, 2, "Cu_t",        True,  "",        None,  0, ["Control", "X"]),
    (24, 2, "_Copy",       True,  "",        None,  0, ["Control", "C"]),
    (25, 2, "_Paste",      True,  "",        None,  0, ["Control", "V"]),

    (3,  0, "_View",      True,  "submenu", None,  0, None),
    (30, 3, "Zoom _In",    True,  "",        None,  0, ["Control", "plus"]),
    (31, 3, "Zoom _Out",   True,  "",        None,  0, ["Control", "minus"]),
    (32, 3, "",            True,  "",        None,  0, None),
    (33, 3, "Show Toolbar",True,  "",        "checkmark", 1, None),
    (34, 3, "Show Sidebar",True,  "",        "checkmark", 0, None),

    (4,  0, "_Help",      True,  "submenu", None,  0, None),
    (40, 4, "About Mock App", True, "",     None,  0, None),
]

def children_of(pid):
    return [i for i in ITEMS if i[1] == pid]

def props_for(item):
    _id, _pid, label, enabled, cdisp, toggle, tstate, shortcut = item
    props = {}
    if label == "":
        props["type"] = GLib.Variant("s", "separator")
    elif label is not None:
        # Keep the mnemonic underscores — menubar strips them.
        # (The synthetic root item has label=None and wants no label prop.)
        props["label"] = GLib.Variant("s", label)
    if not enabled:
        props["enabled"] = GLib.Variant("b", False)
    if cdisp:
        props["children-display"] = GLib.Variant("s", cdisp)
    if toggle:
        props["toggle-type"] = GLib.Variant("s", toggle)
        props["toggle-state"] = GLib.Variant("i", tstate)
    if shortcut:
        # aas — array of chord (array of strings)
        props["shortcut"] = GLib.Variant("aas", [shortcut])
    return props

def build_layout(parent_id):
    # See dbusmenu-mock.py:_build_layout_pytuple for the lecture on
    # why structural slots must be plain Python, not GLib.Variants.
    item = next((i for i in ITEMS if i[0] == parent_id),
                (0, -1, None, True, "submenu", None, 0, None))
    props = props_for(item)
    kids = children_of(parent_id)
    kid_variants = [GLib.Variant("(ia{sv}av)", build_layout(k[0]))
                    for k in kids]
    return (parent_id, props, kid_variants)

# ── DBusMenu server ──────────────────────────────────────────────────
INTROSPECTION_XML = """
<node>
  <interface name='com.canonical.dbusmenu'>
    <property name='Version' type='u' access='read'/>
    <property name='Status'  type='s' access='read'/>
    <method name='GetLayout'>
      <arg type='i' name='parentId'       direction='in'/>
      <arg type='i' name='recursionDepth' direction='in'/>
      <arg type='as' name='propertyNames' direction='in'/>
      <arg type='u' name='revision'       direction='out'/>
      <arg type='(ia{sv}av)' name='layout' direction='out'/>
    </method>
    <method name='GetGroupProperties'>
      <arg type='ai' name='ids'           direction='in'/>
      <arg type='as' name='propertyNames' direction='in'/>
      <arg type='a(ia{sv})' name='properties' direction='out'/>
    </method>
    <method name='Event'>
      <arg type='i' name='id'        direction='in'/>
      <arg type='s' name='eventId'   direction='in'/>
      <arg type='v' name='data'      direction='in'/>
      <arg type='u' name='timestamp' direction='in'/>
    </method>
    <method name='AboutToShow'>
      <arg type='i' name='id'           direction='in'/>
      <arg type='b' name='needUpdate'   direction='out'/>
    </method>
    <signal name='LayoutUpdated'>
      <arg type='u' name='revision'/>
      <arg type='i' name='parent'/>
    </signal>
    <signal name='ItemsPropertiesUpdated'>
      <arg type='a(ia{sv})' name='updated'/>
      <arg type='a(ias)'    name='removed'/>
    </signal>
  </interface>
</node>
"""

class DbusMenuServer:
    def __init__(self):
        self.revision = 1

    def on_method_call(self, connection, sender, path, iface,
                       method, params, invocation):
        if method == "GetLayout":
            parent_id, depth, _props = params.unpack()
            layout = build_layout(parent_id)
            reply = GLib.Variant("(u(ia{sv}av))", (self.revision, layout))
            invocation.return_value(reply)
        elif method == "GetGroupProperties":
            ids, _keys = params.unpack()
            out = []
            for iid in ids:
                item = next((i for i in ITEMS if i[0] == iid), None)
                if item:
                    out.append((iid, props_for(item)))
            invocation.return_value(GLib.Variant("(a(ia{sv}))", (out,)))
        elif method == "Event":
            iid, ev_id, _data, _ts = params.unpack()
            print(f"[mock] Event id={iid} eventId={ev_id}")
            invocation.return_value(None)
        elif method == "AboutToShow":
            iid = params.unpack()[0]
            print(f"[mock] AboutToShow id={iid}")
            invocation.return_value(GLib.Variant("(b)", (False,)))

    def on_get_property(self, connection, sender, path, iface, prop):
        if prop == "Version":
            return GLib.Variant("u", 3)
        if prop == "Status":
            return GLib.Variant("s", "normal")
        return None

# ── X window + Registrar registration ────────────────────────────────
class MockWindow:
    def __init__(self, dpy):
        self.dpy = dpy
        self.screen = dpy.screen()
        self.win = self.screen.root.create_window(
            100, 100, 480, 320, 2,
            self.screen.root_depth,
            X.InputOutput,
            X.CopyFromParent,
            background_pixel=self.screen.white_pixel,
            event_mask=X.ExposureMask | X.KeyPressMask | X.StructureNotifyMask)
        self.win.set_wm_name("DBusMenu Mock App")
        self.win.set_wm_class("MockApp", "MockApp")
        # Hint: dialog so moonrock won't try to struts-inset it.
        self.win.map()
        dpy.sync()
        self.wid = self.win.id
        print(f"[mock] X window wid=0x{self.wid:x}")

    def raise_and_focus(self):
        # Ask the WM to activate us via _NET_ACTIVE_WINDOW client message.
        NET_ACTIVE = self.dpy.intern_atom("_NET_ACTIVE_WINDOW")
        root = self.screen.root
        data = (32, [1, X.CurrentTime, 0, 0, 0])  # source=1 (application)
        msg = event.ClientMessage(
            window=self.win, client_type=NET_ACTIVE, data=data)
        root.send_event(msg,
            event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
        self.dpy.sync()

# ── Main ─────────────────────────────────────────────────────────────
def register_window(wid):
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    bus.call_sync(REGISTRAR_SVC, REGISTRAR_PATH, REGISTRAR_IFACE,
                  "RegisterWindow",
                  GLib.Variant("(uo)", (wid, OBJECT_PATH)),
                  None, Gio.DBusCallFlags.NONE, -1, None)
    print(f"[mock] RegisterWindow wid=0x{wid:x} path={OBJECT_PATH}")

def pump_x(dpy):
    # Drain any queued X events so moonrock / wm don't get backed up.
    while dpy.pending_events():
        dpy.next_event()
    return True

def main():
    dpy = display.Display()
    window = MockWindow(dpy)

    server = DbusMenuServer()
    node_info = Gio.DBusNodeInfo.new_for_xml(INTROSPECTION_XML)

    def on_bus_acquired(connection, name):
        print(f"[mock] bus acquired: {name}")
        connection.register_object(
            OBJECT_PATH,
            node_info.interfaces[0],
            server.on_method_call,
            server.on_get_property,
            None)

    def on_name_acquired(connection, name):
        print(f"[mock] name acquired: {name}")
        # Now that our menu service is live, ask the Registrar to link
        # our X window to it. Slight delay so moonrock has a chance to
        # reparent + map us.
        GLib.timeout_add(300, lambda: (register_window(window.wid),
                                       window.raise_and_focus(),
                                       False)[-1])

    Gio.bus_own_name(Gio.BusType.SESSION, BUS_NAME,
                     Gio.BusNameOwnerFlags.NONE,
                     on_bus_acquired, on_name_acquired, None)

    # Poll X events 30×/s so the window stays responsive.
    GLib.timeout_add(33, pump_x, dpy)

    loop = GLib.MainLoop()
    signal.signal(signal.SIGINT,  lambda *_: loop.quit())
    signal.signal(signal.SIGTERM, lambda *_: loop.quit())
    loop.run()

if __name__ == "__main__":
    main()
