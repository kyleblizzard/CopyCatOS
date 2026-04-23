// CopyCatOS — by Kyle Blizzard at Blizzard.show

// menu_model.c — Allocators and utilities for MenuNode.
//
// Small, no GLib, no X. This file is linked into both the menubar
// executable and the mb-dbusmenu-dump test helper, so it cannot
// depend on anything menubar-specific.

#include "menu_model.h"

#include <stdlib.h>
#include <string.h>

// `_File` → `File`, `__` → `_`, lone trailing `_` dropped.
// Mirrors GTK's pango_parse_markup / gtk_label behaviour closely
// enough for menu labels — we don't try to track which character
// was the accelerator, just produce a clean display string.
char *menu_strip_mnemonic(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '_') {
            if (i + 1 < n && s[i + 1] == '_') {
                out[j++] = '_';
                i++;                 // consume the second underscore
                continue;
            }
            continue;                // drop the mnemonic marker
        }
        out[j++] = s[i];
    }
    out[j] = '\0';
    return out;
}

MenuNode *menu_node_new_item(const char *label) {
    MenuNode *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->type         = MENU_ITEM_STANDARD;
    n->label        = menu_strip_mnemonic(label);
    n->enabled      = true;
    n->visible      = true;
    n->toggle       = MENU_TOGGLE_NONE;
    n->action_kind  = MENU_ACTION_NONE;
    n->legacy_id    = -1;
    return n;
}

MenuNode *menu_node_new_separator(void) {
    MenuNode *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    n->type        = MENU_ITEM_SEPARATOR;
    n->enabled     = false;
    n->visible     = true;
    n->action_kind = MENU_ACTION_NONE;
    n->legacy_id   = -1;
    return n;
}

void menu_node_free(MenuNode *node) {
    if (!node) return;
    for (int i = 0; i < node->n_children; i++) {
        menu_node_free(node->children[i]);
    }
    free(node->children);
    free(node->label);
    free(node->shortcut);
    free(node);
}

// Grow the children array on demand. Start at 4, double thereafter —
// menu trees are shallow, most branches have 2–15 siblings, so doubling
// is cheap and avoids per-insert reallocs.
void menu_node_add_child(MenuNode *parent, MenuNode *child) {
    if (!parent || !child) return;
    if (parent->n_children >= parent->children_alloc) {
        int next = parent->children_alloc ? parent->children_alloc * 2 : 4;
        MenuNode **grown = realloc(parent->children,
                                   (size_t)next * sizeof(*grown));
        if (!grown) return;          // leak the child on OOM — caller's problem
        parent->children       = grown;
        parent->children_alloc = next;
    }
    parent->children[parent->n_children++] = child;
    parent->is_submenu = true;
}

static const char *toggle_str(MenuToggleType t) {
    switch (t) {
        case MENU_TOGGLE_CHECKMARK: return " [check]";
        case MENU_TOGGLE_RADIO:     return " [radio]";
        default:                    return "";
    }
}

void menu_node_dump(const MenuNode *root, int depth, FILE *out) {
    if (!root || !out) return;
    for (int i = 0; i < depth * 2; i++) fputc(' ', out);

    if (root->type == MENU_ITEM_SEPARATOR) {
        fputs("---\n", out);
    } else {
        fprintf(out, "%s%s%s%s%s",
                root->label ? root->label : "(null)",
                root->enabled ? "" : " (disabled)",
                root->visible ? "" : " (hidden)",
                toggle_str(root->toggle),
                root->is_submenu ? " ›" : "");
        if (root->shortcut)        fprintf(out, "  [%s]", root->shortcut);
        if (root->is_legacy)       fputs("  {legacy}", out);
        if (root->legacy_id >= 0)  fprintf(out, "  #%d", root->legacy_id);
        fputc('\n', out);
    }

    for (int i = 0; i < root->n_children; i++) {
        menu_node_dump(root->children[i], depth + 1, out);
    }
}
