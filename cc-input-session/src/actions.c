// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

//
// actions.c — CopiCatOS desktop action dispatch implementation
//
// Each action maps to a concrete system operation. Some use X11
// ClientMessages (the standard EWMH way for programs to ask the
// window manager to do things), some signal other CopiCatOS
// processes, and some shell out to command-line tools.
//

#include "actions.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

// --- Helper: find a process by name and send it a signal ---

// send_signal_to_process — Look up a process by name using pgrep,
// then send it a Unix signal.
//
// This is how we communicate with other CopiCatOS shell components.
// For example, cc-spotlight listens for SIGUSR1 to toggle its
// visibility, so we pgrep for it and send that signal.
//
// Parameters:
//   proc_name — the process name to search for (passed to pgrep)
//   sig       — the signal number to send (e.g., SIGUSR1)
static void send_signal_to_process(const char *proc_name, int sig)
{
    // Build a command like: pgrep -f cc-spotlight
    // pgrep returns the PID(s) of matching processes, one per line.
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "pgrep -f %s", proc_name);

    // popen runs the command and lets us read its stdout
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "[cc-input-session] pgrep failed for '%s'\n", proc_name);
        return;
    }

    // Read each PID line and send the signal
    char line[32];
    while (fgets(line, sizeof(line), fp)) {
        pid_t pid = (pid_t)atoi(line);
        if (pid > 0) {
            kill(pid, sig);
            fprintf(stderr, "[cc-input-session] sent signal %d to %s (pid %d)\n",
                    sig, proc_name, pid);
        }
    }

    pclose(fp);
}

// --- Helper: toggle _NET_SHOWING_DESKTOP via EWMH ClientMessage ---

// toggle_show_desktop — Ask the window manager to show or hide the desktop.
//
// EWMH defines _NET_SHOWING_DESKTOP as a way for clients to request
// that the WM minimize all windows (showing the desktop) or restore
// them. We send a ClientMessage to the root window, which the WM
// picks up and acts on.
//
// The message format is:
//   - type = ClientMessage
//   - message_type = _NET_SHOWING_DESKTOP (atom)
//   - format = 32
//   - data.l[0] = 1 to show desktop, 0 to restore
//
// We toggle: read the current state, then send the opposite.
static void toggle_show_desktop(Display *dpy, Window root)
{
    // First, read the current _NET_SHOWING_DESKTOP property to know
    // whether we should show or restore.
    Atom showing_atom = XInternAtom(dpy, "_NET_SHOWING_DESKTOP", False);

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    // Default: assume desktop is NOT showing, so we'll show it
    long new_state = 1;

    int status = XGetWindowProperty(dpy, root, showing_atom,
                                     0, 1, False, XA_CARDINAL,
                                     &actual_type, &actual_format,
                                     &nitems, &bytes_after, &prop);

    if (status == Success && nitems > 0 && prop) {
        // If desktop IS currently showing, we want to restore (set to 0)
        long current = *((long *)prop);
        new_state = current ? 0 : 1;
        XFree(prop);
    }

    // Build and send the ClientMessage event.
    // ClientMessage is X11's way of letting programs send structured
    // messages to each other through the X server.
    XEvent ev;
    memset(&ev, 0, sizeof(ev));

    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = root;
    ev.xclient.message_type = showing_atom;
    ev.xclient.format       = 32;  // Each data item is 32 bits
    ev.xclient.data.l[0]    = new_state;

    // Send to root window with SubstructureRedirect | SubstructureNotify
    // mask — this is the standard way to talk to the window manager.
    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(dpy);

    fprintf(stderr, "[cc-input-session] show_desktop toggled to %ld\n", new_state);
}

// --- Main dispatch function ---

// actions_dispatch — Route an action name to the right handler.
//
// This is called whenever cc-inputd sends us a COPICATOS_ACTION message.
// The action_name is a simple string like "spotlight" that tells us
// what the user wants to happen.
void actions_dispatch(const char *action_name, Display *dpy, Window root)
{
    if (!action_name || action_name[0] == '\0') {
        fprintf(stderr, "[cc-input-session] empty action received, ignoring\n");
        return;
    }

    fprintf(stderr, "[cc-input-session] dispatching action: '%s'\n", action_name);

    // --- Spotlight: toggle the search overlay ---
    // cc-spotlight listens for SIGUSR1 to show/hide itself.
    if (strcmp(action_name, "spotlight") == 0) {
        send_signal_to_process("cc-spotlight", SIGUSR1);
    }
    // --- Mission Control: show all windows in an overview ---
    // TODO: This needs a custom protocol with cc-wm. For now, log it.
    else if (strcmp(action_name, "mission_control") == 0) {
        fprintf(stderr, "[cc-input-session] mission_control: not yet implemented\n");
        // Future: send a custom atom/client message to cc-wm
    }
    // --- Show Desktop: minimize all windows to reveal the desktop ---
    else if (strcmp(action_name, "show_desktop") == 0) {
        toggle_show_desktop(dpy, root);
    }
    // --- Volume Up: increase system volume by 5% ---
    // Uses PulseAudio's pactl command. @DEFAULT_SINK@ targets whatever
    // audio output is currently active.
    else if (strcmp(action_name, "volume_up") == 0) {
        system("pactl set-sink-volume @DEFAULT_SINK@ +5%");
    }
    // --- Volume Down: decrease system volume by 5% ---
    else if (strcmp(action_name, "volume_down") == 0) {
        system("pactl set-sink-volume @DEFAULT_SINK@ -5%");
    }
    // --- Brightness Up: increase screen brightness by 10% ---
    // Uses brightnessctl, which works with most Linux backlight drivers.
    else if (strcmp(action_name, "brightness_up") == 0) {
        system("brightnessctl set +10%");
    }
    // --- Brightness Down: decrease screen brightness by 10% ---
    else if (strcmp(action_name, "brightness_down") == 0) {
        system("brightnessctl set 10%-");
    }
    // --- Unknown action ---
    else {
        fprintf(stderr, "[cc-input-session] unknown action: '%s'\n", action_name);
    }
}
