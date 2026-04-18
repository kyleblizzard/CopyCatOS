// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// cc-setcursor — Force the root window cursor to the active Xcursor theme.
//
// This is a tiny utility that replaces `xsetroot -cursor_name left_ptr`.
// xsetroot isn't available on Nobara, so we do it ourselves:
//
//   1. Open the display.
//   2. Read XCURSOR_THEME from the environment (set by cc-session.sh).
//   3. Load "left_ptr" via XcursorLibraryLoadCursor (respects the theme).
//   4. Set it on the root window AND all direct children of root.
//      Setting on children covers cc-desktop's window, which sits on top
//      of root and doesn't set its own cursor.
//   5. Flush and exit.
//
// Called from cc-session.sh (after all components start) and from
// game-mode.sh (after restoring the desktop) to ensure the themed
// cursor is always visible regardless of startup timing.
//
// Usage:  cc-setcursor              (uses XCURSOR_THEME from environment)
//         cc-setcursor left_ptr     (explicit cursor name)

#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/cursorfont.h>

int main(int argc, char **argv)
{
    // Which cursor shape to load — default is the standard arrow.
    // Can be overridden on the command line for other shapes.
    const char *name = (argc > 1) ? argv[1] : "left_ptr";

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[cc-setcursor] Cannot open display\n");
        return 1;
    }

    // XcursorLibraryLoadCursor reads XCURSOR_THEME and XCURSOR_SIZE
    // from the environment, then searches the standard icon directories
    // (~/.local/share/icons/<theme>/cursors/, /usr/share/icons/..., etc.)
    Cursor cursor = XcursorLibraryLoadCursor(dpy, name);

    if (!cursor) {
        // Fallback: create a basic left-arrow from X11 core font cursors.
        // This is the generic X11 arrow — not themed, but at least visible.
        fprintf(stderr, "[cc-setcursor] Theme cursor '%s' failed, using fallback\n", name);
        cursor = XCreateFontCursor(dpy, XC_left_ptr);
    }

    if (!cursor) {
        fprintf(stderr, "[cc-setcursor] Could not create any cursor\n");
        XCloseDisplay(dpy);
        return 1;
    }

    Window root = DefaultRootWindow(dpy);

    // Set on the root window itself
    XDefineCursor(dpy, root, cursor);

    // Also set on all direct children of root. This covers cc-desktop's
    // window (which doesn't set CWCursor, so normally it inherits from root).
    // Setting it explicitly on each child ensures the themed cursor appears
    // even if inheritance is broken or delayed.
    Window parent_ret, *children = NULL;
    unsigned int nchildren = 0;
    if (XQueryTree(dpy, root, &parent_ret, &parent_ret, &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; i++) {
            XDefineCursor(dpy, children[i], cursor);
        }
        if (children) XFree(children);
    }

    XFlush(dpy);
    XCloseDisplay(dpy);
    return 0;
}
