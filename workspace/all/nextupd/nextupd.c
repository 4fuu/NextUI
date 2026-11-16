#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include "update_manifest.h"
#include "update_download.h"
#include "update_feed.h"
#include "update_signature.h"
#include "update_transaction.h"
#include "update_zip.h"
#include "sha256_lite.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
    const char* package;
    const char* manifest;
    const char* signature;
    const char* trusted_key;
    const char* platform;
    const char* system_version;
    const char* staging_root;
    const char* id;
    const char* kind;
    const char* version;
    const char* min_system;
    const char* out;
    const char* staged_dir;
    const char* target_root;
    const char* feed;
    int timeout_seconds;
    int quiet;
    int integrity_only;
    int replace;
    int rollback;
    int json;
} Options;

typedef struct
{
    UpdManifest manifest;
    char key_id[UPD_MAX_KEY_ID + 1];
    int signature_verified;
} VerifiedPackage;

static void usage(FILE* out)
{
    fprintf(out,
            "usage: nextupd <command> [options]\n"
            "\n"
            "  check     verify a package against a signed manifest; write nothing\n"
            "  feed      check a configured update feed; write nothing\n"
            "  fetch     check a feed, download, verify, and stage a system update\n"
            "  stage     verify and publish a package into a staging directory\n"
            "  install   apply a staged system package to an explicit target root\n"
            "  recover   resolve an interrupted install in an explicit target root\n"
            "  manifest  build a canonical manifest from a package\n"
            "\n"
            "check:\n"
            "  --package <file> --manifest <file> --platform <platform>\n"
            "  [--signature <file> --trusted-key <file>] [--integrity-only]\n"
            "  [--system-version <vX.Y.Z>] [--json]\n"
            "\n"
            "feed:\n"
            "  [--feed <file|url>] --platform <platform> --system-version <vX.Y.Z>\n"
            "  --trusted-key <file> [--timeout <seconds>] [--json]\n"
            "\n"
            "fetch:\n"
            "  [--feed <file|url>] --platform <platform> --system-version <vX.Y.Z>\n"
            "  --trusted-key <file> --staging-root <dir> [--timeout <seconds>]\n"
            "  [--replace] [--json]\n"
            "\n"
            "stage:\n"
            "  --package <file> --manifest <file> --signature <file>\n"
            "  --trusted-key <file> --platform <platform>\n"
            "  [--staging-root <dir>] [--replace] [--system-version <vX.Y.Z>] [--json]\n"
            "\n"
            "install:\n"
            "  --staged-dir <dir> --target-root <dir> --trusted-key <file>\n"
            "  --platform <platform> --system-version <vX.Y.Z> [--json]\n"
            "\n"
            "recover:\n"
            "  --target-root <dir> [--rollback] [--json]\n"
            "\n"
            "manifest:\n"
            "  --package <file> --id <id> --kind <system|app> --version <vX.Y.Z>\n"
            "  --platform <platform> [--min-system <vX.Y.Z>] [--out <file>]\n"
            "\n"
            "check without --integrity-only fails when no signature and trusted key are\n"
            "provided. stage and fetch always require both. SHA-256 detects damage; it\n"
            "does not establish who produced a manifest. install writes only inside an\n"
            "explicit --target-root that already contains .system; there is no default root\n"
            "and no boot or launch activation. recover is the offline arbitration entry\n"
            "point. feed and fetch have no built-in release source; without --feed or\n"
            "NEXTUI_UPDATE_FEED they report feed_unconfigured. fetch stages only and never\n"
            "installs.\n");
}

static const char* path_basename(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static void print_json_string(const char* text)
{
    const unsigned char* cursor = (const unsigned char*)text;

    fputc('"', stdout);
    for (; *cursor != '\0'; cursor++)
    {
        switch (*cursor)
        {
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (*cursor < 0x20u)
                fprintf(stdout, "\\u%04x", (unsigned)*cursor);
            else
                fputc(*cursor, stdout);
            break;
        }
    }
    fputc('"', stdout);
}

static int report_error(const UpdError* error, int json)
{
    const char* reason = error->reason[0] == '\0' ? "error" : error->reason;
    const char* detail = error->detail[0] == '\0' ? reason : error->detail;

    if (json)
    {
        fputs("{\"ok\":false,\"result\":\"error\",\"code\":", stdout);
        fprintf(stdout, "%d", (int)error->code);
        fputs(",\"reason\":", stdout);
        print_json_string(reason);
        fputs(",\"detail\":", stdout);
        print_json_string(detail);
        fputs("}\n", stdout);
    }
    else
    {
        fprintf(stderr, "nextupd: %s: %s\n", reason, detail);
    }
    return (int)error->code;
}

static const char* resolved_platform(const Options* options)
{
    const char* platform = options->platform;
    if (platform == NULL || platform[0] == '\0') platform = getenv("PLATFORM");
    return platform;
}

static const char* resolved_system_version(const Options* options)
{
    const char* version = options->system_version;
    if (version == NULL || version[0] == '\0') version = getenv("NEXTUI_SYSTEM_VERSION");
    return version;
}

static int check_extra_files(const UpdManifest* manifest, const UpdZip* zip, UpdError* error)
{
    size_t manifest_index = 0;
    size_t zip_index;

    for (zip_index = 0; zip_index < zip->file_count; zip_index++)
    {
        while (manifest_index < manifest->file_count
               && strcmp(manifest->files[manifest_index].path, zip->files[zip_index].name) < 0)
            manifest_index++;
        if (manifest_index >= manifest->file_count
            || strcmp(manifest->files[manifest_index].path, zip->files[zip_index].name) != 0)
        {
            upd_error_set(error, UPD_E_INVALID, "unlisted_file",
                          "archive contains an unlisted file: %s",
                          zip->files[zip_index].name);
            return 0;
        }
    }
    return 1;
}

static int check_directories(const UpdManifest* manifest, const UpdZip* zip, UpdError* error)
{
    size_t directory_index;
    size_t file_index;

    for (directory_index = 0; directory_index < zip->directory_count; directory_index++)
    {
        const char* directory = zip->directories[directory_index];
        size_t length = strlen(directory);
        int implied = 0;

        for (file_index = 0; file_index < manifest->file_count; file_index++)
        {
            if (strncmp(manifest->files[file_index].path, directory, length) == 0
                && manifest->files[file_index].path[length] == '/')
            {
                implied = 1;
                break;
            }
        }
        if (!implied)
        {
            upd_error_set(error, UPD_E_INVALID, "unlisted_entry",
                          "archive contains an unlisted directory: %s", directory);
            return 0;
        }
    }
    return 1;
}

static int verify_archive(const Options* options, const UpdManifest* manifest,
                          UpdError* error)
{
    UpdZip zip;
    unsigned char archive_digest[UPD_SHA256_BYTES];
    unsigned char file_digest[UPD_SHA256_BYTES];
    uint64_t archive_size = 0;
    uint64_t file_size = 0;
    size_t i;

    if (strcmp(path_basename(options->package), manifest->archive_name) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "archive_name_mismatch",
                      "package is named %s but the manifest expects %s",
                      path_basename(options->package), manifest->archive_name);
        return 0;
    }
    if (!upd_file_size(options->package, &archive_size, error))
    {
        if (strcmp(error->reason, "file_missing") == 0)
            upd_error_set(error, UPD_E_MISSING, "archive_missing", "%s", error->detail);
        return 0;
    }
    if (archive_size != manifest->archive_size)
    {
        upd_error_set(error, UPD_E_INTEGRITY, "archive_size_mismatch",
                      "archive is %llu bytes, manifest says %llu",
                      (unsigned long long)archive_size,
                      (unsigned long long)manifest->archive_size);
        return 0;
    }
    if (!upd_hash_file(options->package, archive_digest, &archive_size,
                       UPD_MAX_ARCHIVE_BYTES, error))
        return 0;
    if (memcmp(archive_digest, manifest->archive_sha256, UPD_SHA256_BYTES) != 0)
    {
        char expected[UPD_SHA256_HEX];
        char actual[UPD_SHA256_HEX];
        upd_hex_encode(manifest->archive_sha256, UPD_SHA256_BYTES, expected);
        upd_hex_encode(archive_digest, UPD_SHA256_BYTES, actual);
        upd_error_set(error, UPD_E_INTEGRITY, "archive_hash_mismatch",
                      "archive sha256 is %s, manifest says %s", actual, expected);
        return 0;
    }

    upd_zip_init(&zip);
    if (!upd_zip_open(options->package, &zip, error)) return 0;
    for (i = 0; i < manifest->file_count; i++)
    {
        const UpdZipEntry* entry = upd_zip_find(&zip, manifest->files[i].path);
        if (entry == NULL)
        {
            upd_error_set(error, UPD_E_MISSING, "missing_file",
                          "manifest file is missing from archive: %s",
                          manifest->files[i].path);
            upd_zip_close(&zip);
            return 0;
        }
        if (!upd_zip_hash_entry(&zip, entry, manifest->files[i].size, file_digest,
                                &file_size, error))
        {
            upd_zip_close(&zip);
            return 0;
        }
        if (file_size != manifest->files[i].size)
        {
            upd_error_set(error, UPD_E_INTEGRITY, "file_size_mismatch",
                          "%s: got %llu bytes, manifest says %llu",
                          manifest->files[i].path, (unsigned long long)file_size,
                          (unsigned long long)manifest->files[i].size);
            upd_zip_close(&zip);
            return 0;
        }
        if (memcmp(file_digest, manifest->files[i].sha256, UPD_SHA256_BYTES) != 0)
        {
            upd_error_set(error, UPD_E_INTEGRITY, "file_hash_mismatch",
                          "%s: SHA-256 mismatch", manifest->files[i].path);
            upd_zip_close(&zip);
            return 0;
        }
    }
    if (!check_extra_files(manifest, &zip, error))
    {
        upd_zip_close(&zip);
        return 0;
    }
    if (!check_directories(manifest, &zip, error))
    {
        upd_zip_close(&zip);
        return 0;
    }
    upd_zip_close(&zip);
    return 1;
}

static int verify_package(const Options* options, int require_signature,
                          VerifiedPackage* verified, UpdError* error)
{
    const char* platform;
    const char* system_version;

    memset(verified, 0, sizeof *verified);
    upd_manifest_init(&verified->manifest);
    if (!upd_manifest_load(options->manifest, &verified->manifest, error)) return 0;

    if (options->signature != NULL || options->trusted_key != NULL)
    {
        if (!upd_signature_verify(verified->manifest.raw, verified->manifest.raw_length,
                                  options->signature, options->trusted_key,
                                  verified->key_id, sizeof verified->key_id, error))
            return 0;
        verified->signature_verified = 1;
    }
    else if (require_signature)
    {
        upd_error_set(error, UPD_E_SIGNATURE, "signature_missing",
                      "check/stage requires --signature and --trusted-key");
        return 0;
    }

    platform = resolved_platform(options);
    if (platform == NULL || platform[0] == '\0')
    {
        upd_error_set(error, UPD_E_USAGE, "platform_missing",
                      "--platform or PLATFORM is required");
        return 0;
    }
    if (!upd_manifest_platform_matches(&verified->manifest, platform))
    {
        upd_error_set(error, UPD_E_PLATFORM, "platform_mismatch",
                      "package is for %s, target is %s", verified->manifest.platform,
                      platform);
        return 0;
    }
    system_version = resolved_system_version(options);
    if (!upd_manifest_check_min_system(&verified->manifest, system_version, error)) return 0;
    return verify_archive(options, &verified->manifest, error);
}

static void print_check_result(const Options* options, const VerifiedPackage* verified)
{
    char sha[UPD_SHA256_HEX];
    const char* platform = resolved_platform(options);

    upd_hex_encode(verified->manifest.archive_sha256, UPD_SHA256_BYTES, sha);
    if (!verified->signature_verified)
        fprintf(stderr,
                "nextupd: warning: authenticity is NOT verified; SHA-256 is not a signature\n");
    if (options->json)
    {
        fputs("{\"ok\":true,\"result\":\"ok\",\"id\":", stdout);
        print_json_string(verified->manifest.id);
        fputs(",\"kind\":", stdout);
        print_json_string(verified->manifest.kind);
        fputs(",\"version\":", stdout);
        print_json_string(verified->manifest.version);
        fputs(",\"platform\":", stdout);
        print_json_string(verified->manifest.platform);
        fputs(",\"target\":", stdout);
        print_json_string(platform);
        fputs(",\"archive\":", stdout);
        print_json_string(verified->manifest.archive_name);
        fprintf(stdout,
                ",\"archive_size\":%llu,\"archive_sha256\":\"%s\",\"files\":%zu,"
                "\"bytes\":%llu,\"signature_verified\":%s,\"signature_key_id\":",
                (unsigned long long)verified->manifest.archive_size, sha,
                verified->manifest.file_count,
                (unsigned long long)verified->manifest.total_bytes,
                verified->signature_verified ? "true" : "false");
        print_json_string(verified->key_id);
        fputs("}\n", stdout);
        return;
    }

    printf("package: %s %s (%s)\n", verified->manifest.id, verified->manifest.version,
           verified->manifest.kind);
    printf("platform: %s -> %s\n", verified->manifest.platform, platform);
    printf("archive: %s (%llu bytes, sha256 %s)\n", verified->manifest.archive_name,
           (unsigned long long)verified->manifest.archive_size, sha);
    printf("files: %zu (%llu bytes) verified\n", verified->manifest.file_count,
           (unsigned long long)verified->manifest.total_bytes);
    if (verified->signature_verified)
        printf("signature: verified (key %s)\n", verified->key_id);
    else
        printf("signature: unverified (integrity only)\n");
    printf("result: ok\n");
}

static int run_check(const Options* options)
{
    VerifiedPackage verified;
    UpdError error;
    int result;

    upd_error_clear(&error);
    if (!verify_package(options, !options->integrity_only, &verified, &error))
    {
        result = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return result;
    }
    print_check_result(options, &verified);
    upd_manifest_free(&verified.manifest);
    return 0;
}

static int copy_option_text(char* out, size_t out_size, const char* value,
                            const char* field, UpdError* error)
{
    size_t length = value == NULL ? 0 : strlen(value);
    if (value == NULL || length >= out_size)
    {
        upd_error_set(error, UPD_E_INVALID, "field_too_long", "%s is too long", field);
        return 0;
    }
    memcpy(out, value, length + 1);
    return 1;
}

static int run_manifest(const Options* options)
{
    UpdManifest manifest;
    UpdZip zip;
    UpdError error;
    unsigned char digest[UPD_SHA256_BYTES];
    uint64_t archive_size = 0;
    char* rendered = NULL;
    size_t rendered_length = 0;
    size_t i;
    int result;

    upd_error_clear(&error);
    upd_manifest_init(&manifest);
    if (!copy_option_text(manifest.id, sizeof manifest.id, options->id, "id", &error)
        || !copy_option_text(manifest.kind, sizeof manifest.kind, options->kind, "kind",
                             &error)
        || !copy_option_text(manifest.version, sizeof manifest.version, options->version,
                             "version", &error)
        || !copy_option_text(manifest.platform, sizeof manifest.platform, options->platform,
                             "platform", &error)
        || !copy_option_text(manifest.archive_name, sizeof manifest.archive_name,
                             path_basename(options->package), "archive name", &error))
    {
        result = report_error(&error, 0);
        goto done;
    }
    if (options->min_system != NULL)
    {
        if (!copy_option_text(manifest.min_system, sizeof manifest.min_system,
                              options->min_system, "min-system", &error))
        {
            result = report_error(&error, 0);
            goto done;
        }
        manifest.has_min_system = 1;
    }

    upd_zip_init(&zip);
    if (!upd_file_size(options->package, &archive_size, &error))
    {
        if (strcmp(error.reason, "file_missing") == 0)
            upd_error_set(&error, UPD_E_MISSING, "archive_missing", "%s", error.detail);
        result = report_error(&error, 0);
        goto done;
    }
    manifest.archive_size = archive_size;
    if (!upd_hash_file(options->package, manifest.archive_sha256, &archive_size,
                       UPD_MAX_ARCHIVE_BYTES, &error))
    {
        result = report_error(&error, 0);
        goto done;
    }
    if (!upd_manifest_check_identity(&manifest, &error))
    {
        result = report_error(&error, 0);
        goto done;
    }
    if (!upd_zip_open(options->package, &zip, &error))
    {
        result = report_error(&error, 0);
        goto done;
    }
    for (i = 0; i < zip.file_count; i++)
    {
        uint64_t actual_size = 0;
        if (!upd_zip_hash_entry(&zip, &zip.files[i], zip.files[i].uncompressed_size, digest,
                                &actual_size, &error))
        {
            result = report_error(&error, 0);
            upd_zip_close(&zip);
            goto done;
        }
        if (!upd_manifest_add_file(&manifest, zip.files[i].name, actual_size, digest, &error))
        {
            result = report_error(&error, 0);
            upd_zip_close(&zip);
            goto done;
        }
    }
    if (!check_directories(&manifest, &zip, &error))
    {
        result = report_error(&error, 0);
        upd_zip_close(&zip);
        goto done;
    }
    upd_zip_close(&zip);
    if (!upd_manifest_finalize(&manifest, &error))
    {
        result = report_error(&error, 0);
        goto done;
    }
    if (!upd_manifest_render(&manifest, &rendered, &rendered_length, &error))
    {
        result = report_error(&error, 0);
        goto done;
    }
    if (options->out != NULL)
    {
        if (!upd_manifest_write(&manifest, options->out, &error))
        {
            result = report_error(&error, 0);
            goto done;
        }
        printf("wrote %s: %s %s %s, %zu files, %llu bytes\n", options->out, manifest.id,
               manifest.version, manifest.platform, manifest.file_count,
               (unsigned long long)manifest.total_bytes);
    }
    else if (fwrite(rendered, 1, rendered_length, stdout) != rendered_length)
    {
        upd_error_set(&error, UPD_E_IO, "io_error", "cannot write manifest to stdout");
        result = report_error(&error, 0);
        goto done;
    }
    result = 0;

done:
    free(rendered);
    upd_manifest_free(&manifest);
    return result;
}

#ifdef NEXTUPD_TESTING
static int test_limit(const char* name, long long* value)
{
    const char* text = getenv(name);
    char* end = NULL;
    long long parsed;

    if (text == NULL || text[0] == '\0') return 0;
    parsed = strtoll(text, &end, 10);
    if (end == NULL || *end != '\0' || parsed < 0) return 0;
    *value = parsed;
    return 1;
}
#endif

static int copy_file_verified(const char* source, const char* destination,
                              uint64_t expected_size,
                              const unsigned char expected_digest[UPD_SHA256_BYTES],
                              UpdError* error)
{
    unsigned char buffer[64 * 1024];
    unsigned char digest[UPD_SHA256_BYTES];
    Sha256Lite context;
    uint64_t total = 0;
    uint64_t source_size = 0;
    int input = -1;
    int output = -1;

    if (!upd_file_size(source, &source_size, error)) return 0;
    if (source_size != expected_size)
    {
        upd_error_set(error, UPD_E_INTEGRITY, "file_size_mismatch",
                      "source is %llu bytes, expected %llu",
                      (unsigned long long)source_size, (unsigned long long)expected_size);
        return 0;
    }
    input = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    output = open(destination, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (input < 0 || output < 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot copy %s: %s", source,
                      strerror(errno));
        if (input >= 0) close(input);
        if (output >= 0) close(output);
        unlink(destination);
        return 0;
    }
    sha256_lite_init(&context);
    for (;;)
    {
        ssize_t got = read(input, buffer, sizeof buffer);
        if (got < 0)
        {
            if (errno == EINTR) continue;
            upd_error_set(error, UPD_E_IO, "io_error", "read %s: %s", source,
                          strerror(errno));
            goto failed;
        }
        if (got == 0) break;
        {
            size_t to_write = (size_t)got;
            int fail_after_write = 0;
            int crash_after_write = 0;
#ifdef NEXTUPD_TESTING
            {
                long long fail_after;
                long long crash_after;
                int fail_set = test_limit("NEXTUPD_TEST_FAIL_AFTER_BYTES", &fail_after);
                int crash_set = test_limit("NEXTUPD_TEST_CRASH_AFTER_BYTES", &crash_after);
                if (fail_set && total >= (uint64_t)fail_after)
                {
                    upd_error_set(error, UPD_E_INTERRUPTED, "interrupted",
                                  "test hook stopped after %llu bytes",
                                  (unsigned long long)total);
                    goto failed;
                }
                if (crash_set && total >= (uint64_t)crash_after) _exit(70);
                if (fail_set && total + to_write > (uint64_t)fail_after)
                {
                    to_write = (size_t)((uint64_t)fail_after - total);
                    fail_after_write = 1;
                }
                else if (crash_set && total + to_write > (uint64_t)crash_after)
                {
                    to_write = (size_t)((uint64_t)crash_after - total);
                    crash_after_write = 1;
                }
            }
#endif
            {
                size_t written = 0;
                while (written < to_write)
                {
                    ssize_t count = write(output, buffer + written, to_write - written);
                    if (count < 0)
                    {
                        if (errno == EINTR) continue;
                        if (errno == ENOSPC)
                            upd_error_set(error, UPD_E_SPACE, "insufficient_space",
                                          "storage full while writing %s", destination);
                        else
                            upd_error_set(error, UPD_E_IO, "io_error", "write %s: %s",
                                          destination, strerror(errno));
                        goto failed;
                    }
                    written += (size_t)count;
                }
            }
            total += to_write;
            sha256_lite_update(&context, buffer, to_write);
            if (fail_after_write)
            {
                upd_error_set(error, UPD_E_INTERRUPTED, "interrupted",
                              "test hook stopped after %llu bytes",
                              (unsigned long long)total);
                goto failed;
            }
            if (crash_after_write) _exit(70);
        }
    }
    if (fsync(output) != 0)
    {
        if (errno == ENOSPC)
            upd_error_set(error, UPD_E_SPACE, "insufficient_space",
                          "storage full while syncing %s", destination);
        else
            upd_error_set(error, UPD_E_IO, "io_error", "sync %s: %s", destination,
                          strerror(errno));
        goto failed;
    }
    if (close(output) != 0)
    {
        output = -1;
        upd_error_set(error, UPD_E_IO, "io_error", "close %s: %s", destination,
                      strerror(errno));
        goto failed;
    }
    output = -1;
    close(input);
    input = -1;

    sha256_lite_final(&context, digest);
    if (total != expected_size || memcmp(digest, expected_digest, UPD_SHA256_BYTES) != 0)
    {
        upd_error_set(error, UPD_E_INTEGRITY, "archive_hash_mismatch",
                      "copied archive does not match the manifest");
        unlink(destination);
        return 0;
    }
    return 1;

failed:
    if (input >= 0) close(input);
    if (output >= 0) close(output);
    unlink(destination);
    return 0;
}

static int storage_free_bytes(const char* root, uint64_t* free_bytes, UpdError* error)
{
    struct statvfs status;
    uint64_t block_size;

    if (statvfs(root, &status) != 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot stat staging filesystem: %s",
                      strerror(errno));
        return 0;
    }
    block_size = status.f_frsize != 0 ? status.f_frsize : status.f_bsize;
    *free_bytes = (uint64_t)status.f_bavail * block_size;
    return 1;
}

static int stage_dir_for(const char* root, const UpdManifest* manifest, char* out,
                         size_t out_size, UpdError* error)
{
    char base[256];
    int length;

    length = snprintf(base, sizeof base, "%s-%s-%s", manifest->id, manifest->version,
                      manifest->platform);
    if (length < 0 || (size_t)length >= sizeof base)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "package name is too long");
        return 0;
    }
    length = snprintf(out, out_size, "%s/%s", root, base);
    if (length < 0 || (size_t)length >= out_size)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "staging directory is too long");
        return 0;
    }
    return 1;
}

static int make_temp_dir(const char* base, const char* prefix, char* out, size_t out_size,
                         UpdError* error)
{
    int length = snprintf(out, out_size, "%s/%sXXXXXX", base, prefix);

    if (length < 0 || (size_t)length >= out_size)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "temporary path is too long");
        return 0;
    }
    if (mkdtemp(out) == NULL)
    {
        if (errno == ENOSPC)
            upd_error_set(error, UPD_E_SPACE, "insufficient_space",
                          "cannot create temporary directory: no space left");
        else
            upd_error_set(error, UPD_E_IO, "io_error",
                          "cannot create temporary directory: %s", strerror(errno));
        return 0;
    }
    return 1;
}

static void cleanup_temp_dir(const char* directory, const char* archive_name)
{
    char path[4096];

    if (directory == NULL || directory[0] == '\0') return;
    if (snprintf(path, sizeof path, "%s/manifest.json", directory) < (int)sizeof path)
        unlink(path);
    if (snprintf(path, sizeof path, "%s/manifest.sig", directory) < (int)sizeof path)
        unlink(path);
    if (archive_name != NULL && archive_name[0] != '\0'
        && snprintf(path, sizeof path, "%s/%s", directory, archive_name)
               < (int)sizeof path)
        unlink(path);
    rmdir(directory);
}

static void cleanup_stage(const char* stage_dir, const char* zip_path,
                          const char* manifest_path, const char* signature_path,
                          const char* marker_path)
{
    char part[4096];
    const char* paths[4];
    size_t i;

    paths[0] = zip_path;
    paths[1] = manifest_path;
    paths[2] = signature_path;
    paths[3] = marker_path;
    for (i = 0; i < 4; i++)
    {
        if (paths[i] == NULL) continue;
        unlink(paths[i]);
        if (snprintf(part, sizeof part, "%s.part", paths[i]) < (int)sizeof part)
            unlink(part);
    }
    if (stage_dir != NULL) rmdir(stage_dir);
}

static int run_stage(const Options* options)
{
    VerifiedPackage verified;
    UpdError error;
    char root[4096];
    char lock_path[4096];
    char stage_dir[4096];
    char zip_path[4096];
    char manifest_path[4096];
    char signature_path[4096];
    char marker_path[4096];
    char zip_part[4096];
    unsigned char* signature_bytes = NULL;
    size_t signature_length = 0;
    unsigned char manifest_digest[UPD_SHA256_BYTES];
    char manifest_sha[UPD_SHA256_HEX];
    uint64_t signature_size = 0;
    uint64_t required = 0;
    uint64_t free_bytes;
    int lock_fd = -1;
    int marker_exists = 0;
    int renamed = 0;
    int result;
    char descriptor[2048];
    int descriptor_length;

    upd_error_clear(&error);
    if (!verify_package(options, 1, &verified, &error))
    {
        result = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return result;
    }
    if (verified.manifest.raw_length > UPD_MAX_MANIFEST_BYTES
        || options->signature == NULL || options->trusted_key == NULL)
    {
        upd_error_set(&error, UPD_E_SIGNATURE, "signature_missing",
                      "stage requires a signed manifest and trusted key");
        result = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return result;
    }
    if (options->staging_root != NULL)
        snprintf(root, sizeof root, "%s", options->staging_root);
    else
    {
        const char* sdcard = getenv("SDCARD_PATH");
        if (sdcard != NULL && sdcard[0] != '\0')
            snprintf(root, sizeof root, "%s/.tmp_update/staging", sdcard);
        else
            snprintf(root, sizeof root, ".nextupd-staging");
    }
    if (strlen(root) + 64 >= sizeof root)
    {
        upd_error_set(&error, UPD_E_USAGE, "path_too_long", "staging root is too long");
        result = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return result;
    }
    if (!upd_mkdir_p(root, &error))
    {
        result = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return result;
    }
    if (snprintf(lock_path, sizeof lock_path, "%s/.nextupd.lock", root) >= (int)sizeof lock_path)
    {
        upd_error_set(&error, UPD_E_USAGE, "path_too_long", "lock path is too long");
        result = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return result;
    }
    lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock_fd < 0)
    {
        upd_error_set(&error, UPD_E_IO, "io_error", "cannot open %s: %s", lock_path,
                      strerror(errno));
        result = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return result;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0)
    {
        upd_error_set(&error, UPD_E_BUSY, "busy",
                      errno == EWOULDBLOCK ? "another stage is running"
                                           : "cannot lock staging directory");
        result = report_error(&error, options->json);
        close(lock_fd);
        upd_manifest_free(&verified.manifest);
        return result;
    }

    if (!stage_dir_for(root, &verified.manifest, stage_dir, sizeof stage_dir, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    if (snprintf(zip_path, sizeof zip_path, "%s/%s", stage_dir,
                 verified.manifest.archive_name)
            >= (int)sizeof zip_path
        || snprintf(manifest_path, sizeof manifest_path, "%s/manifest.json", stage_dir)
               >= (int)sizeof manifest_path
        || snprintf(signature_path, sizeof signature_path, "%s/manifest.sig", stage_dir)
               >= (int)sizeof signature_path
        || snprintf(marker_path, sizeof marker_path, "%s/staged.json", stage_dir)
               >= (int)sizeof marker_path)
    {
        upd_error_set(&error, UPD_E_USAGE, "path_too_long", "staging path is too long");
        result = report_error(&error, options->json);
        goto done;
    }
    if (!upd_mkdir_p(stage_dir, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    marker_exists = access(marker_path, F_OK) == 0;
    if (marker_exists && !options->replace)
    {
        upd_error_set(&error, UPD_E_ALREADY_STAGED, "already_staged",
                      "%s %s is already staged; pass --replace to replace it",
                      verified.manifest.id, verified.manifest.version);
        result = report_error(&error, options->json);
        goto done;
    }
    if (marker_exists) unlink(marker_path);
    cleanup_stage(stage_dir, zip_path, manifest_path, signature_path, marker_path);
    if (!upd_mkdir_p(stage_dir, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }

    if (!upd_file_size(options->signature, &signature_size, &error))
    {
        if (strcmp(error.reason, "file_missing") == 0)
            upd_error_set(&error, UPD_E_SIGNATURE, "signature_missing", "%s", error.detail);
        result = report_error(&error, options->json);
        goto done;
    }
    if (verified.manifest.archive_size > UINT64_MAX - verified.manifest.raw_length
        || verified.manifest.archive_size + verified.manifest.raw_length
               > UINT64_MAX - signature_size
        || verified.manifest.archive_size + verified.manifest.raw_length + signature_size
               > UINT64_MAX - 64 * 1024)
    {
        upd_error_set(&error, UPD_E_INVALID, "too_large", "staging requirement overflows");
        result = report_error(&error, options->json);
        goto done;
    }
    required = verified.manifest.archive_size + verified.manifest.raw_length + signature_size
               + 64 * 1024;
    if (!storage_free_bytes(root, &free_bytes, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
#ifdef NEXTUPD_TESTING
    {
        long long override;
        if (test_limit("NEXTUPD_TEST_FREE_BYTES", &override))
            free_bytes = (uint64_t)override;
    }
#endif
    if (free_bytes < required)
    {
        upd_error_set(&error, UPD_E_SPACE, "insufficient_space",
                      "need %llu bytes, %llu available", (unsigned long long)required,
                      (unsigned long long)free_bytes);
        result = report_error(&error, options->json);
        goto done;
    }
    if (snprintf(zip_part, sizeof zip_part, "%s.part", zip_path) >= (int)sizeof zip_part)
    {
        upd_error_set(&error, UPD_E_USAGE, "path_too_long", "staging path is too long");
        result = report_error(&error, options->json);
        goto done;
    }
    if (!copy_file_verified(options->package, zip_part, verified.manifest.archive_size,
                            verified.manifest.archive_sha256, &error))
    {
        result = report_error(&error, options->json);
        cleanup_stage(stage_dir, zip_path, manifest_path, signature_path, marker_path);
        goto done;
    }
    if (rename(zip_part, zip_path) != 0)
    {
        upd_error_set(&error, UPD_E_IO, "io_error", "publish %s: %s", zip_path,
                      strerror(errno));
        result = report_error(&error, options->json);
        cleanup_stage(stage_dir, zip_path, manifest_path, signature_path, marker_path);
        goto done;
    }
    renamed = 1;
    if (!upd_write_file_atomic(manifest_path, verified.manifest.raw,
                               verified.manifest.raw_length, &error))
    {
        result = report_error(&error, options->json);
        goto failed_stage;
    }
    if (!upd_read_file(options->signature, UPD_MAX_SIGNATURE_BYTES, &signature_bytes,
                       &signature_length, &error))
    {
        result = report_error(&error, options->json);
        goto failed_stage;
    }
    if (!upd_write_file_atomic(signature_path, signature_bytes, signature_length, &error))
    {
        result = report_error(&error, options->json);
        goto failed_stage;
    }
    {
        char copied_key_id[UPD_MAX_KEY_ID + 1];
        if (!upd_signature_verify(verified.manifest.raw, verified.manifest.raw_length,
                                  signature_path, options->trusted_key, copied_key_id,
                                  sizeof copied_key_id, &error))
        {
            result = report_error(&error, options->json);
            goto failed_stage;
        }
        if (strcmp(copied_key_id, verified.key_id) != 0)
        {
            upd_error_set(&error, UPD_E_SIGNATURE, "signature_key_mismatch",
                          "staged signature changed key id during staging");
            result = report_error(&error, options->json);
            goto failed_stage;
        }
    }
    {
        Sha256Lite context;
        sha256_lite_init(&context);
        sha256_lite_update(&context, verified.manifest.raw, verified.manifest.raw_length);
        sha256_lite_final(&context, manifest_digest);
        upd_hex_encode(manifest_digest, UPD_SHA256_BYTES, manifest_sha);
    }
    descriptor_length = snprintf(
        descriptor, sizeof descriptor,
        "{\n"
        "  \"schema\": \"nextui-staged-update-v1\",\n"
        "  \"status\": \"staged\",\n"
        "  \"id\": \"%s\",\n"
        "  \"kind\": \"%s\",\n"
        "  \"version\": \"%s\",\n"
        "  \"platform\": \"%s\",\n"
        "  \"archive\": \"%s\",\n"
        "  \"archive_sha256\": \"",
        verified.manifest.id, verified.manifest.kind, verified.manifest.version,
        verified.manifest.platform, verified.manifest.archive_name);
    if (descriptor_length < 0 || descriptor_length >= (int)sizeof descriptor)
    {
        upd_error_set(&error, UPD_E_IO, "io_error", "cannot render staging descriptor");
        result = report_error(&error, options->json);
        goto failed_stage;
    }
    {
        char archive_sha[UPD_SHA256_HEX];
        upd_hex_encode(verified.manifest.archive_sha256, UPD_SHA256_BYTES, archive_sha);
        descriptor_length += snprintf(
            descriptor + descriptor_length, sizeof descriptor - (size_t)descriptor_length,
            "%s\",\n"
            "  \"manifest\": \"manifest.json\",\n"
            "  \"manifest_sha256\": \"%s\",\n"
            "  \"signature\": \"manifest.sig\",\n"
            "  \"signature_key_id\": \"%s\",\n"
            "  \"staged_at\": %lld\n"
            "}\n",
            archive_sha, manifest_sha, verified.key_id,
            (long long)time(NULL));
    }
    if (descriptor_length < 0 || descriptor_length >= (int)sizeof descriptor)
    {
        upd_error_set(&error, UPD_E_IO, "io_error", "cannot render staging descriptor");
        result = report_error(&error, options->json);
        goto failed_stage;
    }
    if (!upd_write_file_atomic(marker_path, descriptor, (size_t)descriptor_length, &error))
    {
        result = report_error(&error, options->json);
        goto failed_stage;
    }
    if (!upd_fsync_dir(root))
        fprintf(stderr, "nextupd: warning: could not sync staging directory\n");
    if (!options->quiet)
    {
        if (options->json)
        {
            fputs("{\"ok\":true,\"result\":\"staged\",\"id\":", stdout);
            print_json_string(verified.manifest.id);
            fputs(",\"version\":", stdout);
            print_json_string(verified.manifest.version);
            fputs(",\"platform\":", stdout);
            print_json_string(verified.manifest.platform);
            fputs(",\"staging_root\":", stdout);
            print_json_string(root);
            fputs(",\"archive_path\":", stdout);
            print_json_string(zip_path);
            fputs(",\"manifest_path\":", stdout);
            print_json_string(manifest_path);
            fputs(",\"signature_path\":", stdout);
            print_json_string(signature_path);
            fputs(",\"marker_path\":", stdout);
            print_json_string(marker_path);
            fputs("}\n", stdout);
        }
        else
        {
            printf("staged %s %s at %s\n", verified.manifest.id, verified.manifest.version,
                   root);
            printf("archive: %s\n", zip_path);
            printf("manifest: %s\n", manifest_path);
            printf("signature: %s (key %s)\n", signature_path, verified.key_id);
            printf("marker: %s\n", marker_path);
        }
    }
    result = 0;
    goto done;

failed_stage:
    if (renamed) cleanup_stage(stage_dir, zip_path, manifest_path, signature_path, marker_path);
done:
    free(signature_bytes);
    if (lock_fd >= 0) close(lock_fd);
    upd_manifest_free(&verified.manifest);
    return result;
}

static int print_feed_up_to_date(const Options* options, const char* platform,
                                 const char* system_version, const UpdFeedEntry* entry)
{
    if (options->json)
    {
        fputs("{\"ok\":true,\"result\":\"up_to_date\",\"platform\":", stdout);
        print_json_string(platform);
        fputs(",\"system_version\":", stdout);
        print_json_string(system_version);
        fputs(",\"latest_version\":", stdout);
        print_json_string(entry != NULL ? entry->version : system_version);
        fputs("}\n", stdout);
    }
    else
    {
        printf("up to date: %s is current (feed latest %s)\n", system_version,
               entry != NULL ? entry->version : system_version);
    }
    return 0;
}

static int print_feed_available(const Options* options, const char* platform,
                                const char* system_version, const UpdManifest* manifest,
                                const char* key_id, const char* manifest_url,
                                const char* signature_url, const char* archive_url)
{
    char archive_sha[UPD_SHA256_HEX];

    upd_hex_encode(manifest->archive_sha256, UPD_SHA256_BYTES, archive_sha);
    if (options->json)
    {
        fputs("{\"ok\":true,\"result\":\"update_available\",\"platform\":", stdout);
        print_json_string(platform);
        fputs(",\"system_version\":", stdout);
        print_json_string(system_version);
        fputs(",\"id\":", stdout);
        print_json_string(manifest->id);
        fputs(",\"version\":", stdout);
        print_json_string(manifest->version);
        fputs(",\"min_system\":", stdout);
        print_json_string(manifest->min_system);
        fputs(",\"archive\":", stdout);
        print_json_string(manifest->archive_name);
        fprintf(stdout, ",\"archive_size\":%llu,\"archive_sha256\":\"%s\"",
                (unsigned long long)manifest->archive_size, archive_sha);
        fputs(",\"manifest_url\":", stdout);
        print_json_string(manifest_url);
        fputs(",\"signature_url\":", stdout);
        print_json_string(signature_url);
        fputs(",\"archive_url\":", stdout);
        print_json_string(archive_url);
        fputs(",\"signature_verified\":true,\"signature_key_id\":", stdout);
        print_json_string(key_id);
        fputs("}\n", stdout);
    }
    else
    {
        printf("update available: %s %s (current %s)\n", manifest->id, manifest->version,
               system_version);
        printf("minimum system: %s\n", manifest->min_system);
        printf("archive: %s (%llu bytes, sha256 %s)\n", manifest->archive_name,
               (unsigned long long)manifest->archive_size, archive_sha);
        printf("manifest: %s\n", manifest_url);
        printf("signature: %s (key %s)\n", signature_url, key_id);
        printf("package: %s\n", archive_url);
    }
    return 0;
}

static int print_fetch_staged(const Options* options, const UpdManifest* manifest,
                              const char* key_id, const char* root, const char* stage_dir,
                              const char* archive_path, const char* manifest_path,
                              const char* signature_path, const char* marker_path,
                              int reused)
{
    if (options->json)
    {
        fputs("{\"ok\":true,\"result\":", stdout);
        print_json_string(reused ? "already_staged" : "staged");
        fputs(",\"reused\":", stdout);
        fputs(reused ? "true" : "false", stdout);
        fputs(",\"id\":", stdout);
        print_json_string(manifest->id);
        fputs(",\"version\":", stdout);
        print_json_string(manifest->version);
        fputs(",\"platform\":", stdout);
        print_json_string(manifest->platform);
        fputs(",\"signature_key_id\":", stdout);
        print_json_string(key_id);
        fputs(",\"staging_root\":", stdout);
        print_json_string(root);
        fputs(",\"staged_dir\":", stdout);
        print_json_string(stage_dir);
        fputs(",\"archive_path\":", stdout);
        print_json_string(archive_path);
        fputs(",\"manifest_path\":", stdout);
        print_json_string(manifest_path);
        fputs(",\"signature_path\":", stdout);
        print_json_string(signature_path);
        fputs(",\"marker_path\":", stdout);
        print_json_string(marker_path);
        fputs("}\n", stdout);
    }
    else
    {
        printf("%s %s %s at %s\n", reused ? "already staged" : "staged", manifest->id,
               manifest->version, stage_dir);
        printf("archive: %s\n", archive_path);
        printf("manifest: %s\n", manifest_path);
        printf("signature: %s (key %s)\n", signature_path, key_id);
        printf("marker: %s\n", marker_path);
    }
    return 0;
}

static int staged_manifest_matches_verified(const UpdManifest* manifest,
                                            const UpdStaged* staged)
{
    Sha256Lite context;
    unsigned char digest[UPD_SHA256_BYTES];

    sha256_lite_init(&context);
    sha256_lite_update(&context, manifest->raw, manifest->raw_length);
    sha256_lite_final(&context, digest);
    return memcmp(digest, staged->manifest_sha256, UPD_SHA256_BYTES) == 0;
}

static int resolve_feed_source(const Options* options, char* out, size_t out_size,
                               UpdError* error)
{
    const char* feed = options->feed;

    if (feed == NULL || feed[0] == '\0') feed = getenv("NEXTUI_UPDATE_FEED");
    if (feed == NULL || feed[0] == '\0')
    {
        upd_error_set(error, UPD_E_UNCONFIGURED, "feed_unconfigured",
                      "no update feed is configured; pass --feed or set "
                      "NEXTUI_UPDATE_FEED (no release source is built in)");
        return 0;
    }
    if (strlen(feed) >= out_size)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "feed source is too long");
        return 0;
    }
    snprintf(out, out_size, "%s", feed);
    return 1;
}

static int run_feed(const Options* options, int fetch)
{
    UpdError error;
    UpdFeed feed;
    VerifiedPackage verified;
    UpdStaged staged;
    const UpdFeedEntry* entry = NULL;
    const char* platform;
    const char* system_version;
    char feed_source[4096];
    char feed_path[4096];
    char temp_dir[4096];
    char manifest_path[4096];
    char signature_path[4096];
    char archive_path[4096];
    char manifest_url[UPD_MAX_URL + 1];
    char signature_url[UPD_MAX_URL + 1];
    char archive_url[UPD_MAX_URL + 1];
    char root[4096];
    char stage_dir[4096];
    char staged_manifest[4096];
    char staged_signature[4096];
    char staged_archive[4096];
    char marker_path[4096];
    const char* temp_base;
    const char* temp_prefix;
    uint64_t signature_size = 0;
    uint64_t required = 0;
    uint64_t free_bytes = 0;
    int timeout;
    int comparison;
    int feed_is_temporary = 0;
    int temp_created = 0;
    int result = 0;

    upd_error_clear(&error);
    upd_feed_init(&feed);
    upd_manifest_init(&verified.manifest);
    memset(&staged, 0, sizeof staged);

    platform = resolved_platform(options);
    system_version = resolved_system_version(options);
    if (platform == NULL || platform[0] == '\0')
    {
        upd_error_set(&error, UPD_E_USAGE, "platform_missing",
                      "--platform or PLATFORM is required");
        result = report_error(&error, options->json);
        goto done;
    }
    if (system_version == NULL || system_version[0] == '\0')
    {
        upd_error_set(&error, UPD_E_USAGE, "system_version_missing",
                      "--system-version or NEXTUI_SYSTEM_VERSION is required");
        result = report_error(&error, options->json);
        goto done;
    }
    if (options->trusted_key == NULL || options->trusted_key[0] == '\0')
    {
        upd_error_set(&error, UPD_E_SIGNATURE, "trusted_key_missing",
                      "feed and fetch require --trusted-key");
        result = report_error(&error, options->json);
        goto done;
    }
    if (fetch && (options->staging_root == NULL || options->staging_root[0] == '\0'))
    {
        upd_error_set(&error, UPD_E_USAGE, "staging_root_missing",
                      "fetch needs --staging-root; there is no default");
        result = report_error(&error, options->json);
        goto done;
    }
    timeout = options->timeout_seconds > 0 ? options->timeout_seconds
                                           : UPD_DEFAULT_TIMEOUT_SECONDS;
    if (!resolve_feed_source(options, feed_source, sizeof feed_source, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    if (upd_url_is_remote(feed_source))
    {
        if (!upd_download_temporary(feed_source, UPD_MAX_FEED_BYTES, timeout, feed_path,
                                    sizeof feed_path, &error))
        {
            result = report_error(&error, options->json);
            goto done;
        }
        feed_is_temporary = 1;
    }
    else
    {
        if (strlen(feed_source) >= sizeof feed_path)
        {
            upd_error_set(&error, UPD_E_USAGE, "path_too_long", "feed path is too long");
            result = report_error(&error, options->json);
            goto done;
        }
        snprintf(feed_path, sizeof feed_path, "%s", feed_source);
    }
    if (!upd_feed_load(feed_path, &feed, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    if (!upd_feed_select(&feed, platform, system_version, &entry, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    if (!upd_semver_compare(entry->version, system_version, &comparison))
    {
        upd_error_set(&error, UPD_E_INVALID, "invalid_version",
                      "feed version is not vX.Y.Z");
        result = report_error(&error, options->json);
        goto done;
    }
    if (comparison <= 0)
    {
        result = print_feed_up_to_date(options, platform, system_version, entry);
        goto done;
    }
    if (!upd_url_resolve(feed_source, entry->manifest_url, manifest_url,
                         sizeof manifest_url, &error)
        || !upd_url_resolve(feed_source, entry->signature_url, signature_url,
                            sizeof signature_url, &error)
        || !upd_url_resolve(feed_source, entry->archive_url, archive_url,
                            sizeof archive_url, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }

    if (fetch)
    {
        if (strlen(options->staging_root) >= sizeof root)
        {
            upd_error_set(&error, UPD_E_USAGE, "path_too_long", "staging root is too long");
            result = report_error(&error, options->json);
            goto done;
        }
        snprintf(root, sizeof root, "%s", options->staging_root);
        if (!upd_mkdir_p(root, &error))
        {
            result = report_error(&error, options->json);
            goto done;
        }
        temp_base = root;
        temp_prefix = ".nextupd-incoming-";
    }
    else
    {
        temp_base = getenv("TMPDIR");
        if (temp_base == NULL || temp_base[0] == '\0') temp_base = "/tmp";
        temp_prefix = "nextupd-check-";
    }
    if (!make_temp_dir(temp_base, temp_prefix, temp_dir, sizeof temp_dir, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    temp_created = 1;
    if (snprintf(manifest_path, sizeof manifest_path, "%s/manifest.json", temp_dir)
            >= (int)sizeof manifest_path
        || snprintf(signature_path, sizeof signature_path, "%s/manifest.sig", temp_dir)
               >= (int)sizeof signature_path
        || snprintf(archive_path, sizeof archive_path, "%s/%s", temp_dir,
                    entry->archive_name)
               >= (int)sizeof archive_path)
    {
        upd_error_set(&error, UPD_E_USAGE, "path_too_long", "temporary path is too long");
        result = report_error(&error, options->json);
        goto done;
    }
    if (!upd_download_to_file(manifest_url, manifest_path, UPD_MAX_MANIFEST_BYTES, timeout,
                              &error)
        || !upd_download_to_file(signature_url, signature_path, UPD_MAX_SIGNATURE_BYTES,
                                 timeout, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    if (!upd_manifest_load(manifest_path, &verified.manifest, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    if (!upd_signature_verify(verified.manifest.raw, verified.manifest.raw_length,
                              signature_path, options->trusted_key, verified.key_id,
                              sizeof verified.key_id, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    verified.signature_verified = 1;
    if (!upd_feed_entry_matches_manifest(entry, &verified.manifest, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    if (!upd_manifest_check_min_system(&verified.manifest, system_version, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    if (!fetch)
    {
        result = print_feed_available(options, platform, system_version,
                                      &verified.manifest, verified.key_id, manifest_url,
                                      signature_url, archive_url);
        goto done;
    }

    if (!stage_dir_for(root, &verified.manifest, stage_dir, sizeof stage_dir, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    if (snprintf(staged_manifest, sizeof staged_manifest, "%s/manifest.json", stage_dir)
            >= (int)sizeof staged_manifest
        || snprintf(staged_signature, sizeof staged_signature, "%s/manifest.sig", stage_dir)
               >= (int)sizeof staged_signature
        || snprintf(staged_archive, sizeof staged_archive, "%s/%s", stage_dir,
                    verified.manifest.archive_name)
               >= (int)sizeof staged_archive
        || snprintf(marker_path, sizeof marker_path, "%s/staged.json", stage_dir)
               >= (int)sizeof marker_path)
    {
        upd_error_set(&error, UPD_E_USAGE, "path_too_long", "staging path is too long");
        result = report_error(&error, options->json);
        goto done;
    }

    if (!options->replace && access(marker_path, F_OK) == 0)
    {
        if (upd_staged_load(stage_dir, &staged, &error)
            && upd_staged_verify(stage_dir, &staged, &verified.manifest, verified.key_id,
                                 &error)
            && staged_manifest_matches_verified(&verified.manifest, &staged))
        {
            result = print_fetch_staged(options, &verified.manifest, verified.key_id, root,
                                        stage_dir, staged_archive, staged_manifest,
                                        staged_signature, marker_path, 1);
            goto done;
        }
        /* A marker that does not verify is not reusable. Replace it rather than
         * leaving a package that a later install could mistake for valid. */
        upd_error_clear(&error);
    }

    if (!upd_file_size(signature_path, &signature_size, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    if (verified.manifest.archive_size > (UINT64_MAX - 64 * 1024) / 2
        || verified.manifest.raw_length > UINT64_MAX - verified.manifest.archive_size * 2
        || signature_size
               > UINT64_MAX - verified.manifest.archive_size * 2
                     - verified.manifest.raw_length - 64 * 1024)
    {
        upd_error_set(&error, UPD_E_INVALID, "too_large", "staging requirement overflows");
        result = report_error(&error, options->json);
        goto done;
    }
    required = verified.manifest.archive_size * 2 + verified.manifest.raw_length
               + signature_size + 64 * 1024;
    if (!storage_free_bytes(root, &free_bytes, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
#ifdef NEXTUPD_TESTING
    {
        long long override;
        if (test_limit("NEXTUPD_TEST_FREE_BYTES", &override))
            free_bytes = (uint64_t)override;
    }
#endif
    if (free_bytes < required)
    {
        upd_error_set(&error, UPD_E_SPACE, "insufficient_space",
                      "need %llu bytes, %llu available", (unsigned long long)required,
                      (unsigned long long)free_bytes);
        result = report_error(&error, options->json);
        goto done;
    }
    if (!upd_download_to_file(archive_url, archive_path, verified.manifest.archive_size,
                              timeout, &error))
    {
        result = report_error(&error, options->json);
        goto done;
    }
    {
        uint64_t downloaded_size = 0;
        if (!upd_file_size(archive_path, &downloaded_size, &error))
        {
            result = report_error(&error, options->json);
            goto done;
        }
        if (downloaded_size != verified.manifest.archive_size)
        {
            upd_error_set(&error, UPD_E_INTEGRITY, "archive_size_mismatch",
                          "archive is %llu bytes, manifest says %llu",
                          (unsigned long long)downloaded_size,
                          (unsigned long long)verified.manifest.archive_size);
            result = report_error(&error, options->json);
            goto done;
        }
    }
    {
        Options stage_options = *options;
        stage_options.package = archive_path;
        stage_options.manifest = manifest_path;
        stage_options.signature = signature_path;
        stage_options.staging_root = root;
        stage_options.feed = NULL;
        stage_options.quiet = 1;
        stage_options.replace = 1;
        result = run_stage(&stage_options);
    }
    if (result != 0) goto done;
    result = print_fetch_staged(options, &verified.manifest, verified.key_id, root,
                                stage_dir, staged_archive, staged_manifest,
                                staged_signature, marker_path, 0);

done:
    if (temp_created) cleanup_temp_dir(temp_dir, entry != NULL ? entry->archive_name : NULL);
    if (feed_is_temporary) unlink(feed_path);
    upd_manifest_free(&verified.manifest);
    upd_feed_free(&feed);
    return result;
}

static void print_transaction_result(const Options* options,
                                     const UpdTransactionResult* result, const char* id,
                                     const char* target_root, const char* staged_dir)
{
    if (options->json)
    {
        fputs("{\"ok\":true,\"result\":", stdout);
        print_json_string(result->outcome);
        fputs(",\"outcome\":", stdout);
        print_json_string(result->outcome);
        fputs(",\"state\":", stdout);
        print_json_string(result->state);
        fputs(",\"version\":", stdout);
        print_json_string(result->version);
        fputs(",\"previous_version\":", stdout);
        print_json_string(result->previous_version);
        if (id != NULL)
        {
            fputs(",\"id\":", stdout);
            print_json_string(id);
        }
        if (target_root != NULL)
        {
            fputs(",\"target_root\":", stdout);
            print_json_string(target_root);
        }
        if (staged_dir != NULL)
        {
            fputs(",\"staged_dir\":", stdout);
            print_json_string(staged_dir);
        }
        fputs("}\n", stdout);
        return;
    }
    printf("%s: %s\n", result->outcome, result->state);
    if (id != NULL) printf("package: %s %s\n", id, result->version);
    else if (result->version[0] != '\0')
        printf("version: %s\n", result->version);
    if (result->previous_version[0] != '\0')
        printf("previous: %s\n", result->previous_version);
    if (target_root != NULL) printf("target: %s\n", target_root);
    if (staged_dir != NULL) printf("staged: %s\n", staged_dir);
}

typedef struct
{
    int descriptor;
} TargetLock;

/* Serialize install and recover on one target root. The lock lives beside the
 * live tree, never inside it, and is advisory: a crashed holder releases it
 * when the process exits. A missing root is left to upd_install_execute() so
 * it can report target_missing instead of a misleading lock error. */
static int target_lock_acquire(const char* root, TargetLock* lock, UpdError* error)
{
    struct stat status;
    int descriptor;

    lock->descriptor = -1;
    if (root == NULL || root[0] == '\0') return 1;
    if (stat(root, &status) != 0 || !S_ISDIR(status.st_mode)) return 1;
    descriptor = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot open %s: %s", root,
                      strerror(errno));
        return 0;
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0)
    {
        upd_error_set(error, UPD_E_BUSY, "busy",
                      errno == EWOULDBLOCK ? "another install or recovery is running"
                                           : "cannot lock target root");
        close(descriptor);
        return 0;
    }
#ifdef NEXTUPD_TESTING
    {
        const char* marker = getenv("NEXTUPD_TEST_LOCK_MARKER");
        const char* hold = getenv("NEXTUPD_TEST_HOLD_LOCK_MS");

        if (marker != NULL && marker[0] != '\0')
        {
            int marker_fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            if (marker_fd >= 0) close(marker_fd);
        }
        if (hold != NULL && hold[0] != '\0')
        {
            long milliseconds = strtol(hold, NULL, 10);
            if (milliseconds > 0) usleep((useconds_t)milliseconds * 1000);
        }
    }
#endif
    lock->descriptor = descriptor;
    return 1;
}

static void target_lock_release(TargetLock* lock)
{
    if (lock->descriptor >= 0)
    {
        close(lock->descriptor);
        lock->descriptor = -1;
    }
}

static int run_install(const Options* options)
{
    UpdStaged staged;
    VerifiedPackage verified;
    UpdInstallRequest request;
    UpdTransactionResult result;
    Options check_options;
    UpdError error;
    TargetLock target_lock;
    char archive[PATH_MAX];
    char manifest[PATH_MAX];
    char signature[PATH_MAX];
    int code;

    upd_error_clear(&error);
    if (!upd_staged_load(options->staged_dir, &staged, &error))
        return report_error(&error, options->json);
    if (snprintf(archive, sizeof archive, "%s/%s", options->staged_dir, staged.archive)
            >= (int)sizeof archive
        || snprintf(manifest, sizeof manifest, "%s/%s", options->staged_dir,
                    "manifest.json")
               >= (int)sizeof manifest
        || snprintf(signature, sizeof signature, "%s/%s", options->staged_dir,
                    "manifest.sig")
               >= (int)sizeof signature)
    {
        upd_error_set(&error, UPD_E_USAGE, "path_too_long",
                      "staged package path is too long");
        return report_error(&error, options->json);
    }

    check_options = *options;
    check_options.package = archive;
    check_options.manifest = manifest;
    check_options.signature = signature;
    if (!verify_package(&check_options, 1, &verified, &error))
    {
        code = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return code;
    }
    if (!upd_staged_verify(options->staged_dir, &staged, &verified.manifest,
                           verified.key_id, &error))
    {
        code = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return code;
    }
    memset(&request, 0, sizeof request);
    request.target_root = options->target_root;
    request.staged_dir = options->staged_dir;
    request.platform = options->platform;
    request.system_version = options->system_version;
    request.manifest = &verified.manifest;
    if (!target_lock_acquire(options->target_root, &target_lock, &error))
    {
        code = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return code;
    }
    if (!upd_install_execute(&request, &result, &error))
    {
        target_lock_release(&target_lock);
        code = report_error(&error, options->json);
        upd_manifest_free(&verified.manifest);
        return code;
    }
    target_lock_release(&target_lock);
    print_transaction_result(options, &result, verified.manifest.id, options->target_root,
                             options->staged_dir);
    upd_manifest_free(&verified.manifest);
    return 0;
}

static int run_recover(const Options* options)
{
    UpdTransactionResult result;
    UpdError error;
    TargetLock target_lock;

    upd_error_clear(&error);
    if (!target_lock_acquire(options->target_root, &target_lock, &error))
        return report_error(&error, options->json);
    if (!upd_install_recover(options->target_root, options->rollback, &result, &error))
    {
        target_lock_release(&target_lock);
        return report_error(&error, options->json);
    }
    target_lock_release(&target_lock);
    print_transaction_result(options, &result, NULL, options->target_root, NULL);
    return 0;
}

static int parse_timeout_option(const char* text, int* timeout_seconds)
{
    char* end = NULL;
    long value;

    if (text == NULL || text[0] == '\0') return 0;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0' || value < 1 || value > 3600)
        return 0;
    *timeout_seconds = (int)value;
    return 1;
}

static int parse_options(int argc, char** argv, const char* command, Options* options)
{
    int i;

    memset(options, 0, sizeof *options);
    for (i = 0; i < argc; i++)
    {
        const char* argument = argv[i];
        const char** target = NULL;

        if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0)
        {
            usage(stdout);
            exit(0);
        }
        if (strcmp(argument, "--package") == 0) target = &options->package;
        else if (strcmp(argument, "--manifest") == 0) target = &options->manifest;
        else if (strcmp(argument, "--signature") == 0) target = &options->signature;
        else if (strcmp(argument, "--trusted-key") == 0) target = &options->trusted_key;
        else if (strcmp(argument, "--platform") == 0) target = &options->platform;
        else if (strcmp(argument, "--system-version") == 0) target = &options->system_version;
        else if (strcmp(argument, "--staging-root") == 0) target = &options->staging_root;
        else if (strcmp(argument, "--feed") == 0) target = &options->feed;
        else if (strcmp(argument, "--id") == 0) target = &options->id;
        else if (strcmp(argument, "--kind") == 0) target = &options->kind;
        else if (strcmp(argument, "--version") == 0) target = &options->version;
        else if (strcmp(argument, "--min-system") == 0) target = &options->min_system;
        else if (strcmp(argument, "--out") == 0) target = &options->out;
        else if (strcmp(argument, "--staged-dir") == 0) target = &options->staged_dir;
        else if (strcmp(argument, "--target-root") == 0) target = &options->target_root;
        else if (strcmp(argument, "--timeout") == 0)
        {
            if (i + 1 >= argc || argv[i + 1][0] == '\0')
            {
                fprintf(stderr, "nextupd: --timeout requires a value\n");
                return 0;
            }
            if (!parse_timeout_option(argv[++i], &options->timeout_seconds))
            {
                fprintf(stderr, "nextupd: --timeout must be between 1 and 3600 seconds\n");
                return 0;
            }
            continue;
        }
        else if (strcmp(argument, "--integrity-only") == 0)
        {
            options->integrity_only = 1;
            continue;
        }
        else if (strcmp(argument, "--replace") == 0)
        {
            options->replace = 1;
            continue;
        }
        else if (strcmp(argument, "--rollback") == 0)
        {
            options->rollback = 1;
            continue;
        }
        else if (strcmp(argument, "--json") == 0)
        {
            options->json = 1;
            continue;
        }
        else
        {
            fprintf(stderr, "nextupd: unknown argument '%s'\n", argument);
            usage(stderr);
            return 0;
        }
        if (i + 1 >= argc || argv[i + 1][0] == '\0')
        {
            fprintf(stderr, "nextupd: %s requires a value\n", argument);
            return 0;
        }
        *target = argv[++i];
    }
    if (strcmp(command, "feed") != 0 && strcmp(command, "fetch") != 0
        && (options->feed != NULL || options->timeout_seconds != 0))
    {
        fprintf(stderr, "nextupd: --feed/--timeout are only valid for feed and fetch\n");
        return 0;
    }
    if (strcmp(command, "manifest") == 0
        && (options->id == NULL || options->kind == NULL || options->version == NULL
            || options->platform == NULL || options->package == NULL))
    {
        fprintf(stderr,
                "nextupd: manifest needs --package, --id, --kind, --version and --platform\n");
        return 0;
    }
    if ((strcmp(command, "check") == 0 || strcmp(command, "stage") == 0)
        && (options->package == NULL || options->manifest == NULL))
    {
        fprintf(stderr, "nextupd: %s needs --package and --manifest\n", command);
        return 0;
    }
    if (strcmp(command, "install") == 0 && options->target_root == NULL)
    {
        fprintf(stderr,
                "nextupd: target_root_missing: --target-root is required; there is no "
                "default\n");
        return 0;
    }
    if (strcmp(command, "install") == 0
        && (options->staged_dir == NULL || options->trusted_key == NULL
            || options->platform == NULL || options->system_version == NULL))
    {
        fprintf(stderr,
                "nextupd: install needs --staged-dir, --target-root, --trusted-key, "
                "--platform and --system-version\n");
        return 0;
    }
    if (strcmp(command, "recover") == 0 && options->target_root == NULL)
    {
        fprintf(stderr, "nextupd: recover needs --target-root\n");
        return 0;
    }
    if ((strcmp(command, "feed") == 0 || strcmp(command, "fetch") == 0)
        && (options->package != NULL || options->manifest != NULL
            || options->signature != NULL || options->id != NULL
            || options->kind != NULL || options->version != NULL
            || options->min_system != NULL || options->out != NULL
            || options->staged_dir != NULL || options->target_root != NULL
            || options->integrity_only || options->rollback))
    {
        fprintf(stderr, "nextupd: %s does not accept package/manifest/install options\n",
                command);
        return 0;
    }
    if ((strcmp(command, "feed") == 0 || strcmp(command, "fetch") == 0)
        && options->trusted_key == NULL)
    {
        fprintf(stderr, "nextupd: %s needs --trusted-key\n", command);
        return 0;
    }
    if (strcmp(command, "feed") == 0 && options->replace)
    {
        fprintf(stderr, "nextupd: feed does not accept --replace\n");
        return 0;
    }
    if (strcmp(command, "feed") == 0 && options->staging_root != NULL)
    {
        fprintf(stderr, "nextupd: feed does not accept --staging-root\n");
        return 0;
    }
    if (strcmp(command, "fetch") == 0 && options->staging_root == NULL)
    {
        fprintf(stderr, "nextupd: fetch needs --staging-root; there is no default\n");
        return 0;
    }
    if (strcmp(command, "manifest") == 0
        && (options->signature != NULL || options->trusted_key != NULL
            || options->system_version != NULL || options->staging_root != NULL
            || options->staged_dir != NULL || options->target_root != NULL
            || options->integrity_only || options->replace || options->rollback
            || options->json))
    {
        fprintf(stderr, "nextupd: manifest does not accept check/stage options\n");
        return 0;
    }
    if (strcmp(command, "check") == 0
        && (options->id != NULL || options->kind != NULL || options->version != NULL
            || options->min_system != NULL || options->out != NULL
            || options->staging_root != NULL || options->staged_dir != NULL
            || options->target_root != NULL || options->replace || options->rollback))
    {
        fprintf(stderr, "nextupd: check does not accept manifest/stage/install options\n");
        return 0;
    }
    if (strcmp(command, "stage") == 0
        && (options->id != NULL || options->kind != NULL || options->version != NULL
            || options->min_system != NULL || options->out != NULL
            || options->staged_dir != NULL || options->target_root != NULL
            || options->integrity_only || options->rollback))
    {
        fprintf(stderr, "nextupd: stage does not accept manifest/check/install options\n");
        return 0;
    }
    if (strcmp(command, "install") == 0
        && (options->package != NULL || options->manifest != NULL
            || options->signature != NULL || options->id != NULL
            || options->kind != NULL || options->version != NULL
            || options->min_system != NULL || options->out != NULL
            || options->staging_root != NULL || options->integrity_only
            || options->replace || options->rollback))
    {
        fprintf(stderr, "nextupd: install does not accept package/manifest options\n");
        return 0;
    }
    if (strcmp(command, "recover") == 0
        && (options->package != NULL || options->manifest != NULL
            || options->signature != NULL || options->trusted_key != NULL
            || options->platform != NULL || options->system_version != NULL
            || options->staging_root != NULL || options->staged_dir != NULL
            || options->id != NULL || options->kind != NULL || options->version != NULL
            || options->min_system != NULL || options->out != NULL
            || options->integrity_only || options->replace))
    {
        fprintf(stderr, "nextupd: recover accepts only --target-root, --rollback and --json\n");
        return 0;
    }
    return 1;
}

int main(int argc, char** argv)
{
    const char* command;
    Options options;

    if (argc < 2)
    {
        usage(stderr);
        return UPD_E_USAGE;
    }
    command = argv[1];
    if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0)
    {
        usage(stdout);
        return 0;
    }
    if (strcmp(command, "check") != 0 && strcmp(command, "stage") != 0
        && strcmp(command, "install") != 0 && strcmp(command, "recover") != 0
        && strcmp(command, "manifest") != 0 && strcmp(command, "feed") != 0
        && strcmp(command, "fetch") != 0)
    {
        fprintf(stderr, "nextupd: unknown command '%s'\n", command);
        usage(stderr);
        return UPD_E_USAGE;
    }
    if (!parse_options(argc - 2, argv + 2, command, &options)) return UPD_E_USAGE;
    if (strcmp(command, "check") == 0) return run_check(&options);
    if (strcmp(command, "feed") == 0) return run_feed(&options, 0);
    if (strcmp(command, "fetch") == 0) return run_feed(&options, 1);
    if (strcmp(command, "stage") == 0) return run_stage(&options);
    if (strcmp(command, "install") == 0) return run_install(&options);
    if (strcmp(command, "recover") == 0) return run_recover(&options);
    return run_manifest(&options);
}
