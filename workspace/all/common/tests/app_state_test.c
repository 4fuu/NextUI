#define _GNU_SOURCE

#include "app_state.h"
#include "pak_manifest.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
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

static int path_join(char* out, size_t out_size, const char* a, const char* b)
{
    int written = snprintf(out, out_size, "%s/%s", a, b);
    return written >= 0 && (size_t)written < out_size;
}

static int copy_text(char* out, size_t out_size, const char* value)
{
    size_t length = strlen(value);
    if (length >= out_size) return 0;
    memcpy(out, value, length + 1);
    return 1;
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

static int write_bytes(const char* path, const void* data, size_t length)
{
    FILE* file;

    if (!ensure_parent(path)) return 0;
    file = fopen(path, "wb");
    if (file == NULL) return 0;
    if (length > 0 && fwrite(data, 1, length, file) != length)
    {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static int write_text(const char* path, const char* text)
{
    return write_bytes(path, text, strlen(text));
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

static int path_exists(const char* path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static int is_dir(const char* path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}

static int rm_cb(const char* path, const struct stat* st, int flag, struct FTW* ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static void remove_path(const char* path)
{
    if (path_exists(path)) (void)nftw(path, rm_cb, 64, FTW_DEPTH | FTW_PHYS);
}

static int temp_create(TempRoot* temp)
{
    snprintf(temp->path, sizeof temp->path, "/tmp/nextui-app-state-XXXXXX");
    return mkdtemp(temp->path) != NULL;
}

static int create_pak(const char* base, const char* dir_name, const char* manifest_json,
                      const char* launch_body, char* pak_out, size_t pak_size)
{
    char pak[512];
    char path[640];

    if (!path_join(pak, sizeof pak, base, dir_name)) return 0;
    if (!mkdir_p(pak)) return 0;
    if (manifest_json != NULL)
    {
        if (!path_join(path, sizeof path, pak, "pak.json")) return 0;
        if (!write_text(path, manifest_json)) return 0;
    }
    if (!path_join(path, sizeof path, pak, "launch.sh")) return 0;
    if (launch_body == NULL)
    {
        if (!write_text(path, "#!/bin/sh\nexit 0\n")) return 0;
    }
    else
    {
        char script[4096];
        snprintf(script, sizeof script, "#!/bin/sh\n%s\n", launch_body);
        if (!write_text(path, script)) return 0;
    }
    if (chmod(path, 0755) != 0) return 0;
    if (pak_out != NULL) snprintf(pak_out, pak_size, "%s", pak);
    return 1;
}

static int payload_write(const AppStateInfo* info, const char* relative,
                         const void* data, size_t length)
{
    char path[APP_STATE_PATH_MAX];
    if (!path_join(path, sizeof path, info->payload_dir, relative)) return 0;
    return write_bytes(path, data, length);
}

static int metadata_path_for(const char* root, const char* pak, const PakManifest* manifest,
                             char* out, size_t out_size)
{
    AppStateInfo info;
    if (!app_state_prepare(root, pak, manifest, &info)) return 0;
    return path_join(out, out_size, info.slot_dir, "resume.meta");
}

static int replace_once(char* text, size_t capacity, const char* old_text, const char* new_text)
{
    char* position = strstr(text, old_text);
    size_t old_length;
    size_t new_length;
    size_t tail;

    if (position == NULL) return 0;
    old_length = strlen(old_text);
    new_length = strlen(new_text);
    tail = strlen(position + old_length);
    if ((size_t)(position - text) + new_length + tail + 1 > capacity) return 0;
    memmove(position + new_length, position + old_length, tail + 1);
    memcpy(position, new_text, new_length);
    return 1;
}

static int remove_line(char* text, size_t capacity, const char* prefix)
{
    char* position = strstr(text, prefix);
    char* end;
    size_t length;

    (void)capacity;
    if (position == NULL) return 0;
    end = strchr(position, '\n');
    length = end != NULL ? (size_t)(end + 1 - position) : strlen(position);
    memmove(position, position + length, strlen(position + length) + 1);
    return 1;
}

static int zero_data_hash(char* text, size_t capacity)
{
    char* position = strstr(text, "file data.txt ");
    char* space;

    (void)capacity;
    if (position == NULL) return 0;
    position += strlen("file data.txt ");
    space = strchr(position, ' ');
    if (space == NULL) return 0;
    if (strlen(space + 1) < 64) return 0;
    memset(space + 1, '0', 64);
    return 1;
}

static PakManifestStatus load_pak(const char* pak, PakManifest* manifest)
{
    return pak_manifest_load(pak, manifest);
}

static void test_slot_and_prepare(TempRoot* temp)
{
    char base[512];
    char pak_a[512];
    char pak_b[512];
    char pak_c[512];
    char pak_evil[512];
    PakManifest manifest;
    AppStateInfo info;
    AppStateInfo info_b;
    PakManifest manual;

    path_join(base, sizeof base, temp->path, "slots");
    CHECK(mkdir_p(base));

    CHECK(create_pak(base, "Notes.pak",
                     "{\"id\":\"notes\",\"version\":\"v1.0.0\",\"capabilities\":[\"saves\"]}",
                     NULL, pak_a, sizeof pak_a));
    CHECK(create_pak(base, "Notes Copy.pak",
                     "{\"id\":\"notes\",\"capabilities\":[\"saves\"]}",
                     NULL, pak_b, sizeof pak_b));
    CHECK(create_pak(base, "Other.pak",
                     "{\"id\":\"notes pro\",\"capabilities\":[\"saves\"]}",
                     NULL, pak_c, sizeof pak_c));
    CHECK(create_pak(base, "Evil.pak",
                     "{\"id\":\"../evil\",\"capabilities\":[\"saves\"]}",
                     NULL, pak_evil, sizeof pak_evil));

    CHECK_INT(load_pak(pak_a, &manifest), PAK_MANIFEST_OK);
    CHECK(app_state_prepare(base, pak_a, &manifest, &info));
    CHECK(is_dir(info.slot_dir));
    CHECK(is_dir(info.payload_dir));
    CHECK(strstr(info.payload_dir, "/state") != NULL);
    CHECK(strstr(info.slot_dir, "notes-") != NULL);
    CHECK(strchr(info.slot_dir, ' ') == NULL);

    CHECK_INT(load_pak(pak_b, &manifest), PAK_MANIFEST_OK);
    CHECK(app_state_prepare(base, pak_b, &manifest, &info_b));
    CHECK_STR(info_b.slot_dir, info.slot_dir);

    CHECK_INT(load_pak(pak_c, &manifest), PAK_MANIFEST_OK);
    CHECK(app_state_prepare(base, pak_c, &manifest, &info_b));
    CHECK(strcmp(info_b.slot_dir, info.slot_dir) != 0);
    CHECK(strchr(strrchr(info_b.slot_dir, '/') + 1, ' ') == NULL);

    memset(&manual, 0, sizeof manual);
    manual.status = PAK_MANIFEST_OK;
    snprintf(manual.id, sizeof manual.id, "../escape");
    manual.capabilities = PAK_CAPABILITY_SAVES;
    CHECK_INT(app_state_inspect(base, pak_a, &manual, &info), APP_STATE_UNSUPPORTED);
    CHECK_STR(info.reason, "invalid_app_id");
    CHECK_INT(app_state_commit(base, pak_a, &manual, NULL, 0), APP_STATE_COMMIT_UNSUPPORTED);
    {
        char escape_path[640];
        path_join(escape_path, sizeof escape_path, temp->path, "escape");
        CHECK(!path_exists(escape_path));
    }

    CHECK_INT(load_pak(pak_evil, &manifest), PAK_MANIFEST_OK);
    CHECK(strstr(manifest.id, "..") == NULL);
    CHECK(app_state_prepare(base, pak_evil, &manifest, &info));
    CHECK(strncmp(info.slot_dir, base, strlen(base)) == 0);
    CHECK(payload_write(&info, "data.txt", "hello", 5));
    CHECK_INT(app_state_commit(base, pak_evil, &manifest, NULL, 0), APP_STATE_COMMIT_OK);
    {
        char escape_path[640];
        path_join(escape_path, sizeof escape_path, temp->path, "evil");
        CHECK(!path_exists(escape_path));
    }

    CHECK_INT(app_state_inspect(base, pak_a, NULL, &info), APP_STATE_ERROR);
    CHECK_STR(info.reason, "invalid_arguments");
}

static void test_save_and_inspect(TempRoot* temp)
{
    char base[512];
    char pak[512];
    char meta[512];
    char metadata[65536];
    char command[APP_STATE_CMD_MAX];
    PakManifest manifest;
    AppStateInfo info;
    unsigned char picture[256];
    const char* hello_hash = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
    const char* empty_hash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    const char* vector_hash = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
    const char* many_hash = "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3";
    const char* vector = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    char many[1000];
    int i;

    path_join(base, sizeof base, temp->path, "save");
    CHECK(mkdir_p(base));
    CHECK(create_pak(base, "Notes.pak",
                     "{\"id\":\"notes\",\"version\":\"v1.2.0\",\"min_system\":\"v99.0.0\","
                     "\"capabilities\":[\"saves\"]}",
                     NULL, pak, sizeof pak));
    CHECK_INT(load_pak(pak, &manifest), PAK_MANIFEST_OK);
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_ABSENT);
    CHECK(app_state_prepare(base, pak, &manifest, &info));
    for (i = 0; i < (int)sizeof picture; i++) picture[i] = (unsigned char)(i * 7);

    CHECK(payload_write(&info, "data.txt", "hello", 5));
    CHECK(payload_write(&info, "img/pic.bin", picture, sizeof picture));
    CHECK(payload_write(&info, "empty.bin", "", 0));
    CHECK(payload_write(&info, "with space.txt", "space", 5));
    CHECK(payload_write(&info, "vector.txt", vector, strlen(vector)));
    memset(many, 'a', sizeof many);
    CHECK(payload_write(&info, "many.bin", many, sizeof many));
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_OK);

    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_VALID);
    CHECK_INT(info.file_count, 6);
    CHECK_INT(info.total_bytes, 1322);
    CHECK_STR(info.app_id, "notes");
    CHECK_STR(info.app_version, "v1.2.0");
    CHECK(info.saved_at > 0);
    CHECK_STR(info.reason, "ok");
    CHECK(app_state_is_resumable(base, pak, &manifest));

    CHECK(metadata_path_for(base, pak, &manifest, meta, sizeof meta));
    CHECK(read_text(meta, metadata, sizeof metadata));
    CHECK(strstr(metadata, "nextui-app-state 1\n") == metadata);
    CHECK(strstr(metadata, "app notes\n") != NULL);
    CHECK(strstr(metadata, "app_version v1.2.0\n") != NULL);
    CHECK(strstr(metadata, "files 6\n") != NULL);
    CHECK(strstr(metadata, "total_bytes 1322\n") != NULL);
    CHECK(strstr(metadata, hello_hash) != NULL);
    CHECK(strstr(metadata, empty_hash) != NULL);
    CHECK(strstr(metadata, vector_hash) != NULL);
    CHECK(strstr(metadata, many_hash) != NULL);
    CHECK(strstr(metadata, "with space.txt 5 ") != NULL);

    CHECK(app_state_build_launch(base, pak, &manifest, APP_LAUNCH_NORMAL,
                                 command, sizeof command));
    CHECK(strstr(command, "NEXTUI_APP_RESUME=0 ") != NULL);
    CHECK(strstr(command, "NEXTUI_APP_ID='notes'") != NULL);
    CHECK(strstr(command, "NEXTUI_APP_STATE_DIR='") != NULL);
    CHECK(strstr(command, "--resume") == NULL);
    CHECK(strstr(command, "launch.sh'") != NULL);

    CHECK(app_state_build_launch(base, pak, &manifest, APP_LAUNCH_RESUME,
                                 command, sizeof command));
    CHECK(strstr(command, "NEXTUI_APP_RESUME=1 ") != NULL);
    CHECK(strstr(command, "--resume") == NULL);
    CHECK(strstr(command, info.payload_dir) != NULL);
    CHECK(strstr(command, "launch.sh'") != NULL);
}

static void test_missing_and_unsupported(TempRoot* temp)
{
    char base[512];
    char pak_empty[512];
    char pak_plain[512];
    char pak_alias[512];
    char pak_broken[512];
    char pak_none[512];
    char command[APP_STATE_CMD_MAX];
    char expected[APP_STATE_CMD_MAX];
    PakManifest manifest;
    AppStateInfo info;

    path_join(base, sizeof base, temp->path, "unsupported");
    CHECK(mkdir_p(base));
    CHECK(create_pak(base, "Empty.pak",
                     "{\"id\":\"empty\",\"capabilities\":[\"saves\"]}",
                     NULL, pak_empty, sizeof pak_empty));
    CHECK(create_pak(base, "Plain.pak",
                     "{\"id\":\"plain\",\"capabilities\":[\"overlay\"]}",
                     NULL, pak_plain, sizeof pak_plain));
    CHECK(create_pak(base, "Alias.pak",
                     "{\"id\":\"alias\",\"capabilities\":[\"resume\"]}",
                     NULL, pak_alias, sizeof pak_alias));
    CHECK(create_pak(base, "Broken.pak",
                     "{\"id\":\"broken\",\"capabilities\":[\"saves\"",
                     NULL, pak_broken, sizeof pak_broken));
    CHECK(create_pak(base, "Legacy.pak", NULL, NULL, pak_none, sizeof pak_none));

    CHECK_INT(load_pak(pak_empty, &manifest), PAK_MANIFEST_OK);
    CHECK_INT(app_state_inspect(base, pak_empty, &manifest, &info), APP_STATE_ABSENT);
    command[0] = 'x';
    CHECK(!app_state_build_launch(base, pak_empty, &manifest, APP_LAUNCH_RESUME,
                                  command, sizeof command));
    CHECK_STR(command, "");
    CHECK(app_state_prepare(base, pak_empty, &manifest, &info));
    CHECK_INT(app_state_commit(base, pak_empty, &manifest, NULL, 0), APP_STATE_COMMIT_NO_PAYLOAD);
    CHECK_INT(app_state_inspect(base, pak_empty, &manifest, &info), APP_STATE_ABSENT);

    {
        char bad_root[640];
        path_join(bad_root, sizeof bad_root, base, "bad-root");
        CHECK(write_text(bad_root, "not a directory"));
        CHECK(app_state_build_launch(bad_root, pak_empty, &manifest, APP_LAUNCH_NORMAL,
                                     command, sizeof command));
        snprintf(expected, sizeof expected, "'%s/launch.sh'", pak_empty);
        CHECK_STR(command, expected);
        CHECK(!app_state_build_launch(bad_root, pak_empty, &manifest, APP_LAUNCH_RESUME,
                                      command, sizeof command));
    }

    CHECK_INT(load_pak(pak_plain, &manifest), PAK_MANIFEST_OK);
    CHECK_INT(app_state_inspect(base, pak_plain, &manifest, &info), APP_STATE_UNSUPPORTED);
    CHECK_STR(info.reason, "capability_not_declared");
    CHECK(!app_state_prepare(base, pak_plain, &manifest, &info));
    CHECK_INT(app_state_commit(base, pak_plain, &manifest, NULL, 0), APP_STATE_COMMIT_UNSUPPORTED);
    CHECK(!app_state_is_resumable(base, pak_plain, &manifest));
    CHECK(app_state_build_launch(base, pak_plain, &manifest, APP_LAUNCH_NORMAL,
                                 command, sizeof command));
    snprintf(expected, sizeof expected, "'%s/launch.sh'", pak_plain);
    CHECK_STR(command, expected);
    CHECK(!app_state_build_launch(base, pak_plain, &manifest, APP_LAUNCH_RESUME,
                                  command, sizeof command));

    CHECK_INT(load_pak(pak_alias, &manifest), PAK_MANIFEST_OK);
    CHECK_INT(app_state_inspect(base, pak_alias, &manifest, &info), APP_STATE_UNSUPPORTED);
    CHECK_STR(info.reason, "capability_not_declared");

    CHECK_INT(load_pak(pak_broken, &manifest), PAK_MANIFEST_INVALID);
    CHECK_INT(app_state_inspect(base, pak_broken, &manifest, &info), APP_STATE_UNSUPPORTED);
    CHECK_STR(info.reason, "capability_not_declared");

    CHECK_INT(load_pak(pak_none, &manifest), PAK_MANIFEST_ABSENT);
    CHECK_INT(app_state_inspect(base, pak_none, &manifest, &info), APP_STATE_UNSUPPORTED);
    CHECK(app_state_build_launch(base, pak_none, &manifest, APP_LAUNCH_NORMAL,
                                 command, sizeof command));
    snprintf(expected, sizeof expected, "'%s/launch.sh'", pak_none);
    CHECK_STR(command, expected);
}

static void test_corruption(TempRoot* temp)
{
    char base[512];
    char pak[512];
    char meta[512];
    char outside[640];
    char metadata[80 * 1024];
    char saved[80 * 1024];
    char payload_file[640];
    PakManifest manifest;
    AppStateInfo info;

    path_join(base, sizeof base, temp->path, "corrupt");
    CHECK(mkdir_p(base));
    CHECK(create_pak(base, "Notes.pak",
                     "{\"id\":\"notes\",\"capabilities\":[\"saves\"]}",
                     NULL, pak, sizeof pak));
    CHECK_INT(load_pak(pak, &manifest), PAK_MANIFEST_OK);
    CHECK(app_state_prepare(base, pak, &manifest, &info));
    CHECK(payload_write(&info, "data.txt", "hello", 5));
    CHECK(payload_write(&info, "pic.bin", "picture", 7));
    CHECK(metadata_path_for(base, pak, &manifest, meta, sizeof meta));

    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_OK);
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_VALID);

    CHECK(payload_write(&info, "data.txt", "HELLO", 5));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "payload_hash");
    CHECK(!app_state_is_resumable(base, pak, &manifest));
    CHECK(payload_write(&info, "data.txt", "hello", 5));
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_OK);

    CHECK(payload_write(&info, "pic.bin", "pic", 3));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "payload_size");
    CHECK(payload_write(&info, "pic.bin", "picture", 7));
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_OK);

    CHECK(path_join(payload_file, sizeof payload_file, info.payload_dir, "pic.bin"));
    CHECK(unlink(payload_file) == 0);
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "missing_payload");
    CHECK(payload_write(&info, "pic.bin", "picture", 7));
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_OK);

    CHECK(write_text(meta, "garbage\n"));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "invalid_metadata");
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_OK);

    CHECK(read_text(meta, metadata, sizeof metadata));
    snprintf(saved, sizeof saved, "%s", metadata);

    snprintf(metadata, sizeof metadata, "%s", saved);
    CHECK(replace_once(metadata, sizeof metadata, "app notes\n", "app other\n"));
    CHECK(write_text(meta, metadata));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "invalid_metadata");
    CHECK(write_text(meta, saved));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_VALID);

    snprintf(metadata, sizeof metadata, "%s", saved);
    CHECK(remove_line(metadata, sizeof metadata, "saved_at "));
    CHECK(write_text(meta, metadata));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "invalid_metadata");

    snprintf(metadata, sizeof metadata, "%s", saved);
    CHECK(replace_once(metadata, sizeof metadata, "total_bytes 12\n", "total_bytes 99\n"));
    CHECK(write_text(meta, metadata));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "invalid_metadata");

    snprintf(metadata, sizeof metadata, "%s", saved);
    CHECK(strlen(metadata) + strlen("bogus 1\n") < sizeof metadata);
    strcat(metadata, "bogus 1\n");
    CHECK(write_text(meta, metadata));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "invalid_metadata");

    snprintf(metadata, sizeof metadata, "%s", saved);
    CHECK(zero_data_hash(metadata, sizeof metadata));
    CHECK(write_text(meta, metadata));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "payload_hash");

    snprintf(metadata, sizeof metadata, "%s", saved);
    {
        size_t length = strlen(metadata);
        CHECK(length > 16);
        metadata[10] = '\0';
        CHECK(write_bytes(meta, metadata, length));
    }
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "invalid_metadata");

    snprintf(metadata, sizeof metadata, "%s", saved);
    CHECK(replace_once(metadata, sizeof metadata, "file data.txt ", "file ../data.txt "));
    CHECK(write_text(meta, metadata));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "unsafe_path");

    snprintf(metadata, sizeof metadata, "%s", saved);
    CHECK(replace_once(metadata, sizeof metadata, "file data.txt ", "file /etc/passwd "));
    CHECK(write_text(meta, metadata));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "unsafe_path");

    snprintf(metadata, sizeof metadata, "%s", saved);
    CHECK(strlen(metadata) < 70 * 1024);
    memset(metadata + strlen(metadata), 'A', 70 * 1024 - strlen(metadata) - 1);
    metadata[70 * 1024 - 1] = '\0';
    CHECK(write_text(meta, metadata));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "metadata_too_large");

    CHECK(write_text(meta, saved));
    CHECK(path_join(outside, sizeof outside, base, "outside.meta"));
    CHECK(write_text(outside, saved));
    CHECK(unlink(meta) == 0);
    CHECK(symlink(outside, meta) == 0);
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "unsafe_metadata");
    CHECK(unlink(meta) == 0);
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_OK);

    CHECK(payload_write(&info, "data.txt", "hello", 5));
    CHECK(path_join(outside, sizeof outside, base, "outside.txt"));
    CHECK(write_text(outside, "HELLO"));
    CHECK(path_join(payload_file, sizeof payload_file, info.payload_dir, "data.txt"));
    CHECK(unlink(payload_file) == 0);
    CHECK(symlink(outside, payload_file) == 0);
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "unsafe_path");
    CHECK(unlink(payload_file) == 0);
    CHECK(payload_write(&info, "data.txt", "hello", 5));
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_OK);
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_VALID);

    CHECK(payload_write(&info, "nested/keep.txt", "keep", 4));
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_OK);
    CHECK(path_join(outside, sizeof outside, base, "nested-target"));
    CHECK(mkdir_p(outside));
    CHECK(path_join(payload_file, sizeof payload_file, outside, "keep.txt"));
    CHECK(write_text(payload_file, "keep"));
    CHECK(path_join(payload_file, sizeof payload_file, info.payload_dir, "nested"));
    remove_path(payload_file);
    CHECK(symlink(outside, payload_file) == 0);
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "unsafe_path");
    CHECK(unlink(payload_file) == 0);
    CHECK(payload_write(&info, "nested/keep.txt", "keep", 4));
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_OK);
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_VALID);
}

static void test_path_safety(TempRoot* temp)
{
    char base[512];
    char pak[512];
    char outside[640];
    char link_path[640];
    char deep[1024];
    char file_path[1200];
    PakManifest manifest;
    AppStateInfo info;
    char command[APP_STATE_CMD_MAX];
    int i;

    path_join(base, sizeof base, temp->path, "paths");
    CHECK(mkdir_p(base));
    CHECK(create_pak(base, "Safe.pak",
                     "{\"id\":\"safe\",\"capabilities\":[\"saves\"]}",
                     NULL, pak, sizeof pak));
    CHECK_INT(load_pak(pak, &manifest), PAK_MANIFEST_OK);
    CHECK(app_state_prepare(base, pak, &manifest, &info));
    CHECK(payload_write(&info, "data.txt", "hello", 5));

    CHECK(path_join(outside, sizeof outside, base, "outside.txt"));
    CHECK(write_text(outside, "outside"));
    CHECK(path_join(link_path, sizeof link_path, info.payload_dir, "link"));
    CHECK(symlink(outside, link_path) == 0);
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_UNSAFE_PATH);
    CHECK(unlink(link_path) == 0);

    CHECK(path_join(outside, sizeof outside, base, "outside-dir"));
    CHECK(mkdir_p(outside));
    CHECK(path_join(file_path, sizeof file_path, outside, "file"));
    CHECK(write_text(file_path, "outside"));
    CHECK(path_join(link_path, sizeof link_path, info.payload_dir, "dirlink"));
    CHECK(symlink(outside, link_path) == 0);
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_UNSAFE_PATH);
    CHECK(unlink(link_path) == 0);

    CHECK(path_join(file_path, sizeof file_path, info.payload_dir, "bad\nname"));
    CHECK(write_text(file_path, "bad"));
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_UNSAFE_PATH);
    CHECK(unlink(file_path) == 0);

    CHECK(path_join(file_path, sizeof file_path, info.payload_dir, "fifo"));
    CHECK(mkfifo(file_path, 0600) == 0);
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_UNSAFE_PATH);
    CHECK(unlink(file_path) == 0);

    snprintf(deep, sizeof deep, "%s", info.payload_dir);
    for (i = 0; i < 9; i++)
    {
        char next[1200];
        char dir_name[16];
        snprintf(dir_name, sizeof dir_name, "d%d", i);
        path_join(next, sizeof next, deep, dir_name);
        CHECK(mkdir(next, 0755) == 0);
        CHECK(copy_text(deep, sizeof deep, next));
    }
    CHECK(path_join(file_path, sizeof file_path, deep, "deep.txt"));
    CHECK(write_text(file_path, "deep"));
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_UNSAFE_PATH);
    CHECK(path_join(file_path, sizeof file_path, info.payload_dir, "d0"));
    remove_path(file_path);

    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_OK);
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_VALID);

    CHECK(app_state_clear(base, pak, &manifest, NULL, 0));
    CHECK(!path_exists(info.slot_dir));
    CHECK(path_join(outside, sizeof outside, base, "slot-target"));
    CHECK(mkdir_p(outside));
    CHECK(path_join(file_path, sizeof file_path, outside, "keep.txt"));
    CHECK(write_text(file_path, "keep"));
    CHECK(symlink(outside, info.slot_dir) == 0);
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "unsafe_slot");
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_IO);
    CHECK(app_state_clear(base, pak, &manifest, NULL, 0));
    CHECK(!path_exists(info.slot_dir));
    CHECK(path_exists(outside));
    CHECK(path_exists(file_path));

    memset(&manifest, 0, sizeof manifest);
    manifest.status = PAK_MANIFEST_OK;
    snprintf(manifest.id, sizeof manifest.id, "safe");
    manifest.capabilities = PAK_CAPABILITY_SAVES;
    CHECK(app_state_prepare(base, pak, &manifest, &info));
    CHECK(app_state_build_launch(base, pak, &manifest, APP_LAUNCH_NORMAL,
                                 command, sizeof command));
    CHECK(strstr(command, "NEXTUI_APP_ID='safe'") != NULL);
}

static void test_limits(TempRoot* temp)
{
    char base[512];
    char pak[512];
    char file_path[640];
    PakManifest manifest;
    AppStateInfo info;
    int fd;
    int i;

    path_join(base, sizeof base, temp->path, "limits");
    CHECK(mkdir_p(base));
    CHECK(create_pak(base, "Big.pak",
                     "{\"id\":\"big\",\"capabilities\":[\"saves\"]}",
                     NULL, pak, sizeof pak));
    CHECK_INT(load_pak(pak, &manifest), PAK_MANIFEST_OK);
    CHECK(app_state_prepare(base, pak, &manifest, &info));

    CHECK(path_join(file_path, sizeof file_path, info.payload_dir, "big.bin"));
    fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd >= 0)
    {
        CHECK(ftruncate(fd, (off_t)APP_STATE_MAX_FILE_BYTES + 1) == 0);
        close(fd);
    }
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_TOO_LARGE);
    CHECK(unlink(file_path) == 0);

    for (i = 0; i < APP_STATE_MAX_FILES + 1; i++)
    {
        char name[32];
        snprintf(name, sizeof name, "f%03d", i);
        CHECK(payload_write(&info, name, "", 0));
    }
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_TOO_LARGE);
    CHECK(app_state_clear(base, pak, &manifest, NULL, 0));
}

static int run_command(const char* command)
{
    int status = system(command);
    if (status < 0) return -1;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static int quote_for_shell(char* out, size_t out_size, const char* value)
{
    size_t used = 0;
    const char* p;

    if (out_size < 3) return 0;
    out[used++] = '\'';
    for (p = value; *p != '\0'; p++)
    {
        if (*p == '\'')
        {
            if (used + 4 >= out_size) return 0;
            memcpy(out + used, "'\\''", 4);
            used += 4;
        }
        else
        {
            if (used + 1 >= out_size) return 0;
            out[used++] = *p;
        }
    }
    if (used + 1 >= out_size) return 0;
    out[used++] = '\'';
    out[used] = '\0';
    return 1;
}

static int run_cli(const char* nextstate, const char* command, const char* pak,
                   const char* root, char* output, size_t output_size)
{
    char q_next[600];
    char q_pak[600];
    char q_root[600];
    char line[4096];
    char redirect[8192];

    if (output != NULL && output_size > 0) output[0] = '\0';
    if (!quote_for_shell(q_next, sizeof q_next, nextstate)
        || !quote_for_shell(q_pak, sizeof q_pak, pak)
        || !quote_for_shell(q_root, sizeof q_root, root))
        return -1;
    snprintf(line, sizeof line, "%s %s --pak %s --root %s", q_next, command, q_pak, q_root);
    if (output != NULL && output_size > 0)
    {
        char temp_path[512];
        char q_out[600];
        snprintf(temp_path, sizeof temp_path, "%s/cli-output.txt", root);
        if (!quote_for_shell(q_out, sizeof q_out, temp_path)) return -1;
        snprintf(redirect, sizeof redirect, "%s > %s 2>&1", line, q_out);
        {
            int status = run_command(redirect);
            (void)read_text(temp_path, output, output_size);
            unlink(temp_path);
            return status;
        }
    }
    return run_command(line);
}

static void test_launch_and_exit(TempRoot* temp, const char* nextstate)
{
    char base[512];
    char pak[512];
    char fail_pak[512];
    char legacy_pak[512];
    char command[APP_STATE_CMD_MAX];
    char log_path[640];
    char log[4096];
    char root_path[512];
    char state_file[640];
    PakManifest manifest;
    AppStateInfo info;
    int status;

    path_join(base, sizeof base, temp->path, "launch");
    CHECK(mkdir_p(base));
    path_join(root_path, sizeof root_path, base, "state-root");

    CHECK(create_pak(base, "Launch App's.pak",
                     "{\"id\":\"launch\",\"version\":\"v2.0.0\",\"capabilities\":[\"saves\"]}",
                     "{\n"
                     "  printf 'id=%s\\n' \"$NEXTUI_APP_ID\"\n"
                     "  printf 'pak=%s\\n' \"$NEXTUI_APP_PAK_DIR\"\n"
                     "  printf 'state=%s\\n' \"$NEXTUI_APP_STATE_DIR\"\n"
                     "  printf 'resume=%s\\n' \"$NEXTUI_APP_RESUME\"\n"
                     "  printf 'arg1=%s\\n' \"$1\"\n"
                     "  printf 'arg2=%s\\n' \"$2\"\n"
                     "} > \"$TEST_LOG\"\n"
                     "if [ \"$NEXTUI_APP_RESUME\" = \"0\" ]; then\n"
                     "  printf 'from-launch' > \"$NEXTUI_APP_STATE_DIR/launch.txt\"\n"
                     "  \"$TEST_NEXTSTATE\" commit --pak \"$NEXTUI_APP_PAK_DIR\" --root \"$TEST_ROOT\" >/dev/null 2>&1\n"
                     "fi\n",
                     pak, sizeof pak));
    CHECK(path_join(log_path, sizeof log_path, base, "launch.log"));
    setenv("TEST_LOG", log_path, 1);
    setenv("TEST_NEXTSTATE", nextstate, 1);
    setenv("TEST_ROOT", root_path, 1);

    CHECK_INT(load_pak(pak, &manifest), PAK_MANIFEST_OK);
    CHECK_INT(app_state_inspect(root_path, pak, &manifest, &info), APP_STATE_ABSENT);
    CHECK(app_state_build_launch(root_path, pak, &manifest, APP_LAUNCH_NORMAL,
                                 command, sizeof command));
    status = run_command(command);
    CHECK_INT(status, 0);
    CHECK_INT(app_state_inspect(root_path, pak, &manifest, &info), APP_STATE_VALID);
    CHECK_INT(info.file_count, 1);
    CHECK(path_join(state_file, sizeof state_file, info.payload_dir, "launch.txt"));
    CHECK(read_text(state_file, log, sizeof log));
    CHECK_STR(log, "from-launch");

    CHECK(read_text(log_path, log, sizeof log));
    CHECK(strstr(log, "id=launch\n") != NULL);
    CHECK(strstr(log, "resume=0\n") != NULL);
    CHECK(strstr(log, "arg1=\n") != NULL);
    CHECK(strstr(log, "arg2=\n") != NULL);
    {
        char expected_pak[640];
        snprintf(expected_pak, sizeof expected_pak, "pak=%s\n", pak);
        CHECK(strstr(log, expected_pak) != NULL);
    }

    CHECK(app_state_build_launch(root_path, pak, &manifest, APP_LAUNCH_RESUME,
                                 command, sizeof command));
    status = run_command(command);
    CHECK_INT(status, 0);
    CHECK(read_text(log_path, log, sizeof log));
    CHECK(strstr(log, "resume=1\n") != NULL);
    CHECK(strstr(log, "arg1=\n") != NULL);
    CHECK(strstr(log, "arg2=\n") != NULL);
    {
        char expected_state[800];
        snprintf(expected_state, sizeof expected_state, "state=%s\n", info.payload_dir);
        CHECK(strstr(log, expected_state) != NULL);
    }
    CHECK_INT(app_state_inspect(root_path, pak, &manifest, &info), APP_STATE_VALID);

    CHECK(create_pak(base, "Quit Fail.pak",
                     "{\"id\":\"quitfail\",\"capabilities\":[\"saves\"]}",
                     "printf 'partial' > \"$NEXTUI_APP_STATE_DIR/partial.txt\"\nexit 1\n",
                     fail_pak, sizeof fail_pak));
    CHECK_INT(load_pak(fail_pak, &manifest), PAK_MANIFEST_OK);
    CHECK(app_state_build_launch(root_path, fail_pak, &manifest, APP_LAUNCH_NORMAL,
                                 command, sizeof command));
    status = run_command(command);
    CHECK(status != 0);
    CHECK_INT(app_state_inspect(root_path, fail_pak, &manifest, &info), APP_STATE_ABSENT);
    CHECK(!app_state_is_resumable(root_path, fail_pak, &manifest));
    {
        char output[64];
        CHECK_INT(run_cli(nextstate, "check", fail_pak, root_path, output, sizeof output), 2);
        CHECK(strstr(output, "absent") != NULL);
    }

    CHECK(create_pak(base, "Legacy.pak", NULL, NULL, legacy_pak, sizeof legacy_pak));
    CHECK_INT(load_pak(legacy_pak, &manifest), PAK_MANIFEST_ABSENT);
    CHECK(app_state_build_launch(root_path, legacy_pak, &manifest, APP_LAUNCH_NORMAL,
                                 command, sizeof command));
    {
        char expected[640];
        snprintf(expected, sizeof expected, "'%s/launch.sh'", legacy_pak);
        CHECK_STR(command, expected);
    }
    CHECK(!app_state_build_launch(root_path, legacy_pak, &manifest, APP_LAUNCH_RESUME,
                                  command, sizeof command));
}

static void test_cli(TempRoot* temp, const char* nextstate)
{
    char base[512];
    char pak[512];
    char plain[512];
    char root_path[512];
    char output[1024];
    char file_path[640];
    char sdcard[512];
    PakManifest manifest;
    AppStateInfo info;

    path_join(base, sizeof base, temp->path, "cli");
    CHECK(mkdir_p(base));
    path_join(root_path, sizeof root_path, base, "root");
    CHECK(mkdir_p(root_path));
    CHECK(create_pak(base, "Cli.pak",
                     "{\"id\":\"cli\",\"version\":\"v1.0.0\",\"capabilities\":[\"saves\"]}",
                     NULL, pak, sizeof pak));
    CHECK(create_pak(base, "Cli Plain.pak",
                     "{\"id\":\"cli\",\"capabilities\":[]}",
                     NULL, plain, sizeof plain));
    CHECK_INT(load_pak(pak, &manifest), PAK_MANIFEST_OK);

    CHECK_INT(run_cli(nextstate, "check", pak, root_path, output, sizeof output), 2);
    CHECK(strstr(output, "absent") != NULL);
    CHECK_INT(run_cli(nextstate, "clear", pak, root_path, output, sizeof output), 0);

    CHECK(app_state_prepare(root_path, pak, &manifest, &info));
    CHECK(payload_write(&info, "data.txt", "hello", 5));
    CHECK_INT(run_cli(nextstate, "commit", pak, root_path, output, sizeof output), 0);
    CHECK(strstr(output, "ok 1 5") != NULL);
    CHECK_INT(run_cli(nextstate, "check", pak, root_path, output, sizeof output), 0);
    CHECK(strstr(output, "valid 1 5") != NULL);

    {
        char meta[640];
        char outside_file[640];
        CHECK(path_join(outside_file, sizeof outside_file, base, "outside.txt"));
        CHECK(write_text(outside_file, "outside"));
        CHECK(metadata_path_for(root_path, pak, &manifest, meta, sizeof meta));
        CHECK(unlink(meta) == 0);
        CHECK(symlink(outside_file, meta) == 0);
        CHECK_INT(run_cli(nextstate, "check", pak, root_path, output, sizeof output), 5);
        CHECK(strstr(output, "invalid unsafe_metadata") != NULL);
        CHECK(unlink(meta) == 0);
    }
    CHECK_INT(run_cli(nextstate, "commit", pak, root_path, output, sizeof output), 0);

    CHECK(payload_write(&info, "data.txt", "HELLO", 5));
    CHECK_INT(run_cli(nextstate, "check", pak, root_path, output, sizeof output), 3);
    CHECK(strstr(output, "invalid payload_hash") != NULL);
    CHECK(payload_write(&info, "data.txt", "hello", 5));

    CHECK_INT(run_cli(nextstate, "check", plain, root_path, output, sizeof output), 4);
    CHECK(strstr(output, "unsupported capability_not_declared") != NULL);
    CHECK_INT(run_cli(nextstate, "commit", plain, root_path, output, sizeof output), 4);

    CHECK(path_join(file_path, sizeof file_path, info.payload_dir, "link"));
    {
        char outside_file[640];
        CHECK(path_join(outside_file, sizeof outside_file, base, "outside-link.txt"));
        CHECK(write_text(outside_file, "outside"));
        CHECK(symlink(outside_file, file_path) == 0);
    }
    CHECK_INT(run_cli(nextstate, "commit", pak, root_path, output, sizeof output), 5);
    CHECK(strstr(output, "invalid unsafe_path") != NULL);
    CHECK(unlink(file_path) == 0);

    CHECK_INT(run_cli(nextstate, "clear", pak, root_path, output, sizeof output), 0);
    CHECK_INT(run_cli(nextstate, "commit", pak, root_path, output, sizeof output), 2);
    CHECK(strstr(output, "no_payload") != NULL);

    CHECK(app_state_prepare(root_path, pak, &manifest, &info));
    CHECK(payload_write(&info, "data.txt", "hello", 5));
    {
        char meta[640];
        CHECK(metadata_path_for(root_path, pak, &manifest, meta, sizeof meta));
        remove_path(meta);
        CHECK(mkdir(meta, 0755) == 0);
    }
    CHECK_INT(run_cli(nextstate, "commit", pak, root_path, output, sizeof output), 7);
    CHECK(strstr(output, "io_error") != NULL);
    {
        char meta[640];
        CHECK(metadata_path_for(root_path, pak, &manifest, meta, sizeof meta));
        remove_path(meta);
    }
    CHECK_INT(run_cli(nextstate, "commit", pak, root_path, output, sizeof output), 0);
    CHECK_INT(run_cli(nextstate, "clear", pak, root_path, output, sizeof output), 0);

    CHECK_INT(run_cli(nextstate, "bogus", pak, root_path, output, sizeof output), 64);

    setenv("NEXTUI_APP_PAK_DIR", pak, 1);
    CHECK(app_state_prepare(root_path, pak, &manifest, &info));
    CHECK(payload_write(&info, "data.txt", "hello", 5));
    {
        char line[2048];
        char q_next[600];
        char q_root[600];
        CHECK(quote_for_shell(q_next, sizeof q_next, nextstate));
        CHECK(quote_for_shell(q_root, sizeof q_root, root_path));
        snprintf(line, sizeof line, "%s commit --root %s >/dev/null 2>&1", q_next, q_root);
        CHECK_INT(run_command(line), 0);
        snprintf(line, sizeof line, "%s check --root %s >/dev/null 2>&1", q_next, q_root);
        CHECK_INT(run_command(line), 0);
    }
    unsetenv("NEXTUI_APP_PAK_DIR");

    path_join(sdcard, sizeof sdcard, base, "sdcard");
    CHECK(mkdir_p(sdcard));
    setenv("SDCARD_PATH", sdcard, 1);
    {
        char default_root[512];
        char line[2048];
        char q_next[600];
        char q_pak[600];
        path_join(default_root, sizeof default_root, sdcard, ".userdata/shared/.minui/apps");
        remove_path(default_root);
        CHECK(app_state_prepare(default_root, pak, &manifest, &info));
        CHECK(payload_write(&info, "data.txt", "hello", 5));
        CHECK(quote_for_shell(q_next, sizeof q_next, nextstate));
        CHECK(quote_for_shell(q_pak, sizeof q_pak, pak));
        snprintf(line, sizeof line, "%s commit --pak %s >/dev/null 2>&1", q_next, q_pak);
        CHECK_INT(run_command(line), 0);
        snprintf(line, sizeof line, "%s check --pak %s >/dev/null 2>&1", q_next, q_pak);
        CHECK_INT(run_command(line), 0);
    }
    unsetenv("SDCARD_PATH");
}

static void test_interrupted_save(TempRoot* temp)
{
    char base[512];
    char pak[512];
    char meta[640];
    char temp_meta[640];
    PakManifest manifest;
    AppStateInfo info;

    path_join(base, sizeof base, temp->path, "interrupted");
    CHECK(mkdir_p(base));
    CHECK(create_pak(base, "Notes.pak",
                     "{\"id\":\"notes\",\"capabilities\":[\"saves\"]}",
                     NULL, pak, sizeof pak));
    CHECK_INT(load_pak(pak, &manifest), PAK_MANIFEST_OK);
    CHECK(app_state_prepare(base, pak, &manifest, &info));
    CHECK(payload_write(&info, "data.txt", "hello", 5));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_ABSENT);

    CHECK(metadata_path_for(base, pak, &manifest, meta, sizeof meta));
    snprintf(temp_meta, sizeof temp_meta, "%s/.resume.meta.tmp.123", info.slot_dir);
    CHECK(write_text(temp_meta, "nextui-app-state 1\napp notes\nsaved_at 1\nfiles 0\ntotal_bytes 0\n"));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_ABSENT);

    CHECK(mkdir(meta, 0755) == 0);
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_IO);
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "unsafe_metadata");
    CHECK(rmdir(meta) == 0);
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_OK);
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_VALID);

    CHECK(payload_write(&info, "data.txt", "HELLO", 5));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_INVALID);
    CHECK_STR(info.reason, "payload_hash");
    CHECK(payload_write(&info, "data.txt", "hello", 5));
    CHECK_INT(app_state_commit(base, pak, &manifest, NULL, 0), APP_STATE_COMMIT_OK);

    CHECK(metadata_path_for(base, pak, &manifest, meta, sizeof meta));
    CHECK(unlink(meta) == 0);
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_ABSENT);
    CHECK(app_state_clear(base, pak, &manifest, NULL, 0));
    CHECK(!path_exists(info.slot_dir));
    CHECK_INT(app_state_inspect(base, pak, &manifest, &info), APP_STATE_ABSENT);
    CHECK(app_state_clear(base, pak, &manifest, NULL, 0));
}

int main(int argc, char** argv)
{
    TempRoot temp;
    const char* nextstate = argc > 1 ? argv[1] : NULL;

    if (nextstate == NULL)
    {
        fprintf(stderr, "usage: %s <nextstate-binary>\n", argv[0]);
        return 2;
    }
    if (!temp_create(&temp))
    {
        fprintf(stderr, "could not create temporary directory\n");
        return 2;
    }

    test_slot_and_prepare(&temp);
    test_save_and_inspect(&temp);
    test_missing_and_unsupported(&temp);
    test_corruption(&temp);
    test_path_safety(&temp);
    test_limits(&temp);
    test_launch_and_exit(&temp, nextstate);
    test_cli(&temp, nextstate);
    test_interrupted_save(&temp);

    remove_path(temp.path);
    if (failures != 0)
    {
        fprintf(stderr, "%d failure(s) in %d checks\n", failures, checks);
        return 1;
    }
    printf("app_state tests passed (%d checks)\n", checks);
    return 0;
}
