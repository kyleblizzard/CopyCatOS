// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// launch.c — App launching, process detection, and window activation
//
// Three main jobs:
//
// 1. LAUNCHING: When the user clicks a dock icon for an app that isn't running,
//    we fork+exec a child process to start it. The dock itself keeps running
//    (it's the parent process). We also start the bounce animation.
//
// 2. PROCESS DETECTION: Every few seconds, we run `ps -eo comm=` to get a
//    list of all running processes. We match each dock item's process_name
//    against this list to update the `running` flag.
//
// 3. WINDOW ACTIVATION: When the user clicks an icon for an app that's already
//    running, we find its window and bring it to the front using the
//    _NET_ACTIVE_WINDOW protocol (a standard way to tell the window manager
//    to focus a specific window).
// ============================================================================

#define _GNU_SOURCE  // For strcasecmp and strncasecmp under strict C11

#include "launch.h"
#include "bounce.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <X11/Xatom.h>

// ---------------------------------------------------------------------------
// Helper: read an X11 property from a window.
// X11 stores window properties (like WM_CLASS, _NET_WM_PID, etc.) as
// typed data on windows. This function retrieves a property and returns
// the raw data. The caller must XFree() the returned data.
// ---------------------------------------------------------------------------
static unsigned char *get_window_property(Display *dpy, Window win, Atom prop,
                                          Atom type, unsigned long *count)
{
    Atom actual_type;
    int actual_format;
    unsigned long bytes_after;
    unsigned char *data = NULL;

    // XGetWindowProperty reads the property. We ask for up to 1024 items.
    int status = XGetWindowProperty(dpy, win, prop, 0, 1024, False, type,
                                     &actual_type, &actual_format,
                                     count, &bytes_after, &data);

    if (status != Success || actual_type == None) {
        if (data) XFree(data);
        *count = 0;
        return NULL;
    }
    return data;
}

Window launch_find_window(DockState *state, DockItem *item)
{
    // Get the list of all client windows from the window manager.
    // _NET_CLIENT_LIST is a standard property on the root window that
    // contains an array of Window IDs for all managed windows.
    Atom client_list_atom = XInternAtom(state->dpy, "_NET_CLIENT_LIST", False);
    unsigned long count = 0;
    unsigned char *data = get_window_property(state->dpy, state->root,
                                              client_list_atom, XA_WINDOW,
                                              &count);
    if (!data || count == 0) {
        if (data) XFree(data);
        return None;
    }

    Window *windows = (Window *)data;
    Window found = None;

    // Check each window's WM_CLASS to see if it matches our dock item.
    // WM_CLASS contains two null-terminated strings: the instance name and
    // the class name. We check both against the item's process_name.
    Atom wm_class_atom = XInternAtom(state->dpy, "WM_CLASS", False);

    for (unsigned long i = 0; i < count && found == None; i++) {
        unsigned long class_count = 0;
        unsigned char *class_data = get_window_property(
            state->dpy, windows[i], wm_class_atom, XA_STRING, &class_count);

        if (class_data && class_count > 0) {
            // WM_CLASS has two strings packed together with null terminators.
            // The first is the instance name, the second is the class name.
            const char *instance = (const char *)class_data;
            const char *classname = instance + strlen(instance) + 1;

            // Case-insensitive comparison against our process name
            if (strcasecmp(instance, item->process_name) == 0 ||
                strcasecmp(classname, item->process_name) == 0 ||
                strcasecmp(instance, item->exec_path) == 0 ||
                strcasecmp(classname, item->exec_path) == 0) {
                found = windows[i];
            }
        }

        if (class_data) XFree(class_data);
    }

    XFree(data);
    return found;
}

void launch_activate_app(DockState *state, DockItem *item)
{
    // Find the window belonging to this app
    Window win = launch_find_window(state, item);
    if (win == None) return;

    // Send a _NET_ACTIVE_WINDOW client message to the root window.
    // This is the EWMH (Extended Window Manager Hints) standard way to
    // ask the window manager to focus and raise a specific window.
    // It works with KWin, GNOME Shell, and most other modern WMs.
    Atom active_atom = XInternAtom(state->dpy, "_NET_ACTIVE_WINDOW", False);

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.serial = 0;
    ev.xclient.send_event = True;
    ev.xclient.message_type = active_atom;
    ev.xclient.window = win;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2;  // Source indication: 2 = pager/taskbar
    ev.xclient.data.l[1] = 0;  // Timestamp (0 = current time)
    ev.xclient.data.l[2] = 0;  // Currently active window (0 = none)

    XSendEvent(state->dpy, state->root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    XFlush(state->dpy);
}

void launch_app(DockState *state, DockItem *item)
{
    // If the app is already running, just bring its window to the front
    if (item->running) {
        launch_activate_app(state, item);
        return;
    }

    // Start the bounce animation to give immediate visual feedback
    bounce_start(item);

    // Fork a child process to launch the application.
    // fork() creates a copy of the current process. The parent continues
    // running the dock, while the child replaces itself with the app.
    pid_t pid = fork();

    if (pid == 0) {
        // --- Child process ---
        // We're the child. Replace ourselves with the target application.

        // Detach from the dock's process group so the app runs independently.
        // setsid() creates a new session, making this process the leader.
        setsid();

        // Close the X display in the child — the app will open its own.
        // (Sharing X connections between processes causes crashes.)
        if (state->dpy) {
            close(ConnectionNumber(state->dpy));
        }

        // Redirect stdout/stderr to /dev/null to avoid cluttering the dock's output
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);

        // Try to exec the application directly
        execlp(item->exec_path, item->exec_path, (char *)NULL);

        // If execlp failed (e.g., command not found), try gio launch as a fallback.
        // gio can handle .desktop files and is common on freedesktop systems.
        char desktop_file[600];
        snprintf(desktop_file, sizeof(desktop_file), "%s.desktop", item->exec_path);
        execlp("gio", "gio", "launch",
               "/usr/share/applications/", desktop_file, (char *)NULL);

        // If we get here, nothing worked
        _exit(127);

    } else if (pid > 0) {
        // --- Parent process (the dock) ---
        // Don't wait for the child. We don't want the dock to block.
        // We'll reap zombie processes periodically.
        // Set SIGCHLD to be ignored so children are auto-reaped.
        signal(SIGCHLD, SIG_IGN);
    } else {
        // fork() failed
        fprintf(stderr, "Error: fork() failed when launching %s\n", item->name);
    }
}

void launch_check_running(DockItem *items, int count)
{
    // Run `ps -eo comm=` which outputs just the command name of every running
    // process, one per line. The `=` after `comm` suppresses the header line.
    // We redirect stderr to /dev/null to avoid noise from ps on some systems.
    FILE *fp = popen("ps -eo comm= 2>/dev/null", "r");
    if (!fp) return;

    // Read all process names into a buffer so we can search it efficiently.
    // We read line-by-line with fgets so we can handle each process name
    // individually. A typical system has a few hundred processes.
    char all_procs[8192];
    size_t total = 0;
    char line[256];
    all_procs[0] = '\0';

    while (fgets(line, sizeof(line), fp)) {
        // Strip trailing newline and whitespace
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                           line[len - 1] == ' ')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        // Append to buffer with newline separator
        if (total + len + 2 < sizeof(all_procs)) {
            memcpy(all_procs + total, line, len);
            total += len;
            all_procs[total++] = '\n';
            all_procs[total] = '\0';
        }
    }
    pclose(fp);

    // Check each dock item against the process list.
    // For each item, we scan every line in the buffer and compare the
    // basename (after the last '/') against the item's process_name.
    // We use case-insensitive EXACT matching to avoid false positives
    // (e.g., "kate" should not match "katepart").
    for (int i = 0; i < count; i++) {
        bool was_running = items[i].running;
        items[i].running = false;

        // Skip items with no process name set
        if (items[i].process_name[0] == '\0') continue;

        // Walk through each line in the buffer
        char *p = all_procs;
        while (*p) {
            // Find the end of this line
            char *nl = strchr(p, '\n');
            if (!nl) break;

            // Temporarily null-terminate this line so we can use string functions
            *nl = '\0';

            // Get just the basename in case ps returned a full path
            // (e.g., "/usr/bin/dolphin" -> "dolphin")
            char *base = strrchr(p, '/');
            base = base ? base + 1 : p;

            // Case-insensitive exact match against our process name
            if (strcasecmp(base, items[i].process_name) == 0) {
                items[i].running = true;
                *nl = '\n';  // Restore the newline before breaking
                break;
            }

            // Restore the newline and advance to the next line
            *nl = '\n';
            p = nl + 1;
        }

        // Log state changes so we can debug process detection
        if (items[i].running != was_running) {
            fprintf(stderr, "[dock] %s is now %s\n",
                    items[i].name, items[i].running ? "running" : "stopped");
        }

        // If an app just started running, stop its bounce animation
        if (items[i].running && !was_running && items[i].bouncing) {
            items[i].bouncing = false;
            items[i].bounce_offset = 0;
        }
    }
}
