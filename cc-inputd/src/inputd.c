// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// ============================================================================
// inputd.c — Core daemon event loop
// ============================================================================
//
// This is the heart of cc-inputd. It sets up all subsystems, then enters
// a single epoll-based event loop that handles:
//
//   - Reading raw events from physical gamepad devices (via evdev)
//   - Translating those events through the mapping engine
//   - Injecting translated events through uinput virtual devices
//   - Ticking the mouse emulator at 120Hz via timerfd
//   - Handling power button short/long press detection
//   - Monitoring for device hotplug via udev
//   - Accepting and serving IPC connections from the session bridge
//   - Reloading configuration on SIGHUP
//   - Shutting down cleanly on SIGINT/SIGTERM
//
// The design is single-threaded. Everything runs in one loop on one thread.
// epoll lets us efficiently wait on many file descriptors simultaneously
// without burning CPU in a polling loop. When nothing is happening, the
// process sleeps in epoll_wait() and uses zero CPU.
//
// Why epoll instead of poll/select?
//   - O(1) per-event cost vs O(n) for poll/select
//   - Level-triggered by default (we get notified until we drain the fd)
//   - Standard on Linux, which is our only target platform
//
// ============================================================================

#include "inputd.h"
#include "config.h"
#include "device.h"
#include "uinput.h"
#include "mapper.h"
#include "mouse.h"
#include "power.h"
#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <linux/input.h>

// ============================================================================
//  Constants
// ============================================================================

// Maximum number of epoll events we process per loop iteration.
// 16 is generous — we typically have 4-6 fds active at once.
#define MAX_EPOLL_EVENTS 16

// epoll_wait timeout in milliseconds. We wake up periodically even if
// no fds are ready, so we can check the power button hold timer.
// 100ms gives responsive hold detection without wasting CPU.
#define EPOLL_TIMEOUT_MS 100

// ============================================================================
//  Signal handling via self-pipe trick
// ============================================================================
//
// Signals are inherently asynchronous — they can interrupt any system call
// at any point. This makes it dangerous to do anything non-trivial in a
// signal handler (no malloc, no fprintf, no mutex operations).
//
// The self-pipe trick converts signals into fd events:
//   1. We create a pipe before entering the main loop
//   2. Signal handlers write a single byte to the pipe's write end
//   3. The pipe's read end is registered with epoll
//   4. When epoll wakes up on the pipe, we handle the signal safely
//      in the main loop context (where we CAN use malloc, fprintf, etc.)
//
// The byte value tells us which signal arrived:
//   'T' = SIGTERM or SIGINT (time to shut down)
//   'H' = SIGHUP (reload configuration)
// ============================================================================

// Global pointer to the daemon's write pipe fd.
// Signal handlers are C functions with a fixed signature — they can't receive
// arbitrary parameters. This global is the only way for them to reach the pipe.
static int g_signal_pipe_write = -1;

// Signal handler for SIGTERM and SIGINT (shutdown requests).
// Writes 'T' to the self-pipe. The main loop will see this and exit.
static void handle_sigterm(int sig)
{
    (void)sig;  // Unused parameter — we treat SIGTERM and SIGINT the same
    char c = 'T';
    // write() is async-signal-safe (one of the few functions that are)
    write(g_signal_pipe_write, &c, 1);
}

// Signal handler for SIGHUP (config reload request).
// Writes 'H' to the self-pipe. The main loop will reload config.
static void handle_sighup(int sig)
{
    (void)sig;
    char c = 'H';
    write(g_signal_pipe_write, &c, 1);
}

// ============================================================================
//  Helper: Add a file descriptor to the epoll set
// ============================================================================
// Registers `fd` with epoll for level-triggered read events (EPOLLIN).
// Level-triggered means: "wake me up whenever this fd has data to read."
// We store the fd itself as the epoll_event data so we can identify which
// fd triggered the event in the main loop.
// ============================================================================

static bool epoll_add_fd(int epoll_fd, int fd)
{
    struct epoll_event ev;
    ev.events  = EPOLLIN;           // We only care about read-readiness
    ev.data.fd = fd;                // Tag the event with the fd for dispatch
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        fprintf(stderr, "inputd: epoll_ctl ADD fd %d failed: %s\n",
                fd, strerror(errno));
        return false;
    }
    return true;
}

// ============================================================================
//  Helper: Remove a file descriptor from the epoll set
// ============================================================================

static void epoll_remove_fd(int epoll_fd, int fd)
{
    // EPOLL_CTL_DEL doesn't need an event struct on modern kernels,
    // but passing NULL caused bugs on very old kernels. We pass a
    // dummy struct for safety.
    struct epoll_event ev;
    ev.events  = 0;
    ev.data.fd = fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev);
}

// ============================================================================
//  Helper: Apply loaded config values to subsystem state
// ============================================================================
// After loading (or reloading) the config, this function pushes the new
// values into the mouse emulator, mapper, and power button handler.
// ============================================================================

static void apply_config(InputDaemon *daemon)
{
    InputConfig *cfg = daemon->config;

    // Update mouse emulator tuning parameters
    daemon->mouse->deadzone    = cfg->deadzone;
    daemon->mouse->sensitivity = cfg->sensitivity;
    daemon->mouse->exponent    = cfg->exponent;
    daemon->mouse->max_speed   = cfg->max_speed;

    // Update mapper trigger threshold
    daemon->mapper->trigger_threshold = cfg->trigger_threshold;

    // Update power button timing thresholds
    daemon->power->short_press_ms = cfg->short_press_ms;
    daemon->power->long_press_ms  = cfg->long_press_ms;

    fprintf(stderr, "inputd: config applied (deadzone=%d, sens=%.1f, exp=%.1f)\n",
            cfg->deadzone, cfg->sensitivity, cfg->exponent);
}

// ============================================================================
//  Helper: Check if CopyCatOS is currently in Game Mode
// ============================================================================
// game-mode.sh writes ~/.local/share/copycatos/gamemode.active when it
// enters game mode and removes it when the desktop is restored. Checking
// for this file is cheaper than IPC and works even if the bridge is down.
// ============================================================================

static bool game_mode_is_active(void)
{
    const char *home = getenv("HOME");
    if (!home) return false;

    char path[512];
    snprintf(path, sizeof(path),
             "%s/.local/share/copycatos/gamemode.active", home);

    // access() returns 0 if the file exists and is readable.
    // F_OK checks existence only — no permissions check needed.
    return access(path, F_OK) == 0;
}

// ============================================================================
//  Helper: Execute a power action
// ============================================================================
// Called when the power button handler returns a completed action.
// Sends the appropriate IPC message to the session bridge, or directly
// executes the system command if no bridge is connected.
// ============================================================================

static void execute_power_action(InputDaemon *daemon, PowerAction action)
{
    if (action == POWER_ACTION_NONE) return;

    // ── Game Mode intercept ────────────────────────────────────────────
    // When in game mode, a short press (suspend) should instead exit
    // gamescope and return to the CopyCatOS desktop rather than suspend.
    // game-mode.sh is blocking on gamescope; killing gamescope causes it
    // to unblock and run the desktop-restore step automatically.
    //
    // Long press (restart) is intentional and destructive — we let it
    // through unchanged so the user always has a hard-reset escape hatch.
    if (action == POWER_ACTION_SUSPEND && game_mode_is_active()) {
        fprintf(stderr, "inputd: game mode active — killing gamescope to return to desktop\n");
        system("pkill -x gamescope");
        return;
    }

    // Determine the action name string from config
    const char *action_name = NULL;
    if (action == POWER_ACTION_SUSPEND) {
        action_name = daemon->config->short_action;
    } else if (action == POWER_ACTION_RESTART) {
        action_name = daemon->config->long_action;
    }

    if (!action_name) return;

    fprintf(stderr, "inputd: power action: %s\n", action_name);

    // Send the action to the session bridge via IPC.
    // The bridge is responsible for actually performing the action
    // (showing a dialog, calling systemctl, etc.)
    IpcMessage msg;
    msg.type   = IPC_MSG_POWER_ACTION;
    msg.length = (int)strlen(action_name);
    if (msg.length > IPC_MAX_PAYLOAD) msg.length = IPC_MAX_PAYLOAD;
    memcpy(msg.payload, action_name, msg.length);

    if (!ipc_server_send(daemon->ipc, &msg)) {
        // No session bridge connected — fall back to direct system command.
        // This handles the case where the daemon is running but the desktop
        // session hasn't started yet (e.g. at the login screen).
        if (strcmp(action_name, "suspend") == 0) {
            fprintf(stderr, "inputd: no bridge, executing suspend directly\n");
            system("systemctl suspend");
        } else if (strcmp(action_name, "restart") == 0) {
            fprintf(stderr, "inputd: no bridge, executing reboot directly\n");
            system("systemctl reboot");
        }
    }
}

// ============================================================================
//  Helper: Send a CopyCatOS action via IPC
// ============================================================================
// Called when the mapper produces a CC_ACTION_* result (e.g. open Spotlight).
// These are desktop-level actions that cc-wm needs to handle.
// ============================================================================

static void send_copycatos_action(InputDaemon *daemon, int cc_action)
{
    // Map the CcAction enum value to a string for the IPC message.
    // The session bridge and cc-wm use string-based action names for
    // flexibility — new actions can be added without changing the protocol.
    const char *action_names[] = {
        "spotlight",           // CC_ACTION_SPOTLIGHT
        "mission_control",     // CC_ACTION_MISSION_CONTROL
        "show_desktop",        // CC_ACTION_SHOW_DESKTOP
        "volume_up",           // CC_ACTION_VOLUME_UP
        "volume_down",         // CC_ACTION_VOLUME_DOWN
        "brightness_up",       // CC_ACTION_BRIGHTNESS_UP
        "brightness_down"      // CC_ACTION_BRIGHTNESS_DOWN
    };

    // Bounds check — make sure the action is a valid enum value
    int max_action = (int)(sizeof(action_names) / sizeof(action_names[0]));
    if (cc_action < 0 || cc_action >= max_action) {
        fprintf(stderr, "inputd: unknown CC action %d\n", cc_action);
        return;
    }

    const char *name = action_names[cc_action];

    IpcMessage msg;
    msg.type   = IPC_MSG_COPYCATOS_ACTION;
    msg.length = (int)strlen(name);
    memcpy(msg.payload, name, msg.length);

    if (!ipc_server_send(daemon->ipc, &msg)) {
        fprintf(stderr, "inputd: failed to send CC action '%s' (no bridge?)\n", name);
    }
}

// ============================================================================
//  Handle an IPC message from the session bridge
// ============================================================================
// The session bridge can send us commands like "switch to gamepad profile"
// or "active window changed." This function dispatches those messages.
// ============================================================================

static void handle_ipc_message(InputDaemon *daemon, const IpcMessage *msg)
{
    switch (msg->type) {

    case IPC_MSG_SET_PROFILE: {
        // Session bridge is telling us to switch mapping profiles.
        // Payload contains the profile name (e.g. "gamepad", "desktop").
        char profile_name[64];
        int len = msg->length;
        if (len >= (int)sizeof(profile_name)) len = (int)sizeof(profile_name) - 1;
        memcpy(profile_name, msg->payload, len);
        profile_name[len] = '\0';

        fprintf(stderr, "inputd: IPC set profile: %s\n", profile_name);

        if (strcmp(profile_name, "login") == 0) {
            mapper_set_profile(daemon->mapper, PROFILE_LOGIN);
        } else if (strcmp(profile_name, "desktop") == 0) {
            mapper_set_profile(daemon->mapper, PROFILE_DESKTOP);
        } else if (strcmp(profile_name, "gamepad") == 0 ||
                   strcmp(profile_name, "game") == 0) {
            mapper_set_profile(daemon->mapper, PROFILE_GAME);
        } else {
            fprintf(stderr, "inputd: unknown profile '%s'\n", profile_name);
        }
        break;
    }

    case IPC_MSG_CONFIG_RELOAD:
        // Session bridge is requesting a config reload (e.g. user changed
        // settings in System Preferences).
        fprintf(stderr, "inputd: IPC config reload request\n");
        daemon->reload_pending = 1;
        break;

    case IPC_MSG_ACTIVE_WINDOW: {
        // Session bridge is telling us which window has focus.
        // We check the game_overrides table to see if we should switch
        // to game mode for this application.
        char wm_class[256];
        int len = msg->length;
        if (len >= (int)sizeof(wm_class)) len = (int)sizeof(wm_class) - 1;
        memcpy(wm_class, msg->payload, len);
        wm_class[len] = '\0';

        // Check each game override pattern against the window class
        InputConfig *cfg = daemon->config;
        bool matched = false;
        for (int i = 0; i < cfg->game_override_count; i++) {
            // Simple substring match for now.
            // TODO: Proper glob/wildcard matching
            if (strstr(wm_class, cfg->game_overrides[i].pattern) != NULL) {
                const char *profile = cfg->game_overrides[i].profile;
                fprintf(stderr, "inputd: window '%s' matched override -> %s\n",
                        wm_class, profile);
                if (strcmp(profile, "gamepad") == 0 ||
                    strcmp(profile, "game") == 0) {
                    mapper_set_profile(daemon->mapper, PROFILE_GAME);
                }
                matched = true;
                break;
            }
        }

        // If no override matched, switch back to desktop mode
        if (!matched) {
            mapper_set_profile(daemon->mapper, PROFILE_DESKTOP);
        }
        break;
    }

    default:
        fprintf(stderr, "inputd: unknown IPC message type 0x%02x\n", msg->type);
        break;
    }
}

// ============================================================================
//  inputd_init — Allocate and initialize every subsystem
// ============================================================================
//
// This function brings the daemon from a zeroed struct to a fully initialized
// state, ready to enter the main loop. The initialization order matters:
//
//   1. epoll fd — needed to register other fds
//   2. Self-pipe — needed for signal handling
//   3. Signal handlers — installed after pipe is ready
//   4. Config — loaded before subsystems that depend on it
//   5. Devices — opens physical controllers
//   6. Virtual devices — creates uinput outputs
//   7. Mapper — needs config and virtual devices
//   8. Mouse — needs config values
//   9. Power — needs config values
//  10. IPC — last, since it's optional for basic operation
//
// Returns 0 on success, -1 on failure (check stderr for details).
// ============================================================================

int inputd_init(InputDaemon *daemon)
{
    // ------------------------------------------------------------------
    // Step 1: Create the epoll instance
    // ------------------------------------------------------------------
    // epoll_create1(0) creates an epoll file descriptor.
    // The 0 flag means no special options (we could pass EPOLL_CLOEXEC
    // but it's not critical for a daemon).
    // ------------------------------------------------------------------
    daemon->epoll_fd = epoll_create1(0);
    if (daemon->epoll_fd < 0) {
        fprintf(stderr, "inputd: epoll_create1 failed: %s\n", strerror(errno));
        return -1;
    }

    // ------------------------------------------------------------------
    // Step 2: Create the self-pipe for signal handling
    // ------------------------------------------------------------------
    // pipe2() creates a pipe with the O_NONBLOCK flag so that write()
    // in the signal handler never blocks (which would be catastrophic
    // since signal handlers must return quickly).
    // ------------------------------------------------------------------
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_NONBLOCK) < 0) {
        fprintf(stderr, "inputd: pipe2 failed: %s\n", strerror(errno));
        close(daemon->epoll_fd);
        return -1;
    }
    daemon->pipe_read  = pipe_fds[0];
    daemon->pipe_write = pipe_fds[1];

    // Register the read end of the pipe with epoll
    if (!epoll_add_fd(daemon->epoll_fd, daemon->pipe_read)) {
        close(daemon->pipe_read);
        close(daemon->pipe_write);
        close(daemon->epoll_fd);
        return -1;
    }

    // Store the write fd globally so signal handlers can reach it
    g_signal_pipe_write = daemon->pipe_write;

    // ------------------------------------------------------------------
    // Step 3: Install signal handlers
    // ------------------------------------------------------------------
    // We use sigaction() instead of signal() because it's more portable
    // and gives us control over SA_RESTART behavior.
    // ------------------------------------------------------------------
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    // SIGTERM and SIGINT -> clean shutdown
    sa.sa_handler = handle_sigterm;
    sa.sa_flags   = SA_RESTART;   // Don't interrupt epoll_wait()
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    // SIGHUP -> reload configuration
    sa.sa_handler = handle_sighup;
    sigaction(SIGHUP, &sa, NULL);

    // ------------------------------------------------------------------
    // Step 4: Load configuration
    // ------------------------------------------------------------------
    daemon->config = calloc(1, sizeof(InputConfig));
    if (!daemon->config) {
        fprintf(stderr, "inputd: failed to allocate config\n");
        return -1;
    }

    if (config_load_input(daemon->config)) {
        fprintf(stderr, "inputd: config loaded from file\n");
    } else {
        fprintf(stderr, "inputd: using default config\n");
    }

    // ------------------------------------------------------------------
    // Step 5: Initialize device manager (discover physical controllers)
    // ------------------------------------------------------------------
    daemon->devices = calloc(1, sizeof(DeviceManager));
    if (!daemon->devices) {
        fprintf(stderr, "inputd: failed to allocate device manager\n");
        return -1;
    }

    if (device_init(daemon->devices) < 0) {
        fprintf(stderr, "inputd: device_init failed\n");
        return -1;
    }

    // Add each discovered device's fd to epoll so we wake up on input events
    for (int i = 0; i < daemon->devices->device_count; i++) {
        int fd = daemon->devices->devices[i].fd;
        if (fd >= 0) {
            epoll_add_fd(daemon->epoll_fd, fd);
        }
    }

    // Add the udev monitor fd to epoll so we detect hotplug events
    if (daemon->devices->mon_fd >= 0) {
        epoll_add_fd(daemon->epoll_fd, daemon->devices->mon_fd);
    }

    // ------------------------------------------------------------------
    // Step 6: Create virtual devices (uinput mouse, keyboard, gamepad)
    // ------------------------------------------------------------------
    daemon->vdevs = calloc(1, sizeof(VirtualDevices));
    if (!daemon->vdevs) {
        fprintf(stderr, "inputd: failed to allocate virtual devices\n");
        return -1;
    }

    if (uinput_init(daemon->vdevs) < 0) {
        fprintf(stderr, "inputd: uinput_init failed\n");
        return -1;
    }

    // ------------------------------------------------------------------
    // Step 7: Initialize the mapping engine
    // ------------------------------------------------------------------
    daemon->mapper = calloc(1, sizeof(Mapper));
    if (!daemon->mapper) {
        fprintf(stderr, "inputd: failed to allocate mapper\n");
        return -1;
    }

    mapper_init(daemon->mapper, daemon->config->trigger_threshold);

    // If the config file had desktop mappings, load them into the mapper.
    // (The mapper starts with hardcoded defaults; config overrides those.)
    // Note: CfgMappingRule and MappingRule have different layouts, so a
    // proper conversion would be needed here. For now, the mapper's defaults
    // are used and config mappings are applied through mapper_load_config
    // in a future integration step.

    // ------------------------------------------------------------------
    // Step 8: Initialize mouse emulator
    // ------------------------------------------------------------------
    daemon->mouse = calloc(1, sizeof(MouseEmulator));
    if (!daemon->mouse) {
        fprintf(stderr, "inputd: failed to allocate mouse emulator\n");
        return -1;
    }

    if (!mouse_init(daemon->mouse)) {
        fprintf(stderr, "inputd: mouse_init failed\n");
        return -1;
    }

    // Apply config values to the mouse emulator
    daemon->mouse->deadzone    = daemon->config->deadzone;
    daemon->mouse->sensitivity = daemon->config->sensitivity;
    daemon->mouse->exponent    = daemon->config->exponent;
    daemon->mouse->max_speed   = daemon->config->max_speed;

    // Add the mouse timer fd to epoll (fires at 120Hz for smooth motion)
    if (daemon->mouse->timer_fd >= 0) {
        epoll_add_fd(daemon->epoll_fd, daemon->mouse->timer_fd);
    }

    // ------------------------------------------------------------------
    // Step 9: Initialize power button handler
    // ------------------------------------------------------------------
    daemon->power = calloc(1, sizeof(PowerButton));
    if (!daemon->power) {
        fprintf(stderr, "inputd: failed to allocate power handler\n");
        return -1;
    }

    power_init(daemon->power);
    daemon->power->short_press_ms = daemon->config->short_press_ms;
    daemon->power->long_press_ms  = daemon->config->long_press_ms;

    // ------------------------------------------------------------------
    // Step 10: Initialize IPC server
    // ------------------------------------------------------------------
    daemon->ipc = calloc(1, sizeof(IpcServer));
    if (!daemon->ipc) {
        fprintf(stderr, "inputd: failed to allocate IPC server\n");
        return -1;
    }

    if (!ipc_server_init(daemon->ipc)) {
        // IPC failure is non-fatal — the daemon can still work without
        // the session bridge (just no Spotlight/profile switching).
        fprintf(stderr, "inputd: IPC init failed (non-fatal)\n");
    } else {
        // Add the IPC listen socket to epoll
        epoll_add_fd(daemon->epoll_fd, daemon->ipc->listen_fd);
    }

    // ------------------------------------------------------------------
    // All subsystems initialized — ready to run
    // ------------------------------------------------------------------
    daemon->running        = true;
    daemon->reload_pending = 0;
    daemon->was_game_mode  = false;

    fprintf(stderr, "inputd: initialization complete (%d devices found)\n",
            daemon->devices->device_count);
    return 0;
}

// ============================================================================
//  inputd_run — Main epoll event loop
// ============================================================================
//
// This is the core of the daemon. It blocks in epoll_wait() until one or
// more file descriptors have data to read, then dispatches each event to
// the appropriate handler.
//
// File descriptors we watch:
//   - signal pipe read end    — signals converted to fd events
//   - device fds              — raw gamepad/power button events
//   - udev monitor fd         — hotplug notifications
//   - mouse timer fd          — 120Hz pointer movement ticks
//   - IPC listen fd           — new session bridge connections
//   - IPC client fd           — messages from the session bridge
//
// The loop runs until daemon->running is set to false by a SIGTERM/SIGINT
// handler (via the self-pipe).
// ============================================================================

void inputd_run(InputDaemon *daemon)
{
    struct epoll_event events[MAX_EPOLL_EVENTS];

    fprintf(stderr, "inputd: entering main loop\n");

    while (daemon->running) {
        // Wait for events, or timeout after EPOLL_TIMEOUT_MS.
        // The timeout ensures we periodically check for power button
        // long-press even if no other events arrive.
        int nfds = epoll_wait(daemon->epoll_fd, events, MAX_EPOLL_EVENTS,
                              EPOLL_TIMEOUT_MS);

        if (nfds < 0) {
            if (errno == EINTR) {
                // epoll_wait was interrupted by a signal. This is normal —
                // our signal handlers write to the self-pipe, and epoll
                // will pick that up on the next iteration.
                continue;
            }
            fprintf(stderr, "inputd: epoll_wait failed: %s\n", strerror(errno));
            break;
        }

        // ------------------------------------------------------------------
        // Dispatch each ready file descriptor
        // ------------------------------------------------------------------
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            // ── Signal pipe: handle SIGTERM/SIGHUP ────────────────────
            if (fd == daemon->pipe_read) {
                char sig_byte;
                while (read(daemon->pipe_read, &sig_byte, 1) > 0) {
                    if (sig_byte == 'T') {
                        // SIGTERM or SIGINT — time to shut down
                        fprintf(stderr, "inputd: received shutdown signal\n");
                        daemon->running = false;
                    } else if (sig_byte == 'H') {
                        // SIGHUP — reload configuration
                        fprintf(stderr, "inputd: received SIGHUP, scheduling reload\n");
                        daemon->reload_pending = 1;
                    }
                }
                continue;
            }

            // ── Mouse timer: tick the pointer emulator ────────────────
            if (daemon->mouse && fd == daemon->mouse->timer_fd) {
                // Read the timer to acknowledge it (timerfd requires this).
                // The value tells us how many expirations occurred, but we
                // don't use it — we just tick once per wakeup.
                uint64_t expirations;
                read(fd, &expirations, sizeof(expirations));

                // Compute pointer movement for this frame
                int dx = 0, dy = 0;
                if (mouse_tick(daemon->mouse, &dx, &dy)) {
                    // Inject the mouse movement into the virtual mouse device
                    uinput_mouse_move(daemon->vdevs, dx, dy);
                }
                continue;
            }

            // ── Udev monitor: device hotplug ──────────────────────────
            if (daemon->devices && fd == daemon->devices->mon_fd) {
                // A device was plugged in or unplugged.
                // Record the current device count so we can detect new ones.
                int old_count = daemon->devices->device_count;

                device_handle_hotplug(daemon->devices);

                // If new devices appeared, add their fds to epoll
                for (int j = old_count; j < daemon->devices->device_count; j++) {
                    int dev_fd = daemon->devices->devices[j].fd;
                    if (dev_fd >= 0) {
                        epoll_add_fd(daemon->epoll_fd, dev_fd);
                        fprintf(stderr, "inputd: added hotplugged device to epoll: %s\n",
                                daemon->devices->devices[j].name);
                    }
                }
                continue;
            }

            // ── IPC listen socket: new session bridge connection ──────
            if (daemon->ipc && fd == daemon->ipc->listen_fd) {
                ipc_server_accept(daemon->ipc);

                // Add the new client fd to epoll so we wake up on messages
                if (daemon->ipc->client_fd >= 0) {
                    epoll_add_fd(daemon->epoll_fd, daemon->ipc->client_fd);
                }
                continue;
            }

            // ── IPC client socket: message from session bridge ────────
            if (daemon->ipc && fd == daemon->ipc->client_fd) {
                IpcMessage msg;
                if (ipc_server_recv(daemon->ipc, &msg)) {
                    handle_ipc_message(daemon, &msg);
                } else if (daemon->ipc->client_fd < 0) {
                    // Client disconnected — remove from epoll.
                    // (ipc_server_recv already closed the fd and set it to -1)
                    epoll_remove_fd(daemon->epoll_fd, fd);
                    fprintf(stderr, "inputd: session bridge disconnected, "
                            "removed from epoll\n");
                }
                continue;
            }

            // ── Device fd: raw input event from a gamepad or power key ─
            // If we get here, this fd must be one of our opened devices.
            // Read raw evdev events and route them through the mapping engine.
            {
                // Find which device this fd belongs to
                InputDevice *dev = NULL;
                for (int j = 0; j < daemon->devices->device_count; j++) {
                    if (daemon->devices->devices[j].fd == fd) {
                        dev = &daemon->devices->devices[j];
                        break;
                    }
                }

                if (!dev) {
                    // Unknown fd — shouldn't happen, but be safe
                    fprintf(stderr, "inputd: event on unknown fd %d\n", fd);
                    continue;
                }

                // Read input events from the device.
                // A single read() can return multiple events (they're packed
                // as an array of struct input_event in the kernel buffer).
                struct input_event ev;
                ssize_t n = read(fd, &ev, sizeof(ev));

                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;   // No data right now — spurious wakeup
                    }
                    // Real error — device may have been unplugged
                    fprintf(stderr, "inputd: read error on %s: %s\n",
                            dev->path, strerror(errno));
                    epoll_remove_fd(daemon->epoll_fd, fd);
                    continue;
                }

                if (n < (ssize_t)sizeof(ev)) {
                    continue;   // Partial read — shouldn't happen with evdev
                }

                // ── Route the event ───────────────────────────────────

                // Power button events go to the power handler
                if (dev->is_power_button && ev.type == EV_KEY &&
                    ev.code == KEY_POWER) {
                    PowerAction action = power_handle(daemon->power, ev.value);
                    execute_power_action(daemon, action);
                    continue;
                }

                // Gamepad events: in desktop/login mode, right stick axes go to
                // the mouse emulator for pointer control. In game mode, they
                // fall through to the mapper so they get forwarded to the
                // virtual gamepad for games to use as camera/aim input.
                if (dev->is_gamepad) {
                    if (ev.type == EV_ABS &&
                        (ev.code == ABS_RX || ev.code == ABS_RY) &&
                        daemon->mapper->active_profile != PROFILE_GAME) {
                        mouse_update_axis(daemon->mouse, ev.code, ev.value);
                        continue;
                    }

                    // All other gamepad events go through the mapping engine.
                    // The mapper decides what to do based on the active profile:
                    //   - DESKTOP: translate buttons to keys/actions
                    //   - GAME: forward raw events to virtual gamepad
                    //   - LOGIN: minimal mappings
                    int cc_action = mapper_process(daemon->mapper, &ev,
                                                   daemon->vdevs,
                                                   daemon->mouse);

                    // If the mapper produced a CopyCatOS action, send it
                    // to the session bridge via IPC
                    if (cc_action >= 0) {
                        send_copycatos_action(daemon, cc_action);
                    }
                }
            }
        }

        // ------------------------------------------------------------------
        // Post-epoll checks (run every iteration, even on timeout)
        // ------------------------------------------------------------------

        // Check if SIGHUP was received and config needs reloading
        if (daemon->reload_pending) {
            daemon->reload_pending = 0;
            fprintf(stderr, "inputd: reloading configuration\n");

            if (config_load_input(daemon->config)) {
                apply_config(daemon);
                fprintf(stderr, "inputd: config reloaded successfully\n");
            } else {
                fprintf(stderr, "inputd: config file not found, keeping current settings\n");
            }
        }

        // ── Game mode auto-switch ────────────────────────────────────
        // game-mode.sh creates a marker file when entering game mode and
        // removes it on exit. We poll for this file each loop iteration
        // (~100ms via EPOLL_TIMEOUT_MS) and automatically switch between
        // PROFILE_GAME and PROFILE_DESKTOP on transitions. This avoids
        // needing the session bridge to be running during game mode.
        {
            bool in_game = game_mode_is_active();
            if (in_game && !daemon->was_game_mode) {
                // Transition: desktop → game mode
                fprintf(stderr, "inputd: game mode activated, switching to PROFILE_GAME\n");
                mapper_set_profile(daemon->mapper, PROFILE_GAME);

                // Zero the mouse emulator's stored right-stick axes so the
                // cursor doesn't drift at whatever velocity was held when
                // game mode kicked in. The 120Hz mouse timer would keep
                // moving the cursor using stale axis values otherwise.
                mouse_update_axis(daemon->mouse, ABS_RX, 0);
                mouse_update_axis(daemon->mouse, ABS_RY, 0);
            } else if (!in_game && daemon->was_game_mode) {
                // Transition: game mode → desktop
                fprintf(stderr, "inputd: game mode deactivated, switching to PROFILE_DESKTOP\n");
                mapper_set_profile(daemon->mapper, PROFILE_DESKTOP);
            }
            daemon->was_game_mode = in_game;
        }
    }

    fprintf(stderr, "inputd: exiting main loop\n");
}

// ============================================================================
//  inputd_shutdown — Tear down all subsystems and free resources
// ============================================================================
//
// Shuts everything down in reverse initialization order. Each shutdown
// function is safe to call with NULL pointers or partially initialized state,
// so this works even if init failed partway through.
// ============================================================================

void inputd_shutdown(InputDaemon *daemon)
{
    fprintf(stderr, "inputd: shutting down...\n");

    // Shut down IPC first (sends close to session bridge)
    if (daemon->ipc) {
        ipc_server_shutdown(daemon->ipc);
        free(daemon->ipc);
        daemon->ipc = NULL;
    }

    // Shut down virtual devices (destroys uinput nodes)
    if (daemon->vdevs) {
        uinput_shutdown(daemon->vdevs);
        free(daemon->vdevs);
        daemon->vdevs = NULL;
    }

    // Shut down mouse emulator (closes timer fd)
    if (daemon->mouse) {
        mouse_shutdown(daemon->mouse);
        free(daemon->mouse);
        daemon->mouse = NULL;
    }

    // Shut down device manager (closes device fds, destroys udev context)
    if (daemon->devices) {
        device_shutdown(daemon->devices);
        free(daemon->devices);
        daemon->devices = NULL;
    }

    // Free mapper (no special cleanup needed, just free memory)
    if (daemon->mapper) {
        free(daemon->mapper);
        daemon->mapper = NULL;
    }

    // Free power button handler (no fds to close)
    if (daemon->power) {
        free(daemon->power);
        daemon->power = NULL;
    }

    // Free config
    if (daemon->config) {
        free(daemon->config);
        daemon->config = NULL;
    }

    // Close epoll fd
    if (daemon->epoll_fd >= 0) {
        close(daemon->epoll_fd);
        daemon->epoll_fd = -1;
    }

    // Close the self-pipe
    if (daemon->pipe_read >= 0) {
        close(daemon->pipe_read);
        daemon->pipe_read = -1;
    }
    if (daemon->pipe_write >= 0) {
        close(daemon->pipe_write);
        daemon->pipe_write = -1;
    }

    // Clear the global signal pipe reference
    g_signal_pipe_write = -1;

    fprintf(stderr, "inputd: shutdown complete\n");
}
