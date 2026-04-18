// CopyCatOS — by Kyle Blizzard at Blizzard.show

// ============================================================================
// main.c — inputmap entry point
// ============================================================================
//
// CopyCatOS Button Mapper — configures gamepad button mappings for the
// Legion Go. Runs in two modes:
//
//   CLI mode (no arguments or --scan):
//     Scans for input devices and prints what's connected. Useful for
//     debugging which devices inputd will see.
//
//   Identify mode (--identify):
//     Waits for the user to press a button on the controller and prints
//     what event code it generated. Essential for mapping unknown buttons.
//
//   GUI mode (--gui, default in future):
//     Opens the Snow Leopard-styled button mapper window. (Not yet implemented.)
//
// ============================================================================

#include <stdio.h>
#include <string.h>

#include "scanner.h"
#include "config_editor.h"

// ============================================================================
//  print_scan_results — Display all discovered devices
// ============================================================================

static void print_scan_results(const ScanResult *result) {
    printf("=== Input Device Scan ===\n");
    printf("Found %d relevant device(s):\n\n", result->count);

    for (int i = 0; i < result->count; i++) {
        const ScannedDevice *d = &result->devices[i];

        printf("  %s\n", d->path);
        printf("    Name:    %s\n", d->name);
        printf("    ID:      %04x:%04x\n", d->vendor, d->product);
        printf("    Type:    %s%s%s\n",
               d->is_gamepad      ? "GAMEPAD "     : "",
               d->is_system_keys  ? "SYSTEM_KEYS " : "",
               d->is_power_button ? "POWER "       : "");
        printf("\n");
    }
}

// ============================================================================
//  print_config — Display current config file contents
// ============================================================================

static void print_config(void) {
    ConfigEditor *ed = config_editor_new();
    if (!ed) {
        fprintf(stderr, "Failed to create config editor\n");
        return;
    }

    printf("=== Current Configuration ===\n\n");

    if (config_editor_load(ed)) {
        printf("Config file loaded.\n\n");
    } else {
        printf("No config file found, showing defaults.\n\n");
    }

    // Mouse settings
    MouseSettings mouse;
    config_editor_get_mouse(ed, &mouse);
    printf("  [mouse]\n");
    printf("    deadzone    = %d\n",   mouse.deadzone);
    printf("    sensitivity = %.1f\n", mouse.sensitivity);
    printf("    exponent    = %.1f\n", mouse.exponent);
    printf("    max_speed   = %d\n",   mouse.max_speed);
    printf("\n");

    // Trigger threshold
    printf("  [triggers]\n");
    printf("    threshold   = %d\n", config_editor_get_trigger_threshold(ed));
    printf("\n");

    // Desktop mappings
    int count = config_editor_get_mapping_count(ed);
    printf("  [desktop_mappings] (%d rules)\n", count);
    for (int i = 0; i < count; i++) {
        MappingEntry entry;
        if (config_editor_get_mapping(ed, i, &entry)) {
            printf("    %s → %s\n", entry.source_name, entry.target_name);
        }
    }

    if (count == 0) {
        printf("    (none — using inputd's hardcoded defaults)\n");
    }
    printf("\n");

    config_editor_free(ed);
}

// ============================================================================
//  run_identify — Interactive "press to identify" mode
// ============================================================================

static void run_identify(void) {
    ScanResult scan;
    if (scanner_scan_all(&scan) < 0) {
        fprintf(stderr, "Failed to scan devices\n");
        return;
    }

    // Count gamepads
    int gamepad_count = 0;
    for (int i = 0; i < scan.count; i++) {
        if (scan.devices[i].is_gamepad) gamepad_count++;
    }

    if (gamepad_count == 0) {
        printf("No gamepad devices found. Make sure inputd is stopped\n");
        printf("(it grabs exclusive access): sudo systemctl stop inputd\n");
        return;
    }

    printf("=== Press to Identify ===\n");
    printf("Found %d gamepad(s). Press any button or move any stick...\n\n",
           gamepad_count);

    // Loop: keep identifying until the user hits Ctrl+C
    while (1) {
        IdentifiedEvent event;
        int ret = scanner_identify_button(&scan, &event, 10000);  // 10s timeout

        if (ret == 0) {
            // Successfully identified a button/axis
            const char *name = scanner_code_to_name(event.ev_type, event.ev_code);
            const char *type_str = (event.ev_type == 1)  ? "Button" :
                                   (event.ev_type == 3)  ? "Axis"   : "Event";

            printf("  %s: %s (type=%d, code=%d, value=%d)\n",
                   type_str, name, event.ev_type, event.ev_code, event.ev_value);
            printf("  Device: %s (%s)\n\n", event.device_name, event.device_path);
        } else {
            printf("  (timeout — no input detected, waiting...)\n");
        }
    }
}

// ============================================================================
//  main
// ============================================================================

int main(int argc, char **argv) {
    // Parse command-line arguments
    bool do_scan     = false;
    bool do_identify = false;
    bool do_config   = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scan") == 0 || strcmp(argv[i], "-s") == 0) {
            do_scan = true;
        } else if (strcmp(argv[i], "--identify") == 0 || strcmp(argv[i], "-i") == 0) {
            do_identify = true;
        } else if (strcmp(argv[i], "--config") == 0 || strcmp(argv[i], "-c") == 0) {
            do_config = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("inputmap — CopyCatOS Button Mapper\n\n");
            printf("Usage:\n");
            printf("  inputmap --scan       Scan and list all input devices\n");
            printf("  inputmap --identify   Press-to-identify mode (interactive)\n");
            printf("  inputmap --config     Show current config file settings\n");
            printf("  inputmap --help       Show this help\n\n");
            printf("Note: --identify requires inputd to be stopped first,\n");
            printf("since inputd grabs exclusive access to gamepad devices.\n");
            return 0;
        }
    }

    // Default to scan if no arguments given
    if (!do_scan && !do_identify && !do_config) {
        do_scan   = true;
        do_config = true;
    }

    if (do_scan) {
        ScanResult result;
        if (scanner_scan_all(&result) == 0) {
            print_scan_results(&result);
        } else {
            fprintf(stderr, "Device scan failed\n");
        }
    }

    if (do_config) {
        print_config();
    }

    if (do_identify) {
        run_identify();
    }

    return 0;
}
