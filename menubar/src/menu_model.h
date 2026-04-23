// CopyCatOS — by Kyle Blizzard at Blizzard.show

// menu_model.h — Source-agnostic menu tree used by the menubar.
//
// The bar's renderer needs one shape to walk whether a menu came from a
// native MoonBase app or was imported over DBusMenu from a Legacy Mode
// GTK3 / Qt5 app. MenuNode is that shape: a tree of items with labels,
// shortcuts, enabled/visible/toggle state, and a tagged-union action.
//
// The native path still runs on the static `const char **` arrays in
// appmenu.c for now — MenuNode is the supplier-neutral surface 18-B's
// DBusMenu client feeds, and 18-C will migrate the dropdown walker onto.
// The `is_legacy` flag lets the renderer pick fallback rules (missing
// accelerator glyphs, icon sources) without the source coupling leaking
// into the model itself.

#ifndef CC_MENUBAR_MENU_MODEL_H
#define CC_MENUBAR_MENU_MODEL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    MENU_ITEM_STANDARD  = 0,
    MENU_ITEM_SEPARATOR = 1,
} MenuItemType;

typedef enum {
    MENU_TOGGLE_NONE      = 0,
    MENU_TOGGLE_CHECKMARK = 1,
    MENU_TOGGLE_RADIO     = 2,
} MenuToggleType;

typedef enum {
    MENU_ACTION_NONE   = 0,   // inert — submenu parent, separator, or a node
                              // whose dispatch isn't wired up yet
    MENU_ACTION_LEGACY = 1,   // dispatch via the owning DbusMenuClient using
                              // legacy_id
    MENU_ACTION_NATIVE = 2,   // dispatch via native callback — reserved for
                              // when appmenu.c migrates onto this model
} MenuActionKind;

typedef struct MenuNode MenuNode;

struct MenuNode {
    MenuItemType    type;
    char           *label;         // NULL for separators; mnemonic already stripped
    char           *shortcut;      // raw "Ctrl+S" style from the source — 18-C
                                   // converts to Aqua glyphs. NULL if none.
    bool            enabled;
    bool            visible;
    MenuToggleType  toggle;
    int             toggle_state;  // 0 / 1 (checkmark) or index (radio group)
    bool            is_submenu;    // has children meant to open as a submenu
    bool            is_legacy;     // supplier tag for renderer fallback rules

    MenuActionKind  action_kind;
    int32_t         legacy_id;     // DBusMenu item id when action_kind==LEGACY

    MenuNode      **children;
    int             n_children;
    int             children_alloc;
};

// Allocators. Ownership: parent owns children; freeing the root frees the
// subtree. Labels/shortcuts passed in are copied — callers retain theirs.
MenuNode *menu_node_new_item(const char *label);
MenuNode *menu_node_new_separator(void);
void      menu_node_free(MenuNode *node);
void      menu_node_add_child(MenuNode *parent, MenuNode *child);

// Strip GTK-style mnemonics: `_File` → `File`, `__` → `_`. Returns a newly
// allocated string (caller frees) even when no mnemonic was present. Safe
// on NULL (returns NULL).
char     *menu_strip_mnemonic(const char *s);

// Pretty-print the tree to `out` indented by depth*2 spaces. Used by
// mb-dbusmenu-dump and debug logging.
void      menu_node_dump(const MenuNode *root, int depth, FILE *out);

#endif // CC_MENUBAR_MENU_MODEL_H
