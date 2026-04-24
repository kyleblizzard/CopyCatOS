// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// shellconf.h — Shell-wide persisted toggles (~/.config/copycatos/shell.conf)
// ============================================================================
//
// Two booleans for now, both defaulting to ON:
//   displays_separate_menu_bars — true → _COPYCATOS_MENUBAR_MODE = "modern"
//                                 false → "classic"
//   displays_separate_spaces    — true → _COPYCATOS_SPACES_MODE  = "per_display"
//                                 false → "global"
//
// systemcontrol loads this file at launch and re-publishes both atoms so a
// fresh X session picks up the saved choices. The Desktop & Dock pane writes
// back here and to the atoms on every toggle.
// ============================================================================

#ifndef CC_SYSPREFS_SHELLCONF_H
#define CC_SYSPREFS_SHELLCONF_H

#include <X11/Xlib.h>
#include <stdbool.h>

// Root-atom names — must match the readers in menubar and moonrock.
#define COPYCATOS_MENUBAR_MODE_ATOM_NAME "_COPYCATOS_MENUBAR_MODE"
#define COPYCATOS_SPACES_MODE_ATOM_NAME  "_COPYCATOS_SPACES_MODE"

typedef struct {
    bool displays_separate_menu_bars;
    bool displays_separate_spaces;
} ShellConf;

// Populate with defaults (both true), then overwrite from the on-disk file
// if it exists. Missing file or missing keys leave defaults in place.
void shellconf_load(ShellConf *conf);

// Atomic write: tmp file + rename. Creates ~/.config/copycatos/ if needed.
void shellconf_save(const ShellConf *conf);

// Publish both root-window atoms from the given conf. Safe to call whenever
// the values change or at startup to rehydrate a fresh session.
void shellconf_publish_atoms(Display *dpy, Window root, const ShellConf *conf);

#endif // CC_SYSPREFS_SHELLCONF_H
