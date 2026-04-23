#!/usr/bin/env python3
# CopyCatOS — by Kyle Blizzard at Blizzard.show
#
# dbusmenu-mock.py — Minimal com.canonical.dbusmenu server for
# exercising mb-dbusmenu-dump without installing a full Legacy Mode
# GTK/Qt app. Exports a representative tree covering the cases the
# DBusMenu client in menubar has to parse: mnemonics, shortcuts,
# separators, submenus, disabled items, checkmark + radio toggles.
#
# This is 18-B test scaffolding. 18-D uses the same dump tool against
# real apps (GIMP, Dolphin, Inkscape) for the end-user-facing proof.
#
# Run:
#   python3 dbusmenu-mock.py
# then in another shell:
#   mb-dbusmenu-dump show.blizzard.DbusMenuMock /DbusMenuMock
#   mb-dbusmenu-dump --watch show.blizzard.DbusMenuMock /DbusMenuMock
#
# Ctrl-C to quit. Sends a LayoutUpdated every 4s to exercise the
# refetch path.

from gi.repository import Gio, GLib

BUS_NAME    = "show.blizzard.DbusMenuMock"
OBJECT_PATH = "/DbusMenuMock"

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
      <arg type='i' name='id'     direction='in'/>
      <arg type='b' name='needUpdate' direction='out'/>
    </method>

    <signal name='LayoutUpdated'>
      <arg type='u' name='revision'/>
      <arg type='i' name='parent'/>
    </signal>

    <signal name='ItemsPropertiesUpdated'>
      <arg type='a(ia{sv})' name='updatedProps'/>
      <arg type='a(ias)'    name='removedProps'/>
    </signal>
  </interface>
</node>
"""

# A modest but realistic tree. id 0 is the synthetic root. Every item
# carries just enough props to exercise one translation case.
TREE = {
    0: {"children": [1, 2, 3]},
    1: {"label": "_File", "children-display": "submenu", "children": [10, 11, 12, 13]},
    2: {"label": "_Edit", "children-display": "submenu", "children": [20, 21, 22, 23]},
    3: {"label": "_View", "children-display": "submenu", "children": [30, 31]},

    10: {"label": "_New",     "shortcut": [["Control", "N"]]},
    11: {"label": "_Open...", "shortcut": [["Control", "O"]]},
    12: {"type": "separator"},
    13: {"label": "_Save",    "shortcut": [["Control", "S"]], "enabled": False},

    20: {"label": "_Undo",    "shortcut": [["Control", "Z"]]},
    21: {"label": "_Redo",    "shortcut": [["Control", "Shift", "Z"]]},
    22: {"type": "separator"},
    23: {"label": "Show Toolbar",
         "toggle-type": "checkmark",
         "toggle-state": 1},

    30: {"label": "Small Icons",
         "toggle-type": "radio", "toggle-state": 0},
    31: {"label": "Large Icons",
         "toggle-type": "radio", "toggle-state": 1},
}

def _variant_for_value(v):
    # Best-effort mapping of Python primitives to the GVariant types
    # dbusmenu property slots expect. Only the keys we actually use
    # need to be correct.
    if isinstance(v, bool):
        return GLib.Variant("b", v)
    if isinstance(v, int):
        return GLib.Variant("i", v)
    if isinstance(v, str):
        return GLib.Variant("s", v)
    if isinstance(v, list):
        # shortcut: aas
        return GLib.Variant("aas", v)
    raise TypeError(f"unhandled property value: {type(v).__name__}")

def _props_dict_for(item_id):
    spec = TREE.get(item_id, {})
    out = {}
    for k, v in spec.items():
        if k == "children":
            continue
        out[k] = _variant_for_value(v)
    return out

def _build_layout_pytuple(item_id, depth):
    """Returns a plain Python tuple (int, dict, list) matching the
    dbusmenu `(ia{sv}av)` shape:
      - id is a raw int
      - props is {str: GLib.Variant}   (Variants only because a{sv} needs them)
      - children is [GLib.Variant, …]  (Variants only because each av element is one)

    PyGObject's Variant constructor iterates/unpacks any Variant it
    receives for a structural-type slot, then tries to re-pack — that
    round-trip collapses at the `v` leaf. Keeping structural slots as
    plain Python avoids the whole trap."""
    props = _props_dict_for(item_id)

    children_variants = []
    children_spec = TREE.get(item_id, {}).get("children", [])
    if depth != 0:
        next_depth = -1 if depth < 0 else depth - 1
        for cid in children_spec:
            inner_tuple = _build_layout_pytuple(cid, next_depth)
            children_variants.append(
                GLib.Variant("(ia{sv}av)", inner_tuple))

    return (item_id, props, children_variants)

class MockServer:
    def __init__(self):
        self.connection = None
        self.reg_id     = None
        self.revision   = 1
        self.tick       = 0
        self.node_info  = Gio.DBusNodeInfo.new_for_xml(INTROSPECTION_XML)

    def run(self):
        owner_id = Gio.bus_own_name(
            Gio.BusType.SESSION, BUS_NAME,
            Gio.BusNameOwnerFlags.NONE,
            self._on_bus_acquired, None, self._on_name_lost)

        loop = GLib.MainLoop()

        # Every 4s, bump the revision and fire LayoutUpdated so the
        # dump tool's --watch mode has something to refetch.
        GLib.timeout_add_seconds(4, self._nudge)

        try:
            loop.run()
        except KeyboardInterrupt:
            pass
        finally:
            Gio.bus_unown_name(owner_id)

    # PyGObject strips user_data before invoking these callbacks, so
    # the signatures here do not mirror the C three-argument form.
    def _on_bus_acquired(self, connection, _name):
        self.connection = connection
        self.reg_id = connection.register_object(
            OBJECT_PATH,
            self.node_info.interfaces[0],
            self._on_method_call,
            self._on_get_property,
            None)
        print(f"[mock] registered at {BUS_NAME} {OBJECT_PATH}", flush=True)

    def _on_name_lost(self, *_a):
        print("[mock] name lost", flush=True)

    def _on_get_property(self, _c, _s, _o, _i, name):
        if name == "Version": return GLib.Variant("u", 3)
        if name == "Status":  return GLib.Variant("s", "normal")
        return None

    def _on_method_call(self, _c, _s, _o, _i, method, params, invocation):
        if method == "GetLayout":
            parent_id, depth, _props_requested = params
            layout_tuple = _build_layout_pytuple(parent_id, depth)
            invocation.return_value(
                GLib.Variant("(u(ia{sv}av))",
                             (self.revision, layout_tuple)))
            return

        if method == "GetGroupProperties":
            ids, _names = params
            entries = []
            for i in ids:
                entries.append((i, _props_dict_for(i)))
            invocation.return_value(GLib.Variant("a(ia{sv})", entries))
            return

        if method == "Event":
            item_id, event_id, _data, _ts = params
            print(f"[mock] Event id={item_id} event_id={event_id}")
            invocation.return_value(None)
            return

        if method == "AboutToShow":
            invocation.return_value(GLib.Variant("(b)", (False,)))
            return

        invocation.return_error_literal(
            Gio.dbus_error_quark(),
            Gio.DBusError.UNKNOWN_METHOD,
            f"unknown method: {method}")

    def _nudge(self):
        if not self.connection:
            return True

        self.tick += 1

        # Alternate between the two refresh paths so both client code
        # branches get exercised in a single --watch run:
        #   even ticks → LayoutUpdated  (full refetch)
        #   odd ticks  → ItemsPropertiesUpdated  (in-place patch)
        if self.tick % 2 == 0:
            # Flip the checkmark state for something observable.
            TREE[23]["toggle-state"] = 1 - TREE[23]["toggle-state"]
            self.revision += 1
            self.connection.emit_signal(
                None, OBJECT_PATH, "com.canonical.dbusmenu",
                "LayoutUpdated",
                GLib.Variant("(ui)", (self.revision, 0)))
        else:
            # Patch item 11's label in place — no tree topology change,
            # so the client should NOT refetch the layout. The prop
            # update alone should produce a new dump.
            new_label = "_Open... (patched)" if self.tick % 4 == 1 else "_Open..."
            TREE[11]["label"] = new_label
            updated = [(11, {"label": GLib.Variant("s", new_label)})]
            removed = []   # a(ias) — no props removed
            self.connection.emit_signal(
                None, OBJECT_PATH, "com.canonical.dbusmenu",
                "ItemsPropertiesUpdated",
                GLib.Variant("(a(ia{sv})a(ias))", (updated, removed)))
        return True

if __name__ == "__main__":
    MockServer().run()
