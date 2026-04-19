// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-consent-reader — consents.toml reader unit test.
//
// Seeds a hand-written consents.toml under a per-test tmpdir
// ($XDG_DATA_HOME/moonbase/<bundle-id>/Preferences/consents.toml),
// then drives mb_consent_query + mb_consent_gate_allows against each
// schema shape we expect in production. The reader has no cache, so a
// single parent process can rewrite the file between phases and see
// the new state without fork churn — but each phase still leaves the
// tree clean for the next.
//
// The gate helper's "first missing warns on stderr" behavior is
// process-wide (single static bool). That's observable only in stderr
// output; the bool return value is what callers branch on, so we
// assert the bool and let the warning noise through.

#include "consents.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        failures++; \
    } \
} while (0)

// Size-bounded so -Wformat-truncation can prove each path
// concatenation below lands strictly inside its destination buffer.
// Each step grows the buffer enough to hold the previous plus the
// static suffix we're appending.
#define TMPROOT_CAP      128
#define PREFS_DIR_CAP    (TMPROOT_CAP + 128)
#define CONSENTS_CAP     (PREFS_DIR_CAP + 64)
#define GENERIC_PATH_CAP CONSENTS_CAP

static char g_tmproot[TMPROOT_CAP];
static char g_bundle_id[] = "show.blizzard.consent-test";
static char g_consents_path[CONSENTS_CAP];

// Build a path under the tmpdir and mkdir -p everything above it.
static void mkdir_p(const char *path) {
    char buf[GENERIC_PATH_CAP];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0700);
            *p = '/';
        }
    }
    mkdir(buf, 0700);
}

// Overwrite the test's consents.toml with `content`, or unlink it if
// content == NULL.
static void set_consents(const char *content) {
    if (!content) {
        unlink(g_consents_path);
        return;
    }
    FILE *f = fopen(g_consents_path, "wb");
    if (!f) { perror("fopen consents.toml"); exit(2); }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static void setup(void) {
    // Per-test tmpdir under /tmp, cleaned by us on exit.
    snprintf(g_tmproot, sizeof(g_tmproot),
             "/tmp/mb-consent-reader-%d", (int)getpid());
    mkdir_p(g_tmproot);

    setenv("XDG_DATA_HOME", g_tmproot, 1);
    setenv("MOONBASE_BUNDLE_ID", g_bundle_id, 1);

    char prefs_dir[PREFS_DIR_CAP];
    snprintf(prefs_dir, sizeof(prefs_dir),
             "%s/moonbase/%s/Preferences", g_tmproot, g_bundle_id);
    mkdir_p(prefs_dir);

    snprintf(g_consents_path, sizeof(g_consents_path),
             "%s/consents.toml", prefs_dir);
}

// Recursive rm. Small enough to hand-roll for a test.
static int rm_rf(const char *path) {
    char cmd[GENERIC_PATH_CAP + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    return system(cmd);
}

static void teardown(void) {
    rm_rf(g_tmproot);
}

// ---------------------------------------------------------------------
// Phases
// ---------------------------------------------------------------------

static void phase_missing_file(void) {
    set_consents(NULL);
    CHECK(mb_consent_query("system", "keychain") == MB_CONSENT_MISSING,
          "no file -> MISSING");
    CHECK(mb_consent_gate_allows("system", "keychain") == true,
          "no file -> gate allows (grant-by-default)");
}

static void phase_allow(void) {
    set_consents(
        "[system.keychain]\n"
        "decision = \"allow\"\n"
        "scope    = \"always\"\n"
        "granted  = 2026-04-18T12:00:00Z\n");
    CHECK(mb_consent_query("system", "keychain") == MB_CONSENT_ALLOW,
          "allow section -> ALLOW");
    CHECK(mb_consent_gate_allows("system", "keychain") == true,
          "allow -> gate allows");
}

static void phase_deny(void) {
    set_consents(
        "[system.keychain]\n"
        "decision = \"deny\"\n");
    CHECK(mb_consent_query("system", "keychain") == MB_CONSENT_DENY,
          "deny section -> DENY");
    CHECK(mb_consent_gate_allows("system", "keychain") == false,
          "deny -> gate refuses");
}

static void phase_wrong_section(void) {
    set_consents(
        "[hardware.camera]\n"
        "decision = \"allow\"\n");
    CHECK(mb_consent_query("system", "keychain") == MB_CONSENT_MISSING,
          "different section -> MISSING");
    CHECK(mb_consent_gate_allows("system", "keychain") == true,
          "different section -> gate allows");
}

static void phase_section_without_decision(void) {
    set_consents(
        "[system.keychain]\n"
        "scope = \"session\"\n"
        "# no decision field yet\n");
    CHECK(mb_consent_query("system", "keychain") == MB_CONSENT_MISSING,
          "section without decision -> MISSING");
}

static void phase_comments_and_blanks(void) {
    set_consents(
        "# consents.toml — seeded by test\n"
        "\n"
        "   # indented comment\n"
        "[hardware.camera]\n"
        "decision = \"deny\"\n"
        "\n"
        "[system.keychain]   # trailing comment on header\n"
        "# comment inside section\n"
        "scope    = \"always\"\n"
        "decision = \"allow\"\n"
        "\n");
    CHECK(mb_consent_query("system", "keychain") == MB_CONSENT_ALLOW,
          "comments + blanks + trailing comment -> ALLOW");
    CHECK(mb_consent_query("hardware", "camera") == MB_CONSENT_DENY,
          "earlier section still reachable -> DENY");
}

static void phase_unknown_decision_value(void) {
    set_consents(
        "[system.keychain]\n"
        "decision = \"maybe\"\n");
    CHECK(mb_consent_query("system", "keychain") == MB_CONSENT_MISSING,
          "unknown decision value -> MISSING");
}

static void phase_null_inputs(void) {
    CHECK(mb_consent_query(NULL, "keychain") == MB_CONSENT_MISSING,
          "NULL group -> MISSING");
    CHECK(mb_consent_query("system", NULL) == MB_CONSENT_MISSING,
          "NULL value -> MISSING");
    CHECK(mb_consent_query("", "keychain") == MB_CONSENT_MISSING,
          "empty group -> MISSING");
}

static void phase_no_bundle_id(void) {
    unsetenv("MOONBASE_BUNDLE_ID");
    CHECK(mb_consent_query("system", "keychain") == MB_CONSENT_MISSING,
          "no bundle id -> MISSING");
    // Restore for subsequent phases.
    setenv("MOONBASE_BUNDLE_ID", g_bundle_id, 1);
}

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------

int main(void) {
    setup();

    phase_missing_file();
    phase_allow();
    phase_deny();
    phase_wrong_section();
    phase_section_without_decision();
    phase_comments_and_blanks();
    phase_unknown_decision_value();
    phase_null_inputs();
    phase_no_bundle_id();

    teardown();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    fprintf(stderr, "OK\n");
    return 0;
}
