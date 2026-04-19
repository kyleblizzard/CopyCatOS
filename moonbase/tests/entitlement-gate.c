// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-entitlement-gate — API-boundary entitlement gate unit test.
//
// Proves every privileged moonbase_keychain_* call returns MB_EPERM
// when MOONBASE_ENTITLEMENTS doesn't declare `system=keychain`. Also
// proves the gate lifts when the declaration is present — the
// libsecret call that follows may fail for unrelated reasons (no D-Bus
// session bus on a bare CI runner), but "not MB_EPERM" is the load-
// bearing invariant the gate contract promises.
//
// Each phase runs in its own forked child so the pthread_once cache
// inside entitlements.c starts fresh with the right env visible.
// DBUS_SESSION_BUS_ADDRESS is pointed at a nonexistent socket in the
// allow phase so libsecret fails fast rather than autostarting a
// real bus — this test cares about the gate, not the bus.

#include "moonbase.h"
#include "moonbase_keychain.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int phase_denied(void) {
    unsetenv("MOONBASE_ENTITLEMENTS");
    int fails = 0;

    char *out = NULL;
    mb_error_t rc = moonbase_keychain_fetch(NULL, "lbl", "acc", &out);
    if (rc != MB_EPERM) { fprintf(stderr, "fetch: rc=%d want MB_EPERM\n", rc); fails++; }
    if (out) { fprintf(stderr, "fetch: out leaked %p\n", (void *)out); free(out); fails++; }

    rc = moonbase_keychain_store(NULL, "lbl", "acc", "secret");
    if (rc != MB_EPERM) { fprintf(stderr, "store: rc=%d want MB_EPERM\n", rc); fails++; }

    rc = moonbase_keychain_delete(NULL, "lbl", "acc");
    if (rc != MB_EPERM) { fprintf(stderr, "delete: rc=%d want MB_EPERM\n", rc); fails++; }

    mb_keychain_list_t *list = NULL;
    rc = moonbase_keychain_list(&list);
    if (rc != MB_EPERM) { fprintf(stderr, "list: rc=%d want MB_EPERM\n", rc); fails++; }
    if (list) { fprintf(stderr, "list: list leaked\n"); moonbase_keychain_list_free(list); fails++; }

    // Last-error slot must carry the denial, not a subsequent EINVAL.
    if (moonbase_last_error() != MB_EPERM) {
        fprintf(stderr, "last-error: %d want MB_EPERM\n", moonbase_last_error());
        fails++;
    }

    // generate_password is not a privileged op; the gate must not touch it.
    char *pw = NULL;
    rc = moonbase_keychain_generate_password(12, MB_KEYCHAIN_PW_ALL, &pw);
    if (rc != MB_EOK || !pw || strlen(pw) != 12) {
        fprintf(stderr, "generate_password: rc=%d pw=%p len=%zu\n",
                rc, (void *)pw, pw ? strlen(pw) : 0);
        fails++;
    }
    moonbase_keychain_secret_free(pw);

    return fails ? 1 : 0;
}

static int phase_allowed(void) {
    setenv("MOONBASE_ENTITLEMENTS", "system=keychain", 1);
    // Force libsecret to fail fast if it tries to reach a bus.
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/mb-gate-test", 1);

    char *out = NULL;
    mb_error_t rc = moonbase_keychain_fetch(NULL, "lbl", "acc", &out);
    if (out) free(out);
    if (rc == MB_EPERM) {
        fprintf(stderr, "allowed: fetch still MB_EPERM with system=keychain\n");
        return 1;
    }
    return 0;
}

static int run_child(const char *name, int (*fn)(void)) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) _exit(fn());
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return 1; }
    if (!WIFEXITED(status)) {
        fprintf(stderr, "%s: child did not exit cleanly (0x%x)\n", name, status);
        return 1;
    }
    int rc = WEXITSTATUS(status);
    if (rc != 0) fprintf(stderr, "%s: FAIL\n", name);
    return rc;
}

int main(void) {
    int fails = 0;
    fails += run_child("denied",  phase_denied);
    fails += run_child("allowed", phase_allowed);
    if (fails) return 1;
    fprintf(stderr, "OK\n");
    return 0;
}
