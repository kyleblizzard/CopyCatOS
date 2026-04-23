// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-dbusmenu-dump — Connect to a com.canonical.dbusmenu endpoint
// and pretty-print the imported MenuNode tree. Useful for verifying
// 18-B against real GTK3 / Qt5 / Qt6 apps without any menubar chrome
// in the way.
//
// Usage:
//   mb-dbusmenu-dump <service> <object-path>                # one-shot
//   mb-dbusmenu-dump --watch <service> <object-path>        # follow updates
//   mb-dbusmenu-dump --activate <id> <service> <object-path># click item <id>, then quit
//
// Typical inputs (paired with appmenu_bridge output):
//   mb-dbusmenu-dump :1.42 /com/canonical/menu/0000001A
//
// This is a debug tool, not part of the menubar runtime. It shares
// menu_model.c and dbusmenu_client.c with the menubar executable so
// any behaviour it exercises is the real import path.

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../dbusmenu_client.h"
#include "../menu_model.h"

typedef struct {
    GMainLoop      *loop;
    DbusMenuClient *client;
    bool            watch;
    int             dumps;
    int             activate_id;   // -1 when --activate was not passed
    bool            activated;
} DumpCtx;

static gboolean quit_loop_cb(gpointer user_data);

static void on_changed(DbusMenuClient *client, void *user_data) {
    DumpCtx *ctx = user_data;
    const MenuNode *root = dbusmenu_client_root(client);
    ctx->dumps++;
    printf("── dump #%d ──────────────────────────────\n", ctx->dumps);
    if (!root) {
        printf("(empty)\n");
    } else {
        menu_node_dump(root, 0, stdout);
    }
    fflush(stdout);

    // --activate fires on the first layout we receive: dispatch the
    // click and quit. The mock's Event handler will print a line with
    // the id + event_id, which is what we came to verify.
    if (ctx->activate_id >= 0 && !ctx->activated) {
        ctx->activated = true;
        printf("── activate id=%d ──\n", ctx->activate_id);
        fflush(stdout);
        dbusmenu_client_activate(ctx->client, ctx->activate_id);
        // Event is fire-and-forget (no reply). Let the main loop spin
        // long enough for the proxy's connection to flush the outbound
        // message before we tear down.
        g_timeout_add(300, quit_loop_cb, ctx->loop);
        return;
    }

    if (!ctx->watch) {
        g_main_loop_quit(ctx->loop);
    }
}

static gboolean on_timeout(gpointer user_data) {
    DumpCtx *ctx = user_data;
    if (ctx->dumps == 0) {
        fprintf(stderr,
                "[mb-dbusmenu-dump] timed out with no layout reply — "
                "is the service still on the bus?\n");
        g_main_loop_quit(ctx->loop);
    }
    return G_SOURCE_REMOVE;
}

static gboolean quit_loop_cb(gpointer user_data) {
    g_main_loop_quit((GMainLoop *)user_data);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    bool watch       = false;
    int  activate_id = -1;
    int  argi        = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--watch") == 0) {
            watch = true;
            argi++;
        } else if (strcmp(argv[argi], "--activate") == 0 && argi + 1 < argc) {
            activate_id = atoi(argv[argi + 1]);
            argi += 2;
        } else {
            fprintf(stderr, "[mb-dbusmenu-dump] unknown flag: %s\n", argv[argi]);
            return 2;
        }
    }
    if (argc - argi != 2) {
        fprintf(stderr,
                "usage: mb-dbusmenu-dump [--watch | --activate <id>] "
                "<service> <object-path>\n"
                "example: mb-dbusmenu-dump :1.42 /com/canonical/menu/0000001A\n");
        return 2;
    }

    const char *service = argv[argi];
    const char *path    = argv[argi + 1];

    DumpCtx ctx = {
        .loop        = g_main_loop_new(NULL, FALSE),
        .watch       = watch,
        .activate_id = activate_id,
    };

    DbusMenuClient *client =
        dbusmenu_client_new(service, path, on_changed, &ctx);
    if (!client) {
        fprintf(stderr, "[mb-dbusmenu-dump] dbusmenu_client_new failed\n");
        return 1;
    }
    ctx.client = client;

    // 5-second deadline on the first reply — a dead service shouldn't
    // leave the tool hanging forever. --watch ignores this because the
    // caller explicitly asked to wait.
    if (!watch) {
        g_timeout_add_seconds(5, on_timeout, &ctx);
    }

    g_main_loop_run(ctx.loop);

    dbusmenu_client_free(client);
    g_main_loop_unref(ctx.loop);
    return 0;
}
