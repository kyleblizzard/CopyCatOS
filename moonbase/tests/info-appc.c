// CopyCatOS — by Kyle Blizzard at Blizzard.show

// mb-info-appc — unit test for the Info.appc schema loader.
//
// Covers: happy path (C bundle), happy path (web bundle), empty
// [permissions], every error class the loader reports, and the
// size-cap branch. Fixtures live inline as C strings — no filesystem
// setup required.

#include "bundle/info_appc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_count = 0;

#define EXPECT(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); \
        fail_count++; \
    } \
} while (0)

static mb_info_appc_err_t parse(const char *src, mb_info_appc_t *out, char *err, size_t err_cap) {
    return mb_info_appc_parse_buffer(src, strlen(src), out, err, err_cap);
}

// -----------------------------------------------------------------------
// fixtures
// -----------------------------------------------------------------------

static const char *HAPPY_C =
"# Happy-path C bundle.\n"
"\n"
"[bundle]\n"
"id = \"show.blizzard.hello\"\n"
"name = \"Hello\"\n"
"version = \"1.0.0\"\n"
"minimum-moonbase = \"1.0\"\n"
"copyright = \"© 2026 Blizzard.show\"\n"
"category = \"utility\"\n"
"\n"
"[executable]\n"
"path = \"Contents/MacOS/hello\"\n"
"language = \"c\"\n"
"\n"
"[permissions]\n"
"filesystem = [\"documents:read\"]\n"
"network = [\"outbound:https\"]\n"
"hardware = [\"controller:read\"]\n"
"system = [\"notifications:post\", \"keychain\"]\n"
"ipc = [\"show.blizzard.mail\"]\n"
"\n"
"[localization]\n"
"base-locale = \"en\"\n"
"supported-locales = [\"en\", \"ja\", \"de\"]\n"
"\n"
"[update]\n"
"url = \"https://blizzard.show/updates/hello.json\"\n"
"channel = \"stable\"\n"
;

static const char *HAPPY_WEB =
"[bundle]\n"
"id = \"show.blizzard.mail\"\n"
"name = \"Mail\"\n"
"version = \"0.1.0\"\n"
"minimum-moonbase = \"1.0\"\n"
"\n"
"[executable]\n"
"path = \"Contents/Resources/index.html\"\n"
"language = \"web\"\n"
"\n"
"[permissions]\n"
"\n"
"[web]\n"
"main-url = \"https://mail.blizzard.show/\"\n"
"allowed-origins = [\"https://cdn.blizzard.show\"]\n"
;

static const char *EMPTY_PERMS =
"[bundle]\n"
"id = \"show.blizzard.minimal\"\n"
"name = \"Minimal\"\n"
"version = \"0.0.1\"\n"
"minimum-moonbase = \"1.0\"\n"
"\n"
"[executable]\n"
"path = \"Contents/MacOS/minimal\"\n"
"language = \"c\"\n"
"\n"
"[permissions]\n"
;

// -----------------------------------------------------------------------
// tests
// -----------------------------------------------------------------------

static void test_happy_c(void) {
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = parse(HAPPY_C, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_OK, "happy-c: rc=%s (%s)", mb_info_appc_err_string(rc), err);
    EXPECT(info.id && strcmp(info.id, "show.blizzard.hello") == 0, "id");
    EXPECT(info.name && strcmp(info.name, "Hello") == 0, "name");
    EXPECT(info.version && strcmp(info.version, "1.0.0") == 0, "version");
    EXPECT(info.minimum_moonbase && strcmp(info.minimum_moonbase, "1.0") == 0, "minmb");
    EXPECT(info.copyright && strcmp(info.copyright, "© 2026 Blizzard.show") == 0, "copyright");
    EXPECT(info.category && strcmp(info.category, "utility") == 0, "category");
    EXPECT(info.exec_path && strcmp(info.exec_path, "Contents/MacOS/hello") == 0, "exec_path");
    EXPECT(info.lang == MB_INFO_APPC_LANG_C, "lang");
    EXPECT(info.render_default == MB_INFO_APPC_RENDER_DEFAULT, "render_default");

    EXPECT(info.perm_filesystem_count == 1
        && strcmp(info.perm_filesystem[0], "documents:read") == 0, "fs[0]");
    EXPECT(info.perm_network_count == 1
        && strcmp(info.perm_network[0], "outbound:https") == 0, "net[0]");
    EXPECT(info.perm_hardware_count == 1
        && strcmp(info.perm_hardware[0], "controller:read") == 0, "hw[0]");
    EXPECT(info.perm_system_count == 2
        && strcmp(info.perm_system[0], "notifications:post") == 0
        && strcmp(info.perm_system[1], "keychain") == 0, "sys");
    EXPECT(info.perm_ipc_count == 1
        && strcmp(info.perm_ipc[0], "show.blizzard.mail") == 0, "ipc[0]");

    EXPECT(info.base_locale && strcmp(info.base_locale, "en") == 0, "base_locale");
    EXPECT(info.supported_locales_count == 3, "loc count");
    EXPECT(info.update_url != NULL, "update_url");
    EXPECT(info.update_channel && strcmp(info.update_channel, "stable") == 0, "channel");
    EXPECT(info.wrap_toolkit == MB_INFO_APPC_WRAP_NATIVE, "wrap_toolkit defaults to native");

    mb_info_appc_free(&info);
    // free-of-free must be safe (second free no-ops).
    mb_info_appc_free(&info);
}

static void test_happy_web(void) {
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = parse(HAPPY_WEB, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_OK, "happy-web: rc=%s (%s)", mb_info_appc_err_string(rc), err);
    EXPECT(info.lang == MB_INFO_APPC_LANG_WEB, "lang web");
    EXPECT(info.web_main_url && strcmp(info.web_main_url, "https://mail.blizzard.show/") == 0, "main-url");
    EXPECT(info.web_allowed_origins_count == 1, "allowed count");
    mb_info_appc_free(&info);
}

static void test_empty_perms(void) {
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = parse(EMPTY_PERMS, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_OK, "empty-perms: rc=%s (%s)", mb_info_appc_err_string(rc), err);
    EXPECT(info.perm_filesystem_count == 0, "fs empty");
    EXPECT(info.perm_network_count == 0, "net empty");
    EXPECT(info.perm_hardware_count == 0, "hw empty");
    EXPECT(info.perm_system_count == 0, "sys empty");
    EXPECT(info.perm_ipc_count == 0, "ipc empty");
    mb_info_appc_free(&info);
}

static void test_missing_bundle(void) {
    const char *src =
        "[executable]\n"
        "path = \"Contents/MacOS/x\"\n"
        "language = \"c\"\n"
        "\n"
        "[permissions]\n";
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = parse(src, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_ERR_MISSING_REQUIRED, "missing-bundle rc");
    mb_info_appc_free(&info);
}

static void test_bad_id(void) {
    const char *src =
        "[bundle]\n"
        "id = \"NoDots\"\n"
        "name = \"X\"\n"
        "version = \"1.0.0\"\n"
        "minimum-moonbase = \"1.0\"\n"
        "\n"
        "[executable]\n"
        "path = \"Contents/MacOS/x\"\n"
        "language = \"c\"\n"
        "\n"
        "[permissions]\n";
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = parse(src, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_ERR_BAD_VALUE, "bad-id rc=%s", mb_info_appc_err_string(rc));
    mb_info_appc_free(&info);
}

static void test_unknown_category(void) {
    const char *src =
        "[bundle]\n"
        "id = \"show.blizzard.x\"\n"
        "name = \"X\"\n"
        "version = \"1.0.0\"\n"
        "minimum-moonbase = \"1.0\"\n"
        "category = \"something-else\"\n"
        "\n"
        "[executable]\n"
        "path = \"Contents/MacOS/x\"\n"
        "language = \"c\"\n"
        "\n"
        "[permissions]\n";
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = parse(src, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_ERR_UNKNOWN_VALUE, "bad-category rc=%s", mb_info_appc_err_string(rc));
    mb_info_appc_free(&info);
}

static void test_unknown_permission(void) {
    const char *src =
        "[bundle]\n"
        "id = \"show.blizzard.x\"\n"
        "name = \"X\"\n"
        "version = \"1.0.0\"\n"
        "minimum-moonbase = \"1.0\"\n"
        "\n"
        "[executable]\n"
        "path = \"Contents/MacOS/x\"\n"
        "language = \"c\"\n"
        "\n"
        "[permissions]\n"
        "system = [\"wiretap-everything\"]\n";
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = parse(src, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_ERR_UNKNOWN_VALUE, "bad-perm rc=%s", mb_info_appc_err_string(rc));
    mb_info_appc_free(&info);
}

static void test_unknown_lang(void) {
    const char *src =
        "[bundle]\n"
        "id = \"show.blizzard.x\"\n"
        "name = \"X\"\n"
        "version = \"1.0.0\"\n"
        "minimum-moonbase = \"1.0\"\n"
        "\n"
        "[executable]\n"
        "path = \"Contents/MacOS/x\"\n"
        "language = \"brainfuck\"\n"
        "\n"
        "[permissions]\n";
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = parse(src, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_ERR_UNKNOWN_VALUE, "bad-lang rc=%s", mb_info_appc_err_string(rc));
    mb_info_appc_free(&info);
}

static void test_web_missing_web_table(void) {
    const char *src =
        "[bundle]\n"
        "id = \"show.blizzard.mail\"\n"
        "name = \"Mail\"\n"
        "version = \"0.1.0\"\n"
        "minimum-moonbase = \"1.0\"\n"
        "\n"
        "[executable]\n"
        "path = \"Contents/Resources/index.html\"\n"
        "language = \"web\"\n"
        "\n"
        "[permissions]\n";
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = parse(src, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_ERR_MISSING_REQUIRED, "web-missing-web rc=%s",
        mb_info_appc_err_string(rc));
    mb_info_appc_free(&info);
}

static void test_ipc_not_reverse_dns(void) {
    const char *src =
        "[bundle]\n"
        "id = \"show.blizzard.x\"\n"
        "name = \"X\"\n"
        "version = \"1.0.0\"\n"
        "minimum-moonbase = \"1.0\"\n"
        "\n"
        "[executable]\n"
        "path = \"Contents/MacOS/x\"\n"
        "language = \"c\"\n"
        "\n"
        "[permissions]\n"
        "ipc = [\"not-reverse-dns\"]\n";
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = parse(src, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_ERR_BAD_VALUE, "bad-ipc rc=%s", mb_info_appc_err_string(rc));
    mb_info_appc_free(&info);
}

static void test_parse_error(void) {
    const char *src = "[bundle\nid = x\n";  // malformed header + bare value
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = parse(src, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_ERR_PARSE, "parse-err rc=%s", mb_info_appc_err_string(rc));
    mb_info_appc_free(&info);
}

static void test_too_large(void) {
    size_t n = MB_INFO_APPC_MAX_BYTES + 16;
    char *src = malloc(n + 1);
    memset(src, 'a', n);
    src[n] = '\0';
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = mb_info_appc_parse_buffer(src, n, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_ERR_TOO_LARGE, "too-large rc=%s", mb_info_appc_err_string(rc));
    free(src);
    mb_info_appc_free(&info);
}

static void test_unknown_table_warned_not_errored(void) {
    // Extra [custom-tier] table should be ignored (forward-compat per schema).
    const char *src =
        "[bundle]\n"
        "id = \"show.blizzard.x\"\n"
        "name = \"X\"\n"
        "version = \"1.0.0\"\n"
        "minimum-moonbase = \"1.0\"\n"
        "\n"
        "[executable]\n"
        "path = \"Contents/MacOS/x\"\n"
        "language = \"c\"\n"
        "\n"
        "[permissions]\n"
        "\n"
        "[sandbox]\n"
        "reserved-future-key = \"whatever\"\n";
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = parse(src, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_OK, "unknown-table rc=%s (%s)",
        mb_info_appc_err_string(rc), err);
    mb_info_appc_free(&info);
}

static void test_wrap_toolkit_qt6(void) {
    const char *src =
        "[bundle]\n"
        "id = \"show.blizzard.kate\"\n"
        "name = \"Kate\"\n"
        "version = \"1.0.0\"\n"
        "minimum-moonbase = \"1.0\"\n"
        "\n"
        "[executable]\n"
        "path = \"Contents/CopyCatOS/kate\"\n"
        "language = \"c\"\n"
        "\n"
        "[permissions]\n"
        "\n"
        "[wrap]\n"
        "toolkit = \"qt6\"\n";
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = parse(src, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_OK, "wrap-qt6 rc=%s (%s)",
        mb_info_appc_err_string(rc), err);
    EXPECT(info.wrap_toolkit == MB_INFO_APPC_WRAP_QT6, "wrap_toolkit qt6");
    mb_info_appc_free(&info);
}

static void test_wrap_toolkit_unknown(void) {
    const char *src =
        "[bundle]\n"
        "id = \"show.blizzard.x\"\n"
        "name = \"X\"\n"
        "version = \"1.0.0\"\n"
        "minimum-moonbase = \"1.0\"\n"
        "\n"
        "[executable]\n"
        "path = \"Contents/CopyCatOS/x\"\n"
        "language = \"c\"\n"
        "\n"
        "[permissions]\n"
        "\n"
        "[wrap]\n"
        "toolkit = \"tk\"\n";
    mb_info_appc_t info;
    char err[256] = {0};
    mb_info_appc_err_t rc = parse(src, &info, err, sizeof(err));
    EXPECT(rc == MB_INFO_APPC_ERR_UNKNOWN_VALUE, "wrap-unknown rc=%s",
        mb_info_appc_err_string(rc));
    mb_info_appc_free(&info);
}

int main(void) {
    test_happy_c();
    test_happy_web();
    test_empty_perms();
    test_missing_bundle();
    test_bad_id();
    test_unknown_category();
    test_unknown_permission();
    test_unknown_lang();
    test_web_missing_web_table();
    test_ipc_not_reverse_dns();
    test_parse_error();
    test_too_large();
    test_unknown_table_warned_not_errored();
    test_wrap_toolkit_qt6();
    test_wrap_toolkit_unknown();

    if (fail_count) {
        fprintf(stderr, "FAIL: %d assertion(s) failed\n", fail_count);
        return 1;
    }
    printf("ok: info-appc\n");
    return 0;
}
