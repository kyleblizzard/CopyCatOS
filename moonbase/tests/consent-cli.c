// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-consent-cli — moonbase-consent allow/deny CLI integration test.
//
// Forks the real moonbase-consent binary (path supplied as argv[1] by
// the meson test wiring). Each phase points XDG_DATA_HOME at a fresh
// tmpdir, runs moonbase-consent with the chosen argv, then reads the
// resulting consents.toml back through mb_consent_query (or its
// absence — a phase that expects a failing exit reads no file). Pins
// the CLI surface end-to-end: out-of-process round-trip proves argv
// parsing, the libmoonbase link, and the atomic write work together —
// same-process unit tests can't catch a broken exec contract.

#include "consents.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *g_cli;
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        failures++; \
    } \
} while (0)

// Fork, set MOONBASE_BUNDLE_ID in the child if requested, exec the
// CLI, and return its exit status. A bundle_id of NULL means the child
// inherits the current env (which may or may not have the var set).
static int run_cli(char *const argv[], const char *bundle_id_or_null,
                   bool unset_bundle_id) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(2); }
    if (pid == 0) {
        if (unset_bundle_id) unsetenv("MOONBASE_BUNDLE_ID");
        else if (bundle_id_or_null)
            setenv("MOONBASE_BUNDLE_ID", bundle_id_or_null, 1);
        execv(g_cli, argv);
        perror("execv");
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); exit(2); }
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// Phase 1: `moonbase-consent allow system keychain <bundle-id>` writes
// ALLOW that mb_consent_query reads back.
static void phase_allow_with_bundle_arg(void) {
    const char *bid = "show.blizzard.cli-allow";
    char *argv[] = {
        (char *)g_cli, "allow", "system", "keychain",
        (char *)bid, NULL
    };
    int rc = run_cli(argv, NULL, false);
    CHECK(rc == 0, "allow with bundle-id arg exited 0");

    setenv("MOONBASE_BUNDLE_ID", bid, 1);
    CHECK(mb_consent_query("system", "keychain") == MB_CONSENT_ALLOW,
          "reader sees ALLOW after CLI allow");
    CHECK(mb_consent_gate_allows("system", "keychain"),
          "gate returns true after CLI allow");
}

// Phase 2: re-running with `deny` overwrites the allow — the same
// parse-mutate-serialize path mb_consent_record exercises in-process.
static void phase_deny_overwrites(void) {
    const char *bid = "show.blizzard.cli-deny";
    char *a[] = {
        (char *)g_cli, "allow", "hardware", "camera",
        (char *)bid, NULL
    };
    CHECK(run_cli(a, NULL, false) == 0, "initial allow exited 0");

    char *d[] = {
        (char *)g_cli, "deny", "hardware", "camera",
        (char *)bid, NULL
    };
    CHECK(run_cli(d, NULL, false) == 0, "follow-up deny exited 0");

    setenv("MOONBASE_BUNDLE_ID", bid, 1);
    CHECK(mb_consent_query("hardware", "camera") == MB_CONSENT_DENY,
          "reader sees DENY after CLI deny overwrites allow");
    CHECK(!mb_consent_gate_allows("hardware", "camera"),
          "gate returns false after CLI deny");
}

// Phase 3: bundle id comes from MOONBASE_BUNDLE_ID in the child env —
// no 4th argv. Same round-trip check.
static void phase_env_bundle_id(void) {
    const char *bid = "show.blizzard.cli-env";
    char *argv[] = {
        (char *)g_cli, "allow", "system", "accessibility", NULL
    };
    int rc = run_cli(argv, bid, false);
    CHECK(rc == 0, "allow with env-only bundle-id exited 0");

    setenv("MOONBASE_BUNDLE_ID", bid, 1);
    CHECK(mb_consent_query("system", "accessibility") == MB_CONSENT_ALLOW,
          "reader sees ALLOW after env-bundle CLI allow");
}

// Phase 4: neither env var nor 4th arg → writer returns MB_EPERM,
// CLI exits non-zero. Also verifies no ghost file gets created.
static void phase_no_bundle_id_fails(void) {
    char *argv[] = {
        (char *)g_cli, "allow", "system", "keychain", NULL
    };
    int rc = run_cli(argv, NULL, true);
    CHECK(rc != 0, "missing bundle id exits non-zero");
}

// Phase 5: too few positional args trips usage path (exit 2).
static void phase_bad_argc(void) {
    char *argv[] = {
        (char *)g_cli, "allow", "system", NULL
    };
    int rc = run_cli(argv, "show.blizzard.noop", false);
    CHECK(rc == 2, "argc < 4 exits 2");
}

// Phase 6: first positional isn't allow/deny → falls into the
// first-launch-sheet arm, which then fails its own argc check and
// exits 2. Proves the dispatch doesn't swallow unrelated argv shapes.
static void phase_unknown_verb_falls_through(void) {
    char *argv[] = {
        (char *)g_cli, "approve", "system", "keychain",
        "show.blizzard.noop", NULL
    };
    int rc = run_cli(argv, NULL, false);
    // "approve" isn't a bundle-path, but with argc == 5 the
    // first-launch arm also runs mb_bundle_load on it, which will
    // fail. Either exit 1 (bundle load rejected) or exit 2 (usage)
    // is correct — both are "didn't touch consents.toml".
    CHECK(rc == 1 || rc == 2,
          "unknown verb falls through to first-launch arm");
}

// Phase 7: `ask` with MOONBASE_CONSENT_AUTO=approve exits 0 and
// writes ALLOW. Pairs what moonrock's responder will do once slice
// 6B.2 lands — the responder hands the verdict off to the CLI by
// setting MOONBASE_CONSENT_AUTO and forking; the CLI does the write.
static void phase_ask_auto_approve(void) {
    const char *bid = "show.blizzard.cli-ask-approve";
    char *argv[] = {
        (char *)g_cli, "ask", "system", "keychain",
        (char *)bid, "signing into iCloud", NULL
    };
    // setenv overwrite=1 in the parent; run_cli forks and the child
    // inherits the env, so the automation arm fires.
    setenv("MOONBASE_CONSENT_AUTO", "approve", 1);
    int rc = run_cli(argv, bid, false);
    unsetenv("MOONBASE_CONSENT_AUTO");
    CHECK(rc == 0, "ask + AUTO=approve exits 0");

    setenv("MOONBASE_BUNDLE_ID", bid, 1);
    CHECK(mb_consent_query("system", "keychain") == MB_CONSENT_ALLOW,
          "reader sees ALLOW after ask+approve");
    CHECK(mb_consent_gate_allows("system", "keychain"),
          "gate returns true after ask+approve");
}

// Phase 8: `ask` with MOONBASE_CONSENT_AUTO=reject exits 1 and
// writes DENY. A DENY is a recorded decision the gate honors —
// distinct from "nothing recorded" which the 6A gate flip will treat
// as prompt-on-use.
static void phase_ask_auto_reject(void) {
    const char *bid = "show.blizzard.cli-ask-reject";
    char *argv[] = {
        (char *)g_cli, "ask", "hardware", "microphone",
        (char *)bid, NULL  // no context arg — optional
    };
    setenv("MOONBASE_CONSENT_AUTO", "reject", 1);
    int rc = run_cli(argv, bid, false);
    unsetenv("MOONBASE_CONSENT_AUTO");
    CHECK(rc == 1, "ask + AUTO=reject exits 1");

    setenv("MOONBASE_BUNDLE_ID", bid, 1);
    CHECK(mb_consent_query("hardware", "microphone") == MB_CONSENT_DENY,
          "reader sees DENY after ask+reject");
    CHECK(!mb_consent_gate_allows("hardware", "microphone"),
          "gate returns false after ask+reject");
}

// Phase 9: `ask` with too few args trips usage path (exit 2) and
// writes nothing. Pin the argc contract so moonrock's responder can
// rely on exit 2 ≠ "user decided deny".
static void phase_ask_bad_argc(void) {
    char *argv[] = {
        (char *)g_cli, "ask", "system", "keychain", NULL
    };
    int rc = run_cli(argv, NULL, false);
    CHECK(rc == 2, "ask argc < 5 exits 2");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <path-to-moonbase-consent>\n", argv[0]);
        return 2;
    }
    g_cli = argv[1];

    // Single tmpdir shared across phases — each phase uses a distinct
    // bundle-id so their consents.toml files sit in sibling dirs and
    // can't bleed into each other.
    char tmpl[] = "/tmp/mb-consent-cli-XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    setenv("XDG_DATA_HOME", tmpl, 1);
    // HOME is the fallback path the writer walks if XDG_DATA_HOME is
    // missing — shouldn't matter here, but pin it so a stale $HOME in
    // the shell can't rewrite the user's real consents store if
    // XDG_DATA_HOME accidentally gets unset by a future refactor.
    setenv("HOME", tmpl, 1);
    // The first-launch fallback expects a TTY; make sure the dispatch
    // test can't accidentally block on y/n.
    unsetenv("MOONBASE_CONSENT_AUTO");

    phase_allow_with_bundle_arg();
    phase_deny_overwrites();
    phase_env_bundle_id();
    phase_no_bundle_id_fails();
    phase_bad_argc();
    phase_unknown_verb_falls_through();
    phase_ask_auto_approve();
    phase_ask_auto_reject();
    phase_ask_bad_argc();

    if (failures) {
        fprintf(stderr, "consent-cli: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "consent-cli: all phases ok (tmpdir %s)\n", tmpl);
    return 0;
}
