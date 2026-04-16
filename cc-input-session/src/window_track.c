// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

//
// window_track.c — X11 active window tracking implementation
//
// Uses XGetWindowProperty to read EWMH and ICCCM properties.
// These are low-level X11 calls that fetch raw property data from
// the X server. We parse the data ourselves rather than using
// higher-level toolkit wrappers, since cc-input-session is a
// lightweight pure-C program with no toolkit dependencies.
//

#include "window_track.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <string.h>
#include <stdio.h>

// window_track_get_active — Read _NET_ACTIVE_WINDOW from the root window.
//
// The window manager updates this property whenever focus changes.
// It contains exactly one Window ID (an unsigned long in X11 terms).
//
// We use XGetWindowProperty, which is the standard way to read any
// X11 window property. It returns raw bytes that we cast to the
// expected type.
Window window_track_get_active(Display *dpy, Window root, Atom net_active_window)
{
    // These variables receive the results of XGetWindowProperty.
    // X11's property system is generic, so we get type/format metadata
    // along with the actual data.
    Atom actual_type;         // The actual type of the property (should be XA_WINDOW)
    int actual_format;        // Bits per item: 8, 16, or 32
    unsigned long nitems;     // How many items were returned
    unsigned long bytes_after; // How many bytes remain (for partial reads)
    unsigned char *prop = NULL; // Pointer to the returned data (we must XFree this)

    // Request the _NET_ACTIVE_WINDOW property from the root window.
    // Parameters:
    //   - root: which window to read from
    //   - net_active_window: which property (atom) to read
    //   - 0, 1: read starting at offset 0, up to 1 item
    //   - False: don't delete the property after reading
    //   - XA_WINDOW: we expect the data to be a Window type
    int status = XGetWindowProperty(dpy, root, net_active_window,
                                     0, 1, False, XA_WINDOW,
                                     &actual_type, &actual_format,
                                     &nitems, &bytes_after, &prop);

    // Check if the read succeeded and we got exactly one item
    if (status != Success || nitems == 0 || prop == NULL) {
        if (prop) XFree(prop);
        return None;  // None is X11's "no window" value (0)
    }

    // The data is a Window ID stored as an unsigned long.
    // We dereference the pointer to get the value.
    Window active = *((Window *)prop);
    XFree(prop);

    return active;
}

// window_track_get_wm_class — Read and parse the WM_CLASS property.
//
// WM_CLASS is an ICCCM (Inter-Client Communication Conventions Manual)
// property. It's stored as two null-terminated ASCII strings concatenated
// together: "instance\0class\0"
//
// For example, a terminal might have:
//   instance = "xterm"
//   class    = "XTerm"
//
// This tells us both the specific program name and its broader category.
bool window_track_get_wm_class(Display *dpy, Window win,
                                char *instance, int inst_len,
                                char *class_name, int class_len)
{
    // Safety: clear the output buffers so callers always get valid strings
    if (inst_len > 0) instance[0] = '\0';
    if (class_len > 0) class_name[0] = '\0';

    // Don't try to read from the "None" window — it doesn't exist
    if (win == None) return false;

    Atom actual_type;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *prop = NULL;

    // WM_CLASS is stored as type XA_STRING with 8-bit format.
    // We request up to 256 bytes, which is more than enough for any
    // reasonable application name.
    int status = XGetWindowProperty(dpy, win, XA_WM_CLASS,
                                     0, 256, False, XA_STRING,
                                     &actual_type, &actual_format,
                                     &nitems, &bytes_after, &prop);

    if (status != Success || nitems == 0 || prop == NULL) {
        if (prop) XFree(prop);
        return false;
    }

    // Parse the two null-terminated strings.
    // The data looks like: "instance\0class\0"
    // We need to find where the first string ends and the second begins.

    // First string: the instance name (starts at byte 0)
    const char *first = (const char *)prop;
    size_t first_len = strnlen(first, nitems);

    // Copy the instance name into the output buffer, respecting the buffer size
    snprintf(instance, inst_len, "%s", first);

    // Second string: the class name (starts right after the first string's null terminator)
    // Make sure there actually IS a second string
    if (first_len + 1 < nitems) {
        const char *second = (const char *)prop + first_len + 1;
        snprintf(class_name, class_len, "%s", second);
    }

    XFree(prop);
    return true;
}
