// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ─── spotlight.c ───
// Core overlay window, hotkey registration, and event dispatch.
//
// This is the glue that ties everything together:
//
//   - Creates a 32-bit ARGB override-redirect window (no WM frame,
//     supports per-pixel transparency).
//   - Grabs Ctrl+Space on the root window so we get a global hotkey
//     even when another app is focused.
//   - Toggles the overlay visible/hidden on each hotkey press.
//   - When visible, grabs the keyboard and routes keystrokes to
//     the search module and text-input handler.
//   - After each state change, asks the render module to repaint.
//
// The event loop uses select() on the X11 connection file
// descriptor so we can wake up on I/O without busy-waiting.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include "spotlight.h"
#include "search.h"
#include "render.h"
#include "icons.h"

// ──────────────────────────────────────────────
// Internal state
// ──────────────────────────────────────────────

// Maximum length of the user's search query.
#define MAX_QUERY_LEN 512

// The query the user has typed so far.
static char query[MAX_QUERY_LEN] = {0};

// Current cursor position (always at the end of the string
// for this simple implementation).
static int query_len = 0;

// Whether "select all" mode is active — the next printable
// keystroke replaces the entire query instead of appending.
static int select_all = 0;

// The set of results matching the current query.
static SearchEntry *results[MAX_SEARCH_RESULTS];
static int result_count = 0;

// Index of the currently highlighted result row.
static int selected = 0;

// Whether the overlay is currently visible (mapped).
static int visible = 0;

// Flag set by the signal handler to request a clean exit.
static volatile sig_atomic_t quit_flag = 0;

// X11 resources.
static Window  overlay_win   = None;
static int     screen_width  = 0;
static int     screen_height = 0;
static Colormap colormap     = None;

// Cairo surface that wraps the overlay window's pixels.
static cairo_surface_t *surface = NULL;

// ──────────────────────────────────────────────
// Quit flag
// ──────────────────────────────────────────────

void spotlight_request_quit(void) {
    quit_flag = 1;
}

// ──────────────────────────────────────────────
// Window size calculation
// ──────────────────────────────────────────────

// The overlay window needs extra space around the content area
// for the drop shadow (about 28px on each side).
#define SHADOW_PAD 28

// Compute the total window height based on how many result
// rows are currently showing.
static int calc_window_height(int num_results) {
    int visible_rows = num_results;
    if (visible_rows > MAX_VISIBLE_RESULTS)
        visible_rows = MAX_VISIBLE_RESULTS;

    // Content height = search field + result rows + 16px padding.
    int content_h = SEARCH_HEIGHT + visible_rows * RESULT_HEIGHT + 16;

    // Add shadow padding on top and bottom.
    return content_h + 2 * SHADOW_PAD;
}

// Total window width including shadow padding.
static int calc_window_width(void) {
    return SPOTLIGHT_WIDTH + 2 * SHADOW_PAD;
}

// ──────────────────────────────────────────────
// ARGB visual lookup
// ──────────────────────────────────────────────

// X11 windows are normally 24-bit RGB.  To get per-pixel
// transparency we need to find a "visual" (pixel format) that
// has a 32-bit depth with an alpha channel.  This function
// scans the available visuals and returns the first match, or
// NULL if the X server doesn't support compositing.
static Visual *find_argb_visual(Display *dpy, int screen, int *depth_out) {
    XVisualInfo tpl;
    tpl.screen = screen;
    tpl.depth  = 32;
    tpl.class = TrueColor;

    int count = 0;
    XVisualInfo *infos = XGetVisualInfo(dpy,
        VisualScreenMask | VisualDepthMask | VisualClassMask,
        &tpl, &count);

    if (!infos || count == 0) return NULL;

    Visual *visual = infos[0].visual;
    *depth_out = 32;
    XFree(infos);
    return visual;
}

// ──────────────────────────────────────────────
// Overlay show / hide
// ──────────────────────────────────────────────

// Repaint the overlay contents.  Called after any state change
// (query change, selection change, window resize).
static void repaint(Display *dpy) {
    int win_w = calc_window_width();
    int win_h = calc_window_height(result_count);

    // Recreate the Cairo surface at the new window size.
    // (Cairo-Xlib surfaces don't automatically resize.)
    if (surface) {
        cairo_surface_destroy(surface);
        surface = NULL;
    }

    surface = cairo_xlib_surface_create(dpy, overlay_win,
        DefaultVisual(dpy, DefaultScreen(dpy)),  // placeholder — replaced below
        win_w, win_h);

    // We actually need the ARGB visual, not the default visual.
    // But cairo_xlib_surface_create requires matching the window's
    // visual.  We stored the correct visual in the window attributes,
    // so let's query it.
    XWindowAttributes wa;
    XGetWindowAttributes(dpy, overlay_win, &wa);

    cairo_surface_destroy(surface);
    surface = cairo_xlib_surface_create(dpy, overlay_win,
        wa.visual, win_w, win_h);

    cairo_t *cr = cairo_create(surface);

    render_frame(cr, win_w, win_h, query, results, result_count, selected);

    cairo_destroy(cr);
    cairo_surface_flush(surface);
}

// Show the overlay: map the window, grab the keyboard, and
// repaint.
static void show_overlay(Display *dpy) {
    if (visible) return;
    visible = 1;

    // Reset input state.
    query[0]     = '\0';
    query_len    = 0;
    select_all   = 0;
    result_count = 0;
    selected     = 0;

    // Position the window: centred horizontally, 22% from the top.
    int win_w = calc_window_width();
    int win_h = calc_window_height(0); // no results initially

    int x = (screen_width  - win_w) / 2;
    int y = (int)(screen_height * SPOTLIGHT_TOP_RATIO) - SHADOW_PAD;
    if (y < 0) y = 0;

    XMoveResizeWindow(dpy, overlay_win, x, y, (unsigned)win_w, (unsigned)win_h);
    XMapRaised(dpy, overlay_win);

    // Grab the keyboard so all keystrokes come to us while
    // the overlay is open.
    //
    // IMPORTANT: owner_events must be False here.
    // With True, keystrokes are still delivered to whatever window
    // currently has focus (e.g., Konsole behind us) — the grab only
    // intercepts events that would otherwise go unhandled.
    // With False, ALL keyboard events are delivered exclusively to
    // overlay_win regardless of which window has X input focus.
    // That is what we need for a proper modal search overlay.
    int grab_result = XGrabKeyboard(dpy, overlay_win, False,
                                    GrabModeAsync, GrabModeAsync, CurrentTime);
    if (grab_result != GrabSuccess) {
        fprintf(stderr, "searchsystem: XGrabKeyboard failed (%d) — "
                        "keyboard input may not work\n", grab_result);
    }

    // Also explicitly take input focus so that window managers and
    // other apps know where keyboard input is going.  PointerRoot
    // as the revert-to target means focus returns to the window
    // under the pointer when we release the grab.
    XSetInputFocus(dpy, overlay_win, RevertToPointerRoot, CurrentTime);

    repaint(dpy);
    XFlush(dpy);
}

// Hide the overlay: unmap the window and release the keyboard.
static void hide_overlay(Display *dpy) {
    if (!visible) return;
    visible = 0;

    XUngrabKeyboard(dpy, CurrentTime);
    XUnmapWindow(dpy, overlay_win);
    XFlush(dpy);

    // Clean up the Cairo surface while the window is hidden.
    if (surface) {
        cairo_surface_destroy(surface);
        surface = NULL;
    }
}

// ──────────────────────────────────────────────
// Application launching
// ──────────────────────────────────────────────

// Strip freedesktop field codes from an Exec string.
// Field codes like %u, %F, %i etc. are placeholders that a
// full desktop launcher would replace with file paths or URLs.
// We just remove them since we're launching without arguments.
static void strip_field_codes(char *exec) {
    char clean[512];
    int  ci = 0;

    for (int i = 0; exec[i] != '\0' && ci < (int)sizeof(clean) - 1; ) {
        if (exec[i] == '%' && exec[i + 1] != '\0') {
            char code = exec[i + 1];
            // List of all standard freedesktop field codes.
            if (code == 'u' || code == 'U' ||
                code == 'f' || code == 'F' ||
                code == 'd' || code == 'D' ||
                code == 'n' || code == 'N' ||
                code == 'i' || code == 'c' ||
                code == 'k' || code == 'v' ||
                code == 'm') {
                i += 2; // skip the % and the code letter
                // Also skip a trailing space so we don't get
                // double-spaces in the command.
                if (exec[i] == ' ') i++;
                continue;
            }
        }
        clean[ci++] = exec[i++];
    }
    clean[ci] = '\0';

    // Copy the cleaned string back.
    strncpy(exec, clean, 511);
    exec[511] = '\0';
}

// Launch the application described by the given search entry.
// We fork a child process, start a new session (setsid), and
// exec through /bin/sh for shell expansion of the command.
static void launch_entry(SearchEntry *entry) {
    char exec[512];
    strncpy(exec, entry->exec, sizeof(exec) - 1);
    exec[sizeof(exec) - 1] = '\0';
    strip_field_codes(exec);

    pid_t pid = fork();
    if (pid == 0) {
        // ── Child process ──

        // Detach from the parent's terminal and process group
        // so the launched app doesn't die when searchsystem
        // exits.
        setsid();

        // Close all inherited file descriptors to avoid leaking
        // the X11 connection socket to the child.
        for (int fd = 3; fd < 1024; fd++) {
            close(fd);
        }

        // Redirect stdin/stdout/stderr to /dev/null so the
        // child doesn't spam our terminal.
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        // Use /bin/sh to execute the command, which handles
        // things like environment variable expansion and pipes.
        execl("/bin/sh", "sh", "-c", exec, (char *)NULL);

        // If exec fails, just exit quietly.
        _exit(127);
    }

    // Parent: we don't wait for the child — it runs independently.
    // Reap any zombie children from previous launches.
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        // keep reaping
    }
}

// ──────────────────────────────────────────────
// Update search results
// ──────────────────────────────────────────────

// Re-run the search with the current query and resize the
// window to fit the new result count.
static void update_results(Display *dpy) {
    result_count = search_query(query, results, MAX_SEARCH_RESULTS);

    // Reset selection to the first result.
    selected = 0;

    // Resize the window to fit the new number of results.
    int win_w = calc_window_width();
    int win_h = calc_window_height(result_count);

    int x = (screen_width - win_w) / 2;
    int y = (int)(screen_height * SPOTLIGHT_TOP_RATIO) - SHADOW_PAD;
    if (y < 0) y = 0;

    XMoveResizeWindow(dpy, overlay_win, x, y, (unsigned)win_w, (unsigned)win_h);
    repaint(dpy);
    XFlush(dpy);
}

// ──────────────────────────────────────────────
// Keyboard input handling
// ──────────────────────────────────────────────

// Process a KeyPress event while the overlay is visible.
// Returns 1 if the event was handled, 0 otherwise.
static int handle_key(Display *dpy, XKeyEvent *ev) {
    // Use XLookupString to translate the key event into a
    // character (taking modifier keys into account).
    char buf[32] = {0};
    KeySym sym   = NoSymbol;
    int len      = XLookupString(ev, buf, sizeof(buf) - 1, &sym, NULL);

    // ── Escape — hide the overlay ──
    if (sym == XK_Escape) {
        hide_overlay(dpy);
        return 1;
    }

    // ── Return/Enter — launch the selected app ──
    if (sym == XK_Return || sym == XK_KP_Enter) {
        if (result_count > 0 && selected >= 0 && selected < result_count) {
            launch_entry(results[selected]);
        }
        hide_overlay(dpy);
        return 1;
    }

    // ── Arrow keys — navigate results ──
    if (sym == XK_Up) {
        if (result_count > 0) {
            int vis = result_count < MAX_VISIBLE_RESULTS
                      ? result_count : MAX_VISIBLE_RESULTS;
            selected = (selected - 1 + vis) % vis; // wrap to bottom
            repaint(dpy);
            XFlush(dpy);
        }
        return 1;
    }
    if (sym == XK_Down) {
        if (result_count > 0) {
            int vis = result_count < MAX_VISIBLE_RESULTS
                      ? result_count : MAX_VISIBLE_RESULTS;
            selected = (selected + 1) % vis; // wrap to top
            repaint(dpy);
            XFlush(dpy);
        }
        return 1;
    }

    // ── Ctrl+A — select all ──
    if ((ev->state & ControlMask) && (sym == XK_a || sym == XK_A)) {
        select_all = 1;
        return 1;
    }

    // ── Backspace — delete one character (or clear if select-all) ──
    if (sym == XK_BackSpace) {
        if (select_all) {
            // "Select all" was active — clear the whole query.
            query[0]  = '\0';
            query_len = 0;
            select_all = 0;
        } else if (query_len > 0) {
            query[--query_len] = '\0';
        }
        update_results(dpy);
        return 1;
    }

    // ── Printable character — append to query ──
    if (len > 0 && !iscntrl((unsigned char)buf[0])) {
        if (select_all) {
            // Replace the entire query with this character.
            query[0]   = buf[0];
            query[1]   = '\0';
            query_len  = 1;
            select_all = 0;
        } else if (query_len < MAX_QUERY_LEN - 1) {
            query[query_len++] = buf[0];
            query[query_len]   = '\0';
        }
        update_results(dpy);
        return 1;
    }

    return 0;
}

// ──────────────────────────────────────────────
// Hotkey registration
// ──────────────────────────────────────────────

// Grab Ctrl+Space on the root window.  We register the same
// key with several modifier combinations so the hotkey works
// regardless of whether NumLock or CapsLock is engaged.
static void grab_hotkey(Display *dpy) {
    Window root = DefaultRootWindow(dpy);

    // XKeysymToKeycode translates the abstract "space" symbol
    // to the physical keycode on this keyboard.
    KeyCode space = XKeysymToKeycode(dpy, XK_space);
    if (space == 0) {
        fprintf(stderr, "searchsystem: cannot find keycode for Space\n");
        return;
    }

    // Modifier masks for NumLock and CapsLock.  These vary by
    // system but are conventionally Mod2Mask and LockMask.
    unsigned int numlock  = Mod2Mask;
    unsigned int capslock = LockMask;

    // Grab with all four combinations of numlock/capslock state.
    XGrabKey(dpy, space, ControlMask,
             root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, space, ControlMask | numlock,
             root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, space, ControlMask | capslock,
             root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, space, ControlMask | numlock | capslock,
             root, True, GrabModeAsync, GrabModeAsync);

    printf("searchsystem: hotkey Ctrl+Space grabbed\n");
}

// Release all our key grabs.
static void ungrab_hotkey(Display *dpy) {
    Window root = DefaultRootWindow(dpy);
    KeyCode space = XKeysymToKeycode(dpy, XK_space);
    if (space == 0) return;

    unsigned int numlock  = Mod2Mask;
    unsigned int capslock = LockMask;

    XUngrabKey(dpy, space, ControlMask, root);
    XUngrabKey(dpy, space, ControlMask | numlock, root);
    XUngrabKey(dpy, space, ControlMask | capslock, root);
    XUngrabKey(dpy, space, ControlMask | numlock | capslock, root);
}

// ──────────────────────────────────────────────
// Initialisation
// ──────────────────────────────────────────────

int spotlight_init(Display *dpy) {
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    screen_width  = DisplayWidth(dpy, screen);
    screen_height = DisplayHeight(dpy, screen);

    // ── Find a 32-bit ARGB visual ──
    int depth = 24;
    Visual *visual = find_argb_visual(dpy, screen, &depth);
    if (!visual) {
        fprintf(stderr, "searchsystem: no 32-bit ARGB visual found, "
                        "falling back to default\n");
        visual = DefaultVisual(dpy, screen);
        depth  = DefaultDepth(dpy, screen);
    }

    // Create a colourmap for our visual.  The default colourmap
    // only works with the default visual, so we need our own.
    colormap = XCreateColormap(dpy, root, visual, AllocNone);

    // ── Create the overlay window ──
    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));

    // Override-redirect means the window manager won't add a
    // title bar or borders — we handle all our own decoration.
    attrs.override_redirect = True;

    // Use our ARGB colourmap.
    attrs.colormap = colormap;

    // We want to start with a transparent background.
    attrs.background_pixel = 0;

    // Request key press, exposure, and focus events.
    // ButtonPressMask lets us detect clicks outside the overlay so
    // we can dismiss it (real Spotlight closes on outside click).
    attrs.event_mask = KeyPressMask | ExposureMask | StructureNotifyMask |
                       FocusChangeMask;

    // Don't inherit the parent's background — avoids flicker.
    attrs.border_pixel = 0;

    int win_w = calc_window_width();
    int win_h = calc_window_height(0);
    int win_x = (screen_width - win_w) / 2;
    int win_y = (int)(screen_height * SPOTLIGHT_TOP_RATIO) - SHADOW_PAD;
    if (win_y < 0) win_y = 0;

    overlay_win = XCreateWindow(
        dpy, root,
        win_x, win_y,
        (unsigned)win_w, (unsigned)win_h,
        0,                              // border width
        depth,                          // depth (32 for ARGB)
        InputOutput,                    // window class
        visual,                         // our ARGB visual
        CWOverrideRedirect | CWColormap | CWBackPixel |
        CWEventMask | CWBorderPixel,
        &attrs
    );

    // Set the WM_NAME so the window shows up nicely in tools
    // like xwininfo / xdotool.
    XStoreName(dpy, overlay_win, "searchsystem");

    // ── Initialise subsystems ──
    search_init();

    // ── Grab the global hotkey ──
    grab_hotkey(dpy);

    // The window starts hidden — we don't map it until the
    // user presses Ctrl+Space.

    return 0;
}

// ──────────────────────────────────────────────
// Event loop
// ──────────────────────────────────────────────

void spotlight_run(Display *dpy) {
    // Get the file descriptor for the X11 connection so we
    // can use select() to wait for events without busy-looping.
    int xfd = ConnectionNumber(dpy);

    while (!quit_flag) {
        // ── Process any queued X events ──
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            switch (ev.type) {
            case KeyPress: {
                // Check if this is our global hotkey (Ctrl+Space).
                // When the overlay is hidden, the only KeyPress we
                // receive is the grabbed Ctrl+Space.  When visible,
                // all keys come through the keyboard grab.
                KeySym sym = XLookupKeysym(&ev.xkey, 0);

                if (sym == XK_space &&
                    (ev.xkey.state & ControlMask) &&
                    !(ev.xkey.state & (Mod1Mask | Mod4Mask | ShiftMask))) {
                    // Toggle visibility.
                    if (visible)
                        hide_overlay(dpy);
                    else
                        show_overlay(dpy);
                } else if (visible) {
                    // Regular keystroke while overlay is shown.
                    handle_key(dpy, &ev.xkey);
                }
                break;
            }

            case Expose:
                // The window was uncovered or needs repainting.
                if (visible && ev.xexpose.count == 0) {
                    repaint(dpy);
                    XFlush(dpy);
                }
                break;

            case FocusOut:
                // The overlay lost focus — this happens when the user clicks
                // on another window or the WM moves focus away. Dismiss the
                // overlay so it doesn't linger invisibly in the background.
                // NotifyGrab (detail=3) is fired when XGrabKeyboard activates,
                // not an actual focus loss — skip those.
                if (visible && ev.xfocus.detail != NotifyGrab
                        && ev.xfocus.detail != NotifyPointerRoot) {
                    hide_overlay(dpy);
                }
                break;

            default:
                break;
            }
        }

        // ── Wait for the next event using select() ──
        // This blocks until either the X server sends us
        // something or the process receives a signal (which
        // sets quit_flag).
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);

        // Use a 1-second timeout so we check quit_flag periodically
        // even if no X events arrive.
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        select(xfd + 1, &fds, NULL, NULL, &tv);
    }
}

// ──────────────────────────────────────────────
// Cleanup
// ──────────────────────────────────────────────

void spotlight_cleanup(Display *dpy) {
    // Hide the overlay if it's still showing.
    hide_overlay(dpy);

    // Release the global hotkey.
    ungrab_hotkey(dpy);

    // Free the Cairo surface.
    if (surface) {
        cairo_surface_destroy(surface);
        surface = NULL;
    }

    // Destroy the window and colourmap.
    if (overlay_win != None) {
        XDestroyWindow(dpy, overlay_win);
        overlay_win = None;
    }
    if (colormap != None) {
        XFreeColormap(dpy, colormap);
        colormap = None;
    }

    // Clean up subsystems.
    search_cleanup();
    icon_cache_cleanup();
}
