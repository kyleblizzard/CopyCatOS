// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-consent-reader — consents.toml reader + writer unit test.
//
// Seeds a hand-written consents.toml under a per-test tmpdir
// ($XDG_DATA_HOME/moonbase/<bundle-id>/Preferences/consents.toml),
// then drives mb_consent_query + mb_consent_gate_allows against each
// schema shape we expect in production. The reader has no cache, so a
// single parent process can rewrite the file between phases and see
// the new state without fork churn — but each phase still leaves the
// tree clean for the next.
//
// The writer phases (mb_consent_record) round-trip: record a decision,
// read it back. They also verify atomic rewrite preserves unrelated
// sections, rejects bad inputs, and lands the file at mode 0600.
//
// The gate helper's "first missing warns on stderr" behavior is
// per-(group,value). That's observable only in stderr output; the
// bool return value is what callers branch on, so we assert the bool
// and let the warning noise through.

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
    CHECK(mb_consent_gate_allows("system", "keychain") == false,
          "no file + no compositor -> gate denies");
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
    CHECK(mb_consent_gate_allows("system", "keychain") == false,
          "different section + no compositor -> gate denies");
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
// Writer phases — mb_consent_record round-trip
// ---------------------------------------------------------------------

// Fresh write: no prior file, record ALLOW, read back ALLOW.
static void phase_record_allow_round_trip(void) {
    set_consents(NULL);
    CHECK(mb_consent_record("system", "keychain", MB_CONSENT_ALLOW) == MB_EOK,
          "record allow -> EOK");
    CHECK(mb_consent_query("system", "keychain") == MB_CONSENT_ALLOW,
          "record allow -> query ALLOW");
    CHECK(mb_consent_gate_allows("system", "keychain") == true,
          "record allow -> gate allows");
}

// Overwrite: ALLOW then DENY for the same section, last write wins.
static void phase_record_deny_overwrites(void) {
    set_consents(NULL);
    CHECK(mb_consent_record("system", "keychain", MB_CONSENT_ALLOW) == MB_EOK,
          "seed allow");
    CHECK(mb_consent_record("system", "keychain", MB_CONSENT_DENY) == MB_EOK,
          "overwrite with deny -> EOK");
    CHECK(mb_consent_query("system", "keychain") == MB_CONSENT_DENY,
          "overwrite deny -> query DENY");
    CHECK(mb_consent_gate_allows("system", "keychain") == false,
          "deny overwrite -> gate refuses");
}

// Preserve neighbors: file has an existing [hardware.camera] section;
// recording [system.keychain] must not touch it.
static void phase_record_preserves_neighbors(void) {
    set_consents(
        "# pre-existing file\n"
        "[hardware.camera]\n"
        "decision = \"allow\"\n");
    CHECK(mb_consent_record("system", "keychain", MB_CONSENT_DENY) == MB_EOK,
          "record into file with neighbor -> EOK");
    CHECK(mb_consent_query("system", "keychain") == MB_CONSENT_DENY,
          "new section -> DENY");
    CHECK(mb_consent_query("hardware", "camera") == MB_CONSENT_ALLOW,
          "neighbor section untouched -> ALLOW");
}

// Same-section rewrite with a neighbor: only target body changes.
static void phase_record_in_place_keeps_neighbors(void) {
    set_consents(
        "[hardware.camera]\n"
        "decision = \"allow\"\n"
        "\n"
        "[system.keychain]\n"
        "decision = \"allow\"\n");
    CHECK(mb_consent_record("system", "keychain", MB_CONSENT_DENY) == MB_EOK,
          "in-place rewrite -> EOK");
    CHECK(mb_consent_query("system", "keychain") == MB_CONSENT_DENY,
          "target flipped -> DENY");
    CHECK(mb_consent_query("hardware", "camera") == MB_CONSENT_ALLOW,
          "neighbor still ALLOW after in-place rewrite");
}

// Bad inputs: NULLs, empty strings, MISSING decision all rejected.
static void phase_record_rejects_bad_inputs(void) {
    set_consents(NULL);
    CHECK(mb_consent_record(NULL, "keychain", MB_CONSENT_ALLOW) == MB_EINVAL,
          "NULL group -> EINVAL");
    CHECK(mb_consent_record("system", NULL, MB_CONSENT_ALLOW) == MB_EINVAL,
          "NULL value -> EINVAL");
    CHECK(mb_consent_record("", "keychain", MB_CONSENT_ALLOW) == MB_EINVAL,
          "empty group -> EINVAL");
    CHECK(mb_consent_record("system", "keychain", MB_CONSENT_MISSING) == MB_EINVAL,
          "MISSING decision -> EINVAL");
}

// No bundle id: writer has nowhere to put the file.
static void phase_record_requires_bundle_id(void) {
    unsetenv("MOONBASE_BUNDLE_ID");
    CHECK(mb_consent_record("system", "keychain", MB_CONSENT_ALLOW) == MB_EPERM,
          "no bundle id -> EPERM");
    setenv("MOONBASE_BUNDLE_ID", g_bundle_id, 1);
}

// Mode check: file must land at 0600.
static void phase_record_file_mode_0600(void) {
    set_consents(NULL);
    CHECK(mb_consent_record("system", "keychain", MB_CONSENT_ALLOW) == MB_EOK,
          "record -> EOK");
    struct stat st;
    if (stat(g_consents_path, &st) != 0) {
        fprintf(stderr, "FAIL: stat consents.toml: %s\n", strerror(errno));
        failures++;
        return;
    }
    mode_t perm = st.st_mode & 0777;
    CHECK(perm == 0600, "file mode is 0600");
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

    phase_record_allow_round_trip();
    phase_record_deny_overwrites();
    phase_record_preserves_neighbors();
    phase_record_in_place_keeps_neighbors();
    phase_record_rejects_bad_inputs();
    phase_record_requires_bundle_id();
    phase_record_file_mode_0600();

    teardown();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    fprintf(stderr, "OK\n");
    return 0;
}
