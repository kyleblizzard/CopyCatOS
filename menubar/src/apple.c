// CopyCatOS — by Kyle Blizzard at Blizzard.show

// apple.c — Apple logo button and Apple menu dropdown
//
// The Apple logo is the leftmost element of the menu bar. It's loaded
// from a PNG file and scaled to 22x15 pixels (measured from real Snow Leopard). Clicking it opens the
// Apple menu — a dropdown with system-level actions like Sleep, Restart,
// Shut Down, and Log Out.
//
// If the PNG files aren't found, we fall back to drawing a small filled
// circle as a placeholder. The actual Apple logo PNGs need to be placed
// at $HOME/.local/share/aqua-widgets/menubar/apple_logo.png (and the
// _selected variant).

#define _GNU_SOURCE  // For M_PI in math.h under strict C11

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "apple.h"
#include "render.h"

// ── Font scaling helper (mirrors the one in render.c) ──────────────
static char *apple_scaled_font(const char *base_name, int base_size)
{
    static char buf[128];
    int scaled_size = (int)(base_size * menubar_scale + 0.5);
    if (scaled_size < base_size) scaled_size = base_size;
    snprintf(buf, sizeof(buf), "%s %d", base_name, scaled_size);
    return buf;
}

// ── Module state ────────────────────────────────────────────────────

// The Apple logo surfaces, scaled to 22x15 pixels (measured from real Snow Leopard).
// NULL if the PNG files couldn't be loaded.
static cairo_surface_t *logo_normal   = NULL;
static cairo_surface_t *logo_selected = NULL;

// The Apple menu dropdown popup window. None if not open.
static Window apple_popup = None;

// Currently hovered item index in the dropdown (-1 = none)
static int apple_hover = -1;

// ── Apple menu item definitions ─────────────────────────────────────
// These match the real Snow Leopard Apple menu per Apple HIG Figure 13-14.
// "---" is a separator. Items have optional right-aligned keyboard shortcuts.

// Dynamic "Log Out" label with the actual username
static char logout_label[128] = "Log Out...";

static const char *apple_items[17] = {
    "About CopyCatOS",
    "---",
    "Software Update...",
    "---",
    "System Preferences...",
    "Controller Settings...",  // Opens systemcontrol directly to the Controller pane
    "Dock",
    "Recent Items",
    "---",
    "Force Quit...",
    "---",
    "Sleep",
    "Restart...",
    "Shut Down...",
    "---",
    NULL  // Placeholder for logout_label (set dynamically in apple_init)
};
static const int apple_item_count = 16;

// Keyboard shortcuts displayed right-aligned next to menu items.
// NULL means no shortcut. Uses Mac-style symbols (⌘ ⌥ ⇧).
static const char *apple_shortcuts[] = {
    NULL,           // About CopyCatOS
    NULL,           // ---
    NULL,           // Software Update...
    NULL,           // ---
    NULL,           // System Preferences...
    NULL,           // Controller Settings...
    NULL,           // Dock
    NULL,           // Recent Items
    NULL,           // ---
    "⌥⌘Esc",       // Force Quit...
    NULL,           // ---
    NULL,           // Sleep
    NULL,           // Restart...
    NULL,           // Shut Down...
    NULL,           // ---
    "⇧⌘Q",         // Log Out
};

// Which items are disabled (grayed out, non-clickable)?
static bool is_disabled(int index)
{
    // Software Update, Dock submenu, Recent Items — still disabled stubs.
    // About CopyCatOS (index 0) now fires a placeholder notify-send toast
    // pending a real Aqua About sheet. Indices shifted by +1 from the
    // "Controller Settings..." insertion.
    return (index == 2 || index == 6 || index == 7);
}

// ── Internal: load a PNG, scale it, and extract an alpha mask ───────

// Loads a PNG file, scales it to the given width/height, and extracts
// just the alpha (opacity) channel as a grayscale mask surface.
//
// Why a mask? The Apple logo PNG is a colorful image (blue gradient),
// but macOS Snow Leopard renders the menu bar Apple logo as a solid
// dark silhouette. We use the logo's shape (alpha channel) as a stencil
// and paint it in solid black/white via cairo_mask_surface() in the
// paint function. This matches real Snow Leopard behavior.
//
// Returns an A8 (alpha-only) surface, or NULL if the file can't be loaded.
static cairo_surface_t *load_and_scale_png(const char *path, int target_w, int target_h)
{
    // Load the original PNG
    cairo_surface_t *original = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(original) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(original);
        return NULL;
    }

    // Get the original dimensions so we can compute the scale factor
    int orig_w = cairo_image_surface_get_width(original);
    int orig_h = cairo_image_surface_get_height(original);

    // Step 1: Scale the original PNG to the target size (keeps color+alpha)
    cairo_surface_t *scaled = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, target_w, target_h
    );
    cairo_t *cr = cairo_create(scaled);

    // Scale the drawing context so the original image maps to the target size
    double sx = (double)target_w / (double)orig_w;
    double sy = (double)target_h / (double)orig_h;
    cairo_scale(cr, sx, sy);

    // Use BEST filter for smooth downscaling (76x88 -> 22x15 is aggressive)
    cairo_pattern_t *pattern;
    cairo_set_source_surface(cr, original, 0, 0);
    pattern = cairo_get_source(cr);
    cairo_pattern_set_filter(pattern, CAIRO_FILTER_BEST);
    cairo_paint(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(original);

    // Step 2: Extract the alpha channel into an A8 mask surface.
    // A8 format stores only opacity — one byte per pixel, no color info.
    // We walk the scaled ARGB32 pixels and copy each pixel's alpha byte
    // into the A8 surface. This gives us the logo's silhouette shape.
    cairo_surface_flush(scaled);
    unsigned char *argb_data = cairo_image_surface_get_data(scaled);
    int argb_stride = cairo_image_surface_get_stride(scaled);

    cairo_surface_t *mask = cairo_image_surface_create(
        CAIRO_FORMAT_A8, target_w, target_h
    );
    cairo_surface_flush(mask);
    unsigned char *a8_data = cairo_image_surface_get_data(mask);
    int a8_stride = cairo_image_surface_get_stride(mask);

    for (int y = 0; y < target_h; y++) {
        for (int x = 0; x < target_w; x++) {
            // ARGB32 on little-endian: bytes are [B, G, R, A] per pixel
            unsigned char alpha = argb_data[y * argb_stride + x * 4 + 3];
            a8_data[y * a8_stride + x] = alpha;
        }
    }

    // Tell Cairo we modified the pixel data directly
    cairo_surface_mark_dirty(mask);
    cairo_surface_destroy(scaled);

    fprintf(stderr, "[apple] Loaded logo mask from %s (%dx%d -> %dx%d)\n",
            path, orig_w, orig_h, target_w, target_h);

    return mask;
}

// ── Public API ──────────────────────────────────────────────────────

void apple_init(MenuBar *mb)
{
    (void)mb;

    // Build the "Log Out <username>..." label from the actual system user.
    // Real Snow Leopard shows the short username (e.g., "Log Out Kyle...").
    const char *user = getenv("USER");
    if (!user) user = "User";
    snprintf(logout_label, sizeof(logout_label), "Log Out %s...", user);
    // Point the last item slot to our dynamic label
    apple_items[apple_item_count - 1] = logout_label;

    // Build the paths to the Apple logo PNGs.
    // We use $HOME to avoid hardcoding a username.
    const char *home = getenv("HOME");
    if (!home) return;

    char path_normal[512];
    char path_selected[512];
    snprintf(path_normal, sizeof(path_normal),
             "%s/.local/share/aqua-widgets/menubar/apple_logo.png", home);
    snprintf(path_selected, sizeof(path_selected),
             "%s/.local/share/aqua-widgets/menubar/apple_logo_selected.png", home);

    // Load and scale to proportionally-sized mask. Measured from real
    // Snow Leopard menu.png: Apple ink x=20..34 (w=15), y=1..16 (h~16) —
    // a portrait aspect ~0.82. Source HiResAppleMenu.png is 76×88, same
    // aspect within 2%. Target 15×18 keeps the logo upright; the earlier
    // 22×15 was a landscape aspect that squashed the logo at 1.75×.
    logo_normal   = load_and_scale_png(path_normal, S(15), S(18));
    logo_selected = load_and_scale_png(path_selected, S(15), S(18));

    if (!logo_normal) {
        fprintf(stderr, "menubar: Apple logo not found at %s (using fallback)\n",
                path_normal);
    }
}

void apple_reload(MenuBar *mb)
{
    (void)mb;

    // Free old logo surfaces
    if (logo_normal)   { cairo_surface_destroy(logo_normal);   logo_normal = NULL; }
    if (logo_selected) { cairo_surface_destroy(logo_selected); logo_selected = NULL; }

    // Reload at the new scale
    const char *home = getenv("HOME");
    if (!home) return;

    char path_normal[512], path_selected[512];
    snprintf(path_normal, sizeof(path_normal),
             "%s/.local/share/aqua-widgets/menubar/apple_logo.png", home);
    snprintf(path_selected, sizeof(path_selected),
             "%s/.local/share/aqua-widgets/menubar/apple_logo_selected.png", home);

    logo_normal   = load_and_scale_png(path_normal, S(15), S(18));
    logo_selected = load_and_scale_png(path_selected, S(15), S(18));

    fprintf(stderr, "[apple] Reloaded logos at scale %.1f (%dx%d)\n",
            menubar_scale, S(15), S(18));
}

void apple_paint(MenuBar *mb, cairo_t *cr)
{
    // Choose which logo variant to draw based on current state.
    // If the Apple menu is open or the mouse is hovering, use the
    // selected variant (or fall back to the normal one).
    bool active = (mb->hover_index == 0 || mb->open_menu == 0);
    cairo_surface_t *logo = (active && logo_selected) ? logo_selected : logo_normal;

    // Position: logo ink left-edge x=20, vertically centered in the bar.
    // Measured from real Snow Leopard menu.png: ink bbox x=20..34, y=1..16.
    double x = SF(20.0);
    double y = (MENUBAR_HEIGHT - S(18)) / 2.0;

    if (logo) {
        // Draw the logo as a solid-color silhouette using the alpha mask.
        // cairo_mask_surface() uses the mask's alpha values to control
        // where the current source color is painted. Where the mask is
        // opaque, the color shows through; where transparent, nothing
        // is drawn. This gives us a crisp black (or white) Apple logo
        // matching real Snow Leopard behavior.
        if (active) {
            // Selected state: white logo on the blue highlight
            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        } else {
            // Normal state: pure black logo on the gray gradient bar
            // Use pure black for maximum contrast — real SL Apple logo
            // is a crisp, solid dark shape
            cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        }
        cairo_mask_surface(cr, logo, x, y);
    } else {
        // Fallback: draw a simple filled circle as a placeholder.
        // This is a dark gray circle roughly matching the Apple logo position.
        cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
        cairo_arc(cr, x + SF(11.0), MENUBAR_HEIGHT / 2.0, SF(7.0), 0, 2 * M_PI);
        cairo_fill(cr);
    }
}

// ── Dropdown helpers ────────────────────────────────────────────────

// Helper: draw a rounded rectangle path (for the dropdown background).
static void apple_rounded_rect(cairo_t *cr, double x, double y,
                               double w, double h, double radius)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius,     radius, -M_PI / 2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0,          M_PI / 2);
    cairo_arc(cr, x + radius,     y + h - radius, radius, M_PI / 2,   M_PI);
    cairo_arc(cr, x + radius,     y + radius,     radius, M_PI,       3 * M_PI / 2);
    cairo_close_path(cr);
}

// Paint the Apple menu dropdown contents.
static void paint_apple_dropdown(MenuBar *mb, int popup_w, int popup_h)
{
    cairo_surface_t *surface = cairo_xlib_surface_create(
        mb->dpy, apple_popup,
        DefaultVisual(mb->dpy, mb->screen),
        popup_w, popup_h
    );
    cairo_t *cr = cairo_create(surface);

    // Background: slightly transparent white with scaled corner radius
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 245.0 / 255.0);
    apple_rounded_rect(cr, 0, 0, popup_w, popup_h, SF(5.0));
    cairo_fill(cr);

    // Border: subtle dark outline
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 60.0 / 255.0);
    cairo_set_line_width(cr, 1.0);
    apple_rounded_rect(cr, 0.5, 0.5, popup_w - 1, popup_h - 1, SF(5.0));
    cairo_stroke(cr);

    // Draw each menu item — all dimensions scale proportionally
    int y = S(4); // Top padding

    for (int i = 0; i < apple_item_count; i++) {
        const char *label = apple_items[i];
        if (!label) continue; // Safety: skip NULL entries

        if (strcmp(label, "---") == 0) {
            // Separator line
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 40.0 / 255.0);
            cairo_set_line_width(cr, 1.0);
            cairo_move_to(cr, S(10), y + SF(3.5));
            cairo_line_to(cr, popup_w - S(10), y + SF(3.5));
            cairo_stroke(cr);
            y += S(7);
        } else {
            // Check if this item is hovered — draw blue highlight
            // behind it, matching the Snow Leopard selection blue (#3875D7)
            bool disabled = is_disabled(i);
            bool hovered = (i == apple_hover && !disabled);

            if (hovered) {
                // Blue selection highlight (rounded rect)
                cairo_set_source_rgba(cr, 56/255.0, 117/255.0, 215/255.0, 0.9);
                double rx = S(4), ry = y, rw = popup_w - S(8), rh = S(22), rr = SF(3.0);
                cairo_new_sub_path(cr);
                cairo_arc(cr, rx + rw - rr, ry + rr, rr, -M_PI/2, 0);
                cairo_arc(cr, rx + rw - rr, ry + rh - rr, rr, 0, M_PI/2);
                cairo_arc(cr, rx + rr, ry + rh - rr, rr, M_PI/2, M_PI);
                cairo_arc(cr, rx + rr, ry + rr, rr, M_PI, 3*M_PI/2);
                cairo_close_path(cr);
                cairo_fill(cr);
            }

            // Text color: white on blue highlight, gray for disabled, dark otherwise
            if (hovered) {
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            } else if (disabled) {
                cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
            } else {
                cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
            }

            // Menu item label (left-aligned) with scaled font
            PangoLayout *layout = pango_cairo_create_layout(cr);
            pango_layout_set_text(layout, label, -1);

            PangoFontDescription *desc = pango_font_description_from_string(
                apple_scaled_font("Lucida Grande", 13)
            );
            pango_layout_set_font_description(layout, desc);
            pango_font_description_free(desc);

            cairo_move_to(cr, S(18), y + S(2));
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);

            // Submenu indicator arrow for Dock and Recent Items
            if (strcmp(label, "Dock") == 0 ||
                strcmp(label, "Recent Items") == 0) {
                // Draw a small right-pointing triangle, scaled proportionally
                double ax = popup_w - S(16);
                double ay = y + SF(10.0);
                cairo_move_to(cr, ax, ay - SF(4.0));
                cairo_line_to(cr, ax + SF(5.0), ay);
                cairo_line_to(cr, ax, ay + SF(4.0));
                cairo_close_path(cr);
                cairo_fill(cr);
            }

            // Keyboard shortcut (right-aligned, smaller scaled font)
            if (i < apple_item_count && apple_shortcuts[i]) {
                PangoLayout *sc_layout = pango_cairo_create_layout(cr);
                pango_layout_set_text(sc_layout, apple_shortcuts[i], -1);

                PangoFontDescription *sc_desc = pango_font_description_from_string(
                    apple_scaled_font("Lucida Grande", 12)
                );
                pango_layout_set_font_description(sc_layout, sc_desc);
                pango_font_description_free(sc_desc);

                int sc_w, sc_h;
                pango_layout_get_pixel_size(sc_layout, &sc_w, &sc_h);

                cairo_move_to(cr, popup_w - sc_w - S(12), y + S(2));
                pango_cairo_show_layout(cr, sc_layout);
                g_object_unref(sc_layout);
            }

            y += S(22);
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    XFlush(mb->dpy);
}

void apple_show_menu(MenuBar *mb)
{
    // Dismiss any existing Apple menu first
    apple_dismiss(mb);

    // ── Calculate popup size (all dimensions scale proportionally) ──
    int popup_w = S(220); // Fixed width matching Snow Leopard Apple menu

    // Height: S(22) per item, S(7) per separator, plus scaled padding
    int popup_h = S(8);
    for (int i = 0; i < apple_item_count; i++) {
        popup_h += (strcmp(apple_items[i], "---") == 0) ? S(7) : S(22);
    }

    // ── Create the popup window ─────────────────────────────────
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.event_mask = ExposureMask | ButtonPressMask
                     | PointerMotionMask | LeaveWindowMask;
    attrs.background_pixel = WhitePixel(mb->dpy, mb->screen);

    apple_popup = XCreateWindow(
        mb->dpy, mb->root,
        0, MENUBAR_HEIGHT,            // Directly below the Apple logo
        (unsigned int)popup_w,
        (unsigned int)popup_h,
        0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWEventMask | CWBackPixel,
        &attrs
    );

    XMapRaised(mb->dpy, apple_popup);

    // Paint the dropdown contents
    paint_apple_dropdown(mb, popup_w, popup_h);
}

void apple_dismiss(MenuBar *mb)
{
    if (apple_popup != None) {
        XDestroyWindow(mb->dpy, apple_popup);
        apple_popup = None;
        apple_hover = -1;
        XFlush(mb->dpy);
    }
}

Window apple_get_popup(void)
{
    return apple_popup;
}

// ── Item hit testing ───────────────────────────────────────────────
// Convert a Y coordinate inside the popup to the menu item index.
// Returns -1 for separators, padding, or out-of-bounds.
static int apple_y_to_item(int y)
{
    int row_y = S(4); // Top padding (scaled)
    for (int i = 0; i < apple_item_count; i++) {
        const char *label = apple_items[i];
        if (!label) continue;

        if (strcmp(label, "---") == 0) {
            row_y += S(7);
        } else {
            if (y >= row_y && y < row_y + S(22)) {
                return i;
            }
            row_y += S(22);
        }
    }
    return -1;
}

// ── Action execution ───────────────────────────────────────────────
// Executes the system action for the clicked Apple menu item.
// This is where menu items become functional — each item maps to
// a real system command matching macOS Snow Leopard behavior.
static void apple_execute(MenuBar *mb, int index)
{
    const char *label = apple_items[index];
    if (!label) return;
    if (is_disabled(index)) return; // Don't execute disabled items

    fprintf(stderr, "[apple] Execute: %s\n", label);

    if (strcmp(label, "About CopyCatOS") == 0) {
        // Placeholder About surface — pulls the live libmoonbase.so runtime
        // version via the `moonbase-version` CLI and fires it through
        // notify-send as an ephemeral toast. This is NOT the final UX.
        // Snow Leopard's "About This Mac" is a bespoke Aqua panel; the
        // real version lands once MoonRock exposes a shell-owned dialog
        // primitive we can draw the panel inside. Until then, a toast
        // with the correct version beats a disabled menu item.
        if (fork() == 0) {
            setsid();
            execlp("sh", "sh", "-c",
                "v=$(moonbase-version 2>/dev/null); "
                "notify-send -a 'CopyCatOS' 'About CopyCatOS' "
                "\"MoonBase runtime v${v:-unknown}\"",
                NULL);
            _exit(1);
        }
    } else if (strcmp(label, "System Preferences...") == 0) {
        // Launch the CopyCatOS System Preferences app
        if (fork() == 0) {
            setsid();
            execlp("systemcontrol", "systemcontrol", NULL);
            _exit(1);
        }
    } else if (strcmp(label, "Controller Settings...") == 0) {
        // Launch System Preferences directly to the Controller pane
        if (fork() == 0) {
            setsid();
            execlp("systemcontrol", "systemcontrol", "--pane", "controller", NULL);
            _exit(1);
        }
    } else if (strcmp(label, "Force Quit...") == 0) {
        // Show xkill or a force-quit dialog
        // For now, launch xkill which lets user click a window to kill it
        if (fork() == 0) {
            setsid();
            execlp("xkill", "xkill", NULL);
            _exit(1);
        }
    } else if (strcmp(label, "Sleep") == 0) {
        // Suspend the system (same as power button short press)
        system("systemctl suspend");
    } else if (strncmp(label, "Restart", 7) == 0) {
        // Reboot the system
        // TODO: Show confirmation dialog first
        system("systemctl reboot");
    } else if (strncmp(label, "Shut Down", 9) == 0) {
        // Power off the system
        // TODO: Show confirmation dialog first
        system("systemctl poweroff");
    } else if (strncmp(label, "Log Out", 7) == 0) {
        // Log out by killing the window manager, which triggers
        // moonrock-session.sh's cleanup (kills all shell components).
        // This is how Snow Leopard does it — the WM exit triggers
        // the loginwindow to reappear.
        pid_t wm_pid = 0;
        FILE *proc = popen("pgrep -x moonrock", "r");
        if (proc) {
            char buf[32];
            if (fgets(buf, sizeof(buf), proc)) {
                wm_pid = (pid_t)atoi(buf);
            }
            pclose(proc);
        }
        if (wm_pid > 0) {
            kill(wm_pid, SIGTERM);
        }
    }
}

// ── Event handling ─────────────────────────────────────────────────
// Handles hover highlighting and click-to-execute inside the Apple
// menu popup. Called from the menubar event loop when events are
// routed to the Apple popup window.

void apple_handle_motion(MenuBar *mb, int motion_y)
{
    int item = apple_y_to_item(motion_y);
    // Don't highlight disabled items or separators
    if (item >= 0 && is_disabled(item)) item = -1;

    if (item != apple_hover) {
        apple_hover = item;
        // Repaint the dropdown with updated highlight
        if (apple_popup != None) {
            XWindowAttributes wa;
            XGetWindowAttributes(mb->dpy, apple_popup, &wa);
            paint_apple_dropdown(mb, wa.width, wa.height);
        }
    }
}

bool apple_handle_click(MenuBar *mb, int click_x, int click_y)
{
    (void)click_x;
    int item = apple_y_to_item(click_y);

    if (item >= 0 && !is_disabled(item)) {
        // Execute the action, then dismiss
        apple_execute(mb, item);
        return true; // Should dismiss
    }
    return false; // Clicked on separator, disabled, or padding
}

bool apple_handle_event(MenuBar *mb, XEvent *ev, bool *should_dismiss)
{
    *should_dismiss = false;

    if (apple_popup == None) return false;
    if (ev->xany.window != apple_popup) return false;

    switch (ev->type) {
    case Expose:
        if (ev->xexpose.count == 0) {
            XWindowAttributes wa;
            XGetWindowAttributes(mb->dpy, apple_popup, &wa);
            paint_apple_dropdown(mb, wa.width, wa.height);
        }
        return true;

    case MotionNotify:
        apple_handle_motion(mb, ev->xmotion.y);
        return true;

    case LeaveNotify:
        if (apple_hover != -1) {
            apple_hover = -1;
            XWindowAttributes wa;
            XGetWindowAttributes(mb->dpy, apple_popup, &wa);
            paint_apple_dropdown(mb, wa.width, wa.height);
        }
        return true;

    case ButtonPress:
        *should_dismiss = apple_handle_click(mb, ev->xbutton.x, ev->xbutton.y);
        return true;
    }

    return false;
}

void apple_cleanup(void)
{
    // Free the loaded PNG surfaces
    if (logo_normal) {
        cairo_surface_destroy(logo_normal);
        logo_normal = NULL;
    }
    if (logo_selected) {
        cairo_surface_destroy(logo_selected);
        logo_selected = NULL;
    }

    apple_popup = None;
}
