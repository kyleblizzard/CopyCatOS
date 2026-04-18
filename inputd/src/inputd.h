// CopyCatOS — by Kyle Blizzard at Blizzard.show

//
// inputd.h — Core daemon state and public API
//
// This is the "top-level" header for inputd. It owns the InputDaemon
// struct, which bundles every subsystem (device discovery, virtual devices,
// mapping engine, mouse emulator, power button handler, config, IPC) into
// one place so the main loop can reach everything through a single pointer.
//

#ifndef INPUTD_H
#define INPUTD_H

#include <stdbool.h>
#include <signal.h>

// --------------------------------------------------------------------------
// Forward declarations
// --------------------------------------------------------------------------
// Each subsystem has its own header with the full struct definition.
// We forward-declare here so InputDaemon can hold them without pulling
// in every subsystem header (keeps compile times down and avoids
// circular includes).
// --------------------------------------------------------------------------
typedef struct DeviceManager  DeviceManager;
typedef struct VirtualDevices VirtualDevices;
typedef struct Mapper         Mapper;
typedef struct MouseEmulator  MouseEmulator;
typedef struct PowerButton    PowerButton;
typedef struct InputConfig    InputConfig;
typedef struct IpcServer      IpcServer;
typedef struct HidParser      HidParser;

// --------------------------------------------------------------------------
// InputDaemon — The root struct that owns the entire daemon's state
// --------------------------------------------------------------------------
// Everything the daemon needs lives here. We allocate it once in main()
// and pass a pointer into inputd_init / inputd_run / inputd_shutdown.
// --------------------------------------------------------------------------
typedef struct InputDaemon {
    // --- Subsystems (each defined in its own header) ---
    DeviceManager  *devices;       // Discovers and opens /dev/input/* nodes
    VirtualDevices *vdevs;         // Creates uinput virtual mouse/kb/gamepad
    Mapper         *mapper;        // Translates physical events -> actions
    MouseEmulator  *mouse;         // Turns stick axes into pointer movement
    PowerButton    *power;         // Handles short/long press of power key
    InputConfig    *config;        // Loaded from ~/.config/copycatos/input.conf
    IpcServer      *ipc;           // Unix socket for moonrock / System Prefs
    HidParser      *hid;           // Parses raw vendor HID reports for buttons

    // --- Event loop ---
    int epoll_fd;                  // epoll(7) file descriptor; watches all
                                   // device fds, udev monitor, IPC socket,
                                   // mouse timer, and the signal pipe.

    // --- Lifecycle flags ---
    volatile bool running;         // Set to false by SIGINT/SIGTERM handler
                                   // to break the main loop gracefully.

    volatile sig_atomic_t reload_pending;
                                   // Set to 1 by SIGHUP handler. The main
                                   // loop checks this each iteration and
                                   // reloads config when it fires.

    bool was_game_mode;            // Tracks whether game mode was active on
                                   // the previous loop iteration. Used to
                                   // detect transitions and auto-switch
                                   // between PROFILE_GAME and PROFILE_DESKTOP.

    // --- Self-pipe trick ---
    // Signals are asynchronous — they can arrive at any point, even in the
    // middle of an epoll_wait(). The "self-pipe trick" converts a signal
    // into an fd event: the signal handler writes a byte to pipe_write,
    // and epoll wakes up because pipe_read becomes readable.
    int pipe_read;                 // Read end — registered with epoll
    int pipe_write;                // Write end — signal handler writes here

} InputDaemon;

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

// inputd_init — Allocate and initialize every subsystem.
// Returns 0 on success, -1 on failure (check stderr for details).
// The daemon struct must be zeroed before calling this.
int  inputd_init(InputDaemon *daemon);

// inputd_run — Enter the main epoll loop. Blocks until `running` is false.
// All event dispatching (device reads, IPC messages, mouse ticks, hotplug)
// happens inside this function.
void inputd_run(InputDaemon *daemon);

// inputd_shutdown — Tear down every subsystem, close fds, free memory.
// Safe to call even if init partially failed (NULL pointers are skipped).
void inputd_shutdown(InputDaemon *daemon);

#endif // INPUTD_H
