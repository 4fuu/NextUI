#define _GNU_SOURCE

#include "app_capabilities.h"
#include "app_state.h"
#include "pak_manifest.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

static void check_true(const char* file, int line, const char* expression, int ok)
{
    checks++;
    if (ok) return;
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    failures++;
}

static void check_int(const char* file, int line, const char* expression, long long actual,
                      long long expected)
{
    checks++;
    if (actual == expected) return;
    fprintf(stderr, "FAIL %s:%d: %s: got %lld, expected %lld\n", file, line, expression,
            actual, expected);
    failures++;
}

static void check_str(const char* file, int line, const char* expression, const char* actual,
                      const char* expected)
{
    checks++;
    if (actual != NULL && expected != NULL && strcmp(actual, expected) == 0) return;
    fprintf(stderr, "FAIL %s:%d: %s: got \"%s\", expected \"%s\"\n", file, line, expression,
            actual != NULL ? actual : "(null)", expected != NULL ? expected : "(null)");
    failures++;
}

#define CHECK(condition) check_true(__FILE__, __LINE__, #condition, (condition))
#define CHECK_INT(actual, expected) check_int(__FILE__, __LINE__, #actual, (long long)(actual), (long long)(expected))
#define CHECK_STR(actual, expected) check_str(__FILE__, __LINE__, #actual, (actual), (expected))

typedef struct
{
    char path[512];
} TempRoot;

static TempRoot temp;

static int path_join(char* out, size_t out_size, const char* a, const char* b)
{
    int written = snprintf(out, out_size, "%s/%s", a, b);
    return written >= 0 && (size_t)written < out_size;
}

static int mkdir_p(const char* path)
{
    char work[512];
    size_t length;
    size_t i;

    if (path == NULL || strlen(path) >= sizeof work) return 0;
    snprintf(work, sizeof work, "%s", path);
    length = strlen(work);
    while (length > 1 && work[length - 1] == '/') work[--length] = '\0';
    for (i = 1; i < length; i++)
    {
        if (work[i] != '/') continue;
        work[i] = '\0';
        if (mkdir(work, 0755) != 0 && errno != EEXIST) return 0;
        work[i] = '/';
    }
    if (mkdir(work, 0755) != 0 && errno != EEXIST) return 0;
    return 1;
}

static int ensure_parent(const char* path)
{
    char copy[512];
    char* slash;

    if (strlen(path) >= sizeof copy) return 0;
    snprintf(copy, sizeof copy, "%s", path);
    slash = strrchr(copy, '/');
    if (slash == NULL) return 1;
    *slash = '\0';
    return mkdir_p(copy);
}

static int write_text(const char* path, const char* text)
{
    FILE* file;
    size_t length;
    int ok;

    if (!ensure_parent(path)) return 0;
    file = fopen(path, "wb");
    if (file == NULL) return 0;
    length = strlen(text);
    ok = fwrite(text, 1, length, file) == length;
    if (fclose(file) != 0) ok = 0;
    return ok;
}

static int read_text(const char* path, char* out, size_t out_size)
{
    FILE* file;
    size_t length;

    if (out_size == 0) return 0;
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    length = fread(out, 1, out_size - 1, file);
    if (ferror(file))
    {
        fclose(file);
        return 0;
    }
    fclose(file);
    out[length] = '\0';
    return 1;
}

static void pak_dir(const char* name, char* out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s", temp.path, name);
}

static int create_pak(const char* name, const char* json)
{
    char directory[640];
    char path[700];

    pak_dir(name, directory, sizeof directory);
    if (mkdir(directory, 0755) != 0) return 0;

    if (!path_join(path, sizeof path, directory, "launch.sh")) return 0;
    if (!write_text(path, "#!/bin/sh\nexit 0\n")) return 0;

    if (json != NULL)
    {
        if (!path_join(path, sizeof path, directory, "pak.json")) return 0;
        if (!write_text(path, json)) return 0;
    }
    return 1;
}

static PakManifestStatus load_manifest_status(const char* name, PakManifest* manifest)
{
    char directory[640];

    pak_dir(name, directory, sizeof directory);
    return pak_manifest_load(directory, manifest);
}

static int load_manifest(const char* name, PakManifest* manifest)
{
    return load_manifest_status(name, manifest) == PAK_MANIFEST_OK;
}

typedef struct
{
    char payload_path[700];
    int writes;
    int reads;
    int write_ok;
    int write_nothing;
    int read_ok;
    int can_save;
    int with_write;
    int with_read;
    int with_can_save;
} TestApp;

static void test_app_init(TestApp* app)
{
    memset(app, 0, sizeof *app);
    app->write_ok = 1;
    app->read_ok = 1;
    app->can_save = 1;
    app->with_write = 1;
    app->with_read = 1;
}

static int test_write_state(void* user)
{
    TestApp* app = (TestApp*)user;

    app->writes++;
    if (app->write_nothing) return 1;
    if (!app->write_ok) return 0;
    return write_text(app->payload_path, "save payload\n");
}

static int test_read_state(void* user)
{
    TestApp* app = (TestApp*)user;

    app->reads++;
    return app->read_ok;
}

static int test_can_save(void* user)
{
    return ((TestApp*)user)->can_save;
}

static AppCapabilityCallbacks callbacks_for(TestApp* app)
{
    AppCapabilityCallbacks callbacks;

    memset(&callbacks, 0, sizeof callbacks);
    callbacks.user = app;
    if (app->with_write) callbacks.write_state = test_write_state;
    if (app->with_read) callbacks.read_state = test_read_state;
    if (app->with_can_save) callbacks.can_save = test_can_save;
    return callbacks;
}

static int init_adapter(AppCapabilityAdapter* adapter, const char* name,
                        const PakManifest* manifest, TestApp* app)
{
    char directory[640];
    AppCapabilityCallbacks callbacks = callbacks_for(app);
    const char* payload;

    pak_dir(name, directory, sizeof directory);
    if (!app_capabilities_adapter_init(adapter, temp.path, directory, manifest, &callbacks))
        return 0;

    payload = app_capabilities_adapter_payload_dir(adapter);
    if (payload != NULL)
        snprintf(app->payload_path, sizeof app->payload_path, "%s/save.dat", payload);
    return 1;
}

static int commit_initial_state(const char* name, const PakManifest* manifest,
                                const char* data)
{
    char directory[640];
    char path[700];
    char error[APP_STATE_REASON_MAX];
    AppStateInfo info;

    pak_dir(name, directory, sizeof directory);
    if (!app_state_prepare(temp.path, directory, manifest, &info)) return 0;
    if (!path_join(path, sizeof path, info.payload_dir, "save.dat")) return 0;
    if (!write_text(path, data)) return 0;
    error[0] = '\0';
    return app_state_commit(temp.path, directory, manifest, error, sizeof error)
           == APP_STATE_COMMIT_OK;
}

static int tamper_payload(const char* name, const PakManifest* manifest)
{
    char directory[640];
    char path[700];
    AppStateInfo info;

    pak_dir(name, directory, sizeof directory);
    if (app_state_inspect(temp.path, directory, manifest, &info) != APP_STATE_VALID)
        return 0;
    if (!path_join(path, sizeof path, info.payload_dir, "save.dat")) return 0;
    return write_text(path, "tampered data");
}

static void inspect_pak(const char* name, const PakManifest* manifest,
                        AppCapabilityInfo* info)
{
    char directory[640];

    pak_dir(name, directory, sizeof directory);
    app_capabilities_inspect(temp.path, directory, manifest, info);
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */
/* ------------------------------------------------------------------------- */

static void test_absent_manifest(void)
{
    PakManifest manifest;
    AppCapabilityInfo info;
    AppCapabilityCallbacks callbacks;
    TestApp app;
    AppCapabilityAdapter adapter;

    CHECK(create_pak("Legacy.pak", NULL));
    CHECK_INT(load_manifest_status("Legacy.pak", &manifest), PAK_MANIFEST_ABSENT);

    inspect_pak("Legacy.pak", &manifest, &info);
    CHECK_INT(info.declared, 0);
    CHECK_INT(info.saves_declared, 0);
    CHECK_INT(info.overlay_declared, 0);
    CHECK_INT(info.state_status, APP_STATE_UNSUPPORTED);
    CHECK_INT(info.state_valid, 0);
    CHECK_INT(app_capabilities_actions(&info, APP_CAPABILITIES_DESKTOP, NULL), 0);
    CHECK_INT(app_capabilities_overlay_supported(&info), 0);
    CHECK_INT(app_capabilities_overlay_capabilities(&info), 0);
    CHECK_INT(app_capabilities_overlay_row_visible(&info, APP_CAPABILITIES_ACTION_SAVE), 0);
    CHECK_INT(app_capabilities_overlay_row_visible(&info, APP_CAPABILITIES_ACTION_RESUME), 0);

    test_app_init(&app);
    callbacks = callbacks_for(&app);
    CHECK_INT(app_capabilities_actions(&info, APP_CAPABILITIES_APP, &callbacks), 0);
    CHECK_INT(init_adapter(&adapter, "Legacy.pak", &manifest, &app), 1);
    CHECK(app_capabilities_adapter_hooks(&adapter) == NULL);
    CHECK(app_capabilities_adapter_payload_dir(&adapter) == NULL);
}

static void test_declared_saves_without_overlay(void)
{
    PakManifest manifest;
    AppCapabilityInfo info;
    AppCapabilityCallbacks callbacks;
    TestApp app;
    AppCapabilityAdapter adapter;

    CHECK(create_pak("SavesOnly.pak",
                     "{\"id\":\"app.saves-only\",\"capabilities\":[\"saves\"]}"));
    CHECK(load_manifest("SavesOnly.pak", &manifest));

    inspect_pak("SavesOnly.pak", &manifest, &info);
    CHECK_INT(info.saves_declared, 1);
    CHECK_INT(info.overlay_declared, 0);
    CHECK_INT(info.state_status, APP_STATE_ABSENT);
    CHECK_INT(info.state_valid, 0);
    CHECK_INT(app_capabilities_actions(&info, APP_CAPABILITIES_DESKTOP, NULL), 0);

    test_app_init(&app);
    callbacks = callbacks_for(&app);
    /* "saves" alone is not an overlay adaptation. */
    CHECK_INT(app_capabilities_actions(&info, APP_CAPABILITIES_APP, &callbacks), 0);
    CHECK_INT(app_capabilities_overlay_supported(&info), 0);
    CHECK_INT(app_capabilities_overlay_capabilities(&info), 0);
    CHECK_INT(app_capabilities_overlay_row_visible(&info, APP_CAPABILITIES_ACTION_SAVE), 0);
    CHECK_INT(init_adapter(&adapter, "SavesOnly.pak", &manifest, &app), 1);
    CHECK(app_capabilities_adapter_hooks(&adapter) == NULL);

    /* A non-overlay app still gets the desktop resume launch when B3 validates
     * a state; it just never gets shared-overlay rows. */
    CHECK(commit_initial_state("SavesOnly.pak", &manifest, "save payload\n"));
    inspect_pak("SavesOnly.pak", &manifest, &info);
    CHECK_INT(info.state_valid, 1);
    CHECK_INT(app_capabilities_actions(&info, APP_CAPABILITIES_DESKTOP, NULL),
              APP_CAPABILITIES_ACTION_RESUME);
    CHECK_INT(app_capabilities_overlay_capabilities(&info), 0);
}

static void test_adapted_first_save_and_resume(void)
{
    PakManifest manifest;
    AppCapabilityInfo info;
    AppCapabilityCallbacks callbacks;
    TestApp app;
    AppCapabilityAdapter adapter;
    const OverlayAppHooks* hooks;
    char payload[64];

    CHECK(create_pak("Adapted.pak",
                     "{\"id\":\"app.adapted\",\"name\":\"Adapted\","
                     "\"capabilities\":[\"saves\",\"overlay\"]}"));
    CHECK(load_manifest("Adapted.pak", &manifest));

    inspect_pak("Adapted.pak", &manifest, &info);
    CHECK_INT(info.saves_declared, 1);
    CHECK_INT(info.overlay_declared, 1);
    CHECK_INT(info.state_status, APP_STATE_ABSENT);

    test_app_init(&app);
    callbacks = callbacks_for(&app);

    /* Save is possible before any state exists; Resume is not. */
    CHECK_INT(app_capabilities_overlay_row_visible(&info, APP_CAPABILITIES_ACTION_SAVE), 1);
    CHECK_INT(app_capabilities_overlay_row_visible(&info, APP_CAPABILITIES_ACTION_RESUME), 1);
    CHECK_INT(app_capabilities_overlay_row_enabled(&info, APP_CAPABILITIES_ACTION_SAVE,
                                                   &callbacks), 1);
    CHECK_INT(app_capabilities_overlay_row_enabled(&info, APP_CAPABILITIES_ACTION_RESUME,
                                                   &callbacks), 0);
    CHECK_INT(app_capabilities_actions(&info, APP_CAPABILITIES_DESKTOP, NULL), 0);

    CHECK_INT(init_adapter(&adapter, "Adapted.pak", &manifest, &app), 1);
    hooks = app_capabilities_adapter_hooks(&adapter);
    CHECK(hooks != NULL);
    if (hooks == NULL) return;

    CHECK_INT(hooks->capabilities,
              UI_OVERLAY_CAP_SAVE | UI_OVERLAY_CAP_RESUME);
    CHECK_STR(hooks->app_name, "Adapted");
    CHECK(hooks->save != NULL);
    CHECK(hooks->can_save != NULL);
    CHECK(hooks->resume != NULL);
    CHECK(hooks->can_resume != NULL);
    CHECK_INT(hooks->can_save(hooks->user), 1);
    CHECK_INT(hooks->can_resume(hooks->user), 0);

    CHECK_INT(hooks->save(hooks->user), 1);
    CHECK_INT(app.writes, 1);
    CHECK_INT(app_capabilities_overlay_row_enabled(&adapter.info,
                                                   APP_CAPABILITIES_ACTION_RESUME,
                                                   &adapter.callbacks), 1);
    CHECK_INT(hooks->can_resume(hooks->user), 1);

    /* Desktop may now offer Resume, but still never Save or Overlay. */
    CHECK_INT(app_capabilities_actions(&adapter.info, APP_CAPABILITIES_DESKTOP, NULL),
              APP_CAPABILITIES_ACTION_RESUME);

    CHECK_INT(hooks->resume(hooks->user), 1);
    CHECK_INT(app.reads, 1);

    CHECK(read_text(app.payload_path, payload, sizeof payload));
    CHECK_STR(payload, "save payload\n");
}

static void test_save_commit_failure_is_false(void)
{
    PakManifest manifest;
    TestApp app;
    AppCapabilityAdapter adapter;
    const OverlayAppHooks* hooks;

    CHECK(create_pak("NoPayload.pak",
                     "{\"id\":\"app.no-payload\","
                     "\"capabilities\":[\"saves\",\"overlay\"]}"));
    CHECK(load_manifest("NoPayload.pak", &manifest));

    test_app_init(&app);
    app.write_nothing = 1;
    CHECK_INT(init_adapter(&adapter, "NoPayload.pak", &manifest, &app), 1);
    hooks = app_capabilities_adapter_hooks(&adapter);
    CHECK(hooks != NULL);
    if (hooks == NULL) return;

    /* write_state reports success but publishes no payload. app_state_commit
     * returns APP_STATE_COMMIT_NO_PAYLOAD (2); the adapter must return 0, not
     * the raw enum value. */
    CHECK_INT(hooks->save(hooks->user), 0);
    CHECK_INT(app.writes, 1);
    CHECK_INT(adapter.info.state_status, APP_STATE_ABSENT);
    CHECK_INT(hooks->can_resume(hooks->user), 0);
}

static void test_write_failure_preserves_state(void)
{
    PakManifest manifest;
    TestApp app;
    AppCapabilityAdapter adapter;
    const OverlayAppHooks* hooks;

    CHECK(create_pak("WriteFails.pak",
                     "{\"id\":\"app.write-fails\","
                     "\"capabilities\":[\"saves\",\"overlay\"]}"));
    CHECK(load_manifest("WriteFails.pak", &manifest));

    test_app_init(&app);
    CHECK_INT(init_adapter(&adapter, "WriteFails.pak", &manifest, &app), 1);
    hooks = app_capabilities_adapter_hooks(&adapter);
    CHECK(hooks != NULL);
    if (hooks == NULL) return;

    CHECK_INT(hooks->save(hooks->user), 1);
    CHECK_INT(hooks->can_resume(hooks->user), 1);

    app.write_ok = 0;
    CHECK_INT(hooks->save(hooks->user), 0);
    CHECK_INT(app.writes, 2);
    /* The previous valid state must remain untouched. */
    CHECK_INT(adapter.info.state_status, APP_STATE_VALID);
    CHECK_INT(hooks->can_resume(hooks->user), 1);
}

static void test_bad_state_hides_resume_but_not_save(void)
{
    PakManifest manifest;
    TestApp app;
    AppCapabilityAdapter adapter;
    const OverlayAppHooks* hooks;

    CHECK(create_pak("BadState.pak",
                     "{\"id\":\"app.bad-state\","
                     "\"capabilities\":[\"saves\",\"overlay\"]}"));
    CHECK(load_manifest("BadState.pak", &manifest));
    CHECK(commit_initial_state("BadState.pak", &manifest, "save payload\n"));

    test_app_init(&app);
    CHECK_INT(init_adapter(&adapter, "BadState.pak", &manifest, &app), 1);
    hooks = app_capabilities_adapter_hooks(&adapter);
    CHECK(hooks != NULL);
    if (hooks == NULL) return;

    CHECK_INT(hooks->can_resume(hooks->user), 1);
    CHECK(tamper_payload("BadState.pak", &manifest));

    app_capabilities_adapter_refresh(&adapter);
    CHECK_INT(adapter.info.state_status, APP_STATE_INVALID);
    CHECK_STR(adapter.info.state_reason, "payload_hash");
    CHECK_INT(app_capabilities_actions(&adapter.info, APP_CAPABILITIES_DESKTOP, NULL), 0);
    CHECK_INT(app_capabilities_overlay_row_visible(&adapter.info,
                                                   APP_CAPABILITIES_ACTION_RESUME), 1);
    CHECK_INT(app_capabilities_overlay_row_enabled(&adapter.info,
                                                   APP_CAPABILITIES_ACTION_RESUME,
                                                   &adapter.callbacks), 0);
    CHECK_INT(hooks->can_resume(hooks->user), 0);
    CHECK_INT(hooks->resume(hooks->user), 0);
    CHECK_INT(app.reads, 0);

    /* A bad state must not block a repair save. */
    CHECK_INT(app_capabilities_overlay_row_enabled(&adapter.info,
                                                   APP_CAPABILITIES_ACTION_SAVE,
                                                   &adapter.callbacks), 1);
    CHECK_INT(hooks->save(hooks->user), 1);
    CHECK_INT(adapter.info.state_status, APP_STATE_VALID);
    CHECK_INT(hooks->can_resume(hooks->user), 1);
}

static void test_invalid_manifest(void)
{
    PakManifest manifest;
    AppCapabilityInfo info;
    TestApp app;
    AppCapabilityAdapter adapter;

    CHECK(create_pak("Broken.pak", "{\"id\":\"app.broken\",\"capabilities\":["));
    CHECK_INT(load_manifest_status("Broken.pak", &manifest), PAK_MANIFEST_INVALID);

    inspect_pak("Broken.pak", &manifest, &info);
    CHECK_INT(info.declared, 0);
    CHECK_INT(info.saves_declared, 0);
    CHECK_INT(info.overlay_declared, 0);
    CHECK_INT(app_capabilities_actions(&info, APP_CAPABILITIES_DESKTOP, NULL), 0);
    CHECK_INT(app_capabilities_overlay_supported(&info), 0);

    test_app_init(&app);
    CHECK_INT(init_adapter(&adapter, "Broken.pak", &manifest, &app), 1);
    CHECK(app_capabilities_adapter_hooks(&adapter) == NULL);
}

static void test_missing_callbacks_keep_rows_visible_disabled(void)
{
    PakManifest manifest;
    AppCapabilityInfo info;
    AppCapabilityCallbacks callbacks;
    TestApp app;
    AppCapabilityAdapter adapter;
    const OverlayAppHooks* hooks;

    CHECK(create_pak("NoCallbacks.pak",
                     "{\"id\":\"app.no-callbacks\","
                     "\"capabilities\":[\"saves\",\"overlay\"]}"));
    CHECK(load_manifest("NoCallbacks.pak", &manifest));
    CHECK(commit_initial_state("NoCallbacks.pak", &manifest, "save payload\n"));

    inspect_pak("NoCallbacks.pak", &manifest, &info);
    CHECK_INT(info.state_valid, 1);

    test_app_init(&app);
    app.with_write = 0;
    app.with_read = 0;
    callbacks = callbacks_for(&app);

    /* Declared capabilities keep the rows visible; missing runtime callbacks
     * disable them rather than hiding the capability or claiming success. */
    CHECK_INT(app_capabilities_overlay_row_visible(&info, APP_CAPABILITIES_ACTION_SAVE), 1);
    CHECK_INT(app_capabilities_overlay_row_visible(&info, APP_CAPABILITIES_ACTION_RESUME), 1);
    CHECK_INT(app_capabilities_overlay_row_enabled(&info, APP_CAPABILITIES_ACTION_SAVE,
                                                   &callbacks), 0);
    CHECK_INT(app_capabilities_overlay_row_enabled(&info, APP_CAPABILITIES_ACTION_RESUME,
                                                   &callbacks), 0);
    CHECK_INT(app_capabilities_actions(&info, APP_CAPABILITIES_APP, &callbacks),
              APP_CAPABILITIES_ACTION_OVERLAY);
    CHECK_INT(app_capabilities_overlay_row_enabled(&info, APP_CAPABILITIES_ACTION_SAVE,
                                                   NULL), 0);
    CHECK_INT(app_capabilities_overlay_row_enabled(&info, APP_CAPABILITIES_ACTION_RESUME,
                                                   NULL), 0);

    CHECK_INT(init_adapter(&adapter, "NoCallbacks.pak", &manifest, &app), 1);
    hooks = app_capabilities_adapter_hooks(&adapter);
    CHECK(hooks != NULL);
    if (hooks != NULL)
    {
        CHECK_INT(hooks->capabilities, UI_OVERLAY_CAP_SAVE | UI_OVERLAY_CAP_RESUME);
        CHECK(hooks->save == NULL);
        CHECK(hooks->resume == NULL);
        CHECK(hooks->can_save == NULL);
        CHECK(hooks->can_resume == NULL);
    }

    /* The desktop still sees the valid state and may launch B3 resume. */
    CHECK_INT(app_capabilities_actions(&info, APP_CAPABILITIES_DESKTOP, NULL),
              APP_CAPABILITIES_ACTION_RESUME);
}

static void test_can_save_gate(void)
{
    PakManifest manifest;
    TestApp app;
    AppCapabilityAdapter adapter;
    const OverlayAppHooks* hooks;

    CHECK(create_pak("Gate.pak",
                     "{\"id\":\"app.gate\","
                     "\"capabilities\":[\"saves\",\"overlay\"]}"));
    CHECK(load_manifest("Gate.pak", &manifest));

    test_app_init(&app);
    app.with_can_save = 1;
    app.can_save = 0;
    CHECK_INT(init_adapter(&adapter, "Gate.pak", &manifest, &app), 1);
    hooks = app_capabilities_adapter_hooks(&adapter);
    CHECK(hooks != NULL);
    if (hooks == NULL) return;

    CHECK_INT(app_capabilities_overlay_row_enabled(&adapter.info,
                                                   APP_CAPABILITIES_ACTION_SAVE,
                                                   &adapter.callbacks), 0);
    CHECK_INT(hooks->can_save(hooks->user), 0);
    CHECK_INT(hooks->save(hooks->user), 0);
    CHECK_INT(app.writes, 0);

    app.can_save = 1;
    CHECK_INT(hooks->can_save(hooks->user), 1);
    CHECK_INT(hooks->save(hooks->user), 1);
    CHECK_INT(app.writes, 1);
}

static void test_resume_requires_read_callback(void)
{
    PakManifest manifest;
    TestApp app;
    AppCapabilityAdapter adapter;
    const OverlayAppHooks* hooks;

    CHECK(create_pak("ReadMissing.pak",
                     "{\"id\":\"app.read-missing\","
                     "\"capabilities\":[\"saves\",\"overlay\"]}"));
    CHECK(load_manifest("ReadMissing.pak", &manifest));
    CHECK(commit_initial_state("ReadMissing.pak", &manifest, "save payload\n"));

    test_app_init(&app);
    app.with_read = 0;
    CHECK_INT(init_adapter(&adapter, "ReadMissing.pak", &manifest, &app), 1);
    hooks = app_capabilities_adapter_hooks(&adapter);
    CHECK(hooks != NULL);
    if (hooks == NULL) return;

    /* The row is visible because the capability is declared, but unavailable
     * without the runtime callback. The desktop can still launch a B3 resume
     * because it does not need an in-process callback. */
    CHECK_INT(app_capabilities_overlay_row_visible(&adapter.info,
                                                   APP_CAPABILITIES_ACTION_RESUME), 1);
    CHECK_INT(app_capabilities_overlay_row_enabled(&adapter.info,
                                                   APP_CAPABILITIES_ACTION_RESUME,
                                                   &adapter.callbacks), 0);
    CHECK(hooks->resume == NULL);
    CHECK_INT(app_capabilities_actions(&adapter.info, APP_CAPABILITIES_DESKTOP, NULL),
              APP_CAPABILITIES_ACTION_RESUME);
}

static void test_overlay_only_and_unknown_alias(void)
{
    PakManifest manifest;
    AppCapabilityInfo info;
    TestApp app;
    AppCapabilityAdapter adapter;
    const OverlayAppHooks* hooks;

    CHECK(create_pak("OverlayOnly.pak",
                     "{\"id\":\"app.overlay-only\",\"capabilities\":[\"overlay\"]}"));
    CHECK(load_manifest("OverlayOnly.pak", &manifest));
    inspect_pak("OverlayOnly.pak", &manifest, &info);
    CHECK_INT(info.overlay_declared, 1);
    CHECK_INT(info.saves_declared, 0);
    CHECK_INT(app_capabilities_overlay_supported(&info), 1);
    CHECK_INT(app_capabilities_overlay_capabilities(&info), 0);
    CHECK_INT(app_capabilities_overlay_row_visible(&info, APP_CAPABILITIES_ACTION_SAVE), 0);
    CHECK_INT(app_capabilities_overlay_row_visible(&info, APP_CAPABILITIES_ACTION_RESUME), 0);

    test_app_init(&app);
    CHECK_INT(init_adapter(&adapter, "OverlayOnly.pak", &manifest, &app), 1);
    hooks = app_capabilities_adapter_hooks(&adapter);
    CHECK(hooks != NULL);
    if (hooks != NULL)
    {
        CHECK_INT(hooks->capabilities, 0);
        CHECK(hooks->save == NULL);
        CHECK(hooks->resume == NULL);
    }

    /* "resume" is not an alias for "saves"; B1 keeps the exact names. */
    CHECK(create_pak("Alias.pak",
                     "{\"id\":\"app.alias\",\"capabilities\":[\"resume\",\"save\"]}"));
    CHECK(load_manifest("Alias.pak", &manifest));
    inspect_pak("Alias.pak", &manifest, &info);
    CHECK_INT(info.saves_declared, 0);
    CHECK_INT(info.overlay_declared, 0);
    CHECK_INT(app_capabilities_actions(&info, APP_CAPABILITIES_DESKTOP, NULL), 0);
    CHECK_INT(app_capabilities_overlay_supported(&info), 0);
}

static void test_desktop_never_saves(void)
{
    PakManifest manifest;
    AppCapabilityInfo info;
    TestApp app;
    AppCapabilityAdapter adapter;

    CHECK(create_pak("Desktop.pak",
                     "{\"id\":\"app.desktop\","
                     "\"capabilities\":[\"saves\",\"overlay\"]}"));
    CHECK(load_manifest("Desktop.pak", &manifest));
    CHECK(commit_initial_state("Desktop.pak", &manifest, "save payload\n"));

    inspect_pak("Desktop.pak", &manifest, &info);
    CHECK_INT(info.state_valid, 1);
    CHECK_INT(app_capabilities_actions(&info, APP_CAPABILITIES_DESKTOP, NULL),
              APP_CAPABILITIES_ACTION_RESUME);

    /* Even with a running app's callbacks available, the desktop decision
     * never claims Save or Overlay: the launcher is not the app process. */
    test_app_init(&app);
    CHECK_INT(init_adapter(&adapter, "Desktop.pak", &manifest, &app), 1);
    CHECK_INT(app_capabilities_actions(&adapter.info, APP_CAPABILITIES_DESKTOP,
                                       &adapter.callbacks),
              APP_CAPABILITIES_ACTION_RESUME);
}

static void test_payload_dir_matches_b3(void)
{
    PakManifest manifest;
    TestApp app;
    AppCapabilityAdapter adapter;
    AppStateInfo state;
    char directory[640];

    CHECK(create_pak("PayloadDir.pak",
                     "{\"id\":\"app.payload-dir\","
                     "\"capabilities\":[\"saves\",\"overlay\"]}"));
    CHECK(load_manifest("PayloadDir.pak", &manifest));
    test_app_init(&app);
    CHECK_INT(init_adapter(&adapter, "PayloadDir.pak", &manifest, &app), 1);

    pak_dir("PayloadDir.pak", directory, sizeof directory);
    CHECK_INT(app_state_inspect(temp.path, directory, &manifest, &state), APP_STATE_ABSENT);
    CHECK_STR(app_capabilities_adapter_payload_dir(&adapter), state.payload_dir);
}

int main(void)
{
    snprintf(temp.path, sizeof temp.path, "/tmp/nextui-app-capabilities-XXXXXX");
    if (mkdtemp(temp.path) == NULL)
    {
        perror("mkdtemp");
        return 1;
    }

    test_absent_manifest();
    test_declared_saves_without_overlay();
    test_adapted_first_save_and_resume();
    test_save_commit_failure_is_false();
    test_write_failure_preserves_state();
    test_bad_state_hides_resume_but_not_save();
    test_invalid_manifest();
    test_missing_callbacks_keep_rows_visible_disabled();
    test_can_save_gate();
    test_resume_requires_read_callback();
    test_overlay_only_and_unknown_alias();
    test_desktop_never_saves();
    test_payload_dir_matches_b3();

    {
        char command[600];
        int status;

        snprintf(command, sizeof command, "rm -rf -- '%s'", temp.path);
        status = system(command);
        if (status != 0)
            fprintf(stderr, "warning: could not remove %s (status %d)\n", temp.path, status);
    }

    if (failures != 0)
    {
        fprintf(stderr, "%d/%d app capability checks failed\n", failures, checks);
        return 1;
    }
    printf("app capability tests passed (%d checks)\n", checks);
    return 0;
}
